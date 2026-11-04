/*
 * Offline microbenchmark: cost of the 0xB830 capture wrapper when capture is
 * OFF (g_c690_tail_on == 0).  Compares the wrapper entry
 *   jobmod_..._0000B830(ctx)        (wrapper -> _body)
 * with the unwrapped leaf
 *   jobmod_..._0000B830_body(ctx).
 *
 * The second is the code the pre-wrapper build called.  If the compiler did not
 * inline _body into the wrapper, the difference is the call/ret plus one
 * predicted-not-taken branch.  Everything else is identical.
 *
 * NOTE: the play module is shipped stripped (no `_body` symbols).  Re-apply
 * the capture wrappers with `tools/c690_ref/patch_cull_hooks.py --capture`
 * before building this harness; it is not a test of the play build.
 *
 * Build (same runtime/link set as test_c690_tail_replay):
 *   clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *     -I ps3recomp-play/runtime/spu -I ps3recomp-play/include \
 *     -o /tmp/opencode/test_c690_b830_wrapper \
 *     mlb16-recomp/tools/replay/test_c690_b830_wrapper.c \
 *     mlb16-recomp/tools/replay/reg_10f78.c \
 *     recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
 *     ps3recomp-play/runtime/spu/c690_corpus.c \
 *     ps3recomp-play/runtime/spu/c690_cull_fix.c \
 *     ps3recomp-play/runtime/spu/c690_native_cull.c \
 *     ps3recomp-play/runtime/spu/c690_native_screen.c \
 *     ps3recomp-play/runtime/spu/job_c690_native.c -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "c690_corpus.h"

uint8_t* vm_base = NULL;
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;
static spu_fn lookup_fn(uint32_t a) {
    a &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++) if (g_c690_reg[i].addr == a) return g_c690_reg[i].fn;
    return NULL;
}
void spu_indirect_branch(spu_context* ctx) { spu_fn f = lookup_fn(ctx->pc); if (f) f(ctx); }
void spu_drain_call(spu_context* ctx, uint32_t ret) {
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch && (ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) { g_spu_trampoline_fn = NULL; return; }
            g_spu_trampoline_fn = NULL; tf(ctx);
        }
        if ((ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) return;
        spu_fn f = lookup_fn(ctx->pc);
        if (!f) return;
        g_spu_trampoline_fn = spu_indirect_branch;
    }
}
void spu_img_restore(spu_context* c, int32_t i) { c->image_id = i; }
void spu_stop(spu_context* c) { (void)c; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) {
    switch (ch) {
    case MFC_LSA: c->mfc_lsa = v._u32[0]; break;
    case MFC_EAH: c->mfc_eah = v._u32[0]; break;
    case MFC_EAL: c->mfc_eal = v._u32[0]; break;
    case MFC_Size: c->mfc_size = v._u32[0]; break;
    case MFC_TagID: c->mfc_tag = v._u32[0]; break;
    case MFC_WrTagMask: c->mfc_tag_mask = v._u32[0]; break;
    case MFC_WrTagUpdate: c->mfc_tag_status = 0; break;
    default: break;
    }
}
u128 spu_rdch(spu_context* c, uint32_t ch) {
    u128 r; r._u32[0]=r._u32[1]=r._u32[2]=r._u32[3]=0;
    switch (ch) {
    case MFC_RdTagMask: r._u32[0] = c->mfc_tag_mask; break;
    case SPU_RdDec: r._u32[0] = 0xFFFFFFFFu; break;
    case SPU_RdMachStat: r._u32[0] = 1; break;
    default: break;
    }
    return r;
}
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; return 0; }
void spu_fncap_capture(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { (void)a; (void)f; }

void jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830(spu_context* ctx);
void jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830_body(spu_context* ctx);

#define LS_SIZE (256u * 1024u)
#define META_WORDS 24
#define BITMAP_BYTES 2048
#define GPR_BYTES (128u * 16u)

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double median(double* v, int n) {
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (v[j] < v[i]) { double t = v[i]; v[i] = v[j]; v[j] = t; }
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* First op==0xA7 IN image from a tail spool. */
static int load_nth_pair(const char* dir, uint8_t* ls, uint8_t* gpr, int want) {
    DIR* dp = opendir(dir);
    if (!dp) { perror(dir); return 0; }
    struct dirent* de;
    int got = 0, seen = 0;
    while (!got && (de = readdir(dp))) {
        if (strncmp(de->d_name, "tail_", 5) || !strstr(de->d_name, ".bin")) continue;
        char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE* f = fopen(path, "rb"); if (!f) continue;
        char magic[8]; uint32_t ver;
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "C69STG01", 8) ||
            fread(&ver, 4, 1, f) != 1) { fclose(f); continue; }
        for (;;) {
            uint8_t hdr[24];
            if (fread(hdr, 1, 24, f) != 24) break;
            uint32_t type = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
            uint32_t size = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
            if (size != META_WORDS * 4u + BITMAP_BYTES + LS_SIZE + GPR_BYTES) break;
            uint8_t* p = (uint8_t*)malloc(size);
            if (!p) exit(1);
            if (fread(p, 1, size, f) != size) { free(p); break; }
            uint32_t meta[META_WORDS]; memcpy(meta, p, META_WORDS * 4);
            if (type == 9 && meta[4] == 0xA7) {
                if (seen++ == want) {
                    memcpy(ls, p + META_WORDS * 4 + BITMAP_BYTES, LS_SIZE);
                    memcpy(gpr, p + META_WORDS * 4 + BITMAP_BYTES + LS_SIZE, GPR_BYTES);
                    got = 1; free(p); break;
                }
            }
            free(p);
        }
        fclose(f);
    }
    closedir(dp);
    return got;
}

