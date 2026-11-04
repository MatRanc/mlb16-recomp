/*
 * Offline replay of a PS3RECOMP_C690_TAIL_CAPTURE spool.
 *
 * For every IN/OUT pair in the spool it runs the captured command handler on
 * the IN local store + GPRs and compares the resulting LS with the captured
 * OUT. This is the corpus gate for the GPU tail: a pair that does not replay
 * byte-exactly is not usable as kernel input/output.
 *
 * Build:
 *   clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I<ps3recomp-play>/runtime/spu -I<ps3recomp-play>/include \
 *       -o /tmp/opencode/test_c690_tail_replay \
 *       test_c690_tail_replay.c reg_10f78.c <spu_lifted>/spu_recomp.c \
 *       <ps3recomp-play>/runtime/spu/c690_corpus.c \
 *       <ps3recomp-play>/runtime/spu/c690_cull_fix.c \
 *       <ps3recomp-play>/runtime/spu/job_c690_native.c -lm
 *
 * Run: /tmp/opencode/test_c690_tail_replay <spool-dir> [--verbose]
 *
 * The channel model logs every MFC/mailbox access. GET payloads are zero-filled
 * because guest memory is not in the spool; this is reported per class.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <dirent.h>

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


/* ---- tail spool ---------------------------------------------------------- */
#define T_IN 9
#define T_OUT 10
#define META_WORDS 24
#define BITMAP_BYTES 2048
#define GPR_BYTES (128u * 16u)
#define HDR_SIZE 24

typedef struct {
    uint32_t type, size, lsa, aux;
    uint64_t ea;
    uint32_t meta[META_WORDS];
    uint8_t bitmap[BITMAP_BYTES];
    uint8_t* ls;
    uint8_t* gpr;
} tail_rec;

static int read_rec(FILE* f, tail_rec* r)
{
    uint8_t hdr[24];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) return 0;
    r->type = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    r->size = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    r->ea = 0;
    for (int i = 0; i < 8; i++) r->ea |= (uint64_t)hdr[8 + i] << (8 * i);
    r->lsa = (uint32_t)hdr[16] | (uint32_t)hdr[17] << 8 | (uint32_t)hdr[18] << 16 | (uint32_t)hdr[19] << 24;
    r->aux = (uint32_t)hdr[20] | (uint32_t)hdr[21] << 8 | (uint32_t)hdr[22] << 16 | (uint32_t)hdr[23] << 24;
    if (r->size != META_WORDS * 4 + BITMAP_BYTES + SPU_LS_SIZE + GPR_BYTES) {
        fprintf(stderr, "bad record size %u type %u\n", r->size, r->type);
        return 0;
    }
    uint8_t* payload = malloc(r->size);
    if (!payload) { perror("malloc"); exit(1); }
    if (fread(payload, 1, r->size, f) != r->size) { free(payload); return 0; }
    memcpy(r->meta, payload, META_WORDS * 4);
    memcpy(r->bitmap, payload + META_WORDS * 4, BITMAP_BYTES);
    r->ls = malloc(SPU_LS_SIZE);
    r->gpr = malloc(GPR_BYTES);
    if (!r->ls || !r->gpr) { perror("malloc"); exit(1); }
    memcpy(r->ls, payload + META_WORDS * 4 + BITMAP_BYTES, SPU_LS_SIZE);
    memcpy(r->gpr, payload + META_WORDS * 4 + BITMAP_BYTES + SPU_LS_SIZE, GPR_BYTES);
    free(payload);
    return 1;
}

static uint32_t handler_for_op(uint32_t op)
{
    switch (op) {
    case 0x21: return 0x13080;
    case 0x25: return 0x13520;
    case 0x26: return 0x13518;
    case 0x2A: return 0x10F78;
    default:   return 0;
    }
}

