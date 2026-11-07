import re,sys
J='/home/m/.claude/jobs/3a167ccc/tmp/'; R='/home/m/Documents/mlb16-online/recomp-work/recompiled/'
POLL=r'''
extern "C" { volatile unsigned g_exp60_mask, g_exp60_shadow, g_exp60_frame; }
static void exp60_poll(void) {
    unsigned f = ++g_exp60_frame;
    if (f & 7) return;
    static const char* p = getenv("PS3RECOMP_CTL");
    if (!p) return;
    char b[512]; FILE* h = fopen(p, "r"); if (!h) return;
    size_t n = fread(b, 1, sizeof b - 1, h); fclose(h); b[n] = 0;
    const char* m = strstr(b, "EXP60_MASK="); if (m) g_exp60_mask = (unsigned)strtoul(m + 11, 0, 16);
    const char* s = strstr(b, "EXP60_SHADOW="); if (s) g_exp60_shadow = (unsigned)strtoul(s + 13, 0, 10);
}
static inline int exp60_skip_shadow(void) {
    unsigned s = g_exp60_shadow;
    return s == 1 || (s == 2 && (g_exp60_frame & 1));
}
'''
EXT='extern "C" { extern volatile unsigned g_exp60_mask, g_exp60_shadow, g_exp60_frame; }\nstatic inline int exp60_skip_shadow(void) { unsigned s = g_exp60_shadow; return s == 1 || (s == 2 && (g_exp60_frame & 1)); }\n'
def patch(tu,hooks,head):
    s=open(R+tu).read()
    s=s.replace('#include <math.h>\n','#include <math.h>\n#include <stdlib.h>\n'+head,1)
    for fn,code in hooks.items():
        a=f'void func_{fn}(ppu_context* ctx) {{\n    PPU_MM_SNAP\n'
        assert s.count(a)==1,fn
        s=s.replace(a,a+'    '+code+'\n')
    open(J+'bx/'+tu,'w').write(s)
patch('ppu_recomp_001.cpp',{'002B9368':'exp60_poll();','002AC53C':'if (exp60_skip_shadow()) return;'},POLL)
patch('ppu_recomp_008.cpp',{'008541C8':'if (exp60_skip_shadow()) return;'},EXT)
patch('ppu_recomp_004.cpp',{'009E6D98':'if (g_exp60_mask & (uint32_t)ctx->gpr[4]) return;'},EXT)
