/*
 * Offline replay of the lifted SPU 0x10F78 handler (pitch 0x2A command)
 * on a captured LS/GPR state, with an instrumented channel/DMA model.
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I<ps3recomp-native>/include -I<ps3recomp-native>/runtime/spu \
 *       -o /tmp/opencode/test_10f78 \
 *       test_job_c690_10f78_replay.c reg_10f78.c \
 *       <spu_lifted>/spu_recomp.c <ps3recomp-native>/runtime/spu/job_c690_native.c -lm
 *
 * Run: /tmp/opencode/test_10f78 <capture-dir> [out-ls.bin]
 *
 * The channel model logs every MFC/mailbox access. Guest-memory DMA cannot be
 * reproduced from an LS-only capture, so GETs are optionally zero-filled and
 * PUTs are logged; whether the handler reaches its output is reported.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"

/* ---- minimal dispatch / drain runtime ---------------------------------- */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;

static spu_fn lookup_fn(uint32_t addr)
{
    addr &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++)
        if (g_c690_reg[i].addr == addr) return g_c690_reg[i].fn;
    return NULL;
}

static long g_dispatch_n, g_max_dispatch = 200000;
static int g_verbose = 1;

void spu_indirect_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    spu_fn fn = lookup_fn(pc);
    if (++g_dispatch_n > g_max_dispatch) {
        fprintf(stderr, "[replay] ABORT: dispatch budget exceeded (last pc=0x%05X)\n", pc);
        exit(2);
    }
    if (!fn) {
        fprintf(stderr, "[replay] ABORT: no lifted code at LS 0x%05X (indirect branch)\n", pc);
        exit(3);
    }
    fn(ctx);
}

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch &&
                (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
                g_spu_trampoline_fn = NULL;
                return;
            }
            g_spu_trampoline_fn = NULL;
            tf(ctx);
            if (++g_dispatch_n > g_max_dispatch) {
                fprintf(stderr, "[replay] ABORT: dispatch budget exceeded (ret=0x%05X pc=0x%05X)\n",
                        return_pc, ctx->pc & SPU_LS_MASK);
                exit(2);
            }
        }
        if ((ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) return;
        spu_fn fn = lookup_fn(ctx->pc);
        if (fn) { g_spu_trampoline_fn = spu_indirect_branch; continue; }
        fprintf(stderr, "[replay] ABORT: drain lost code, ret=0x%05X pc=0x%05X\n",
                return_pc & SPU_LS_MASK, ctx->pc & SPU_LS_MASK);
        exit(3);
    }
}

void spu_img_restore(spu_context* ctx, int32_t saved_img) { ctx->image_id = saved_img; }

void spu_stop(spu_context* ctx)
{
    if (g_verbose)
        fprintf(stderr, "[replay] spu_stop at pc=0x%05X code=0x%X\n",
                ctx->pc & SPU_LS_MASK, ctx->stop_code);
}

/* ---- instrumented channels / DMA --------------------------------------- */
static long g_ch_ops = 0;
static long g_ch_budget = 2000000;
static long g_dma_gets, g_dma_puts, g_dma_other;
static uint32_t g_last_mfc_pc;
static int g_zero_get = 1;      /* zero-fill GET payloads */
static int g_trace_ch = 1;

