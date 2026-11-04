/*
 * Offline replay of the lifted MLB16 WWS DXT job kernel (EBOOT EA 0x010A1700,
 * LS 0x4000) on the captured job corpus.
 *
 * The corpus (runtime/spu/dxt_capture.c) is one spool per SPURS lane. With
 * PS3RECOMP_DXT_CAPTURE_ENTRY=1 each wid-0 policy run also records the full
 * local store (type 7) and all 128 GPRs (type 8) at the first dispatch of
 * the job body at LS 0x4030. The DMA records before that point are the PM's
 * outstanding GETs (the entry snapshot is taken before they land), so they
 * are applied to the entry local store; the DMA records after it are the
 * kernel's own GETs and PUTs.
 *
 * Build (native header copy, the one the lifted module was compiled against):
 *   cc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *      -I<ps3recomp-native>/include -I<ps3recomp-native>/runtime/spu \
 *      -o /tmp/opencode/test_dxt_replay test_dxt_job_replay.c \
 *      <spu_lifted>/jobmod_647a2bf49000_e0030/spu_recomp.c -lm
 *
 * Run: /tmp/opencode/test_dxt_replay <capture-dir> [max-runs]
 *
 * For every run it compares the kernel's PUT payloads byte-for-byte with the
 * captured ones. 100 % match is the lift's exactness gate.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>

#include "spu_context.h"
#include "spu_helpers.h"

/* ---- minimal runtime glue ------------------------------------------------ */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;

typedef void (*spu_fn)(spu_context*);
extern void jobmod_647a2bf49000_e0030_spu_recomp_register(void);
extern void wwsjob_pm_spu_recomp_register(void);

#define REG_MAX 4096
static struct { uint32_t addr; spu_fn fn; } s_reg[REG_MAX];
static int s_reg_n;

void spu_register_function(uint32_t addr, void (*fn)(spu_context*))
{
    if (s_reg_n < REG_MAX) { s_reg[s_reg_n].addr = addr & SPU_LS_MASK; s_reg[s_reg_n].fn = fn; s_reg_n++; }
}
void spu_begin_image(int image_id) { (void)image_id; }

static spu_fn lookup_fn(uint32_t addr)
{
    addr &= SPU_LS_MASK;
    for (int i = 0; i < s_reg_n; i++)
        if (s_reg[i].addr == addr) return s_reg[i].fn;
    return NULL;
}

void spu_indirect_branch(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    /* SPURS kernel services below the 0xA00 policy-module base. The PM's job
     * API branches here; the runtime's resolver HLEs them. 0x9D0 = poll/select
     * workload ("no contention, keep running"), 0x9C0 = exit to kernel. */
    if (pc == 0x9D0u) {
        ctx->gpr[3] = spu_splat_u32(0);
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        return;
    }
    if (pc == 0x9C0u) {
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        return;
    }
    spu_fn fn = lookup_fn(pc);
    if (!fn) {
        fprintf(stderr, "[dxt-replay] FATAL: no lifted code at LS 0x%05X\n", pc);
        exit(3);
    }
    fn(ctx);
}

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch && (ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) {
                g_spu_trampoline_fn = NULL;
                return;
            }
            g_spu_trampoline_fn = NULL;
            tf(ctx);
        }
        if ((ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) return;
        spu_fn fn = lookup_fn(ctx->pc);
        if (fn) { g_spu_trampoline_fn = spu_indirect_branch; continue; }
        fprintf(stderr, "[dxt-replay] FATAL: drain lost code at 0x%05X (ret 0x%05X)\n",
                ctx->pc & SPU_LS_MASK, return_pc & SPU_LS_MASK);
        exit(3);
    }
}

void spu_img_restore(spu_context* ctx, int32_t saved_img) { ctx->image_id = saved_img; }
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_fncap_capture(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }

/* ---- corpus model -------------------------------------------------------- */
#define REC_HDR 24u
typedef struct { uint64_t ea; uint32_t lsa, size, actual; uint8_t* data; int pre; } xfer;

typedef struct {
    xfer* gets; int ngets;
    xfer* puts; int nputs;
    uint8_t* ls; uint8_t* gpr;
    int have_entry;
    int lane, seq;
} run_t;

