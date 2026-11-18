"""genrescan.py MACHO OUT.json : find static Swift arrays of small digit strings (Genre.ID lists)"""
import sys,struct,json
data=open(sys.argv[1],'rb').read()
# scan all 8-aligned offsets for runs of GenreFilter-free plain arrays of small digit strings (stride 16)
def small(w0,w1):
    d=w1>>56
    if d&0xF0!=0xE0: return None
    n=d&0xF
    if n==0 or n>15: return None
    b=struct.pack('<QQ',w0,w1&0x00FFFFFFFFFFFFFF)
    s=b[:n]
    if not s.isdigit() or any(b[n:15]): return None
    return s.decode()
runs=[];i=0;N=len(data)-16
while i<N:
    w0,w1=struct.unpack_from('<QQ',data,i)
    s=small(w0,w1)
    if s:
        j=i;r=[]
        while j<N:
            w0,w1=struct.unpack_from('<QQ',data,j); t=small(w0,w1)
            if not t: break
            r.append(t); j+=16
        if len(r)>=3:
            # array header just before: count at i-16
            cnt=struct.unpack_from('<Q',data,i-16)[0]
            runs.append((i,cnt,r))
        i=j
    else: i+=8
out=[(hex(o),c,r) for o,c,r in runs]
json.dump(out,open(sys.argv[2],'w'))
for o,c,r in out: print(o,c,len(r),r[:8])