static const char* class_name(uint32_t key, uint32_t op)
{
    static char buf[64];
    if (op == 0x21) {
        const uint32_t sel = key & 7u;
        const char* path = (key & 0x10u) ? (sel == 1 ? "BB80" : "BB98")
                                         : (sel == 1 ? "A730" : "A3E0");
        snprintf(buf, sizeof buf, "0x21 sel%u %s", sel, path);
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%02X", op);
    return buf;
}

typedef struct {
    uint32_t key;
    unsigned pairs, exact, dma;
    unsigned long long diff_bytes, ls_bytes, diff_in_bmp, diff_out_bmp;
} class_stat;

uint8_t* vm_base = NULL;

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap2b2";
    int verbose = getenv("FF_VERBOSE") != NULL;
    const char* e;
    if ((e = getenv("FF_ZERO_GET"))) g_zero_get = atoi(e);
    if ((e = getenv("FF_TRACE_CH"))) g_trace_ch = atoi(e);
    if ((e = getenv("FF_MAX_DISPATCH"))) g_max_dispatch = atol(e);
    if ((e = getenv("FF_CH_BUDGET"))) g_ch_budget = atol(e);

    DIR* dp = opendir(dir);
    if (!dp) { perror(dir); return 1; }
    class_stat stats[64]; int nstats = 0;
    unsigned total = 0, exact = 0;
    struct dirent* de;
    static spu_context ctx;
    tail_rec in;
    memset(&in, 0, sizeof in);

    while ((de = readdir(dp))) {
        const char* name = de->d_name;
        if (strncmp(name, "tail_", 5) || !strstr(name, ".bin")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        FILE* f = fopen(path, "rb");
        if (!f) { perror(path); continue; }
        char magic[8];
        uint32_t ver;
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "C69STG01", 8) ||
            fread(&ver, 4, 1, f) != 1) {
            fprintf(stderr, "%s: bad header\n", path);
            fclose(f);
            continue;
        }
        tail_rec r;
        memset(&r, 0, sizeof r);
        while (read_rec(f, &r)) {
            if (r.type == T_IN) {
                if (in.ls) { free(in.ls); free(in.gpr); }
                in = r;
                memset(&r, 0, sizeof r);
                continue;
            }
            if (r.type != T_OUT) {
                if (r.ls) { free(r.ls); free(r.gpr); }
                memset(&r, 0, sizeof r);
                continue;
            }
            if (!in.ls || in.meta[7] != r.meta[7]) {
                if (r.ls) { free(r.ls); free(r.gpr); }
                memset(&r, 0, sizeof r);
                continue;
            }
            const uint32_t op = in.meta[4];
            const uint32_t entry = handler_for_op(op);
            const uint32_t key = in.meta[5];
            if (!entry) {
                fprintf(stderr, "pair %u: no handler for op 0x%X\n", in.meta[7], op);
                goto next;
            }
            spu_context_init(&ctx, 0);
            ctx.image_id = 6;
            ctx.spu_id = in.aux;
            ctx.pc = entry;
            /* Match the live runtime: a running context has a nonzero step
             * count, so SPU_DIRECT takes its musttail path instead of leaving
             * a trampoline pending at the top-level call. */
            ctx.steps = 1;
            static int drain21 = -1;
            if (drain21 < 0) drain21 = getenv("FF_DRAIN_21") ? 1 : 0;
            if (op == 0x21 && !drain21) ctx.steps = 0;   /* stop at the prefix */
            g_spu_trampoline_fn = NULL;
            memcpy(ctx.ls, in.ls, SPU_LS_SIZE);
            memcpy(ctx.gpr, in.gpr, GPR_BYTES);
            spu_fn fn = lookup_fn(entry);
            if (!fn) {
                fprintf(stderr, "pair %u: no lifted function at 0x%05X\n", in.meta[7], entry);
                goto next;
            }
            uint32_t ret_pc;
            memcpy(&ret_pc, in.gpr, 4);
            ret_pc &= SPU_LS_MASK;
            fn(&ctx);
            /* The command resolver can leave the sub-selector on the
             * trampoline path (dynamic `bi $r2`); drain it exactly as the
             * live SPU drain does, up to the captured caller link. */
            if (op == 0x21) {
                /* A command-boundary 0x21 pair needs the sub-selector drain
                 * (the handler leaves it on the trampoline). A sub-selector
                 * entry pair was captured when that body returned, so its
                 * OUT is the prefix only and must not be drained. */
                if (drain21 && (g_spu_trampoline_fn ||
                                (ctx.pc & SPU_LS_MASK) != ret_pc))
                    spu_drain_call(&ctx, ret_pc);
            } else if (g_spu_trampoline_fn ||
                       (ctx.pc & SPU_LS_MASK) != ret_pc) {
                spu_drain_call(&ctx, ret_pc);
            }

            unsigned long long diff = 0, first = (unsigned long long)-1;
            unsigned long long diff_in = 0, diff_out = 0;
            for (unsigned i = 0; i < SPU_LS_SIZE; i++) {
                if (ctx.ls[i] != r.ls[i]) {
                    if (first == (unsigned long long)-1) first = i;
                    diff++;
                    const unsigned line = i >> 4;
                    if (in.bitmap[line >> 3] & (1u << (line & 7))) diff_in++;
                    else diff_out++;
                }
            }
            {
                static const char* dump = NULL;
                if (!dump) dump = getenv("FF_DUMP_SEQ");
                if (dump && strtoul(dump, NULL, 0) == in.meta[7]) {
                    const char* out = getenv("FF_DUMP_OUT");
                    if (out) {
                        FILE* df = fopen(out, "wb");
                        if (df) { fwrite(ctx.ls, 1, SPU_LS_SIZE, df); fclose(df); }
                    }
                    fprintf(stderr, "DUMP pair %u op=0x%X first_diff=0x%llX in_bmp=%llu out_bmp=%llu\n",
                            in.meta[7], op, first, diff_in, diff_out);
                    for (unsigned i = 0; i < SPU_LS_SIZE; i++) {
                        if (ctx.ls[i] != r.ls[i]) {
                            fprintf(stderr, "  diff @0x%05X replay=%02X out=%02X in=%02X%s\n",
                                    i, ctx.ls[i], r.ls[i], in.ls[i],
                                    (in.bitmap[(i >> 4) >> 3] & (1u << ((i >> 4) & 7))) ? " (bmp)" : "");
                            if (i > first + 64) break;
                        }
                    }
                }
            }
            class_stat* st = NULL;
            for (int i = 0; i < nstats; i++)
                if (stats[i].key == key) { st = &stats[i]; break; }
            if (!st) {
                st = &stats[nstats++];
                memset(st, 0, sizeof *st);
                st->key = key;
            }
            st->pairs++;
            st->diff_bytes += diff;
            st->diff_in_bmp += diff_in;
            st->diff_out_bmp += diff_out;
            st->ls_bytes += SPU_LS_SIZE;
            if (g_dma_gets || g_dma_puts) st->dma++;
            total++;
            if (!diff) { st->exact++; exact++; }
            else if (verbose)
                fprintf(stderr, "pair %u op=0x%X %s: %llu differing LS bytes, first 0x%llX\n",
                        in.meta[7], op, class_name(key, op), diff, first);
        next:
            if (r.ls) { free(r.ls); free(r.gpr); }
            if (in.ls) { free(in.ls); free(in.gpr); }
            memset(&in, 0, sizeof in);
            memset(&r, 0, sizeof r);
            g_dma_gets = g_dma_puts = g_dma_other = 0;
        }
        fclose(f);
    }
    closedir(dp);

    printf("%-22s %6s %6s %10s %10s %10s\n", "class", "pairs", "exact", "diff_bytes", "in_bmp", "out_bmp");
    for (int i = 0; i < nstats; i++)
        printf("%-22s %6u %6u %10llu %10llu %10llu\n",
               class_name(stats[i].key, stats[i].key > 0xFF ? 0x21 : stats[i].key),
               stats[i].pairs, stats[i].exact, stats[i].diff_bytes,
               stats[i].diff_in_bmp, stats[i].diff_out_bmp);
    printf("TOTAL pairs=%u exact=%u\n", total, exact);
    return exact == total ? 0 : 1;
}