static run_t g_run;
static int g_verbose, g_stop_on_fail = 1;
static uint32_t g_sentinel = 0x20000;
static int g_run_gets, g_run_puts, g_run_missing_get, g_run_bad_put, g_run_extra_put;
static long g_total_puts, g_total_bad, g_total_missing;
static uint64_t g_ns_sum;
static long g_block_sum;

static void run_reset(void)
{
    for (int i = 0; i < g_run.ngets; i++) free(g_run.gets[i].data);
    for (int i = 0; i < g_run.nputs; i++) free(g_run.puts[i].data);
    free(g_run.gets); free(g_run.puts); free(g_run.ls); free(g_run.gpr);
    memset(&g_run, 0, sizeof g_run);
}

static void add_xfer(xfer** arr, int* n, uint64_t ea, uint32_t lsa, uint32_t size,
                     uint32_t actual, const uint8_t* data, int pre)
{
    *arr = realloc(*arr, (size_t)(*n + 1) * sizeof **arr);
    xfer* x = &(*arr)[(*n)++];
    x->ea = ea; x->lsa = lsa; x->size = size; x->actual = actual; x->pre = pre;
    x->data = malloc(size ? size : 1);
    if (size) memcpy(x->data, data, size);
}

static xfer* find_get(uint64_t ea, uint32_t size)
{
    for (int i = 0; i < g_run.ngets; i++)
        if (g_run.gets[i].ea == ea && g_run.gets[i].size >= size)
            return &g_run.gets[i];
    return NULL;
}

static void record_put(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size)
{
    int idx = g_run_puts++;
    int expect = -1, seen = 0;
    for (int i = 0; i < g_run.nputs; i++) {
        if (g_run.puts[i].pre) continue;
        if (seen == idx) { expect = i; break; }
        seen++;
    }
    if (expect < 0) {
        g_run_extra_put++;
        fprintf(stderr, "[dxt-replay]   extra PUT lsa=0x%05X ea=0x%08llX size=%u\n",
                lsa, (unsigned long long)ea, size);
        return;
    }
    xfer* e = &g_run.puts[expect];
    if (e->size != size || e->ea != ea || e->lsa != lsa) {
        g_run_bad_put++;
        fprintf(stderr, "[dxt-replay]   PUT HEADER MISMATCH run=%d put=%d: got lsa=0x%05X ea=0x%08llX size=%u, "
                        "want lsa=0x%05X ea=0x%08llX size=%u\n",
                g_run.seq, idx, lsa, (unsigned long long)ea, size,
                e->lsa, (unsigned long long)e->ea, e->size);
        return;
    }
    if (memcmp(ctx->ls + e->lsa, e->data, e->size) != 0) {
        g_run_bad_put++;
        fprintf(stderr, "[dxt-replay]   PUT PAYLOAD MISMATCH run=%d put=%d lsa=0x%05X size=%u\n",
                g_run.seq, idx, e->lsa, e->size);
    }
}

