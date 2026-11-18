#!/usr/bin/env python3
"""dsc_extract_one.py CACHE_BASE IMAGE_PATH OUT [--info]

Rebuild ONE image from a split dyld shared cache as a standalone Mach-O for static analysis
(llvm-objdump / otool / strings) without extracting the whole cache (dsc_extractor writes every image).

- Segments keep their cache VM addresses; file offsets are rewritten into the output file.
- __LINKEDIT is replaced by a small one holding only this image's function starts and symbol table
  (exported/indirect nlists with their strings) — the cache's LINKEDIT is shared and hundreds of MB.
- Load commands that point at data we don't carry are zeroed. Pointers into other images stay as-is
  (cache addresses); they can be attributed with the cache's .map file.
RE-only: the result is for reading, not loading.
"""
import glob, mmap, os, struct, sys

LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB = 0x19, 0x2, 0xB
LC_DYLD_INFO, LC_DYLD_INFO_ONLY = 0x22, 0x80000022
LINKEDIT_DATA = {0x1D: "CODE_SIGNATURE", 0x1E: "SEGMENT_SPLIT_INFO", 0x26: "FUNCTION_STARTS", 0x29: "DATA_IN_CODE",
                 0x2B: "DYLIB_CODE_SIGN_DRS", 0x2E: "LINKER_OPTIMIZATION_HINT", 0x80000033: "DYLD_EXPORTS_TRIE",
                 0x80000034: "DYLD_CHAINED_FIXUPS", 0x35: "FILESET_ENTRY?", 0x31: "NOTE?"}


class Cache:
    def __init__(self, base):
        self.maps = []
        self.files = []
        self.slid = []      # (addr, size, mm, fileoff, slide_info_fileoff) for mappings with slide info
        for f in sorted(glob.glob(base + "*")):
            if f.endswith((".map", ".atlas")):
                continue
            fh = open(f, "rb")
            mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
            if mm[:4] != b"dyld":
                continue
            self.files.append(mm)
            off, cnt = struct.unpack_from("<II", mm, 16)
            for i in range(cnt):
                a, sz, fo, _, _ = struct.unpack_from("<QQQII", mm, off + 32 * i)
                self.maps.append((a, sz, mm, fo))
            mwo, mwc = struct.unpack_from("<II", mm, 0x138)
            if mwo and mwo < 0x1000:
                for i in range(mwc):
                    a, sz, fo, so, ss, _, _, _ = struct.unpack_from("<QQQQQQII", mm, mwo + 56 * i)
                    if ss:
                        self.slid.append((a, sz, mm, fo, so))
        self.maps.sort(key=lambda m: m[0])

    def read(self, addr, n):
        out = bytearray()
        while n > 0:
            for a, sz, mm, fo in self.maps:
                if a <= addr < a + sz:
                    k = min(n, a + sz - addr)
                    out += mm[fo + addr - a: fo + addr - a + k]
                    addr += k; n -= k
                    break
            else:
                raise ValueError(f"unmapped address {addr:#x}")
        return bytes(out)


