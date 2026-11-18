#!/usr/bin/env python3
"""dsc_grep.py CACHE_BASE REGEX [--show]
Search every sub-cache file of a dyld shared cache for REGEX (bytes, case-insensitive),
translate hits to VM addresses via each file's mapping table, and attribute them to
images via the .map file. Prints hit counts per image (and sample strings with --show).
"""
import mmap, os, re, struct, sys, glob, bisect, collections

base, pat = sys.argv[1], sys.argv[2]
show = "--show" in sys.argv
rx = re.compile(pat.encode(), re.I)

files = sorted(f for f in glob.glob(base + "*") if not f.endswith((".map", ".atlas")))

# ---- parse .map -> list of (start, end, image) for every segment
segs = []
cur = None
with open(base + ".map", errors="replace") as fh:
    for line in fh:
        if line.startswith("/"):
            cur = line.strip()
        elif cur and line.startswith("\t"):
            m = re.match(r"\t\s*(\S+)\s+0x([0-9A-Fa-f]+) -> 0x([0-9A-Fa-f]+)", line)
            if m:
                segs.append((int(m.group(2), 16), int(m.group(3), 16), cur, m.group(1)))
segs.sort()
starts = [s[0] for s in segs]

def image_for(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and segs[i][0] <= addr < segs[i][1]:
        return segs[i][2], segs[i][3]
    return "?", "?"

counts = collections.Counter()
samples = collections.defaultdict(set)
for f in files:
    with open(f, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        if mm[:4] != b"dyld":
            continue
        mapOff, mapCnt = struct.unpack_from("<II", mm, 16)
        maps = [struct.unpack_from("<QQQII", mm, mapOff + 32 * i) for i in range(mapCnt)]
        for m in rx.finditer(mm):
            off = m.start()
            addr = None
            for a, sz, fo, _, _ in maps:
                if fo <= off < fo + sz:
                    addr = a + (off - fo)
                    break
            if addr is None:
                continue
            img, seg = image_for(addr)
            counts[img] += 1
            if show and len(samples[img]) < 40:
                # expand to surrounding printable string
                s = off
                while s > 0 and 32 <= mm[s - 1] < 127:
                    s -= 1
                e = off
                while e < len(mm) and 32 <= mm[e] < 127:
                    e += 1
                samples[img].add(mm[s:e][:160].decode("ascii", "replace"))
        mm.close()

for img, n in counts.most_common():
    print(f"{n:6d}  {img}")
    if show:
        for s in sorted(samples[img]):
            print(f"          {s}")
