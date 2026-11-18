"""fconst.py MACHO ANNOTATED_ASM FUNC... : per function, list FP constants it loads and small integer immediates"""
import sys,struct,re,bisect
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swiftvals as sv
g=sv.load(sys.argv[1])
asm=[l for l in open(sys.argv[2]).read().splitlines()]
addrs=[]
for l in asm:
    m=re.match(r'([0-9a-f]+):',l); addrs.append(int(m.group(1),16) if m else 0)
# function starts from strrefs logic
exec(open(''+__import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)),'fstarts.py')+'').read())
for f in sys.argv[3:]:
    f=int(f,16); k=bisect.bisect_right(st,f); end=st[k] if k<len(st) else f+0x1000
    i=bisect.bisect_left(addrs,f)
    out=[]
    while i<len(asm) and addrs[i]<end:
        l=asm[i]; i+=1
        m=re.search(r'(xmm\d+|qword ptr).*; -> (0x[0-9a-f]+)',l)
        if m and ('sd' in l.split('\t')[1] or 'pd' in l.split('\t')[1]):
            a=int(m.group(2),16)
            try: v=struct.unpack('<d',sv.rd(a,8))[0]
            except Exception: v=None
            out.append(f"{l.split(':')[0]} {l.split(chr(9))[1]} {v}")
        elif re.search(r'\t(cmp|mov|add|sub|imul|lea)\t.*, (-?0x[0-9a-f]+|\d+)$',l) and not 'rsp' in l and not 'rbp' in l:
            out.append(l.split(';')[0].strip())
        elif re.search(r'\tpush\t(-?0x[0-9a-f]+|\d+)$',l):
            out.append(l.strip())
        elif re.search(r'\tmovabs\t',l):
            out.append(l.strip())
    print(f"== {f:#x}-{end:#x}"); print('\n'.join('   '+o for o in out))
