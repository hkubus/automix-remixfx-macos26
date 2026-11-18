"""helpers on top of swiftmeta.py: decode Swift Strings / static arrays from an extracted image (+cache)"""
import runpy, struct, sys, os
_g = None
def load(macho, cache=None):
    global _g
    argv = sys.argv
    sys.argv = ["swiftmeta.py", macho] + (["--cache", cache] if cache else [])
    _g = runpy.run_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "swiftmeta.py"), run_name="x")
    sys.argv = argv
    return _g
def u64(a): return _g["u64"](a)
def rd(a, n): return _g["rd"](a, n)
def swift_string(w0, w1):
    """decode a Swift String from its two words (x86_64 ABI)"""
    disc = w1 >> 56
    if disc & 0x20 and (disc & 0xF0) in (0xE0, 0xA0):       # small string: count in low nibble of top byte
        n = disc & 0x0F
        b = struct.pack("<QQ", w0, w1 & 0x00FFFFFFFFFFFFFF)[:n]
        return b.decode("utf-8", "replace")
    count = w0 & 0x0000FFFFFFFFFFFF
    obj = w1 & 0x00FFFFFFFFFFFFFF
    if (w0 >> 60) & 0x8 or True:                              # native/immortal: chars at obj+32
        try:
            return rd(obj + 32, count).decode("utf-8", "replace")
        except Exception:
            return f"<str {w0:#x} {w1:#x}>"
def static_array(a, stride):
    meta, rc, cnt, cap = (u64(a + 8 * i) for i in range(4))
    return cnt, [a + 32 + stride * i for i in range(cnt)]
