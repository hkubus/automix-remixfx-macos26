data=open(sys.argv[1],'rb').read()
ncmds=struct.unpack_from('<I',data,16)[0]; o=32; fs=None; tv=None
for _ in range(ncmds):
    cmd,size=struct.unpack_from('<II',data,o)
    if cmd==0x19 and data[o+8:o+24].rstrip(b'\0')==b'__TEXT': tv=struct.unpack_from('<Q',data,o+24)[0]
    if cmd==0x26:
        a,b=struct.unpack_from('<II',data,o+8); fs=data[a:a+b]
    o+=size
st=[];a=tv;j=0
while j<len(fs):
    v=0;sh=0
    while True:
        b=fs[j];j+=1;v|=(b&0x7f)<<sh;sh+=7
        if not b&0x80:break
    if v==0:break
    a+=v;st.append(a)
