#!/usr/bin/env python3
"""swiftmeta.py MACHO [--types] [--conf] [--filter REGEX] [--cache CACHE_BASE]

Dump Swift reflection metadata from an x86_64 Mach-O (e.g. one rebuilt by dsc_extract_one.py):
  --types  every nominal type in __swift5_types: kind, qualified name, fields (enum cases) with their types
  --conf   every protocol conformance in __swift5_proto: Type : Protocol -> witness table + its entries
Symbolic references inside mangled type names are resolved to names when they point into this image.
--cache: also read addresses outside the image (shared GOT, other images) from the dyld cache it came from.
"""
import re, struct, subprocess, sys

data = open(sys.argv[1], "rb").read()
args = sys.argv[2:]
flt = re.compile(args[args.index("--filter") + 1]) if "--filter" in args else None
CACHE = None
if "--cache" in args:
    import importlib.util, os
    spec = importlib.util.spec_from_file_location("dx", os.path.join(os.path.dirname(os.path.abspath(__file__)), "dsc_extract_one.py"))
    dx = importlib.util.module_from_spec(spec); spec.loader.exec_module(dx)
    CACHE = dx.Cache(args[args.index("--cache") + 1])


def unslide_ptr(raw):      # slide-info v2 encoding used in x86_64 cache data (delta in bits 40-55)
    if raw == 0 or raw >= 0x7FF800000000 and raw >> 56 == 0 and not (raw & 0x00FFFF0000000000) >> 47:
        return raw
    return (raw & 0x000000FFFFFFFFFF) + 0x7FF800000000

ncmds, sizecmds = struct.unpack_from("<II", data, 16)
segs, sects = [], {}
o = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", data, o)
    if cmd == 0x19:
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, o + 24)
        segs.append((vmaddr, vmsize, fileoff, filesize))
        nsects = struct.unpack_from("<I", data, o + 64)[0]
        for k in range(nsects):
            so = o + 72 + 80 * k
            sn = data[so:so + 16].rstrip(b"\0").decode()
            addr, ssz = struct.unpack_from("<QQ", data, so + 32)
            sects[sn] = (addr, ssz)
    o += size


def off(addr):
    for vm, vs, fo, fs in segs:
        if vm <= addr < vm + min(vs, fs):
            return fo + addr - vm
    return None


def rd(a, n):
    p = off(a)
    if p is not None: return data[p:p + n]
    if CACHE is not None: return CACHE.read(a, n)
    raise ValueError(f"address {a:#x} outside image (use --cache)")
def u32(a): return struct.unpack("<I", rd(a, 4))[0]
def i32(a): return struct.unpack("<i", rd(a, 4))[0]
def u64(a): return struct.unpack("<Q", rd(a, 8))[0]
def readable(a):
    if off(a) is not None: return True
    if CACHE is None: return False
    try: CACHE.read(a, 1); return True
    except Exception: return False


def cstr(a):
    p = off(a)
    if p is None:
        if not readable(a): return f"<{a:#x}>"
        b = rd(a, 256); return b[:b.index(b"\0") if b"\0" in b else 256].decode("utf-8", "replace")
    e = data.index(b"\0", p)
    return data[p:e].decode("utf-8", "replace")


def rel(a):            # relative direct pointer
    v = i32(a); return a + v if v else 0


def rel_ind(a):        # relative indirectable pointer (low bit = indirect through GOT)
    v = i32(a)
    if not v: return 0
    t = a + (v & ~1)
    if v & 1:
        return unslide_ptr(u64(t)) if readable(t) else 0
    return t


ctx_cache = {}
def ctx_name(a, depth=0):
    """qualified name of a context descriptor (module / type / extension / anonymous)"""
    if a in ctx_cache: return ctx_cache[a]
    if not readable(a) or depth > 12: return f"<ext {a:#x}>"
    flags = u32(a); kind = flags & 0x1F
    parent = rel_ind(a + 4)
    if kind == 0:        # module
        n = cstr(rel(a + 8))
    elif kind == 1:      # extension: name is the extended context (mangled) — use parent
        n = (ctx_name(parent, depth + 1) if parent else "") + ".(extension)"
        ctx_cache[a] = n; return n
    elif kind == 2:      # anonymous
        n = (ctx_name(parent, depth + 1) if parent else "") + ".(anon)"
        ctx_cache[a] = n; return n
    elif kind == 3:      # protocol
        n = cstr(rel(a + 8))
    else:                # class 16, struct 17, enum 18
        n = cstr(rel(a + 8))
    if parent and kind != 0:
        n = ctx_name(parent, depth + 1) + "." + n
    ctx_cache[a] = n
    return n