int main(int argc, char** argv) {
    const char* spool = argc > 1 ? argv[1] : "/tmp/opencode/fncap2b3a730";
    long iters = argc > 2 ? atol(argv[2]) : 200000;
    int want = argc > 3 ? atoi(argv[3]) : 0;
    uint8_t* ls = (uint8_t*)malloc(LS_SIZE);
    uint8_t* gpr = (uint8_t*)malloc(GPR_BYTES);
    if (!load_nth_pair(spool, ls, gpr, want)) { fprintf(stderr, "no 0xA7 IN pair %d in %s\n", want, spool); return 1; }

    static spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = 6; ctx.spu_id = 0; ctx.steps = 1;
    memcpy(ctx.ls, ls, LS_SIZE);
    memcpy(ctx.gpr, gpr, GPR_BYTES);

    /* Warm caches / branch predictors. */
    for (int i = 0; i < 10000; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830(&ctx);
    for (int i = 0; i < 10000; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830_body(&ctx);

    double tw[9], tb[9];
    for (int r = 0; r < 9; r++) {
        double t0 = now_sec();
        if (r & 1) {
            for (long i = 0; i < iters; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830_body(&ctx);
        } else {
            for (long i = 0; i < iters; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830(&ctx);
        }
        double t1 = now_sec();
        if (r & 1) {
            for (long i = 0; i < iters; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830(&ctx);
        } else {
            for (long i = 0; i < iters; i++) jobmod_c690c2f7b09a_eF5B8_spu_func_0000B830_body(&ctx);
        }
        double t2 = now_sec();
        double a = (t1 - t0) * 1e9 / (double)iters;
        double b = (t2 - t1) * 1e9 / (double)iters;
        if (r & 1) { tw[r] = b; tb[r] = a; } else { tw[r] = a; tb[r] = b; }
    }
    double mw = median(tw, 9), mb = median(tb, 9);
    printf("0xB830 capture OFF (g_c690_tail_on=%d), iters=%ld x9, pair %d\n",
           g_c690_tail_on, iters, want);
    printf("  wrapper  %8.3f ns/call\n", mw);
    printf("  _body    %8.3f ns/call\n", mb);
    printf("  delta    %8.3f ns/call (%.2f %%)\n", mw - mb, 100.0 * (mw - mb) / mb);
    return 0;
}