static void ch_abort(const char* what)
{
    fprintf(stderr, "[replay] ABORT: channel budget exceeded (%s), ops=%ld\n", what, g_ch_ops);
    exit(4);
}

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];
    if (++g_ch_ops > g_ch_budget) ch_abort("wrch");
    switch (channel) {
    case MFC_LSA:  ctx->mfc_lsa = v; break;
    case MFC_EAH:  ctx->mfc_eah = v; break;
    case MFC_EAL:  ctx->mfc_eal = v; break;
    case MFC_Size: ctx->mfc_size = v; break;
    case MFC_TagID:ctx->mfc_tag = v; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v; break;
    case MFC_WrTagUpdate:
        ctx->mfc_tag_status = 0;         /* all tags complete */
        if (g_trace_ch)
            fprintf(stderr, "[ch] WrTagUpdate mask=0x%X pc=0x%05X\n",
                    ctx->mfc_tag_mask, ctx->pc & SPU_LS_MASK);
        break;
    case MFC_Cmd: {
        uint32_t cmd = v, lsa = ctx->mfc_lsa & SPU_LS_MASK;
        uint64_t ea = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
        g_last_mfc_pc = ctx->pc & SPU_LS_MASK;
        if (cmd == MFC_GET_CMD || cmd == MFC_GETB_CMD) {
            g_dma_gets++;
            if (g_trace_ch && g_dma_gets <= 64)
                fprintf(stderr, "[dma] GET  cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X tag=%u pc=0x%05X%s\n",
                        cmd, lsa, ea, ctx->mfc_size, ctx->mfc_tag,
                        ctx->pc & SPU_LS_MASK, g_zero_get ? " (zero-fill)" : "");
            if (g_zero_get && ctx->mfc_size && ctx->mfc_size <= SPU_LS_SIZE)
                memset(&ctx->ls[lsa], 0, ctx->mfc_size);
        } else if (cmd == MFC_PUT_CMD || cmd == MFC_PUTB_CMD || cmd == MFC_PUTF_CMD) {
            g_dma_puts++;
            if (g_trace_ch && g_dma_puts <= 64)
                fprintf(stderr, "[dma] PUT  cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X tag=%u pc=0x%05X\n",
                        cmd, lsa, ea, ctx->mfc_size, ctx->mfc_tag, ctx->pc & SPU_LS_MASK);
        } else {
            g_dma_other++;
            if (g_trace_ch && g_dma_other <= 64)
                fprintf(stderr, "[dma] CMD  cmd=0x%02X lsa=0x%05X ea=0x%08X size=0x%X tag=%u pc=0x%05X\n",
                        cmd, lsa, ea, ctx->mfc_size, ctx->mfc_tag, ctx->pc & SPU_LS_MASK);
        }
        break;
    }
    case SPU_WrOutMbox:
        if (g_trace_ch && g_ch_ops < 200)
            fprintf(stderr, "[ch] WrOutMbox 0x%08X pc=0x%05X\n", v, ctx->pc & SPU_LS_MASK);
        break;
    case SPU_WrOutIntrMbox:
        if (g_trace_ch && g_ch_ops < 200)
            fprintf(stderr, "[ch] WrOutIntrMbox 0x%08X pc=0x%05X\n", v, ctx->pc & SPU_LS_MASK);
        break;
    default:
        if (g_trace_ch && g_ch_ops < 400)
            fprintf(stderr, "[ch] wrch ch=%u val=0x%08X pc=0x%05X\n", channel, v, ctx->pc & SPU_LS_MASK);
        break;
    }
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    u128 r; r._u32[0]=r._u32[1]=r._u32[2]=r._u32[3]=0;
    if (++g_ch_ops > g_ch_budget) ch_abort("rdch");
    switch (channel) {
    case MFC_RdTagStat: r._u32[0] = 0; break;       /* complete */
    case MFC_RdTagMask: r._u32[0] = ctx->mfc_tag_mask; break;
    case MFC_RdAtomicStat: r._u32[0] = 0; break;
    case SPU_RdInMbox:
        if (g_trace_ch && g_ch_ops < 200)
            fprintf(stderr, "[ch] RdInMbox -> 0 pc=0x%05X\n", ctx->pc & SPU_LS_MASK);
        break;
    case SPU_RdEventStat: r._u32[0] = 0; break;
    case SPU_RdDec: r._u32[0] = 0xFFFFFFFFu; break;
    case SPU_RdMachStat: r._u32[0] = 1; break;
    default:
        if (g_trace_ch && g_ch_ops < 400)
            fprintf(stderr, "[ch] rdch ch=%u pc=0x%05X\n", channel, ctx->pc & SPU_LS_MASK);
        break;
    }
    return r;
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    (void)ctx; (void)channel;
    return 0;
}

void spu_fncap_capture(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }

/* The generated image registers its symbols with the runtime dispatcher; we
 * use our own static table (reg_10f78.c) instead. */
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

/* Optional per-lifted-function entry histogram (probe-built spu_recomp). */
unsigned long g_fncount[65536];
void __probe_fn(unsigned addr) { g_fncount[(addr >> 2) & 0xFFFFu]++; }
static void dump_fncount(void)
{
    typedef struct { unsigned long n; unsigned a; } ent;
    static ent top[40]; int nt = 0;
    for (unsigned i = 0; i < 65536; i++) {
        if (!g_fncount[i]) continue;
        ent e = { g_fncount[i], i << 2 };
        if (nt < 40) top[nt++] = e;
        else {
            int mn = 0;
            for (int j = 1; j < 40; j++) if (top[j].n < top[mn].n) mn = j;
            if (e.n > top[mn].n) top[mn] = e;
        }
    }
    for (int i = 0; i < nt; i++) for (int j = i+1; j < nt; j++)
        if (top[j].n > top[i].n) { ent t = top[i]; top[i] = top[j]; top[j] = t; }
    fprintf(stderr, "[fncount] top lifted-function entries:\n");
    for (int i = 0; i < nt && i < 40; i++)
        fprintf(stderr, "  0x%05X  %lu\n", top[i].a, top[i].n);
}

/* ---- replay ------------------------------------------------------------ */
#define GPR_BYTES (128u * 16u)

