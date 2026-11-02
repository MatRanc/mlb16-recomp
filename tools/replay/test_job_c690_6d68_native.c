/*
 * Offline equivalence + benchmark test for the native AVX2/FMA kernel of
 * lifted SPU function 0x6D68 against the lifted reference, on the captured
 * real game state.
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/include \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/runtime/spu \
 *       -o /tmp/opencode/fncap/test_job_c690_6d68_native \
 *       /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/test_job_c690_6d68_native.c -lm
 *
 * Run:
 *   /tmp/opencode/fncap/test_job_c690_6d68_native /tmp/opencode/fncap
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

/* Minimal runtime stubs for SPU_RET / indirect dispatch. */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

#include "ref_spu_func_6d68.c"
#include "native_6d68.c"

#define GPR_BYTES (128u * 16u)
#define BENCH_CALLS 300000
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

__attribute__((noinline))
static double bench_run(spu_context* ctx, void (*fn)(spu_context*),
                        const uint8_t* gpr0, uint32_t calls)
{
    double t0 = now_sec();
    for (uint32_t i = 0; i < calls; i++) {
        memcpy(ctx->gpr, gpr0, GPR_BYTES);
        ctx->pc = 0x06D68;
        ctx->host_depth = 0;
        fn(ctx);
    }
    return now_sec() - t0;
}

