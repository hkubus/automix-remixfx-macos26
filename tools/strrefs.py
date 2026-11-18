#!/usr/bin/env python3
"""strrefs.py MACHO REFS_TSV REGEX  — find C strings (cstring / oslogstring sections) matching REGEX, list the
instructions that reference them and the function (LC_FUNCTION_STARTS) each reference sits in.
Also understands Swift large string literals, whose code references point at (string address - 0x20)."""
import re, struct, sys, bisect

data = open(sys.argv[1], "rb").read()
rx = re.compile(sys.argv[3].encode())
ncmds = struct.unpack_from("<I", data, 16)[0]
o, sects, segs, fstarts_blob, text_vm = 32, [], [], None, None
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", data, o)
    if cmd == 0x19:
        name = data[o + 8:o + 24].rstrip(b"\0").decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, o + 24)
        segs.append((vmaddr, vmsize, fileoff, filesize))
        if name == "__TEXT": text_vm = vmaddr
        for k in range(struct.unpack_from("<I", data, o + 64)[0]):
            so = o + 72 + 80 * k
            sects.append((data[so:so + 16].rstrip(b"\0").decode(), *struct.unpack_from("<QQ", data, so + 32), struct.unpack_from("<I", data, so + 48)[0]))
    elif cmd == 0x26:
        dof, dsz = struct.unpack_from("<II", data, o + 8)
        fstarts_blob = data[dof:dof + dsz]
    o += size

fstarts = []
if fstarts_blob:
    a, i = text_vm, 0
    while i < len(fstarts_blob):
        v, sh = 0, 0
        while True:
            b = fstarts_blob[i]; i += 1
            v |= (b & 0x7F) << sh; sh += 7
            if not b & 0x80: break
        if v == 0: break
        a += v; fstarts.append(a)

def func_of(addr):
    k = bisect.bisect_right(fstarts, addr) - 1
    return fstarts[k] if k >= 0 else None

refs = {}
for line in open(sys.argv[2]):
    t, a, mn = line.split("\t")
    refs.setdefault(int(t, 16), []).append((int(a, 16), mn.strip()))

for sn, addr, size, off in sects:
    if sn not in ("__cstring", "__oslogstring"): continue
    blob = data[off:off + size]
    for m in re.finditer(rb"[^\0]+", blob):
        if not rx.search(m.group(0)): continue
        sa = addr + m.start()
        hits = refs.get(sa, []) + [(x, mn + " (swift lit)") for x, mn in refs.get(sa - 0x20, [])]
        print(f"{sa:#x} [{sn}] {m.group(0)[:110].decode('utf-8', 'replace')!r}")
        for x, mn in hits:
            print(f"      ref {x:#x} {mn:6s} in func {func_of(x):#x}")
