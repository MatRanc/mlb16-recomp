/*
 * Native AVX2/FMA kernel for lifted SPU function 0x6D68
 * (jobmod_c690c2f7b09a_eF5B8_spu_func_00006D68).
 *
 * Semantics mirror spu_helpers.h exactly as compiled by this harness:
 *   -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS
 * i.e. SPU_FAST_FLOAT + __FMA__ single-precision FMA with the SPU
 * zero-exponent -> +0 flush on every operand and result, SSE4.1 shufb,
 * word-lane variable rotate, and the lifted big-endian LS accessors.
 *
 * Uses AVX2 variable shifts (vpsllvd/vpsrlvd) for the SPU word rotates, VEX
 * encoded SSE for everything else, and fused FMA (vfmadd132ps) for the eight
 * a*scale+offset lanes. Independent groups sharing A/B are written so the
 * compiler can CSE the shufb control decode.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__clang__)
#pragma clang attribute push (__attribute__((target("avx2,fma"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC target("avx2,fma")
#endif

#include "spu_context.h"
#include <immintrin.h>
#include <stdint.h>

#define BSWAP_MASK _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)

static inline __attribute__((always_inline))
__m128i native_ls_read(const uint8_t* ls, uint32_t addr, __m128i bswap)
{
    __m128i v = _mm_loadu_si128((const __m128i*)&ls[addr & 0x3FFF0]);
    return _mm_shuffle_epi8(v, bswap);
}

static inline __attribute__((always_inline))
void native_ls_write(uint8_t* ls, uint32_t addr, __m128i v, __m128i bswap)
{
    _mm_storeu_si128((__m128i*)&ls[addr & 0x3FFF0], _mm_shuffle_epi8(v, bswap));
}

static inline __attribute__((always_inline))
__m128i native_shufb(__m128i A, __m128i B, __m128i C)
{
    const __m128i idx = _mm_xor_si128(_mm_and_si128(C, _mm_set1_epi8((char)0x8F)), _mm_set1_epi8(3));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), _mm_slli_epi16(C, 3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
    r = _mm_or_si128(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F))));
    return r;
}

/* spu_ff_ftz: a lane whose biased exponent is zero becomes +0. */
static inline __attribute__((always_inline))
__m128i native_ftz128(__m128i v)
{
    const __m128i e = _mm_and_si128(v, _mm_set1_epi32(0x7F800000));
    return _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), v);
}

static inline __attribute__((always_inline))
__m128i native_fma128(__m128i a, __m128i b_ftz, __m128i c_ftz)
{
    __m128 r = _mm_fmadd_ps(_mm_castsi128_ps(native_ftz128(a)),
                            _mm_castsi128_ps(b_ftz), _mm_castsi128_ps(c_ftz));
    return native_ftz128(_mm_castps_si128(r));
}

/* lane-local word rotate: (x << n) | (x >> (32-n)), n = control & 31 */
static inline __attribute__((always_inline))
__m128i native_rot(__m128i x, __m128i n, __m128i rn)
{
    return _mm_or_si128(_mm_sllv_epi32(x, n), _mm_srlv_epi32(x, rn));
}

#define ROTPAIR(var, src) \
    const __m128i var##_n  = _mm_and_si128((src), _mm_set1_epi32(31)); \
    const __m128i var##_rn = _mm_sub_epi32(_mm_set1_epi32(32), var##_n)

