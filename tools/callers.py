"""callers.py MACHO REFS_TSV name=ADDR ... : list the functions (LC_FUNCTION_STARTS) that reference each ADDR"""
import sys,struct,bisect
data=open(sys.argv[1],'rb').read()
ncmds=struct.unpack_from('<I',data,16)[0]; o=32; fs=None; tv=None
for _ in range(ncmds):
    cmd,size=struct.unpack_from('<II',data,o)
    if cmd==0x19 and data[o+8:o+24].rstrip(b'\0')==b'__TEXT': tv=struct.unpack_from('<Q',data,o+24)[0]
    if cmd==0x26:
        a,b=struct.unpack_from('<II',data,o+8); fs=data[a:a+b]
    o+=size
st=[];a=tv;i=0
while i<len(fs):
    v=0;sh=0
    while True:
        b=fs[i];i+=1;v|=(b&0x7f)<<sh;sh+=7
        if not b&0x80:break
    if v==0:break
    a+=v;st.append(a)
refs={}
for l in open(sys.argv[2]):
    t,x,m=l.split('\t'); refs.setdefault(int(t,16),[]).append((int(x,16),m.strip()))
def fo(x): return st[bisect.bisect_right(st,x)-1]
for name_t in sys.argv[3:]:
    name,t=name_t.split('=') if '=' in name_t else (name_t,name_t)
    t=int(t,16)
    fns=sorted(set(fo(x) for x,m in refs.get(t,[])))
    print(f"{name:14s} {t:#x} <- " + ' '.join(hex(f) for f in fns))
