#!/usr/bin/env python3
"""asmrange.py ANNOTATED_ASM START [END|+N] [--image MACHO]  — print a function/range, decoding Swift
small-string immediates; with --image, RIP-relative loads into xmm registers show the double/float constant."""
import sys, re, struct
IMG = None
if "--image" in sys.argv:
    i = sys.argv.index("--image"); IMG = sys.argv[i + 1]; del sys.argv[i:i + 2]
    _d = open(IMG, "rb").read(); _segs = []
    _n = struct.unpack_from("<I", _d, 16)[0]; _o = 32
    for _ in range(_n):
        c, sz = struct.unpack_from("<II", _d, _o)
        if c == 0x19: _segs.append(struct.unpack_from("<QQQQ", _d, _o + 24))
        _o += sz
    def _rd(a, n):
        for vm, vs, fo, fs in _segs:
            if vm <= a < vm + min(vs, fs): return _d[fo + a - vm: fo + a - vm + n]
        return None
lo = int(sys.argv[2], 16)
hi = int(sys.argv[3][1:]) if len(sys.argv) > 3 and sys.argv[3].startswith("+") else (int(sys.argv[3], 16) if len(sys.argv) > 3 else None)
def smallstr(v):
    b = (v & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")
    top = b[7]
    if 0xE0 <= top <= 0xEF:                     # second word of a small string: count in low nibble
        s = b[:7].rstrip(b"\0")
        return (s.decode("latin1") + f"  [small str, len {top & 15}]") if all(32 <= c < 127 for c in s) else None
    s = b.rstrip(b"\0")
    return s.decode("latin1") if len(s) >= 1 and all(32 <= c < 127 for c in s) and v > 0xFF else None
n = 0
for l in open(sys.argv[1]):
    m = re.match(r"([0-9a-f]+):", l)
    if not m: continue
    a = int(m.group(1), 16)
    if a < lo: continue
    if hi is not None and hi > 1 << 32 and a >= hi: break
    l = l.rstrip()
    mm = re.search(r"movabs\s+\w+, (-?0x[0-9a-f]+|-?\d+)", l)
    if mm:
        s = smallstr(int(mm.group(1), 0))
        if s: l += f'   "{s}"'
    if IMG:
        mt = re.search(r"; -> (0x[0-9a-f]+)", l)
        if mt and re.search(r"xmm|qword ptr \[rip|dword ptr \[rip", l):
            b = _rd(int(mt.group(1), 16), 16)
            if b:
                if "ss\t" in l or "dword ptr" in l: l += f"   [{struct.unpack('<f', b[:4])[0]:g}]"
                elif "pd\t" in l or "xmmword" in l: l += f"   [{struct.unpack('<d', b[:8])[0]:g}, {struct.unpack('<d', b[8:16])[0]:g}]"
                else: l += f"   [{struct.unpack('<d', b[:8])[0]:g} | {struct.unpack('<q', b[:8])[0]}]"
    print(l); n += 1
    if hi is not None and hi < 1 << 32 and n >= hi: break
    if hi is None and re.search(r"\tret\b", l): break