__attribute__((noinline))
static double bench_reset(spu_context* ctx, const uint8_t* gpr0, uint32_t calls)
{
    double t0 = now_sec();
    for (uint32_t i = 0; i < calls; i++)
        memcpy(ctx->gpr, gpr0, GPR_BYTES);
    return now_sec() - t0;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap";
    char ls_file[600], gpr_file[600];
    snprintf(ls_file, sizeof ls_file, "%s/ls.bin", dir);
    snprintf(gpr_file, sizeof gpr_file, "%s/gpr.bin", dir);

    static spu_context ctx_ref, ctx_nat;
    spu_context_init(&ctx_ref, 0);
    spu_context_init(&ctx_nat, 0);

    uint8_t* ls0  = (uint8_t*)malloc(SPU_LS_SIZE);
    uint8_t* gpr0 = (uint8_t*)malloc(GPR_BYTES);
    if (!ls0 || !gpr0) { perror("malloc"); return 1; }

    load_capture(ls_file, ls0, SPU_LS_SIZE);
    load_capture(gpr_file, gpr0, GPR_BYTES);

    ctx_ref.pc = ctx_nat.pc = 0x06D68;
    ctx_ref.image_id = ctx_nat.image_id = 6;
    memcpy(ctx_ref.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_nat.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_ref.gpr, gpr0, GPR_BYTES);
    memcpy(ctx_nat.gpr, gpr0, GPR_BYTES);

    uint32_t loop_count = infer_loop_count(&ctx_ref);

    printf("=================================================================\n");
    printf("MLB 16 SPU job c690 0x6D68: native AVX2/FMA vs lifted reference\n");
    printf("Build Flags: -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS\n");
    printf("Capture: lsa=0x06D68 pc=0x06D68 image=6\n");
    printf("Inferred loop count: %u (sr40 loop control, sr29 += 128/iter)\n", loop_count);
    printf("=================================================================\n");

    jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx_ref);
    native_spu_func_00006D68(&ctx_nat);

    uint64_t ref_ls  = fnv1a_update(0xcbf29ce484222325ull, ctx_ref.ls, SPU_LS_SIZE);
    uint64_t ref_gpr = fnv1a_update(0xcbf29ce484222325ull, ctx_ref.gpr, GPR_BYTES);
    uint64_t ref_all = fnv1a_update(ref_ls, ctx_ref.gpr, GPR_BYTES);
    uint64_t nat_ls  = fnv1a_update(0xcbf29ce484222325ull, ctx_nat.ls, SPU_LS_SIZE);
    uint64_t nat_gpr = fnv1a_update(0xcbf29ce484222325ull, ctx_nat.gpr, GPR_BYTES);
    uint64_t nat_all = fnv1a_update(nat_ls, ctx_nat.gpr, GPR_BYTES);

    printf("FNV-1a64 LS      lifted: 0x%016llx   native: 0x%016llx\n",
           (unsigned long long)ref_ls, (unsigned long long)nat_ls);
    printf("FNV-1a64 GPR     lifted: 0x%016llx   native: 0x%016llx\n",
           (unsigned long long)ref_gpr, (unsigned long long)nat_gpr);
    printf("FNV-1a64 LS+GPR  lifted: 0x%016llx   native: 0x%016llx\n",
           (unsigned long long)ref_all, (unsigned long long)nat_all);
    printf("Final pc         lifted: 0x%05X          native: 0x%05X\n",
           ctx_ref.pc & SPU_LS_MASK, ctx_nat.pc & SPU_LS_MASK);

    size_t ls_diff_bytes = 0, ls_diff_quads = 0;
    for (size_t b = 0; b < SPU_LS_SIZE; b++) if (ctx_ref.ls[b] != ctx_nat.ls[b]) ls_diff_bytes++;
    for (size_t b = 0; b < SPU_LS_SIZE; b += 16)
        if (memcmp(ctx_ref.ls + b, ctx_nat.ls + b, 16) != 0) ls_diff_quads++;

    int gpr_diff = 0, pc_diff = (ctx_ref.pc != ctx_nat.pc);
    for (int i = 0; i < 128; i++)
        if (memcmp(&ctx_ref.gpr[i], &ctx_nat.gpr[i], 16) != 0) {
            printf("DIFF gpr r%-3d lifted=%08x %08x %08x %08x native=%08x %08x %08x %08x\n",
                   i, ctx_ref.gpr[i]._u32[0], ctx_ref.gpr[i]._u32[1],
                   ctx_ref.gpr[i]._u32[2], ctx_ref.gpr[i]._u32[3],
                   ctx_nat.gpr[i]._u32[0], ctx_nat.gpr[i]._u32[1],
                   ctx_nat.gpr[i]._u32[2], ctx_nat.gpr[i]._u32[3]);
            gpr_diff++;
        }

    printf("Diff: LS bytes=%zu quads=%zu  GPRs=%d  pc=%d\n",
           ls_diff_bytes, ls_diff_quads, gpr_diff, pc_diff);

    int pass = (ls_diff_bytes == 0 && gpr_diff == 0 && pc_diff == 0
                && ref_all == 0xfc2e9569dc2feb11ull && nat_all == 0xfc2e9569dc2feb11ull);
    printf("RESULT: %s\n", pass ? "PASS (bit-exact)" : "FAIL");
    printf("=================================================================\n");

    /* ------------------------------------------------------------------ */
    /* Benchmark: GPRs reset per call; LS reset once, written in place.    */
    /* ------------------------------------------------------------------ */
    double reset_s[BENCH_RUNS], ref_s[BENCH_RUNS], nat_s[BENCH_RUNS];

    memcpy(ctx_ref.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_nat.ls, ls0, SPU_LS_SIZE);
    for (int i = 0; i < BENCH_WARMUP; i++) {
        memcpy(ctx_ref.gpr, gpr0, GPR_BYTES); ctx_ref.pc = 0x06D68; ctx_ref.host_depth = 0;
        jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx_ref);
        memcpy(ctx_nat.gpr, gpr0, GPR_BYTES); ctx_nat.pc = 0x06D68; ctx_nat.host_depth = 0;
        native_spu_func_00006D68(&ctx_nat);
    }
    for (int r = 0; r < BENCH_RUNS; r++) {
        reset_s[r] = bench_reset(&ctx_ref, gpr0, BENCH_CALLS);
        if (r & 1) {
            nat_s[r] = bench_run(&ctx_nat, native_spu_func_00006D68, gpr0, BENCH_CALLS);
            ref_s[r] = bench_run(&ctx_ref, jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68, gpr0, BENCH_CALLS);
        } else {
            ref_s[r] = bench_run(&ctx_ref, jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68, gpr0, BENCH_CALLS);
            nat_s[r] = bench_run(&ctx_nat, native_spu_func_00006D68, gpr0, BENCH_CALLS);
        }
    }

    qsort(reset_s, BENCH_RUNS, sizeof(double), cmp_double);
    qsort(ref_s, BENCH_RUNS, sizeof(double), cmp_double);
    qsort(nat_s, BENCH_RUNS, sizeof(double), cmp_double);
    double med_reset = reset_s[BENCH_RUNS / 2];
    double med_ref   = ref_s[BENCH_RUNS / 2];
    double med_nat   = nat_s[BENCH_RUNS / 2];

    double ns_reset = med_reset / BENCH_CALLS * 1e9;
    double ns_ref   = med_ref / BENCH_CALLS * 1e9;
    double ns_nat   = med_nat / BENCH_CALLS * 1e9;
    double fn_ref   = ns_ref - ns_reset;
    double fn_nat   = ns_nat - ns_reset;

    printf("\nBenchmark (median of %d runs, %d calls each, GPRs reset per call):\n",
           BENCH_RUNS, BENCH_CALLS);
    printf("  reset GPR only    : %8.2f ns/call\n", ns_reset);
    printf("  lifted + reset    : %8.2f ns/call\n", ns_ref);
    printf("  native + reset    : %8.2f ns/call\n", ns_nat);
    printf("  lifted fn only    : %8.2f ns/call\n", fn_ref);
    printf("  native fn only    : %8.2f ns/call\n", fn_nat);
    printf("  speedup           : %8.2fx\n", fn_nat > 0 ? fn_ref / fn_nat : 0.0);
    printf("=================================================================\n");

    free(ls0);
    free(gpr0);
    return pass ? 0 : 1;
}