/* ---- channel model ------------------------------------------------------- */

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    uint32_t v = value._u32[0];
    switch (channel) {
    case MFC_LSA:  ctx->mfc_lsa = v; break;
    case MFC_EAH:  ctx->mfc_eah = v; break;
    case MFC_EAL:  ctx->mfc_eal = v; break;
    case MFC_Size: ctx->mfc_size = v; break;
    case MFC_TagID:ctx->mfc_tag = v; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = v; break;
    case MFC_WrTagUpdate:
        ctx->mfc_tag_status = 0;
        break;
    case MFC_Cmd: {
        uint32_t cmd = v, lsa = ctx->mfc_lsa & SPU_LS_MASK;
        uint64_t ea = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
        uint32_t size = ctx->mfc_size;
        if (cmd == MFC_GET_CMD || cmd == MFC_GETB_CMD || cmd == MFC_GETF_CMD) {
            g_run_gets++;
            xfer* g = find_get(ea, size);
            if (!g) {
                /* The PM's own job-list/control reads (LS below the job's data
                 * windows) are not part of the DXT kernel's input; a missing
                 * one is zero-filled like the real lock-line read would be.
                 * Input tiles are the GETs into 0x7000+; those must match. */
                if (lsa >= 0x7000u) {
                    g_run_missing_get++;
                    fprintf(stderr, "[dxt-replay]   MISSING GET run=%d ea=0x%08llX size=0x%X lsa=0x%05X\n",
                            g_run.seq, (unsigned long long)ea, size, lsa);
                }
                memset(ctx->ls + lsa, 0, size);
            } else {
                memcpy(ctx->ls + lsa, g->data, size);
            }
        } else if (cmd == MFC_PUT_CMD || cmd == MFC_PUTB_CMD || cmd == MFC_PUTF_CMD) {
            record_put(ctx, lsa, ea, size);
        } else if (cmd == MFC_GETLLAR_CMD) {
            /* Lock-line read of the SPURS job queue / buffer tag. The corpus
             * does not spool atomics (they bypass the DMA spool); fill from a
             * captured GET at the same EA when one exists, else zero. */
            xfer* g = find_get(ea, 128);
            if (g) memcpy(ctx->ls + lsa, g->data, 128);
            else memset(ctx->ls + lsa, 0, 128);
            ctx->mfc_tag_status = 0;
        } else if (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD ||
                   cmd == MFC_PUTQLLUC_CMD) {
            /* Store-conditional/unconditional: accept; the result is compared
             * through the job's ordinary PUTs, not the lock line. */
            ctx->mfc_tag_status = 0;
        } else if (cmd == MFC_SNDSIG_CMD) {
            /* no LS operand */
        } else if (cmd == MFC_BARRIER_CMD || cmd == MFC_EIEIO_CMD || cmd == MFC_SYNC_CMD ||
                   cmd == MFC_SDCRZ_CMD) {
            /* no LS operand */
        } else {
            fprintf(stderr, "[dxt-replay] FATAL: unsupported MFC cmd 0x%02X (lsa=0x%05X ea=0x%08llX size=0x%X)\n",
                    cmd, lsa, (unsigned long long)ea, size);
            exit(4);
        }
        break;
    }
    case SPU_WrOutMbox:
    case SPU_WrOutIntrMbox:
        if (g_verbose) fprintf(stderr, "[dxt-replay]   wrch mbox 0x%08X\n", v);
        break;
    default:
        if (g_verbose) fprintf(stderr, "[dxt-replay]   wrch ch=%u v=0x%08X\n", channel, v);
        break;
    }
}

u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = 0;
    switch (channel) {
    case MFC_RdTagStat: r._u32[0] = 0; break;
    case MFC_RdTagMask: r._u32[0] = ctx->mfc_tag_mask; break;
    case MFC_RdAtomicStat: r._u32[0] = 0; break;
    case SPU_RdDec: r._u32[0] = 0; break;
    case SPU_RdInMbox: r._u32[0] = 0; break;
    case SPU_RdEventStat: r._u32[0] = 0; break;
    case SPU_RdMachStat: r._u32[0] = 1; break;
    default: break;
    }
    return r;
}

uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) { (void)ctx; (void)channel; return 0; }

/* ---- spool parser -------------------------------------------------------- */
/* A simpler two-pass design: read the whole file, then walk it run by run. */
typedef struct { uint32_t type, sz; uint64_t ea; uint32_t lsa, aux; uint8_t* data; } rec_t;
static rec_t* load_spool(const char* path, int* nrec, int* lane)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t hdr[28];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "DXTCP01", 7) != 0) {
        fprintf(stderr, "%s: bad header\n", path); fclose(f); return NULL;
    }
    *lane = *(uint32_t*)(hdr + 12);
    size_t cap = 1024, n = 0;
    rec_t* recs = malloc(cap * sizeof *recs);
    for (;;) {
        uint32_t h[6];
        if (fread(h, 1, REC_HDR, f) != REC_HDR) break;
        uint32_t type = h[0], sz = h[1];
        uint8_t* data = malloc(sz ? sz : 1);
        if (sz && fread(data, 1, sz, f) != sz) { free(data); break; }
        if (n == cap) { cap *= 2; recs = realloc(recs, cap * sizeof *recs); }
        recs[n].type = type; recs[n].sz = sz;
        recs[n].ea = ((uint64_t)h[3] << 32) | h[2];
        recs[n].lsa = h[4]; recs[n].aux = h[5];
        recs[n].data = data;
        n++;
    }
    fclose(f);
    *nrec = (int)n;
    return recs;
}

