#!/usr/bin/env python3
"""asmx.py ASM_IN ASM_OUT [REFS_OUT]

Annotate an `llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel` listing: every RIP-relative operand
gets its absolute target (`; -> 0x...`), computed from the next instruction's address. Optionally writes a
reference index (target<TAB>instruction-address<TAB>mnemonic) for "who references X" queries.
"""
import re, sys

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
RIP = re.compile(r"\[rip ([+-]) 0x([0-9a-f]+)\]")

lines = open(sys.argv[1]).read().splitlines()
parsed = []
for l in lines:
    m = LINE.match(l)
    parsed.append((int(m.group(1), 16), m.group(2), m.group(3)) if m else None)

out, refs = [], []
for i, p in enumerate(parsed):
    if p is None:
        out.append(lines[i]); continue
    addr, mn, ops = p
    note = ""
    m = RIP.search(ops)
    if m:
        nxt = next((q[0] for q in parsed[i + 1:i + 4] if q), None)
        if nxt is not None and nxt > addr:
            disp = int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
            t = nxt + disp
            note = f"  ; -> {t:#x}"
            refs.append((t, addr, mn))
    elif mn.startswith(("call", "j")) and re.match(r"0x[0-9a-f]+$", ops.strip()):
        refs.append((int(ops.strip(), 16), addr, mn))
    out.append(f"{addr:x}:\t{mn}\t{ops}{note}")

open(sys.argv[2], "w").write("\n".join(out) + "\n")
if len(sys.argv) > 3:
    with open(sys.argv[3], "w") as f:
        for t, a, mn in sorted(refs):
            f.write(f"{t:x}\t{a:x}\t{mn}\n")
