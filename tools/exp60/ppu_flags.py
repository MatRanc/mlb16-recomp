#!/usr/bin/env python3
"""Prototype: promote PPU registers in lifted functions to C locals.
Flush may-dirty regs before any statement that passes ctx; reload after. usage: in.cpp out.cpp [--no-xer-flush]"""
import re,sys
src,dst=sys.argv[1],sys.argv[2]
NOX='--no-xer-flush' in sys.argv
FN=re.compile(r'^void func_[0-9A-Fa-f]+\(ppu_context\* ctx\) \{')
BARR=re.compile(r'\bctx\b(?!->)')
REF=re.compile(r'ctx->(?:(gpr|fpr)X\[(\d+)\]|(cr|xer|ctr)\b)')
DYN=re.compile(r'ctx->(gpr|fpr)\[(?!\d+\])')
WR=re.compile(r'(?:&\s*ctx->(?:(gpr|fpr)X\[(\d+)\]|(cr|xer|ctr)\b))|(?:ctx->(?:(gpr|fpr)X\[(\d+)\]|(cr|xer|ctr)\b)\s*(?:=(?!=)|[-+*/|&^%]=|<<=|>>=|\+\+|--))')
LABEL=re.compile(r'^\s*(loc_[0-9A-Fa-f]+)\s*:')
GOTO=re.compile(r'\bgoto\s+(loc_[0-9A-Fa-f]+)\s*;')
SPEC={'cr':64,'xer':65,'lr':66,'ctr':67}
def bit(g,n,s):
    return (0 if g=='gpr' else 32)+int(n) if g else SPEC[s]
def lname(b):
    if b<32: return f'l_g{b}'
    if b<64: return f'l_f{b-32}'
    return 'l_'+{64:'cr',65:'xer',66:'lr',67:'ctr'}[b]
def cname(b):
    if b<32: return f'ctx->gpr[{b}]'
    if b<64: return f'ctx->fpr[{b-32}]'
    return 'ctx->'+{64:'cr',65:'xer',66:'lr',67:'ctr'}[b]
def ctype(b):
    if b<32: return 'uint64_t'
    if b<64: return 'double'
    return {64:'uint32_t',65:'uint32_t',66:'uint64_t',67:'uint64_t'}[b]
def masks(m):
    b=0
    while m:
        if m&1: yield b
        m>>=1;b+=1
def writes(l):
    m=0
    for x in WR.finditer(l):
        g,n,s,g2,n2,s2=x.groups()
        m|=1<<(bit(g,n,s) if (g or s) else bit(g2,n2,s2))
    return m
def uses(l):
    m=0
    for x in REF.finditer(l): m|=1<<bit(x[1],x[2],x[3])
    return m
def process(body):
    # body: lines between signature and closing brace
    if any(DYN.search(l) for l in body): return None
    used=0
    for l in body: used|=uses(l)
    if not used: return None
    n=len(body); labels={}
    for i,l in enumerate(body):
        m=LABEL.match(l)
        if m: labels[m[1]]=i
    isb=[bool(BARR.search(l)) and 'PPU_MM_SNAP' not in l for l in body]
    # any statement with ctx in a form we do not understand across lines? fine
    wr=[writes(l) for l in body]
    inn=[-1]*n; inn[0]=0  # -1 unvisited
    work=[0]
    def term(l):
        s=l.strip()
        return s.startswith('return;') or s.startswith('goto ') or s=='return' 
    while work:
        i=work.pop()
        st=inn[i]; l=body[i]
        out=0 if isb[i] else st|wr[i]
        succ=[]
        for g in GOTO.findall(l):
            if g in labels: succ.append(labels[g])
            else: return None
        if not term(l) and i+1<n: succ.append(i+1)
        for s in succ:
            if inn[s]<0 or (inn[s]|out)!=inn[s]:
                inn[s]=(inn[s] if inn[s]>=0 else 0)|out
                work.append(s)
    flushmask=~(1<<65) if NOX else ~0
    res=[]
    reload_txt=' '.join(f'{lname(b)} = {cname(b)};' for b in masks(used))
    def flush(m): return ' '.join(f'{cname(b)} = {lname(b)};' for b in masks(m&flushmask))
    def rep(l):
        def f(x):
            return lname(bit(x[1],x[2],x[3]))
        return REF.sub(f,l)
    for i,l in enumerate(body):
        st=inn[i] if inn[i]>=0 else 0
        if 'PPU_MM_SNAP' in l:
            res.append(l)
            res.append(' '.join(f'{ctype(b)} {lname(b)} = {cname(b)};' for b in masks(used)))
            continue
        if isb[i]:
            f=flush(st)
            if f: res.append(f)
            nl=GOTO.sub(lambda m:'{ '+reload_txt+' goto '+m[1]+'; }',l)
            res.append(nl)
            if not term(l): res.append(reload_txt)
            continue
        s=l.strip()
        cur=st|wr[i]
        if 'return;' in l:
            f=flush(cur)
            nl=rep(l)
            if f:
                if s=='return;': res.append(f); res.append(nl)
                else: res.append(nl.replace('return;','{ '+f+' return; }'))
            else: res.append(nl)
            continue
        res.append(rep(l))
    # fallthrough off the end of the function
    last=len(body)-1
    res.append(flush(inn[last]|wr[last] if inn[last]>=0 else 0))
    return res
out=[];i=0;n_ok=n_skip=0
L=open(src).read().split('\n')
while i<len(L):
    l=L[i]
    if FN.match(l):
        j=i+1
        while L[j]!='}': j+=1
        r=process(L[i+1:j])
        out.append(l)
        if r is None: out+=L[i+1:j]; n_skip+=1
        else: out+=r; n_ok+=1
        out.append('}')
        i=j+1
    else:
        out.append(l); i+=1
open(dst,'w').write('\n'.join(out))
print('localized',n_ok,'skipped',n_skip)
