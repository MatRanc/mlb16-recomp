/*
 * Bit-exact CPU reference for the pitch (0x2A / stream-2) vertex pipeline,
 * built from the captured SPU state.  Offline only: no game launch.
 *
 *   Stage A  position unpack  (SPU 0x6D68)      -- VERIFIED bit-exact
 *   Stage B  3x4 affine transform (SPU 0xA798)  -- semantics from capture
 *   Stage C  normalize (SPU 0xAF98)             -- SPU frsqest + FI
 *   Stage D  snorm16 pack (SPU 0x7C18 branch)   -- cflts 157 semantics
 *
 * Build:
 *   gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *       -I<ps3recomp-native>/include -I<ps3recomp-native>/runtime/spu \
 *       -o /tmp/opencode/pitch_ref pitch_pipeline_ref.c -lm
 *
 * Run:
 *   /tmp/opencode/pitch_ref <capture-dir-with-ls+gpr> [out.bin]
 *
 * The default capture is /tmp/opencode/fncap_full/0x06D68.  Stage A is checked
 * byte-for-byte against the lifted 0x6D68 body on the same state.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "spu_context.h"
#include "spu_helpers.h"
#include "pitch_pipeline_constants.h"

/* Minimal stubs for the lifted leaf. */
volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
#include "ref_spu_func_6d68.c"

#define GPR_BYTES (128u * 16u)

static void load_exact(const char* path, void* dst, size_t want)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fread(dst, 1, want, f) != want) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
}