void native_spu_func_00006D68(spu_context* ctx)
{
    const __m128i bswap = BSWAP_MASK;
    uint8_t* ls = ctx->ls;

    __m128i s2  = _mm_loadu_si128((const __m128i*)&ctx->gpr[2]);
    __m128i s3  = _mm_loadu_si128((const __m128i*)&ctx->gpr[3]);
    __m128i s4  = _mm_loadu_si128((const __m128i*)&ctx->gpr[4]);
    __m128i s6  = _mm_loadu_si128((const __m128i*)&ctx->gpr[6]);
    __m128i s7  = _mm_loadu_si128((const __m128i*)&ctx->gpr[7]);
    __m128i s8  = _mm_loadu_si128((const __m128i*)&ctx->gpr[8]);
    __m128i s9  = _mm_loadu_si128((const __m128i*)&ctx->gpr[9]);
    __m128i s10 = _mm_loadu_si128((const __m128i*)&ctx->gpr[10]);
    __m128i s11 = _mm_loadu_si128((const __m128i*)&ctx->gpr[11]);
    __m128i s12 = _mm_loadu_si128((const __m128i*)&ctx->gpr[12]);
    __m128i s13 = _mm_loadu_si128((const __m128i*)&ctx->gpr[13]);
    __m128i s14 = _mm_loadu_si128((const __m128i*)&ctx->gpr[14]);
    __m128i s15 = _mm_loadu_si128((const __m128i*)&ctx->gpr[15]);
    __m128i s16 = _mm_loadu_si128((const __m128i*)&ctx->gpr[16]);
    __m128i s17 = _mm_loadu_si128((const __m128i*)&ctx->gpr[17]);
    __m128i s18 = _mm_loadu_si128((const __m128i*)&ctx->gpr[18]);
    __m128i s19 = _mm_loadu_si128((const __m128i*)&ctx->gpr[19]);
    __m128i s20 = _mm_loadu_si128((const __m128i*)&ctx->gpr[20]);
    __m128i s21 = _mm_loadu_si128((const __m128i*)&ctx->gpr[21]);
    __m128i s22 = _mm_loadu_si128((const __m128i*)&ctx->gpr[22]);
    __m128i s23 = _mm_loadu_si128((const __m128i*)&ctx->gpr[23]);
    __m128i s24 = _mm_loadu_si128((const __m128i*)&ctx->gpr[24]);
    __m128i s25 = _mm_loadu_si128((const __m128i*)&ctx->gpr[25]);
    __m128i s26 = _mm_loadu_si128((const __m128i*)&ctx->gpr[26]);
    __m128i s27 = _mm_loadu_si128((const __m128i*)&ctx->gpr[27]);
    __m128i s28 = _mm_loadu_si128((const __m128i*)&ctx->gpr[28]);
    __m128i s29 = _mm_loadu_si128((const __m128i*)&ctx->gpr[29]);
    __m128i s30 = _mm_loadu_si128((const __m128i*)&ctx->gpr[30]);
    __m128i s31 = _mm_loadu_si128((const __m128i*)&ctx->gpr[31]);
    __m128i s32 = _mm_loadu_si128((const __m128i*)&ctx->gpr[32]);
    __m128i s33 = _mm_loadu_si128((const __m128i*)&ctx->gpr[33]);
    __m128i s34 = _mm_loadu_si128((const __m128i*)&ctx->gpr[34]);
    __m128i s35 = _mm_loadu_si128((const __m128i*)&ctx->gpr[35]);
    __m128i s36 = _mm_loadu_si128((const __m128i*)&ctx->gpr[36]);
    __m128i s37 = _mm_loadu_si128((const __m128i*)&ctx->gpr[37]);
    __m128i s38 = _mm_loadu_si128((const __m128i*)&ctx->gpr[38]);
    __m128i s39 = _mm_loadu_si128((const __m128i*)&ctx->gpr[39]);
    __m128i s40 = _mm_loadu_si128((const __m128i*)&ctx->gpr[40]);
    __m128i s41 = _mm_loadu_si128((const __m128i*)&ctx->gpr[41]);
    __m128i s42 = _mm_loadu_si128((const __m128i*)&ctx->gpr[42]);
    __m128i s43 = _mm_loadu_si128((const __m128i*)&ctx->gpr[43]);
    __m128i s44 = _mm_loadu_si128((const __m128i*)&ctx->gpr[44]);
    __m128i s45 = _mm_loadu_si128((const __m128i*)&ctx->gpr[45]);
    __m128i s47 = _mm_loadu_si128((const __m128i*)&ctx->gpr[47]);
    __m128i s48 = _mm_loadu_si128((const __m128i*)&ctx->gpr[48]);
    __m128i s49 = _mm_loadu_si128((const __m128i*)&ctx->gpr[49]);
    __m128i s50 = _mm_loadu_si128((const __m128i*)&ctx->gpr[50]);
    __m128i s51 = _mm_loadu_si128((const __m128i*)&ctx->gpr[51]);
    __m128i s52 = _mm_loadu_si128((const __m128i*)&ctx->gpr[52]);
    __m128i s53 = _mm_loadu_si128((const __m128i*)&ctx->gpr[53]);
    __m128i s54 = _mm_loadu_si128((const __m128i*)&ctx->gpr[54]);
    __m128i s55 = _mm_loadu_si128((const __m128i*)&ctx->gpr[55]);
    __m128i s56 = _mm_loadu_si128((const __m128i*)&ctx->gpr[56]);
    __m128i s57 = _mm_loadu_si128((const __m128i*)&ctx->gpr[57]);
    __m128i s58 = _mm_loadu_si128((const __m128i*)&ctx->gpr[58]);

    const __m128i k15 = _mm_set1_epi32(15);
    const __m128i k128 = _mm_set1_epi32(128);
    const __m128i mask17 = s17;
    const __m128i selb_b  = _mm_and_si128(mask17, s2);

    ROTPAIR(c6,  s6);
    ROTPAIR(c8,  s8);
    ROTPAIR(c15, s15);
    ROTPAIR(c18, s18);
    ROTPAIR(c19, s19);
    ROTPAIR(c20, s20);
    ROTPAIR(c24, s24);
    ROTPAIR(c28, s28);

    const __m128i fb = native_ftz128(s11);
    const __m128i fc = native_ftz128(s12);



    /* Write-base words are loop-invariant. */
    const uint32_t b39 = (uint32_t)_mm_cvtsi128_si32(s39);
    const uint32_t b37 = (uint32_t)_mm_cvtsi128_si32(s37);
    const uint32_t b36 = (uint32_t)_mm_cvtsi128_si32(s36);
    const uint32_t b4  = (uint32_t)_mm_cvtsi128_si32(s4);
    const uint32_t b27 = (uint32_t)_mm_cvtsi128_si32(s27);
    const uint32_t b31 = (uint32_t)_mm_cvtsi128_si32(s31);
    const uint32_t b35 = (uint32_t)_mm_cvtsi128_si32(s35);
    const uint32_t b25 = (uint32_t)_mm_cvtsi128_si32(s25);

loc_00006D68: ;
    /* sr46 = andi(sr3,15) */
    __m128i x46 = _mm_and_si128(s3, k15);
    /* sr40 = shufb(sr40, sr29, sr14) */
    s40 = native_shufb(s40, s29, s14);
    /* sr47 = andi(sr30,15) */
    __m128i x47 = _mm_and_si128(s30, k15);

    /* sr48 = shufb(sr41,sr44,sr7); sr49 = shufb(...,sr34);
       sr54 = shufb(...,sr22); sr41 = shufb(...,sr23)
       -- all four share the old sr41/sr44. */
    {
        __m128i A = s41, B = s44;
        s48 = native_shufb(A, B, s7);
        s49 = native_shufb(A, B, s34);
        s54 = native_shufb(A, B, s22);
        s41 = native_shufb(A, B, s23);
    }

    /* sr45 = selb(sr45,sr2,sr17) */
    s45 = _mm_or_si128(_mm_andnot_si128(mask17, s45), selb_b);
    /* sr42 = selb(sr42,sr2,sr17) */
    s42 = _mm_or_si128(_mm_andnot_si128(mask17, s42), selb_b);
    /* sr46 = shufb(sr46,sr46,sr26) */
    x46 = native_shufb(x46, x46, s26);
    /* sr38 = selb(sr38,sr2,sr17) */
    s38 = _mm_or_si128(_mm_andnot_si128(mask17, s38), selb_b);

    /* sr50 = lqd(sr3+0) */
    s50 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s3) + 0x0, bswap);
    /* sr48 = rot(sr48,sr19) */
    s48 = native_rot(s48, c19_n, c19_rn);
    /* sr51 = lqd(sr30+0x10) */
    s51 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s30) + 0x10, bswap);
    /* sr49 = rot(sr49,sr15) */
    s49 = native_rot(s49, c15_n, c15_rn);
    /* sr52 = lqd(sr3+0x10) */
    s52 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s3) + 0x10, bswap);
    /* sr46 = a(sr46,sr16) */
    x46 = _mm_add_epi32(x46, s16);
    /* sr53 = lqd(sr3+0x20) */
    s53 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s3) + 0x20, bswap);
    /* sr43 = selb(sr43,sr2,sr17) */
    s43 = _mm_or_si128(_mm_andnot_si128(mask17, s43), selb_b);
    /* sr38 = a(sr38,sr10) */
    s38 = _mm_add_epi32(s38, s10);
    /* sr55 = lqd(sr30+0) */
    s55 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s30) + 0x0, bswap);
    /* sr49 = selb(sr49,sr2,sr17) */
    s49 = _mm_or_si128(_mm_andnot_si128(mask17, s49), selb_b);
    /* sr43 = a(sr43,sr10) */
    s43 = _mm_add_epi32(s43, s10);
    /* sr44 = shufb(sr47,sr47,sr26) */
    __m128i x44 = native_shufb(x47, x47, s26);
    /* sr47 = rot(sr54,sr6) */
    s47 = native_rot(s54, c6_n, c6_rn);
    /* sr50 = shufb(sr50,sr52,sr46) */
    s50 = native_shufb(s50, s52, x46);
    /* sr42 = a(sr42,sr10) */
    s42 = _mm_add_epi32(s42, s10);
    /* sr46 = shufb(sr52,sr53,sr46) */
    x46 = native_shufb(s52, s53, x46);
    /* sr41 = rot(sr41,sr20) */
    s41 = native_rot(s41, c20_n, c20_rn);
    /* sr52 = lqd(sr30+0x20) */
    s52 = native_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s30) + 0x20, bswap);
    /* sr45 = a(sr45,sr10) */
    s45 = _mm_add_epi32(s45, s10);
    /* sr47 = selb(sr47,sr2,sr17) */
    s47 = _mm_or_si128(_mm_andnot_si128(mask17, s47), selb_b);
    /* sr53 = a(sr44,sr16) */
    s53 = _mm_add_epi32(x44, s16);

    /* sr54 = shufb(sr50,sr46,sr32); sr56 = shufb(...,sr9);
       sr57 = shufb(...,sr33); sr46 = shufb(...,sr21) */
    {
        __m128i A = s50, B = x46;
        s54 = native_shufb(A, B, s32);
        s56 = native_shufb(A, B, s9);
        s57 = native_shufb(A, B, s33);
        x46 = native_shufb(A, B, s21);
    }

    /* sr41 = selb(sr41,sr2,sr17) */
    s41 = _mm_or_si128(_mm_andnot_si128(mask17, s41), selb_b);
    /* sr47 = a(sr10,sr47) */
    s47 = _mm_add_epi32(s10, s47);
    /* sr58 = a(sr41,sr10) */
    s58 = _mm_add_epi32(s41, s10);
    /* sr44 = shufb(sr51,sr52,sr53) */
    s44 = native_shufb(s51, s52, s53);
    /* sr48 = selb(sr48,sr2,sr17) */
    s48 = _mm_or_si128(_mm_andnot_si128(mask17, s48), selb_b);
    /* sr49 = a(sr49,sr10) */
    s49 = _mm_add_epi32(s49, s10);
    /* sr41 = shufb(sr55,sr51,sr53) */
    s41 = native_shufb(s55, s51, s53);
    /* sr48 = a(sr48,sr10) */
    s48 = _mm_add_epi32(s48, s10);

    /* fma + store interleaved to keep the live range of each result to one. */
    const uint32_t a40 = (uint32_t)_mm_cvtsi128_si32(s40);

    const __m128i t47 = native_fma128(s47, fb, fc);
    native_ls_write(ls, b39 + a40, t47, bswap);
    const __m128i t49 = native_fma128(s49, fb, fc);
    native_ls_write(ls, b37 + a40, t49, bswap);
    const __m128i t58 = native_fma128(s58, fb, fc);
    native_ls_write(ls, b36 + a40, t58, bswap);
    const __m128i t45 = native_fma128(s45, fb, fc);
    native_ls_write(ls, b4  + a40, t45, bswap);
    const __m128i t48 = native_fma128(s48, fb, fc);
    native_ls_write(ls, b27 + a40, t48, bswap);
    const __m128i t43 = native_fma128(s43, fb, fc);
    native_ls_write(ls, b31 + a40, t43, bswap);
    const __m128i t42 = native_fma128(s42, fb, fc);
    native_ls_write(ls, b35 + a40, t42, bswap);
    const __m128i t38 = native_fma128(s38, fb, fc);
    native_ls_write(ls, b25 + a40, t38, bswap);

    s49 = t49; s50 = t58; s48 = t48; s51 = t43; s52 = t42; s47 = t38;

    /* sr3 = a(sr13,sr3) */
    s3 = _mm_add_epi32(s13, s3);
    /* sr42 = rot(sr57,sr8) */
    s42 = native_rot(s57, c8_n, c8_rn);
    /* sr43 = rot(sr56,sr24) */
    s43 = native_rot(s56, c24_n, c24_rn);
    /* sr45 = rot(sr54,sr18) */
    s45 = native_rot(s54, c18_n, c18_rn);
    /* sr29 = ai(sr29,128) */
    s29 = _mm_add_epi32(s29, k128);
    /* sr38 = rot(sr46,sr28) */
    s38 = native_rot(x46, c28_n, c28_rn);

    /* sr30 = a(sr30,sr13) */
    s30 = _mm_add_epi32(s30, s13);

    if (_mm_cvtsi128_si32(s40) != 0)
        goto loc_00006D68;

    _mm_storeu_si128((__m128i*)&ctx->gpr[3],  s3);
    _mm_storeu_si128((__m128i*)&ctx->gpr[29], s29);
    _mm_storeu_si128((__m128i*)&ctx->gpr[30], s30);
    _mm_storeu_si128((__m128i*)&ctx->gpr[38], s38);
    _mm_storeu_si128((__m128i*)&ctx->gpr[40], s40);
    _mm_storeu_si128((__m128i*)&ctx->gpr[41], s41);
    _mm_storeu_si128((__m128i*)&ctx->gpr[42], s42);
    _mm_storeu_si128((__m128i*)&ctx->gpr[43], s43);
    _mm_storeu_si128((__m128i*)&ctx->gpr[44], s44);
    _mm_storeu_si128((__m128i*)&ctx->gpr[45], s45);
    _mm_storeu_si128((__m128i*)&ctx->gpr[46], x46);
    _mm_storeu_si128((__m128i*)&ctx->gpr[47], s47);
    _mm_storeu_si128((__m128i*)&ctx->gpr[48], s48);
    _mm_storeu_si128((__m128i*)&ctx->gpr[49], s49);
    _mm_storeu_si128((__m128i*)&ctx->gpr[50], s50);
    _mm_storeu_si128((__m128i*)&ctx->gpr[51], s51);
    _mm_storeu_si128((__m128i*)&ctx->gpr[52], s52);
    _mm_storeu_si128((__m128i*)&ctx->gpr[53], s53);
    _mm_storeu_si128((__m128i*)&ctx->gpr[54], s54);
    _mm_storeu_si128((__m128i*)&ctx->gpr[55], s55);
    _mm_storeu_si128((__m128i*)&ctx->gpr[56], s56);
    _mm_storeu_si128((__m128i*)&ctx->gpr[57], s57);
    _mm_storeu_si128((__m128i*)&ctx->gpr[58], s58);

    ctx->pc = ctx->gpr[0]._u32[0];
    if (ctx->host_depth == 0)
        g_spu_trampoline_fn = spu_indirect_branch;
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
