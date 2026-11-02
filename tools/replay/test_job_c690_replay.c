/*
 * Standalone offline replay test for lifted SPU function 0x6D68
 * (jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68).
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/include \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/runtime/spu \
 *       -o /tmp/opencode/fncap/test_job_c690_replay \
 *       /home/m/Documents/mlb16-online/recomp-work/notes/test_job_c690_replay.c -lm
 *
 * Run:
 *   /tmp/opencode/fncap/test_job_c690_replay
 *
 * Inputs (captured from the running game at entry to LS 0x6D68):
 *   /tmp/opencode/fncap/ls.bin   262144 bytes (full SPU local store)
 *   /tmp/opencode/fncap/gpr.bin    2048 bytes (128 GPRs x 16 bytes)
 *   /tmp/opencode/fncap/pc.txt      lsa=0x06D68 pc=0x06D68 image=6
 *
 * 0x6D68 is a true leaf: no SPU_DIRECT, no spu_indirect_branch, no DMA.
 * It only touches ctx->gpr and ctx->ls through spu_helpers.h. The one
 * non-semantic line in the generated body (spu_fncap_capture, the probe
 * that produced these capture files) was removed in ref_spu_func_6d68.c.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <immintrin.h>

#include "spu_context.h"
#include "spu_helpers.h"

/* Minimal runtime stubs: SPU_RET() at the end of the lifted body publishes
 * the return pc and references spu_indirect_branch. This leaf has no
 * SPU_DIRECT, so g_spu_stack_reset_hint is not needed. */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

#include "ref_spu_func_6d68.c"

static char g_ls_file[600], g_gpr_file[600];
#define GPR_BYTES (128u * 16u)
#define BENCH_CALLS 200000
#define BENCH_RUNS  5
#define BENCH_WARMUP 20000