DM = None
def demangle_type(a):
    """mangled type name at a, with symbolic refs (0x01 direct / 0x02 indirect ctx) resolved"""
    p = off(a); out = ""; i = p
    while data[i] != 0:
        c = data[i]
        if 1 <= c <= 0x17:
            r = struct.unpack_from("<i", data, i + 1)[0]; tgt = a + (i - p) + 1 + r
            if c == 2:
                q = off(tgt); tgt = struct.unpack_from("<Q", data, q)[0] if q is not None else 0
            out += "{" + (ctx_name(tgt) if c in (1, 2) else f"sym{c}") + "}"
            i += 5
        elif 0x18 <= c <= 0x1F:
            out += f"{{ref{c}}}"; i += 9
        else:
            out += chr(c); i += 1
    return out


def pretty(m):
    # demangle the plain parts with swift-demangle where possible
    if "{" not in m:
        try:
            r = subprocess.run(["xcrun", "swift-demangle", "--simplified", "--compact", "$s" + m], capture_output=True, text=True)
            s = r.stdout.strip()
            if s and not s.startswith("$s"): return s
        except Exception:
            pass
    return m


KINDS = {16: "class", 17: "struct", 18: "enum"}
if "--types" in args:
    a0, n = sects["__swift5_types"]
    for k in range(n // 4):
        d = rel_ind(a0 + 4 * k)
        flags = u32(d); kind = flags & 0x1F
        if kind not in KINDS: continue
        name = ctx_name(d)
        if flt and not flt.search(name): continue
        fields_ptr = rel(d + 16)
        print(f"{KINDS[kind]} {name}   @{d:#x}")
        if fields_ptr:
            fkind, fsize, nf = struct.unpack_from("<HHI", data, off(fields_ptr + 8))
            for j in range(nf):
                fr = fields_ptr + 16 + j * fsize
                fflags = u32(fr); mt = rel(fr + 4); fn = rel(fr + 8)
                tname = pretty(demangle_type(mt)) if mt else ""
                print(f"    {'var' if fflags & 2 else 'let' if kind != 18 else 'case'} {cstr(fn) if fn else '?'}: {tname}")

if "--conf" in args:
    a0, n = sects["__swift5_proto"]
    for k in range(n // 4):
        c = rel(a0 + 4 * k)
        proto = rel_ind(c)
        flags = u32(c + 12)
        tkind = (flags >> 3) & 7
        tr = c + 4
        if tkind == 0: tname = ctx_name(rel(tr))
        elif tkind == 1:
            t = rel(tr); q = off(t); tname = ctx_name(struct.unpack_from("<Q", data, q)[0]) if q is not None else "?"
        else: tname = f"objc:{cstr(rel(tr))}"
        pname = ctx_name(proto) if off(proto) is not None else f"<ext proto {proto:#x}>"
        line = f"{tname} : {pname}"
        if flt and not flt.search(line): continue
        # protocol descriptor: flags, parent, name, NumRequirementsInSignature, NumRequirements, AssociatedTypeNames,
        # then trailing requirement signature (12 bytes each) and requirements (8 bytes each)
        nreq = u32(proto + 16) if readable(proto) else 0
        reqs = proto + 24 + 12 * u32(proto + 12) if readable(proto) else 0
        print(f"{line}   flags {flags:#x}")
        q = c + 16
        if flags & 0x40: q += 4                                    # retroactive context
        q += 12 * ((flags >> 8) & 0xFF)                           # conditional requirements
        KN = {0: "base", 1: "method", 2: "init", 3: "get", 4: "set", 5: "read", 6: "modify", 7: "assocType", 8: "assocConf"}
        if not flags & 0x30000:                                    # plain static witness table
            wt = rel(c + 8)
            if wt and readable(wt):
                print(f"    witness table @{wt:#x}")
                for j in range(nreq):
                    e = unslide_ptr(u64(wt + 8 * (j + 1)))
                    kind = KN.get(u32(reqs + 8 * j) & 0xF, "?") if reqs else "?"
                    print(f"    req[{j:2d}] {kind:9s} -> {e:#x}")
        if flags & 0x10000:                                        # resilient witnesses
            nw = u32(q); q += 4
            for j in range(nw):
                r = rel_ind(q); w = rel(q + 4); q += 8
                idx = (r - reqs) // 8 if reqs and reqs <= r < reqs + 8 * nreq else -1
                kind = KN.get(u32(r) & 0xF, "?") if r and readable(r) else "?"
                print(f"    req[{idx:2d}] {kind:9s} -> {w:#x}")