def unslide(C, vmaddr, data):
    """Decode slide-info v2 pointer chains for the pages covering [vmaddr, vmaddr+len(data)) in place."""
    out = bytearray(data)
    for a, sz, mm, fo, so in C.slid:
        if not (a < vmaddr + len(data) and vmaddr < a + sz):
            continue
        ver, psz, ps_off, ps_cnt, pe_off, pe_cnt, dmask, vadd = struct.unpack_from("<IIIIIIQQ", mm, so)
        assert ver == 2, ver
        shift = (dmask & -dmask).bit_length() - 1 - 2
        first = max(0, (vmaddr - a) // psz); last = min(ps_cnt - 1, (vmaddr + len(data) - 1 - a) // psz)
        def chain(page_addr, start):
            o = start
            while True:
                raw = struct.unpack_from("<Q", mm, fo + (page_addr - a) + o)[0]
                delta = (raw & dmask) >> shift
                val = raw & ~dmask & 0xFFFFFFFFFFFFFFFF
                if val: val += vadd
                t = page_addr + o
                if vmaddr <= t and t + 8 <= vmaddr + len(data):
                    struct.pack_into("<Q", out, t - vmaddr, val)
                if not delta: break
                o += delta
        for pg in range(first, last + 1):
            st = struct.unpack_from("<H", mm, so + ps_off + 2 * pg)[0]
            if st == 0x4000: continue                       # no rebase on this page
            page_addr = a + pg * psz
            if st & 0x8000:                                  # multiple chains (extras)
                j = st & 0x3FFF
                while True:
                    e = struct.unpack_from("<H", mm, so + pe_off + 2 * j)[0]
                    chain(page_addr, (e & 0x3FFF) * 4)
                    if e & 0x8000: break
                    j += 1
            else:
                chain(page_addr, st * 4)
    return bytes(out)


def image_text_addr(base, path):
    cur = None
    for line in open(base + ".map", errors="replace"):
        if line.startswith("/"):
            cur = line.strip()
        elif cur == path and "__TEXT" in line:
            return int(line.split()[1], 16)
    raise SystemExit(f"{path} not in {base}.map")


def main():
    base, path, out = sys.argv[1], sys.argv[2], sys.argv[3]
    info = "--info" in sys.argv
    C = Cache(base)
    text = image_text_addr(base, path)
    magic, cpu, sub, ftype, ncmds, sizecmds, flags, _ = struct.unpack("<IiiIIIII", C.read(text, 32))
    assert magic == 0xFEEDFACF, hex(magic)
    cmds = bytearray(C.read(text + 32, sizecmds))

    segs, linkedit = [], None
    o = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, o)
        if cmd == LC_SEGMENT_64:
            name = cmds[o + 8:o + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", cmds, o + 24)
            nsects = struct.unpack_from("<I", cmds, o + 64)[0]
            segs.append(dict(name=name, o=o, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize, nsects=nsects))
            if name == "__LINKEDIT":
                linkedit = segs[-1]
            if info:
                print(f"LC_SEGMENT_64 {name:14s} vm {vmaddr:#x}+{vmsize:#x} file {fileoff:#x}+{filesize:#x} nsects {nsects}")
                for s in range(nsects):
                    so = o + 72 + 80 * s
                    sn = cmds[so:so + 16].rstrip(b"\0").decode(); sg = cmds[so + 16:so + 32].rstrip(b"\0").decode()
                    addr, ssz = struct.unpack_from("<QQ", cmds, so + 32)
                    print(f"      {sg},{sn:24s} {addr:#x}+{ssz:#x}")
        elif info:
            print(f"cmd {cmd:#x} ({LINKEDIT_DATA.get(cmd, '')}) size {size}")
        o += size

    def le_read(fileoff, n):          # LINKEDIT data by file offset -> VM address in the shared LINKEDIT
        return C.read(linkedit["vmaddr"] + (fileoff - linkedit["fileoff"]), n)

    # ---- lay out: __TEXT at 0 (holds the header), other segments page-aligned after it
    body = {}
    pos = 0
    for s in segs:
        if s["name"] == "__LINKEDIT":
            continue
        pos = (pos + 0xFFF) & ~0xFFF
        n = s["vmsize"] if s["name"] == "__TEXT" else min(s["vmsize"], s["filesize"] or s["vmsize"])
        data = C.read(s["vmaddr"], n)
        if s["name"] != "__TEXT":
            data = unslide(C, s["vmaddr"], data)
        body[s["name"]] = (pos, data)
        delta = pos - s["fileoff"]
        struct.pack_into("<QQ", cmds, s["o"] + 40, pos, len(data))          # fileoff, filesize
        for k in range(s["nsects"]):
            so = s["o"] + 72 + 80 * k
            off = struct.unpack_from("<I", cmds, so + 48)[0]
            if off:
                addr = struct.unpack_from("<Q", cmds, so + 32)[0]
                struct.pack_into("<I", cmds, so + 48, pos + (addr - s["vmaddr"]))
            struct.pack_into("<II", cmds, so + 56, 0, 0)                        # reloff, nreloc
        pos += len(data)
    pos = (pos + 0xFFF) & ~0xFFF
    le_start = pos
    le = bytearray()

    # ---- carry function starts + symtab; zero everything else that points into LINKEDIT
    o = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, o)
        if cmd in LINKEDIT_DATA:
            dataoff, datasize = struct.unpack_from("<II", cmds, o + 8)
            if cmd == 0x26 and datasize:                                     # function starts
                blob = le_read(dataoff, datasize)
                struct.pack_into("<II", cmds, o + 8, le_start + len(le), len(blob)); le += blob
            else:
                struct.pack_into("<II", cmds, o + 8, 0, 0)
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", cmds, o + 8)
            syms = le_read(symoff, 16 * nsyms) if nsyms else b""
            strtab = bytearray(b"\0 ")
            newsyms = bytearray()
            for i in range(nsyms):
                strx, ntype, nsect, ndesc, nvalue = struct.unpack_from("<IBBHQ", syms, 16 * i)
                name = b""
                if strx:
                    # strings live in the shared pool; read until NUL in chunks
                    chunk = le_read(stroff + strx, 256)
                    while b"\0" not in chunk:
                        chunk += le_read(stroff + strx + len(chunk), 256)
                    name = chunk[:chunk.index(b"\0")]
                newsyms += struct.pack("<IBBHQ", len(strtab), ntype, nsect, ndesc, nvalue)
                strtab += name + b"\0"
            so = le_start + len(le); le += newsyms
            st = le_start + len(le); le += strtab
            struct.pack_into("<IIII", cmds, o + 8, so, nsyms, st, len(strtab))
            if info:
                print(f"symtab: {nsyms} symbols carried")
        elif cmd == LC_DYSYMTAB:
            # keep local/extdef/undef index ranges (they index our symtab), drop tables we don't carry
            struct.pack_into("<IIIIIIII", cmds, o + 32, 0, 0, 0, 0, 0, 0, 0, 0)   # tocoff..nindirectsyms
            struct.pack_into("<IIII", cmds, o + 64, 0, 0, 0, 0)                 # extreloff..nlocrel
        elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
            struct.pack_into("<IIIIIIIIII", cmds, o + 8, *([0] * 10))
        o += size
    struct.pack_into("<QQQQ", cmds, linkedit["o"] + 24, linkedit["vmaddr"], (len(le) + 0xFFF) & ~0xFFF, le_start, len(le))

    with open(out, "wb") as f:
        hdr, tdata = body["__TEXT"][0], bytearray(body["__TEXT"][1])
        tdata[32:32 + sizecmds] = cmds
        f.write(tdata)
        for name, (p, data) in body.items():
            if name == "__TEXT":
                continue
            f.seek(p); f.write(data)
        f.seek(le_start); f.write(le)
    print(f"wrote {out}: {len(segs)} segments, linkedit {len(le)} bytes")


if __name__ == "__main__":
    main()