static uint64_t fnv1a_update(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmp_double(const void* a, const void* b)
{
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

/* Replay the exact loop-control expression from the lifted body:
 *   sr40 = shufb(sr40, sr29, sr14);
 *   sr29 = ai(sr29, 128);
 *   ... while (sr40[0] != 0)
 * The body never reassigns sr40, so this counts the iterations exactly. */
static uint32_t infer_loop_count(const spu_context* ctx)
{
    spu_v4u sr40 = spu_u2v(ctx->gpr[40]);
    spu_v4u sr29 = spu_u2v(ctx->gpr[29]);
    spu_v4u sr14 = spu_u2v(ctx->gpr[14]);
    uint32_t n = 0;
    do {
        sr40 = spu_u2v(spu_shufb(spu_v2u(sr40), spu_v2u(sr29), spu_v2u(sr14)));
        n++;
        sr29 = spu_u2v(spu_ai(spu_v2u(sr29), 128));
        if (n > 100000000u) break;
    } while (sr40[0] != 0);
    return n;
}

static void load_capture(const char* path, void* dst, size_t want)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t got = fread(dst, 1, want, f);
    if (got != want) {
        fprintf(stderr, "%s: short read %zu/%zu\n", path, got, want);
        exit(1);
    }
    if (fgetc(f) != EOF) {
        fprintf(stderr, "%s: larger than %zu bytes\n", path, want);
        exit(1);
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap";
    snprintf(g_ls_file, sizeof g_ls_file, "%s/ls.bin", dir);
    snprintf(g_gpr_file, sizeof g_gpr_file, "%s/gpr.bin", dir);
    static spu_context ctx;
    uint8_t* ls0  = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* gpr0 = (uint8_t*)malloc(GPR_BYTES);
    if (!ls0 || !gpr0) { perror("malloc"); return 1; }

    load_capture(g_ls_file, ls0, SPU_LS_SIZE);
    load_capture(g_gpr_file, gpr0, GPR_BYTES);

    spu_context_init(&ctx, 0);
    ctx.pc = 0x06D68;
    ctx.image_id = 6;

    memcpy(ctx.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx.gpr, gpr0, GPR_BYTES);

    uint32_t loop_count = infer_loop_count(&ctx);

    printf("=================================================================\n");
    printf("MLB 16 SPU job c690c2f7b09a 0x6D68 offline replay (leaf)\n");
    printf("Build Flags: -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS\n");
    printf("Capture: lsa=0x06D68 pc=0x06D68 image=6\n");
    printf("Inferred loop count: %u (gpr[40]/sr40 loop control, sr29 += 128/iter)\n",
           loop_count);
    printf("=================================================================\n");

    /* One call from the captured state -> deterministic checksums. */
    jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx);

    uint64_t h_ls = fnv1a_update(0xcbf29ce484222325ull, ctx.ls, SPU_LS_SIZE);
    uint64_t h_gpr = fnv1a_update(0xcbf29ce484222325ull, ctx.gpr, GPR_BYTES);
    uint64_t h_all = fnv1a_update(h_ls, ctx.gpr, GPR_BYTES);

    printf("FNV-1a64 LS        : 0x%016llx\n", (unsigned long long)h_ls);
    printf("FNV-1a64 GPR       : 0x%016llx\n", (unsigned long long)h_gpr);
    printf("FNV-1a64 LS+GPR    : 0x%016llx\n", (unsigned long long)h_all);
    printf("Final pc=0x%05X  gpr[29]=0x%08X gpr[40]=0x%08X\n",
           ctx.pc & SPU_LS_MASK, ctx.gpr[29]._u32[0], ctx.gpr[40]._u32[0]);

    /* Benchmark: every call replays the SAME captured state. Only the 2 KB GPR
     * set needs resetting to reproduce the loop count (gpr[40]/gpr[29]/gpr[14]);
     * the 256 KB LS is reset once and then written in place at the same
     * addresses, so the per-call work is identical. */
    memcpy(ctx.ls, ls0, SPU_LS_SIZE);
    double reset_s[BENCH_RUNS], call_s[BENCH_RUNS];

    for (int i = 0; i < BENCH_WARMUP; i++) {
        memcpy(ctx.gpr, gpr0, GPR_BYTES);
        ctx.pc = 0x06D68;
        ctx.host_depth = 0;
        jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx);
    }
    for (int i = 0; i < BENCH_WARMUP; i++)
        memcpy(ctx.gpr, gpr0, GPR_BYTES);

    for (int r = 0; r < BENCH_RUNS; r++) {
        if (r & 1) {
            double t0 = now_sec();
            for (int i = 0; i < BENCH_CALLS; i++) memcpy(ctx.gpr, gpr0, GPR_BYTES);
            reset_s[r] = now_sec() - t0;
            t0 = now_sec();
            for (int i = 0; i < BENCH_CALLS; i++) {
                memcpy(ctx.gpr, gpr0, GPR_BYTES);
                ctx.pc = 0x06D68;
                ctx.host_depth = 0;
                jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx);
            }
            call_s[r] = now_sec() - t0;
        } else {
            double t0 = now_sec();
            for (int i = 0; i < BENCH_CALLS; i++) {
                memcpy(ctx.gpr, gpr0, GPR_BYTES);
                ctx.pc = 0x06D68;
                ctx.host_depth = 0;
                jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx);
            }
            call_s[r] = now_sec() - t0;
            t0 = now_sec();
            for (int i = 0; i < BENCH_CALLS; i++) memcpy(ctx.gpr, gpr0, GPR_BYTES);
            reset_s[r] = now_sec() - t0;
        }
    }

    qsort(reset_s, BENCH_RUNS, sizeof(double), cmp_double);
    qsort(call_s, BENCH_RUNS, sizeof(double), cmp_double);
    double med_reset = reset_s[BENCH_RUNS / 2];
    double med_call  = call_s[BENCH_RUNS / 2];

    double ns_total  = med_call / BENCH_CALLS * 1e9;
    double ns_reset  = med_reset / BENCH_CALLS * 1e9;
    double ns_fn_est = ns_total - ns_reset;

    printf("\nBenchmark (median of %d runs, %d calls each, captured state reset per call):\n",
           BENCH_RUNS, BENCH_CALLS);
    printf("  reset LS+GPR only : %8.3f ms | %8.2f ns/call\n",
           med_reset * 1e3, ns_reset);
    printf("  reset + lifted fn : %8.3f ms | %8.2f ns/call\n",
           med_call * 1e3, ns_total);
    printf("  lifted fn only    : %8.3f ms | %8.2f ns/call (reset subtracted)\n",
           (med_call - med_reset) * 1e3, ns_fn_est);
    printf("  =================================================================\n");

    free(ls0);
    free(gpr0);
    return 0;
}
