J='/home/m/.claude/jobs/3a167ccc/tmp/'; f=J+'bx/ppu_recomp_004.cpp'
s=open(f).read()
done=[]
for i,fn in enumerate(['008530AC','00853AF0','00853C6C','00853E10','00853EDC','00853F8C'],1):
    a=f'void func_{fn}(ppu_context* ctx) {{\n    PPU_MM_SNAP\n'
    if s.count(a)!=1: print('missing',fn); continue
    extra=f'if (g_exp60_shadow == {10+i}) return;'
    if fn=='00853C6C': extra+=' if (g_exp60_shadow == 18 && (vm_read8(ctx->gpr[3] + 0x464) & 0x80)) return; if (g_exp60_shadow == 19 && !(vm_read8(ctx->gpr[3] + 0x464) & 0x80)) return;'
    s=s.replace(a,a+'    '+extra+'\n'); done.append((10+i,fn))
open(f,'w').write(s); print(done)