static int replay_run(int lane, int seq, rec_t* r, int n, uint64_t* ns_out, long* blocks_out)
{
    int entry_ls = -1, entry_gpr = -1;
    for (int i = 0; i < n; i++) {
        if (r[i].type == 7 && entry_ls < 0) entry_ls = i;
        if (r[i].type == 8 && entry_gpr < 0) entry_gpr = i;
    }
    if (entry_ls < 0 || entry_gpr < 0) return 0;

    static spu_context ctx;
    spu_context_init(&ctx, 0);
    memcpy(ctx.ls, r[entry_ls].data, r[entry_ls].sz < SPU_LS_SIZE ? r[entry_ls].sz : SPU_LS_SIZE);
    memcpy(ctx.gpr, r[entry_gpr].data, sizeof ctx.gpr);
    ctx.pc = 0x4030;
    ctx.image_id = 207;
    ctx.gpr[0]._u32[0] = g_sentinel;

    /* collect the run's GETs/PUTs, and apply the PM's outstanding GETs */
    g_run.ngets = g_run.nputs = 0;
    g_run.gets = NULL; g_run.puts = NULL;
    for (int i = 0; i < n; i++) {
        if (r[i].type == 2)
            add_xfer(&g_run.gets, &g_run.ngets, r[i].ea, r[i].lsa, r[i].sz, r[i].aux,
                     r[i].data, i < entry_ls);
        else if (r[i].type == 3)
            add_xfer(&g_run.puts, &g_run.nputs, r[i].ea, r[i].lsa, r[i].sz, r[i].aux,
                     r[i].data, i < entry_ls);
    }
    for (int i = 0; i < g_run.ngets; i++)
        if (g_run.gets[i].pre) {
            uint32_t lsa = g_run.gets[i].lsa & SPU_LS_MASK;
            uint32_t sz = g_run.gets[i].size;
            if (sz > g_run.gets[i].actual) sz = g_run.gets[i].actual;
            memcpy(ctx.ls + lsa, g_run.gets[i].data, sz);
        }

    g_run_gets = g_run_puts = g_run_missing_get = g_run_bad_put = g_run_extra_put = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    spu_fn fn = lookup_fn(0x4030);
    if (!fn) { fprintf(stderr, "[dxt-replay] no lifted entry 0x4030\n"); return -1; }
    fn(&ctx);
    if (g_spu_trampoline_fn) spu_drain_call(&ctx, g_sentinel);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *ns_out = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (t1.tv_nsec - t0.tv_nsec);

    int expect_puts = 0;
    long blocks = 0;
    for (int i = 0; i < g_run.nputs; i++) {
        if (g_run.puts[i].pre) continue;
        expect_puts++;
        blocks += g_run.puts[i].size / 8;
    }
    *blocks_out = blocks;
    if (expect_puts == 0) {
        /* A capture stopped before the job's PUTs (torn tail / SIGKILL).
         * There is no expected output to gate; do not count it as a failure. */
        for (int i = 0; i < g_run.ngets; i++) free(g_run.gets[i].data);
        for (int i = 0; i < g_run.nputs; i++) free(g_run.puts[i].data);
        free(g_run.gets); free(g_run.puts);
        memset(&g_run, 0, sizeof g_run);
        return 0;
    }
    int ok = (ctx.pc & SPU_LS_MASK) == (g_sentinel & SPU_LS_MASK) || g_run_puts == expect_puts;
    /* The dispatch may end on the sentinel through the drain; anything else is
     * a stop/halt. A missing lift also shows as a bad PC. */
    if ((ctx.pc & SPU_LS_MASK) != (g_sentinel & SPU_LS_MASK)) {
        fprintf(stderr, "[dxt-replay] run %d lane %d end pc=0x%05X status=0x%X\n",
                seq, lane, ctx.pc & SPU_LS_MASK, ctx.status);
        ok = 0;
    }
    if (g_run_missing_get || g_run_bad_put || g_run_extra_put ||
        g_run_puts != expect_puts) {
        fprintf(stderr, "[dxt-replay] run %d lane %d: gets=%d(missing input %d) puts=%d/%d bad=%d extra=%d\n",
                seq, lane, g_run_gets, g_run_missing_get, g_run_puts, expect_puts,
                g_run_bad_put, g_run_extra_put);
        ok = 0;
    }
    if (g_verbose) {
        fprintf(stderr, "[dxt-replay] run %d lane %d: gets=%d puts=%d(%d blocks) %.3f ms pc=0x%05X\n",
                seq, lane, g_run_gets, g_run_puts, (int)blocks, (double)*ns_out / 1e6,
                ctx.pc & SPU_LS_MASK);
    }
    g_total_puts += g_run_puts;
    g_total_bad += g_run_bad_put;
    g_total_missing += g_run_missing_get;
    g_ns_sum += *ns_out;
    g_block_sum += blocks;

    for (int i = 0; i < g_run.ngets; i++) free(g_run.gets[i].data);
    for (int i = 0; i < g_run.nputs; i++) free(g_run.puts[i].data);
    free(g_run.gets); free(g_run.puts);
    memset(&g_run, 0, sizeof g_run);
    return ok ? 1 : -1;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "dxt_capA_20261103";
    int max_runs = argc > 2 ? atoi(argv[2]) : 100000;
    if (getenv("DXT_VERBOSE")) g_verbose = atoi(getenv("DXT_VERBOSE"));
    if (getenv("DXT_CONTINUE")) g_stop_on_fail = 0;

    jobmod_647a2bf49000_e0030_spu_recomp_register();
    wwsjob_pm_spu_recomp_register();
    fprintf(stderr, "[dxt-replay] %d lifted entries (DXT module + WWS policy module)\n", s_reg_n);

    DIR* d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    struct dirent* de;
    int total = 0, ok = 0, fail = 0, skipped = 0;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "dxt_lane", 8) != 0) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        int nrec = 0, lane = 0;
        rec_t* recs = load_spool(path, &nrec, &lane);
        if (!recs) continue;
        int run_start = -1;
        for (int i = 0; i < nrec; i++) {
            if (recs[i].type == 1) run_start = i;
            if (recs[i].type == 4 && run_start >= 0) {
                int run_end = i;
                /* Every distinct job-body entry inside this policy run is a
                 * replay unit; the PUTs after it and before the next entry
                 * are its expected output. */
                int entry_ls[64], entry_gpr[64], nentry = 0;
                for (int j = run_start; j <= run_end && nentry < 64; j++) {
                    if (recs[j].type == 7) {
                        entry_ls[nentry] = j;
                        entry_gpr[nentry] = -1;
                        for (int k = j + 1; k <= run_end && k < j + 3; k++)
                            if (recs[k].type == 8) { entry_gpr[nentry] = k; break; }
                        if (entry_gpr[nentry] >= 0) nentry++;
                        else entry_ls[nentry] = -1;
                    }
                }
                for (int e = 0; e < nentry; e++) {
                    if (entry_ls[e] < 0) continue;
                    int seg_start = (e == 0) ? run_start : entry_gpr[e - 1] + 1;
                    int seg_end = (e + 1 < nentry) ? entry_ls[e + 1] : run_end + 1;
                    if (total >= max_runs) { skipped++; continue; }
                    uint64_t ns = 0; long blocks = 0;
                    int rr = replay_run(lane, total, &recs[seg_start], seg_end - seg_start, &ns, &blocks);
                    if (rr > 0) ok++; else if (rr < 0) fail++; else skipped++;
                    total++;
                }
                run_start = -1;
            }
        }
        for (int i = 0; i < nrec; i++) free(recs[i].data);
        free(recs);
    }
    closedir(d);

    printf("=================================================================\n");
    printf("DXT job lift gate: %d runs (%d pass, %d fail, %d skipped)\n", total, ok, fail, skipped);
    printf("PUTs compared: %ld, mismatched: %ld, missing GETs: %ld\n",
           g_total_puts, g_total_bad, g_total_missing);
    printf("lifted kernel: %.3f s total, %ld blocks, %.1f ns/block\n",
           (double)g_ns_sum / 1e9, g_block_sum,
           g_block_sum ? (double)g_ns_sum / (double)g_block_sum : 0.0);
    return fail ? 1 : 0;
}
