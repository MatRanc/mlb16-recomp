/*
 * Offline equivalence + benchmark test for the native AVX2/FMA kernel of the
 * lifted SPU replay unit 0xAF98 -> 0xB238, against the lifted reference, on
 * the captured real game state.
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/include \
 *       -I/home/m/Documents/mlb16-online/ps3recomp-native/runtime/spu \
 *       -o /tmp/opencode/fncap_af98/test_job_c690_af98_native \
 *       /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/test_job_c690_af98_native.c -lm
 *
 * Run:
 *   /tmp/opencode/fncap_af98/test_job_c690_af98_native /tmp/opencode/fncap_af98
 *
 * The unit is run the way the runtime runs it: call 0xAF98, follow the
 * SPU_DIRECT trampoline into 0xB238, and stop when 0xB238 publishes its
 * indirect exit (ctx->pc = gpr[73], trampoline = spu_indirect_branch).
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

/* Minimal runtime stubs for SPU_DIRECT / indirect dispatch. */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }

#include "ref_spu_func_af98_b238.c"
#include "native_af98_b238.c"

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

/* Run the lifted unit exactly as the drain loop would: 0xAF98 tail-calls
 * 0xB238; 0xB238 leaves through spu_indirect_branch. We stop at that exit. */
static int run_lifted_unit(spu_context* ctx)
{
    int hops = 0;
    g_spu_trampoline_fn = NULL;
    jobmod_c690c2f7b09a_eF5B8_spu_func_0000AF98(ctx);
    while (g_spu_trampoline_fn) {
        void (*tf)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        if (tf == spu_indirect_branch) { hops++; break; }
        tf(ctx);
        hops++;
    }
    return hops;
}

__attribute__((noinline))
static double bench_lifted(spu_context* ctx, const uint8_t* gpr0, uint32_t calls)
{
    double t0 = now_sec();
    for (uint32_t i = 0; i < calls; i++) {
        memcpy(ctx->gpr, gpr0, GPR_BYTES);
        ctx->pc = 0x0AF98;
        ctx->host_depth = 0;
        run_lifted_unit(ctx);
    }
    return now_sec() - t0;
}

