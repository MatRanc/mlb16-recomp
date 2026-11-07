import csv,re,collections,sys
def load(f):
    d=collections.defaultdict(lambda:collections.defaultdict(float))
    for l in open(f):
        p=l.strip().split(',')
        if len(p)<5 or p[0].startswith('#'): continue
        th=re.sub(r'-\d+$','',p[0]); 
        try:v=float(p[1])
        except: continue
        ev=p[3]
        g='lanes' if th.startswith('spurs pm') else th
        if g=='ppu main' and False: pass
        d[g][ev]+=v
    return d
for t in ['ct0','ct1']:
    a=load(f'ctrA_{t}.csv'); b=load(f'ctrB_{t}.csv')
    print('==',t)
    for g in ['ppu main','rsx walker','lanes','ppu Kinematics']:
        x=a[g]; y=b[g]
        c=x.get('cycles',0)
        if not c: print(g,'none',list(a)[:6]); continue
        ins=x.get('instructions',0)
        cy=y.get('cycles',1)
        print(f"{g:14} Gcyc/12s {c/1e9:6.1f} IPC {ins/c:4.2f} icstall {100*x.get('ic_fetch_stall.ic_stall_any',0)/c:4.1f}% iTLBl2miss/kI {1e3*x.get('bp_l1_tlb_miss_l2_miss',0)/ins:5.2f} iL1TLBm/kI {1e3*x.get('bp_l1_tlb_miss_l2_hit',0)/ins:5.2f} dTLBmiss/kI {1e3*x.get('ls_l1_d_tlb_miss.all',0)/ins:5.2f} | ic32miss% {100*y.get('ic_fw32_miss',0)/max(1,y.get('ic_fw32',1)):4.1f} L2miss/Mcyc {1e6*y.get('l2_cache_req_stat.ic_dc_miss_in_l2',0)/cy:6.0f} brmisp/Mcyc {1e6*y.get('ex_ret_brn_misp',0)/cy:6.0f} walks/Mcyc {1e6*y.get('ls_tablewalker.dside',0)/cy:6.0f}")
