import re,sys,collections,pickle
J='/home/m/.claude/jobs/3a167ccc/tmp/'
comm_want=sys.argv[1]; symre=re.compile(sys.argv[2])
sam=collections.Counter(); tot=0; syms={}
for l in open(J+'samples.txt'):
    p=l.split()
    # comm may have spaces: ip is first hex token of len>=6 after comm
    m=re.match(r'\s*(.+?)\s+([0-9a-f]{6,16})\s+(.*)',l)
    if not m: continue
    c,ip,s=m[1].strip(),int(m[2],16),m[3]
    if c!=comm_want: continue
    tot+=1
    if symre.search(s): sam[ip]+=1; syms[ip]=s
print('comm',comm_want,'samples',tot,'matched',sum(sam.values()))
# load instrs for sampled functions: find function ranges
want=set(sam)
ins={}; func=None; fi=collections.defaultdict(list)
hdr=re.compile(r'^([0-9a-f]+) <(.+)>:')
line=re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
wantf=set()
# first pass build per-func only for those containing sampled ip
cur=[];name=None;
def flush():
    global cur
    if cur and any(a in want for a,_ in cur):
        fi[name]=cur
    cur=[]
with open(J+'all.s') as f:
    for l in f:
        m=line.match(l)
        if m: cur.append((int(m[1],16),m[2].strip()))
        else:
            m=hdr.match(l)
            if m: flush(); name=m[2]
flush()
print('funcs',len(fi))
cat=collections.Counter(); ex=collections.Counter()
for n,L in fi.items():
    bc=collections.Counter()
    for a,t in L:
        for r in re.findall(r'0x[0-9a-f]+\((%r[a-z0-9]+)\)',t):
            bc[r]+=1
    bc.pop('%rsp',None); bc.pop('%rip',None)
    ctxr=bc.most_common(1)[0][0] if bc else None
    for a,t in L:
        k=sam.get(a,0)
        if not k: continue
        op=t.split()[0]
        if re.search(r'g_null_sweep_hi|g_barrier_sync_watch|g_spu_coh_bits',t) or op in('bextr',) or op=='bt' : c='store-check globals/bitmap'
        elif op=='movzbl' and re.search(r'\(%r[a-z0-9]+,%r[a-z0-9]+,1\)',t) : c='bitmap/byte load (rr,1)'
        elif ctxr and re.search(r'0x[0-9a-f]+\('+re.escape(ctxr)+r'\)',t) or (ctxr and re.search(r'\('+re.escape(ctxr)+r'\)',t) and '%rip' not in t):
            parts=t.split(None,1)[1] if ' ' in t else ''
            dst=parts.split(',')[-1]
            c='ctx store' if ctxr in dst else 'ctx load'
        elif op.startswith('v') : c='simd'
        elif re.search(r'\(%r[a-z0-9]+,%r[a-z0-9]+,1\)',t): c='guest mem access'
        elif op=='bswap' or op=='movbe': c='bswap'
        elif op in('call','ret','jmp') or op.startswith('j'): c='branch/call'
        elif op in('cmp','test','setb','sbb','adc','cmov') or op.startswith(('set','cmov')): c='cmp/flags (CR/XER)'
        else: c='other int'
        cat[c]+=k; ex[(c,op)]+=k
s=sum(cat.values())
for c,v in cat.most_common(): print(f'{100*v/s:5.1f}%  {c}')
print([ (k[1],v) for k,v in ex.most_common(14)])
