import struct,collections,sys
b=open('/home/m/Documents/mlb16-online/recomp-work/captures/meter_cap/mcap.fifo','rb').read()
h=struct.unpack('<8I',b[:32]); print('hdr',[hex(x) for x in h])
w=struct.unpack('>%dI'%((len(b)-32)//4),b[32:])
# endianness check
print([hex(x) for x in w[:12]])
reg={}; i=0;n=len(w)
passes=[]; cur=None; order=[]
def key():
    return (reg.get(0x210),reg.get(0x214),reg.get(0x18C),reg.get(0x200),reg.get(0x204),reg.get(0x208))
stat=collections.OrderedDict()
hist=collections.Counter()
def stats():
    k=key()
    if k not in stat: stat[k]=dict(begin=0,draws=0,refs=0,arr=0,mesh=0,meshrefs=0)
    return stat[k]
inbegin=False; dr=0
vaddr={}
def ismesh():
    # any enabled vertex array high in main memory ring region
    for a in range(16):
        f=reg.get(0x1740+4*a,0); o=reg.get(0x1680+4*a)
        if o is None: continue
        if (f>>4)&0xF==0: continue   # size 0
        hist[hex(o>>20)]+=1
        if MESHLO<= (o&0x7FFFFFFF) <MESHHI: return True
    return False
MESHLO,MESHHI=0,0
if len(sys.argv)>2: MESHLO,MESHHI=int(sys.argv[1],16),int(sys.argv[2],16)
while i<n:
    c=w[i]; i+=1
    if c==0: continue
    if c&0xE0000003==0x20000000 or (c&3)==1 or (c&3)==2 or c==0x20000: continue
    cnt=(c>>18)&0x7FF; m=c&0xFFFC; ni=bool(c&0x40000000)
    for j in range(cnt):
        if i+j>=n: break
        v=w[i+j]; mm=m if ni else m+4*j
        reg[mm]=v
        if mm==0x1808:
            if v: 
                s=stats(); s['begin']+=1; inbegin=True; cm=ismesh(); s['mesh']+=cm; dr=cm
            else: inbegin=False
        elif mm==0x1814 or mm==0x1824:   # DRAW_ARRAYS / DRAW_INDEX_ARRAY
            s=stats(); r=(v>>24)+1; s['refs']+=r; s['draws']+=1
            if dr: s['meshrefs']+=r
    i+=cnt
t=collections.Counter()
print('passes',len(stat))
rows=[]
for k,s in stat.items():
    if s['begin']==0: continue
    rows.append((s['begin'],s['mesh'],s['refs'],s['meshrefs'],k))
tb=sum(r[0] for r in rows); tr=sum(r[2] for r in rows); tm=sum(r[1] for r in rows); tmr=sum(r[3] for r in rows)
print('total begin/end',tb,'mesh',tm,'refs',tr,'meshrefs',tmr)
for r in sorted(rows,key=lambda r:-r[2])[:40]:
    k=r[4]; clip=k[3] or 0; cl2=k[4] or 0
    print(f"be={r[0]:5} mesh={r[1]:5} refs={r[2]:7} meshrefs={r[3]:7}  col={k[0] if k[0] is None else hex(k[0])} z={k[1] if k[1] is None else hex(k[1])} {clip>>16}x{cl2>>16} fmt={k[5] if k[5] is None else hex(k[5])}")
print(hist.most_common(24))