static void load_capture(const char* path, void* dst, size_t want)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t got = fread(dst, 1, want, f);
    if (got != want) { fprintf(stderr, "%s: short read %zu/%zu\n", path, got, want); exit(1); }
    fclose(f);
}

static uint64_t fnv1a_update(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

void jobmod_c690c2f7b09a_eF5B8_spu_func_00010F78(spu_context* ctx);

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap_full/0x10F78";
    const char* outls = argc > 2 ? argv[2] : NULL;
    const char* e;
    if ((e = getenv("FF_ZERO_GET"))) g_zero_get = atoi(e);
    if ((e = getenv("FF_QUIET"))) g_verbose = !atoi(e);
    if ((e = getenv("FF_TRACE_CH"))) g_trace_ch = atoi(e);
    if ((e = getenv("FF_MAX_DISPATCH"))) g_max_dispatch = atol(e);
    if ((e = getenv("FF_CH_BUDGET"))) g_ch_budget = atol(e);

    char ls_path[600], gpr_path[600];
    snprintf(ls_path, sizeof ls_path, "%s/ls.bin", dir);
    snprintf(gpr_path, sizeof gpr_path, "%s/gpr.bin", dir);

    static spu_context ctx;
    uint8_t* ls0 = malloc(SPU_LS_SIZE);
    uint8_t* gpr0 = malloc(GPR_BYTES);
    if (!ls0 || !gpr0) { perror("malloc"); return 1; }
    load_capture(ls_path, ls0, SPU_LS_SIZE);
    load_capture(gpr_path, gpr0, GPR_BYTES);

    spu_context_init(&ctx, 0);
    ctx.pc = 0x10F78;
    ctx.image_id = 6;
    memcpy(ctx.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx.gpr, gpr0, GPR_BYTES);

    uint32_t entry = 0x10F78;
    if ((e = getenv("FF_ENTRY"))) entry = (uint32_t)strtoul(e, 0, 16);
    fprintf(stderr, "=== 0x%05X replay: dir=%s zero_get=%d ===\n", entry, dir, g_zero_get);
    uint64_t h_ls0 = fnv1a_update(0xcbf29ce484222325ull, ctx.ls, SPU_LS_SIZE);

    {
        spu_fn _fn = lookup_fn(entry);
        if (!_fn) { fprintf(stderr, "no lifted function at 0x%05X\n", entry); return 1; }
        _fn(&ctx);
    }
    /* Run any tail transfers the entry left pending (gcc SPU_DIRECT path). */
    if (g_spu_trampoline_fn && ctx.host_depth == 0 &&
        (ctx.pc & SPU_LS_MASK) == (ctx.gpr[0]._u32[0] & SPU_LS_MASK)) {
        g_spu_trampoline_fn = NULL;
    }

    uint64_t h_ls = fnv1a_update(0xcbf29ce484222325ull, ctx.ls, SPU_LS_SIZE);
    uint64_t h_gpr = fnv1a_update(0xcbf29ce484222325ull, ctx.gpr, GPR_BYTES);
    uint64_t h_all = fnv1a_update(h_ls, ctx.gpr, GPR_BYTES);

    printf("=================================================================\n");
    printf("MLB16 SPU job c690c690 0x10F78 offline replay (instrumented)\n");
    printf("Capture: %s\n", dir);
    printf("LS FNV (input)   : 0x%016llx\n", (unsigned long long)h_ls0);
    printf("LS FNV (output)  : 0x%016llx\n", (unsigned long long)h_ls);
    printf("GPR FNV (output) : 0x%016llx\n", (unsigned long long)h_gpr);
    printf("LS+GPR FNV       : 0x%016llx\n", (unsigned long long)h_all);
    printf("DMA get=%ld put=%ld other=%ld  ch_ops=%ld  dispatches=%ld\n",
           g_dma_gets, g_dma_puts, g_dma_other, g_ch_ops, g_dispatch_n);
    printf("Final pc=0x%05X status=0x%X stop=0x%X host_depth=%u\n",
           ctx.pc & SPU_LS_MASK, ctx.status, ctx.stop_code, ctx.host_depth);

    /* checksum of the position output region LS 0x1F7C0 .. +0xC00 */
    {
        uint64_t hp = fnv1a_update(0xcbf29ce484222325ull, &ctx.ls[0x1F7C0], 0xC00);
        printf("Position region 0x1F7C0[0xC00] FNV: 0x%016llx\n", (unsigned long long)hp);
    }
    if (getenv("FF_FNCOUNT")) dump_fncount();
    if (outls) {
        FILE* f = fopen(outls, "wb");
        if (!f) { perror(outls); return 1; }
        fwrite(ctx.ls, 1, SPU_LS_SIZE, f);
        fclose(f);
        printf("Wrote %s\n", outls);
    }
    return 0;
}