__attribute__((noinline))
static double bench_native(spu_context* ctx, const uint8_t* gpr0, uint32_t calls)
{
    double t0 = now_sec();
    for (uint32_t i = 0; i < calls; i++) {
        memcpy(ctx->gpr, gpr0, GPR_BYTES);
        ctx->pc = 0x0AF98;
        ctx->host_depth = 0;
        native_spu_func_0000AF98(ctx);
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

static void print_checksums(const char* tag, const spu_context* ctx)
{
    uint64_t hls = fnv1a_update(0xcbf29ce484222325ull, ctx->ls, SPU_LS_SIZE);
    uint64_t hgp = fnv1a_update(hls, ctx->gpr, GPR_BYTES);
    printf("%s FNV-1a64 LS=0x%016llx GPR=0x%016llx LS+GPR=0x%016llx pc=0x%08X\n",
           tag, (unsigned long long)hls, (unsigned long long)hgp,
           (unsigned long long)hgp, ctx->pc);
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap_af98";
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

    printf("=================================================================\n");
    printf("MLB 16 SPU job c690 unit 0xAF98 -> 0xB238: native AVX2/FMA vs lifted\n");
    printf("Build Flags: -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS\n");
    printf("Capture: lsa=0x0AF98 pc=0x0AF98 image=6\n");
    printf("=================================================================\n");

    ctx_ref.pc = ctx_nat.pc = 0x0AF98;
    ctx_ref.image_id = ctx_nat.image_id = 6;
    memcpy(ctx_ref.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_nat.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_ref.gpr, gpr0, GPR_BYTES);
    memcpy(ctx_nat.gpr, gpr0, GPR_BYTES);

    int hops = run_lifted_unit(&ctx_ref);
    printf("Lifted unit: terminated at indirect exit after %d dispatch hop(s)\n", hops);
    printf("  final pc=0x%08X (LS 0x%05X) published by 0xB238 from gpr[73]\n",
           ctx_ref.pc, ctx_ref.pc & SPU_LS_MASK);

    native_spu_func_0000AF98(&ctx_nat);
    g_spu_trampoline_fn = NULL;

    uint64_t ref_ls  = fnv1a_update(0xcbf29ce484222325ull, ctx_ref.ls, SPU_LS_SIZE);
    uint64_t ref_all = fnv1a_update(ref_ls, ctx_ref.gpr, GPR_BYTES);
    uint64_t nat_ls  = fnv1a_update(0xcbf29ce484222325ull, ctx_nat.ls, SPU_LS_SIZE);
    uint64_t nat_all = fnv1a_update(nat_ls, ctx_nat.gpr, GPR_BYTES);

    printf("REFERENCE (lifted): FNV-1a64 LS+GPR = 0x%016llx  pc=0x%08X\n",
           (unsigned long long)ref_all, ctx_ref.pc);
    printf("NATIVE           : FNV-1a64 LS+GPR = 0x%016llx  pc=0x%08X\n",
           (unsigned long long)nat_all, ctx_nat.pc);

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

    for (size_t b = 0; b < SPU_LS_SIZE; b += 16)
        if (memcmp(ctx_ref.ls + b, ctx_nat.ls + b, 16) != 0) {
            printf("DIFF ls 0x%05zX lifted=%08x %08x %08x %08x native=%08x %08x %08x %08x\n",
                   b,
                   *(uint32_t*)(ctx_ref.ls+b), *(uint32_t*)(ctx_ref.ls+b+4),
                   *(uint32_t*)(ctx_ref.ls+b+8), *(uint32_t*)(ctx_ref.ls+b+12),
                   *(uint32_t*)(ctx_nat.ls+b), *(uint32_t*)(ctx_nat.ls+b+4),
                   *(uint32_t*)(ctx_nat.ls+b+8), *(uint32_t*)(ctx_nat.ls+b+12));
            if (--ls_diff_quads == 0) break;
        }

    printf("Diff: LS bytes=%zu  GPRs=%d  pc=%d\n", ls_diff_bytes, gpr_diff, pc_diff);
    int pass = (ls_diff_bytes == 0 && gpr_diff == 0 && pc_diff == 0);
    printf("RESULT: %s\n", pass ? "PASS (bit-exact)" : "FAIL");
    printf("=================================================================\n");

    /* ------------------------------------------------------------------ */
    /* Benchmark: GPRs reset per call; LS reset once, written in place.    */
    /* ------------------------------------------------------------------ */
    double reset_s[BENCH_RUNS], ref_s[BENCH_RUNS], nat_s[BENCH_RUNS];

    memcpy(ctx_ref.ls, ls0, SPU_LS_SIZE);
    memcpy(ctx_nat.ls, ls0, SPU_LS_SIZE);
    for (int i = 0; i < BENCH_WARMUP; i++) {
        memcpy(ctx_ref.gpr, gpr0, GPR_BYTES); ctx_ref.pc = 0x0AF98; ctx_ref.host_depth = 0;
        run_lifted_unit(&ctx_ref);
        memcpy(ctx_nat.gpr, gpr0, GPR_BYTES); ctx_nat.pc = 0x0AF98; ctx_nat.host_depth = 0;
        native_spu_func_0000AF98(&ctx_nat);
    }
    for (int r = 0; r < BENCH_RUNS; r++) {
        reset_s[r] = bench_reset(&ctx_ref, gpr0, BENCH_CALLS);
        if (r & 1) {
            nat_s[r] = bench_native(&ctx_nat, gpr0, BENCH_CALLS);
            ref_s[r] = bench_lifted(&ctx_ref, gpr0, BENCH_CALLS);
        } else {
            ref_s[r] = bench_lifted(&ctx_ref, gpr0, BENCH_CALLS);
            nat_s[r] = bench_native(&ctx_nat, gpr0, BENCH_CALLS);
        }
    }

    qsort(reset_s, BENCH_RUNS, sizeof(double), cmp_double);
    qsort(ref_s, BENCH_RUNS, sizeof(double), cmp_double);
    qsort(nat_s, BENCH_RUNS, sizeof(double), cmp_double);
    double ns_reset = reset_s[BENCH_RUNS / 2] / BENCH_CALLS * 1e9;
    double ns_ref   = ref_s[BENCH_RUNS / 2] / BENCH_CALLS * 1e9;
    double ns_nat   = nat_s[BENCH_RUNS / 2] / BENCH_CALLS * 1e9;

    printf("\nBenchmark (median of %d runs, %d calls each, GPRs reset per call):\n",
           BENCH_RUNS, BENCH_CALLS);
    printf("  reset GPR only          : %8.2f ns/call\n", ns_reset);
    printf("  lifted unit + reset     : %8.2f ns/call\n", ns_ref);
    printf("  native unit + reset     : %8.2f ns/call\n", ns_nat);
    printf("  lifted unit fn only     : %8.2f ns/call\n", ns_ref - ns_reset);
    printf("  native unit fn only     : %8.2f ns/call\n", ns_nat - ns_reset);
    printf("  speedup                 : %8.2fx\n",
           (ns_nat - ns_reset) > 0 ? (ns_ref - ns_reset) / (ns_nat - ns_reset) : 0.0);
    printf("=================================================================\n");

    free(ls0);
    free(gpr0);
    return pass ? 0 : 1;
}
