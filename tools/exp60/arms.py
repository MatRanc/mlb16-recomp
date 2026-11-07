import re,sys,collections,statistics as st
log=sys.argv[1]; drop=3000
marks=[l.split() for l in open(log.rsplit('.log',1)[0]+'.marks')]
marks=[(a,int(t)) for a,t,*_ in marks]
fl=[];wp=[]
for l in open(log,errors='ignore'):
    m=re.search(r'\[flip\] (\d+) at (\d+) ms.*real=(\d+)',l)
    if m: fl.append((int(m[2]),int(m[1]),int(m[3])))
    elif 'walker-prof' in l:
        wp.append(l)
print(len(fl),'flips',len(wp),'wprof'); print(wp[len(wp)//2][:320])
tw=[]
for l in wp:
    m=re.search(r'walker=([\d.]+).*?wait=([\d.]+)',l); d=re.search(r'draws=(\d+)',l); t=re.search(r'flip=(\d+)|at (\d+) ?ms',l)
    if m and d: tw.append((float(m[1]),float(m[2]),int(d[1]),l))
def real(a,b):
    x=[f for f in fl if a<=f[0]<=b]
    return (x[-1][2]-x[0][2])/((x[-1][0]-x[0][0])/1000) if len(x)>1 else None
R=collections.defaultdict(list)
for i,(arm,t) in enumerate(marks):
    e=marks[i+1][1] if i+1<len(marks) else t+12000
    R[arm].append(real(t+drop,e))
for a,v in sorted(R.items()): print(a,[None if x is None else round(x,2) for x in v])
keys=['draws','walker','wait','parse','fetch','tex','pipe','draw','other','src_main','src_mesh','mainms','meshms']
rows=collections.defaultdict(lambda:collections.defaultdict(list))
for l in wp:
    m=re.search(r'at=(\d+)ms',l)
    if not m: continue
    t=int(m[1])
    for i,(arm,t0) in enumerate(marks):
        e=marks[i+1][1] if i+1<len(marks) else t0+12000
        if t0+3000<=t<e:
            for k in keys:
                x=re.search(r'\b'+k+r'=([\d.]+)',l)
                if x: rows[arm][k].append(float(x[1]))
print('arm '+' '.join(f'{k:>8}' for k in keys))
for a in sorted(rows):
    print(a,'  '+' '.join(f'{st.mean(rows[a][k]):8.2f}' if rows[a][k] else '     nan' for k in keys))