static uint64_t fnv1a(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static uint32_t be32(const uint8_t* ls, uint32_t b)
{
    return ((uint32_t)ls[b] << 24) | ((uint32_t)ls[b+1] << 16) |
           ((uint32_t)ls[b+2] << 8) | (uint32_t)ls[b+3];
}

static uint32_t read_bits(const uint8_t* ls, uint32_t bitpos, uint32_t n)
{
    uint32_t sh = bitpos & 7u, b = bitpos >> 3u;
    uint32_t w0 = be32(ls, b), w1 = be32(ls, b + 4u);
    uint32_t win = (w0 << sh) | (sh ? (w1 >> (32u - sh)) : 0u);
    return win >> (32u - n);
}

static uint32_t ftz(uint32_t v) { return ((v & 0x7F800000u) == 0u) ? 0u : v; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ---------------- Stage A: 12/12/11 position unpack -------------------- */
void pitch_stage_a_positions(const uint8_t* in_ls, uint8_t* out_ls,
                             uint32_t in_base, uint32_t out_base,
                             uint32_t nverts, const uint32_t A[4],
                             const uint32_t B[4], const uint32_t C[4])
{
    for (uint32_t v = 0; v < nverts; v++) {
        uint32_t bp = in_base * 8u + v * PITCH_POS_STRIDE_BITS;
        uint32_t F[4] = {
            read_bits(in_ls, bp, 12),
            read_bits(in_ls, bp + 12, 12),
            read_bits(in_ls, bp + 24, 11),
            0u
        };
        uint32_t o = out_base + v * 16u;
        for (int lane = 0; lane < 4; lane++) {
            uint32_t abits = (0x3F800000u + B[lane] + (F[lane] << 2)) & 0xFFFFFFFFu;
            uint32_t a = ftz(abits);
            uint32_t ca = ftz(A[lane]);
            uint32_t cc = ftz(C[lane]);
            float r = fmaf(u2f(a), u2f(ca), u2f(cc));
            uint32_t rb; memcpy(&rb, &r, 4);
            rb = ftz(rb);
            /* local store is big-endian per 32-bit lane */
            uint32_t be = __builtin_bswap32(rb);
            memcpy(&out_ls[o + 4*lane], &be, 4);
        }
    }
}

/* ---------------- Stage B: 3x4 affine transform ------------------------ */
/* m is row-major 3x4: rows xyz|translation.  SPU fma with fast-float ftz. */
void pitch_stage_b_transform(const float* v, const float m[12], float out[3])
{
    for (int r = 0; r < 3; r++) {
        float acc = m[r*4+3];
        acc = fmaf(v[0], m[r*4+0], acc);
        acc = fmaf(v[1], m[r*4+1], acc);
        acc = fmaf(v[2], m[r*4+2], acc);
        out[r] = acc;
    }
}

/* ---------------- Stage C: SPU normalize ------------------------------ */
/* Uses the same SPU frsqest model + FI step as the lift (spu_helpers.h). */
void pitch_stage_c_normalize(const float* v, float out[4])
{
    /* len2 = x*x + y*y + z*z, accumulated with SPU fma lane semantics. */
    float x = v[0], y = v[1], z = v[2];
    float dot = fmaf(x, x, 0.0f);
    dot = fmaf(y, y, dot);
    dot = fmaf(z, z, dot);
    u128 dl; memset(&dl, 0, sizeof dl); dl._f32[0] = dot;
    /* SPU reciprocal-sqrt estimate + FI refinement (not GLSL inversesqrt). */
    u128 rl = spu_fi(dl, spu_frsqest(dl));
    float r = rl._f32[0];
    out[0] = x * r; out[1] = y * r; out[2] = z * r; out[3] = 0.0f;
}

/* ---------------- Stage D: snorm16 pack ------------------------------- */
/* q = trunc((v*scale + offset) * 2^16); store the high 16 bits as int16. */
int16_t pitch_stage_d_pack_snorm16(float v, float scale, float offset)
{
    float f = fmaf(v, scale, offset);
    int32_t q = (int32_t)(f * 65536.0f);   /* cflts 157: 2^(173-157) */
    int32_t s = q >> 16;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap_full/0x06D68";
    const char* outpath = argc > 2 ? argv[2] : NULL;
    char lsp[600], gprp[600];
    snprintf(lsp, sizeof lsp, "%s/ls.bin", dir);
    snprintf(gprp, sizeof gprp, "%s/gpr.bin", dir);

    uint8_t* in_ls = malloc(SPU_LS_SIZE);
    uint8_t* ref_ls = malloc(SPU_LS_SIZE);
    uint8_t* lut_ls = malloc(SPU_LS_SIZE);
    uint8_t* gpr0 = malloc(GPR_BYTES);
    if (!in_ls || !ref_ls || !lut_ls || !gpr0) { perror("malloc"); return 1; }
    load_exact(lsp, in_ls, SPU_LS_SIZE);
    load_exact(gprp, gpr0, GPR_BYTES);

    /* Golden: lifted 0x6D68 on the captured state. */
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.pc = 0x06D68; ctx.image_id = 6;
    memcpy(ctx.ls, in_ls, SPU_LS_SIZE);
    memcpy(ctx.gpr, gpr0, GPR_BYTES);
    jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68(&ctx);

    memcpy(ref_ls, in_ls, SPU_LS_SIZE);
    memcpy(lut_ls, in_ls, SPU_LS_SIZE);
    pitch_stage_a_positions(in_ls, ref_ls,
                            PITCH_POS_IN_BASE, PITCH_POS_OUT_BASE, PITCH_POS_VERTS,
                            pitch_pos_A, pitch_pos_B, pitch_pos_C);

    size_t diffs = 0;
    for (uint32_t i = 0; i < PITCH_POS_OUT_BYTES; i++)
        if (ref_ls[PITCH_POS_OUT_BASE + i] != ctx.ls[PITCH_POS_OUT_BASE + i]) diffs++;

    uint64_t h_ref = fnv1a(0xcbf29ce484222325ull, ref_ls + PITCH_POS_OUT_BASE, PITCH_POS_OUT_BYTES);
    uint64_t h_lift = fnv1a(0xcbf29ce484222325ull, ctx.ls + PITCH_POS_OUT_BASE, PITCH_POS_OUT_BYTES);

    printf("=== pitch pipeline CPU reference ===\n");
    printf("capture            : %s\n", dir);
    printf("stage A in/out     : LS 0x%05X -> 0x%05X, %u verts, %u bits, %u/%u/%u\n",
           PITCH_POS_IN_BASE, PITCH_POS_OUT_BASE, PITCH_POS_VERTS,
           PITCH_POS_STRIDE_BITS, PITCH_POS_BITS_X, PITCH_POS_BITS_Y, PITCH_POS_BITS_Z);
    printf("stage A FNV ref    : 0x%016llx\n", (unsigned long long)h_ref);
    printf("stage A FNV lifted : 0x%016llx\n", (unsigned long long)h_lift);
    printf("stage A diff bytes : %zu   -> %s\n", diffs,
           diffs == 0 && h_ref == h_lift ? "BIT-EXACT" : "MISMATCH");

    /* Demonstrate B/C/D on the first vertex, using the captured matrix. */
    const uint8_t* base = in_ls;
    uint32_t bp = PITCH_POS_IN_BASE * 8u;
    uint32_t F[3] = { read_bits(base, bp, 12), read_bits(base, bp+12, 12),
                      read_bits(base, bp+24, 11) };
    printf("\nfirst vertex packed fields: x=%u y=%u z=%u\n", F[0], F[1], F[2]);
    float pos[4];
    for (int i = 0; i < 4; i++) {
        uint32_t w = be32(ref_ls, PITCH_POS_OUT_BASE + 4*i);
        pos[i] = u2f(w);
    }
    printf("  stage A pos   : %.6f %.6f %.6f %.6f\n", pos[0], pos[1], pos[2], pos[3]);
    float tp[3];
    pitch_stage_b_transform(pos, pitch_transform_matrix, tp);
    printf("  stage B xform : %.6f %.6f %.6f   (matrix LS 0x%05X)\n",
           tp[0], tp[1], tp[2], PITCH_TRANSFORM_MATRIX_LS);
    float nrm[4];
    pitch_stage_c_normalize(tp, nrm);
    printf("  stage C norm  : %.6f %.6f %.6f %.6f\n", nrm[0], nrm[1], nrm[2], nrm[3]);
    int16_t px = pitch_stage_d_pack_snorm16(nrm[0], 32767.0f, 0.0f);
    int16_t py = pitch_stage_d_pack_snorm16(nrm[1], 32767.0f, 0.0f);
    int16_t pz = pitch_stage_d_pack_snorm16(nrm[2], 32767.0f, 0.0f);
    printf("  stage D snorm16: %d %d %d   (cflts %d)\n", px, py, pz, PITCH_PACK_SNORM16_I8);

    if (outpath) {
        FILE* f = fopen(outpath, "wb");
        if (!f) { perror(outpath); return 1; }
        fwrite(ref_ls, 1, SPU_LS_SIZE, f);
        fclose(f);
        printf("\nwrote reference LS: %s\n", outpath);
    }
    printf("\nRESULT: %s\n", (diffs == 0 && h_ref == h_lift) ? "PASS (stage A bit-exact)" : "FAIL");
    return 0;
}
