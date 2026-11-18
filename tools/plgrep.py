#!/usr/bin/env python3
"""plgrep.py MODE PATTERN FILE...
MODE=keys  -> list all top-level keys (with DevelopmentPhase/Enabled for feature flags)
MODE=grep  -> print key/value pairs whose key or value matches PATTERN (case-insensitive)
"""
import plistlib, re, sys

mode, pat, files = sys.argv[1], sys.argv[2], sys.argv[3:]
rx = re.compile(pat, re.I)

def load(path):
    with open(path, "rb") as f:
        data = f.read()
    try:
        return plistlib.loads(data)
    except Exception:
        pass
    # old-style .strings ("key" = "value";) possibly UTF-16
    for enc in ("utf-16", "utf-8"):
        try:
            txt = data.decode(enc)
            break
        except Exception:
            txt = None
    if txt is None:
        return {}
    out = {}
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', txt):
        out[m.group(1)] = m.group(2)
    return out

def walk(obj, prefix=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from walk(v, f"{prefix}/{k}" if prefix else str(k))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from walk(v, f"{prefix}[{i}]")
    else:
        yield prefix, obj

for path in files:
    try:
        d = load(path)
    except Exception as e:
        print(f"!! {path}: {e}")
        continue
    if mode == "keys":
        items = []
        for k, v in sorted(d.items()):
            tag = ""
            if isinstance(v, dict):
                tag = v.get("DevelopmentPhase", v.get("Enabled", ""))
            if rx.search(k):
                items.append(f"{k}[{tag}]" if tag != "" else k)
        print(f"--- {path}\n    " + ", ".join(items))
    else:
        hits = [(k, v) for k, v in walk(d) if rx.search(k) or rx.search(str(v))]
        if hits:
            print(f"--- {path}")
            for k, v in hits[:60]:
                print(f"    {k} = {str(v)[:160]}")
