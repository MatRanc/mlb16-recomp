/*
 * Native AVX2/FMA kernel for the lifted SPU replay unit 0xAF98 -> 0xB238
 * (jobmod_c690c2f7b09a_eF5B8_spu_func_0000AF98 tail-calling 0xB238).
 *
 * This is the live path of the two lifted bodies fused into one function:
 * 0xAF98's body, then 0xB238's entry block. 0xB238's loc_* copies are dead
 * (nothing branches to them) and are omitted; 0xB238 ends by publishing
 * ctx->pc = gpr[73] with an indirect-branch trampoline, which this kernel
 * reproduces.
 *
 * Semantics mirror spu_helpers.h as compiled with
 *   -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS
 * i.e. SPU_FAST_FLOAT (__FMA__) single-precision with the SPU zero-exponent
 * flush on every operand and result, SSE4.1 shufb, big-endian LS accessors,
 * and the exact AVX2 frsqest/fi estimate-and-interpolate helpers.
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
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdint.h>

#define BSWAP_MASK _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)

static inline __attribute__((always_inline))
__m128i nd_ls_read(const uint8_t* ls, uint32_t addr)
{
    const __m128i sw = BSWAP_MASK;
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)&ls[addr & 0x3FFF0u]), sw);
}

static inline __attribute__((always_inline))
void nd_ls_write(uint8_t* ls, uint32_t addr, __m128i v)
{
    const __m128i sw = BSWAP_MASK;
    _mm_storeu_si128((__m128i*)&ls[addr & 0x3FFF0u], _mm_shuffle_epi8(v, sw));
}

static inline __attribute__((always_inline))
__m128i nd_ftz(__m128i v)
{
    const __m128i e = _mm_and_si128(v, _mm_set1_epi32(0x7F800000));
    return _mm_andnot_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), v);
}

static inline __attribute__((always_inline))
__m128i nd_fma(__m128i a, __m128i b, __m128i c)
{
    __m128 r = _mm_fmadd_ps(_mm_castsi128_ps(nd_ftz(a)),
                            _mm_castsi128_ps(nd_ftz(b)),
                            _mm_castsi128_ps(nd_ftz(c)));
    return nd_ftz(_mm_castps_si128(r));
}

static inline __attribute__((always_inline))
__m128i nd_fm(__m128i a, __m128i b)
{
    __m128 r = _mm_mul_ps(_mm_castsi128_ps(nd_ftz(a)), _mm_castsi128_ps(nd_ftz(b)));
    return nd_ftz(_mm_castps_si128(r));
}

static inline __attribute__((always_inline))
__m128i nd_fms(__m128i a, __m128i b, __m128i c)
{
    __m128 r = _mm_fmsub_ps(_mm_castsi128_ps(nd_ftz(a)),
                            _mm_castsi128_ps(nd_ftz(b)),
                            _mm_castsi128_ps(nd_ftz(c)));
    return nd_ftz(_mm_castps_si128(r));
}

static inline __attribute__((always_inline))
__m128i nd_fnms(__m128i a, __m128i b, __m128i c)
{
    __m128 r = _mm_fnmadd_ps(_mm_castsi128_ps(nd_ftz(a)),
                             _mm_castsi128_ps(nd_ftz(b)),
                             _mm_castsi128_ps(nd_ftz(c)));
    return nd_ftz(_mm_castps_si128(r));
}

static inline __attribute__((always_inline))
__m128i nd_selb(__m128i a, __m128i b, __m128i c)
{
    return _mm_or_si128(_mm_andnot_si128(c, a), _mm_and_si128(c, b));
}

static inline __attribute__((always_inline))
__m128i nd_shufb(__m128i A, __m128i B, __m128i C)
{
    const __m128i idx = _mm_xor_si128(_mm_and_si128(C, _mm_set1_epi8((char)0x8F)), _mm_set1_epi8(3));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), _mm_slli_epi16(C, 3));
    const __m128i lut = _mm_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      (char)0xFF, (char)0xFF, (char)0x80, (char)0x80);
    r = _mm_or_si128(r, _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(C, 4), _mm_set1_epi8(0x0F))));
    return r;
}

/* spu_rotqby on x86: byte permute with the same host-lane mapping. */
static inline __attribute__((always_inline))
__m128i nd_rotqby(__m128i a, __m128i sh)
{
    const __m128i iota = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    const __m128i ir = _mm_and_si128(_mm_add_epi8(iota, _mm_set1_epi8((char)_mm_cvtsi128_si32(sh))),
                                     _mm_set1_epi8(15));
    const __m128i oob = _mm_cmpeq_epi8(_mm_max_epu8(ir, _mm_set1_epi8(16)), ir);
    return _mm_shuffle_epi8(a, _mm_or_si128(_mm_xor_si128(ir, _mm_set1_epi8(3)), oob));
}

static inline __attribute__((always_inline))
__m128i nd_frsqest(__m128i x)
{
    u128 u; memcpy(&u, &x, 16);
    u128 r = spu_frsqest(u);
    __m128i v; memcpy(&v, &r, 16); return v;
}

static inline __attribute__((always_inline))
__m128i nd_fi(__m128i a, __m128i b)
{
    u128 ua, ub; memcpy(&ua, &a, 16); memcpy(&ub, &b, 16);
    u128 r = spu_fi(ua, ub);
    __m128i v; memcpy(&v, &r, 16); return v;
}

void native_spu_func_0000AF98(spu_context* ctx)
{
    const __m128i bswap = BSWAP_MASK;
    (void)bswap;
    uint8_t* ls = ctx->ls;
    __m128i s0 = _mm_loadu_si128((const __m128i*)&ctx->gpr[0]);
    __m128i s2 = _mm_loadu_si128((const __m128i*)&ctx->gpr[2]);
    __m128i s3 = _mm_loadu_si128((const __m128i*)&ctx->gpr[3]);
    __m128i s5 = _mm_loadu_si128((const __m128i*)&ctx->gpr[5]);
    __m128i s6 = _mm_loadu_si128((const __m128i*)&ctx->gpr[6]);
    __m128i s7 = _mm_loadu_si128((const __m128i*)&ctx->gpr[7]);
    __m128i s8 = _mm_loadu_si128((const __m128i*)&ctx->gpr[8]);
    __m128i s9 = _mm_loadu_si128((const __m128i*)&ctx->gpr[9]);
    __m128i s10 = _mm_loadu_si128((const __m128i*)&ctx->gpr[10]);
    __m128i s11 = _mm_loadu_si128((const __m128i*)&ctx->gpr[11]);
    __m128i s12 = _mm_loadu_si128((const __m128i*)&ctx->gpr[12]);
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
    __m128i s34 = _mm_loadu_si128((const __m128i*)&ctx->gpr[34]);
    __m128i s35 = _mm_loadu_si128((const __m128i*)&ctx->gpr[35]);
    __m128i s37 = _mm_loadu_si128((const __m128i*)&ctx->gpr[37]);
    __m128i s38 = _mm_loadu_si128((const __m128i*)&ctx->gpr[38]);
    __m128i s42 = _mm_loadu_si128((const __m128i*)&ctx->gpr[42]);
    __m128i s43 = _mm_loadu_si128((const __m128i*)&ctx->gpr[43]);
    __m128i s44 = _mm_loadu_si128((const __m128i*)&ctx->gpr[44]);
    __m128i s45 = _mm_loadu_si128((const __m128i*)&ctx->gpr[45]);
    __m128i s46 = _mm_loadu_si128((const __m128i*)&ctx->gpr[46]);
    __m128i s50 = _mm_loadu_si128((const __m128i*)&ctx->gpr[50]);
    __m128i s51 = _mm_loadu_si128((const __m128i*)&ctx->gpr[51]);
    __m128i s52 = _mm_loadu_si128((const __m128i*)&ctx->gpr[52]);
    __m128i s54 = _mm_loadu_si128((const __m128i*)&ctx->gpr[54]);
    __m128i s55 = _mm_loadu_si128((const __m128i*)&ctx->gpr[55]);
    __m128i s56 = _mm_loadu_si128((const __m128i*)&ctx->gpr[56]);
    __m128i s57 = _mm_loadu_si128((const __m128i*)&ctx->gpr[57]);
    __m128i s58 = _mm_loadu_si128((const __m128i*)&ctx->gpr[58]);
    __m128i s59 = _mm_loadu_si128((const __m128i*)&ctx->gpr[59]);
    __m128i s60 = _mm_loadu_si128((const __m128i*)&ctx->gpr[60]);
    __m128i s61 = _mm_loadu_si128((const __m128i*)&ctx->gpr[61]);
    __m128i s62 = _mm_loadu_si128((const __m128i*)&ctx->gpr[62]);
    __m128i s63 = _mm_loadu_si128((const __m128i*)&ctx->gpr[63]);
    __m128i s64 = _mm_loadu_si128((const __m128i*)&ctx->gpr[64]);
    __m128i s65 = _mm_loadu_si128((const __m128i*)&ctx->gpr[65]);
    __m128i s66 = _mm_loadu_si128((const __m128i*)&ctx->gpr[66]);
    __m128i s67 = _mm_loadu_si128((const __m128i*)&ctx->gpr[67]);
    __m128i s68 = _mm_loadu_si128((const __m128i*)&ctx->gpr[68]);
    __m128i s69 = _mm_loadu_si128((const __m128i*)&ctx->gpr[69]);
    __m128i s70 = _mm_loadu_si128((const __m128i*)&ctx->gpr[70]);
    __m128i s71 = _mm_loadu_si128((const __m128i*)&ctx->gpr[71]);
    __m128i s72 = _mm_loadu_si128((const __m128i*)&ctx->gpr[72]);
    __m128i s73 = _mm_loadu_si128((const __m128i*)&ctx->gpr[73]);
    __m128i s74 = _mm_loadu_si128((const __m128i*)&ctx->gpr[74]);
    __m128i s75 = _mm_loadu_si128((const __m128i*)&ctx->gpr[75]);
    __m128i s76 = _mm_loadu_si128((const __m128i*)&ctx->gpr[76]);
    __m128i s77 = _mm_loadu_si128((const __m128i*)&ctx->gpr[77]);
    __m128i s78 = _mm_loadu_si128((const __m128i*)&ctx->gpr[78]);
    __m128i s79 = _mm_loadu_si128((const __m128i*)&ctx->gpr[79]);
    __m128i s80 = _mm_loadu_si128((const __m128i*)&ctx->gpr[80]);
    __m128i s81 = _mm_loadu_si128((const __m128i*)&ctx->gpr[81]);
    __m128i s82 = _mm_loadu_si128((const __m128i*)&ctx->gpr[82]);
    __m128i s83 = _mm_loadu_si128((const __m128i*)&ctx->gpr[83]);
    __m128i s84 = _mm_loadu_si128((const __m128i*)&ctx->gpr[84]);
    __m128i s85 = _mm_loadu_si128((const __m128i*)&ctx->gpr[85]);
    __m128i s86 = _mm_loadu_si128((const __m128i*)&ctx->gpr[86]);
    __m128i s87 = _mm_loadu_si128((const __m128i*)&ctx->gpr[87]);
    __m128i s88 = _mm_loadu_si128((const __m128i*)&ctx->gpr[88]);
    __m128i s89 = _mm_loadu_si128((const __m128i*)&ctx->gpr[89]);
    __m128i s90 = _mm_loadu_si128((const __m128i*)&ctx->gpr[90]);
    __m128i s91 = _mm_loadu_si128((const __m128i*)&ctx->gpr[91]);
    __m128i s92 = _mm_loadu_si128((const __m128i*)&ctx->gpr[92]);
    __m128i s93 = _mm_loadu_si128((const __m128i*)&ctx->gpr[93]);
    __m128i s94 = _mm_loadu_si128((const __m128i*)&ctx->gpr[94]);
    __m128i s95 = _mm_loadu_si128((const __m128i*)&ctx->gpr[95]);
    __m128i s96 = _mm_loadu_si128((const __m128i*)&ctx->gpr[96]);
    __m128i s97 = _mm_loadu_si128((const __m128i*)&ctx->gpr[97]);
    __m128i s98 = _mm_loadu_si128((const __m128i*)&ctx->gpr[98]);
    __m128i s99 = _mm_loadu_si128((const __m128i*)&ctx->gpr[99]);
    __m128i s100 = _mm_loadu_si128((const __m128i*)&ctx->gpr[100]);
    __m128i s101 = _mm_loadu_si128((const __m128i*)&ctx->gpr[101]);
    __m128i s102 = _mm_loadu_si128((const __m128i*)&ctx->gpr[102]);
    __m128i s103 = _mm_loadu_si128((const __m128i*)&ctx->gpr[103]);
    __m128i s104 = _mm_loadu_si128((const __m128i*)&ctx->gpr[104]);
    __m128i s105 = _mm_loadu_si128((const __m128i*)&ctx->gpr[105]);
    __m128i s106 = _mm_loadu_si128((const __m128i*)&ctx->gpr[106]);
    __m128i s107 = _mm_loadu_si128((const __m128i*)&ctx->gpr[107]);
    __m128i s108 = _mm_loadu_si128((const __m128i*)&ctx->gpr[108]);
    __m128i s109 = _mm_loadu_si128((const __m128i*)&ctx->gpr[109]);
    __m128i s110 = _mm_loadu_si128((const __m128i*)&ctx->gpr[110]);
    __m128i s111 = _mm_loadu_si128((const __m128i*)&ctx->gpr[111]);
    __m128i s112 = _mm_loadu_si128((const __m128i*)&ctx->gpr[112]);
    __m128i s113 = _mm_loadu_si128((const __m128i*)&ctx->gpr[113]);
    __m128i s115 = _mm_loadu_si128((const __m128i*)&ctx->gpr[115]);
    __m128i s116 = _mm_loadu_si128((const __m128i*)&ctx->gpr[116]);
    __m128i s117 = _mm_loadu_si128((const __m128i*)&ctx->gpr[117]);
    __m128i s118 = _mm_loadu_si128((const __m128i*)&ctx->gpr[118]);
    __m128i s119 = _mm_loadu_si128((const __m128i*)&ctx->gpr[119]);
    __m128i s120 = _mm_loadu_si128((const __m128i*)&ctx->gpr[120]);
    __m128i s122 = _mm_loadu_si128((const __m128i*)&ctx->gpr[122]);
    __m128i s123 = _mm_loadu_si128((const __m128i*)&ctx->gpr[123]);
    __m128i s124 = _mm_loadu_si128((const __m128i*)&ctx->gpr[124]);
    __m128i s125 = _mm_loadu_si128((const __m128i*)&ctx->gpr[125]);
    __m128i s127 = _mm_loadu_si128((const __m128i*)&ctx->gpr[127]);

    s81   = nd_fma(s64, s64, s103);
    s91   = nd_shufb(s85, s86, s117);
    s92   = nd_fma(s108, s108, s67);
    s97   = nd_shufb(s93, s94, s117);
    s11   = _mm_add_epi32(s57, _mm_set1_epi32(0));
    s82   = nd_shufb(s83, s84, s113);
    s65   = _mm_add_epi32(s45, _mm_set1_epi32(0));
    s93   = nd_shufb(s93, s94, s113);
    s66   = _mm_add_epi32(s44, _mm_set1_epi32(0));
    s94   = nd_shufb(s95, s96, s117);
    s100  = _mm_add_epi32(s18, _mm_set1_epi32(64));
    s98   = nd_shufb(s95, s96, s113);
    s67   = _mm_add_epi32(s7, _mm_set1_epi32(1));
    s96   = nd_frsqest(s81);
    s95   = _mm_add_epi32(s55, _mm_set1_epi32(0));
    s80   = nd_shufb(s82, s93, s117);
    s68   = _mm_add_epi32(s43, _mm_set1_epi32(0));
    s110  = nd_shufb(s59, s61, s125);
    s102  = _mm_add_epi32(s58, _mm_set1_epi32(0));
    s99   = nd_frsqest(s92);
    s12   = nd_fi(s81, s96);
    s81   = nd_shufb(s82, s93, s113);
    s24   = _mm_add_epi32(s50, s24);
    s82   = nd_shufb(s91, s94, s113);
    s93   = nd_selb(s60, s110, s52);
    s106  = nd_shufb(s87, s88, s113);
    s96   = _mm_add_epi32(s56, _mm_set1_epi32(0));
    s42   = nd_shufb(s85, s86, s113);
    s92   = nd_fi(s92, s99);
    s103  = nd_shufb(s87, s88, s117);
    s85   = nd_shufb(s89, s90, s113);
    s99   = nd_fm(s82, s80);
    s88   = nd_shufb(s83, s84, s117);
    s69   = nd_fm(s63, s12);
    s83   = nd_shufb(s42, s98, s117);
    s55   = nd_fm(s82, s81);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s22) + (uint32_t)_mm_cvtsi128_si32(s24), s93);
    s105  = nd_fm(s64, s12);
    s84   = nd_shufb(s106, s85, s113);
    s70   = nd_fm(s62, s12);
    s85   = nd_shufb(s106, s85, s117);
    s108  = nd_fm(s108, s92);
    s86   = nd_shufb(s88, s97, s113);
    s106  = nd_fm(s107, s92);
    s87   = nd_shufb(s42, s98, s113);
    s93   = nd_fm(s84, s83);
    s45   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s30) + (uint32_t)_mm_cvtsi128_si32(s18));
    s98   = nd_fm(s83, s81);
    s44   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s26) + (uint32_t)_mm_cvtsi128_si32(s18));
    s57   = nd_fm(s85, s82);
    s88   = nd_shufb(s88, s97, s117);
    s97   = nd_fm(s109, s92);
    s42   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s9) + (uint32_t)_mm_cvtsi128_si32(s18));
    s109  = nd_fm(s84, s80);
    s12   = nd_shufb(s89, s90, s117);
    s62   = nd_fm(s84, s82);
    s63   = nd_shufb(s106, s108, s125);
    s89   = nd_fnms(s87, s85, s93);
    s90   = nd_shufb(s91, s94, s117);
    s91   = nd_fms(s80, s87, s98);
    s43   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s23) + (uint32_t)_mm_cvtsi128_si32(s18));
    s92   = nd_fms(s86, s83, s99);
    s93   = nd_shufb(s103, s12, s113);
    s98   = nd_fm(s86, s85);
    s107  = nd_shufb(s97, s108, s37);
    s94   = nd_fnms(s87, s86, s55);
    s99   = nd_shufb(s44, s45, s123);
    s55   = nd_selb(s97, s63, s52);
    s58   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s27) + (uint32_t)_mm_cvtsi128_si32(s18));
    s64   = nd_fm(s86, s84);
    s108  = nd_shufb(s44, s45, s35);
    s71   = nd_selb(s106, s107, s51);
    s72   = nd_shufb(s42, s43, s123);
    s96   = nd_selb(s55, s96, s54);
    s56   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s29) + (uint32_t)_mm_cvtsi128_si32(s18));
    s73   = nd_selb(s71, s95, s54);
    s74   = nd_shufb(s63, s97, s16);
    s95   = nd_fms(s93, s87, s62);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s29) + (uint32_t)_mm_cvtsi128_si32(s24), s96);
    s96   = nd_fnms(s93, s83, s57);
    s55   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s10) + (uint32_t)_mm_cvtsi128_si32(s18));
    s97   = nd_fnms(s93, s81, s64);
    s71   = nd_shufb(s116, s104, s35);
    s57   = nd_selb(s72, s99, s46);
    s62   = nd_shufb(s42, s43, s119);
    s98   = nd_fms(s93, s80, s98);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s10) + (uint32_t)_mm_cvtsi128_si32(s24), s73);
    s99   = nd_fms(s85, s81, s109);
    s109  = nd_shufb(s101, s120, s123);
    s64   = nd_fm(s95, s57);
    s72   = nd_shufb(s101, s120, s119);
    s101  = nd_fm(s94, s57);
    s63   = nd_shufb(s108, s62, s127);
    s120  = nd_fm(s97, s57);
    s50   = nd_shufb(s50, s50, s34);
    s73   = nd_selb(s74, s11, s54);
    s57   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s28) + (uint32_t)_mm_cvtsi128_si32(s18));
    s11   = nd_selb(s62, s108, s46);
    s18   = s100;
    s74   = nd_selb(s72, s71, s46);
    s62   = nd_shufb(s107, s106, s112);
    s107  = nd_fma(s96, s11, s64);
    s75   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s7) + (uint32_t)_mm_cvtsi128_si32(s17));
    s108  = nd_fma(s92, s11, s101);
    s101  = nd_shufb(s116, s104, s123);
    s76   = nd_fma(s98, s11, s120);
    s104  = nd_shufb(s55, s56, s123);
    s77   = nd_fma(s81, s74, s88);
    s116  = nd_shufb(s57, s58, s123);
    s106  = nd_fma(s87, s74, s90);
    s100  = nd_shufb(s70, s105, s125);
    s11   = nd_selb(s109, s101, s46);
    s101  = nd_shufb(s55, s56, s119);
    s78   = nd_selb(s62, s102, s54);
    s102  = nd_shufb(s103, s12, s117);
    s103  = nd_selb(s104, s116, s46);
    s104  = nd_shufb(s57, s58, s35);
    s64   = nd_fma(s91, s63, s108);
    s116  = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s20) + (uint32_t)_mm_cvtsi128_si32(s18));
    s12   = nd_fm(s80, s103);
    s105  = nd_shufb(s69, s105, s37);
    s106  = nd_fma(s83, s11, s106);
    s79   = nd_shufb(s110, s60, s16);
    s62   = nd_fma(s89, s63, s107);
    s120  = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s22) + (uint32_t)_mm_cvtsi128_si32(s18));
    s107  = nd_fm(s103, s83);
    s108  = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s9) + (uint32_t)_mm_cvtsi128_si32(s24));
    s109  = nd_selb(s101, s104, s46);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s27) + (uint32_t)_mm_cvtsi128_si32(s24), s78);
    s110  = nd_fma(s80, s11, s77);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s20) + (uint32_t)_mm_cvtsi128_si32(s24), s79);
    s12   = nd_fma(s109, s81, s12);
    s75   = nd_rotqby(s75, s7);
    s63   = nd_fma(s99, s63, s76);
    s76   = nd_shufb(s100, s69, s16);
    s7    = nd_fma(s84, s74, s102);
    s104  = nd_shufb(s104, s101, s127);
    s101  = nd_fm(s103, s85);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s28) + (uint32_t)_mm_cvtsi128_si32(s24), s73);
    s74   = nd_fma(s109, s87, s107);
    s73   = nd_rotqby(s32, s75);
    s66   = nd_selb(s76, s66, s54);
    s103  = nd_shufb(s60, s61, s37);
    s107  = nd_fma(s104, s86, s12);
    s60   = nd_shufb(s71, s72, s127);
    s61   = nd_selb(s70, s105, s51);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s26) + (uint32_t)_mm_cvtsi128_si32(s24), s66);
    s11   = nd_fma(s85, s11, s7);
    s100  = nd_selb(s69, s100, s52);
    s66   = nd_shufb(s103, s59, s112);
    s69   = nd_fma(s109, s84, s101);
    s70   = nd_shufb(s105, s70, s112);
    s109  = nd_fma(s104, s82, s74);
    s7    = s67;
    s67   = nd_fm(s107, s107);
    s101  = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s8) + (uint32_t)_mm_cvtsi128_si32(s18));
    s100  = nd_selb(s100, s68, s54);
    s12   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s6) + 0x0);
    s68   = nd_fm(s62, s62);
    s105  = nd_shufb(s14, s14, s31);
    s65   = nd_selb(s70, s65, s54);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s21) + (uint32_t)_mm_cvtsi128_si32(s24), s66);
    s61   = nd_selb(s61, s108, s54);
    s72   = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s2) + (uint32_t)_mm_cvtsi128_si32(s111));
    s108  = nd_fma(s104, s93, s69);
    s104  = nd_ls_read(ls, (uint32_t)_mm_cvtsi128_si32(s21) + (uint32_t)_mm_cvtsi128_si32(s18));
    s67   = nd_fma(s109, s109, s67);
    s59   = nd_selb(s59, s103, s51);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s23) + (uint32_t)_mm_cvtsi128_si32(s24), s100);
    s103  = nd_fma(s63, s63, s68);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s9) + (uint32_t)_mm_cvtsi128_si32(s24), s61);
    s61   = nd_fma(s93, s60, s11);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s8) + (uint32_t)_mm_cvtsi128_si32(s24), s59);
    s59   = nd_fma(s86, s60, s110);
    nd_ls_write(ls, (uint32_t)_mm_cvtsi128_si32(s30) + (uint32_t)_mm_cvtsi128_si32(s24), s65);
    s60   = nd_fma(s82, s60, s106);

    _mm_storeu_si128((__m128i*)&ctx->gpr[7], s7);
    _mm_storeu_si128((__m128i*)&ctx->gpr[11], s11);
    _mm_storeu_si128((__m128i*)&ctx->gpr[12], s12);
    _mm_storeu_si128((__m128i*)&ctx->gpr[18], s18);
    _mm_storeu_si128((__m128i*)&ctx->gpr[24], s24);
    _mm_storeu_si128((__m128i*)&ctx->gpr[42], s42);
    _mm_storeu_si128((__m128i*)&ctx->gpr[43], s43);
    _mm_storeu_si128((__m128i*)&ctx->gpr[44], s44);
    _mm_storeu_si128((__m128i*)&ctx->gpr[45], s45);
    _mm_storeu_si128((__m128i*)&ctx->gpr[50], s50);
    _mm_storeu_si128((__m128i*)&ctx->gpr[55], s55);
    _mm_storeu_si128((__m128i*)&ctx->gpr[56], s56);
    _mm_storeu_si128((__m128i*)&ctx->gpr[57], s57);
    _mm_storeu_si128((__m128i*)&ctx->gpr[58], s58);
    _mm_storeu_si128((__m128i*)&ctx->gpr[59], s59);
    _mm_storeu_si128((__m128i*)&ctx->gpr[60], s60);
    _mm_storeu_si128((__m128i*)&ctx->gpr[61], s61);
    _mm_storeu_si128((__m128i*)&ctx->gpr[62], s62);
    _mm_storeu_si128((__m128i*)&ctx->gpr[63], s63);
    _mm_storeu_si128((__m128i*)&ctx->gpr[64], s64);
    _mm_storeu_si128((__m128i*)&ctx->gpr[65], s65);
    _mm_storeu_si128((__m128i*)&ctx->gpr[66], s66);
    _mm_storeu_si128((__m128i*)&ctx->gpr[67], s67);
    _mm_storeu_si128((__m128i*)&ctx->gpr[68], s68);
    _mm_storeu_si128((__m128i*)&ctx->gpr[69], s69);
    _mm_storeu_si128((__m128i*)&ctx->gpr[70], s70);
    _mm_storeu_si128((__m128i*)&ctx->gpr[71], s71);
    _mm_storeu_si128((__m128i*)&ctx->gpr[72], s72);
    _mm_storeu_si128((__m128i*)&ctx->gpr[73], s73);
    _mm_storeu_si128((__m128i*)&ctx->gpr[74], s74);
    _mm_storeu_si128((__m128i*)&ctx->gpr[75], s75);
    _mm_storeu_si128((__m128i*)&ctx->gpr[76], s76);
    _mm_storeu_si128((__m128i*)&ctx->gpr[77], s77);
    _mm_storeu_si128((__m128i*)&ctx->gpr[78], s78);
    _mm_storeu_si128((__m128i*)&ctx->gpr[79], s79);
    _mm_storeu_si128((__m128i*)&ctx->gpr[80], s80);
    _mm_storeu_si128((__m128i*)&ctx->gpr[81], s81);
    _mm_storeu_si128((__m128i*)&ctx->gpr[82], s82);
    _mm_storeu_si128((__m128i*)&ctx->gpr[83], s83);
    _mm_storeu_si128((__m128i*)&ctx->gpr[84], s84);
    _mm_storeu_si128((__m128i*)&ctx->gpr[85], s85);
    _mm_storeu_si128((__m128i*)&ctx->gpr[86], s86);
    _mm_storeu_si128((__m128i*)&ctx->gpr[87], s87);
    _mm_storeu_si128((__m128i*)&ctx->gpr[88], s88);
    _mm_storeu_si128((__m128i*)&ctx->gpr[89], s89);
    _mm_storeu_si128((__m128i*)&ctx->gpr[90], s90);
    _mm_storeu_si128((__m128i*)&ctx->gpr[91], s91);
    _mm_storeu_si128((__m128i*)&ctx->gpr[92], s92);
    _mm_storeu_si128((__m128i*)&ctx->gpr[93], s93);
    _mm_storeu_si128((__m128i*)&ctx->gpr[94], s94);
    _mm_storeu_si128((__m128i*)&ctx->gpr[95], s95);
    _mm_storeu_si128((__m128i*)&ctx->gpr[96], s96);
    _mm_storeu_si128((__m128i*)&ctx->gpr[97], s97);
    _mm_storeu_si128((__m128i*)&ctx->gpr[98], s98);
    _mm_storeu_si128((__m128i*)&ctx->gpr[99], s99);
    _mm_storeu_si128((__m128i*)&ctx->gpr[100], s100);
    _mm_storeu_si128((__m128i*)&ctx->gpr[101], s101);
    _mm_storeu_si128((__m128i*)&ctx->gpr[102], s102);
    _mm_storeu_si128((__m128i*)&ctx->gpr[103], s103);
    _mm_storeu_si128((__m128i*)&ctx->gpr[104], s104);
    _mm_storeu_si128((__m128i*)&ctx->gpr[105], s105);
    _mm_storeu_si128((__m128i*)&ctx->gpr[106], s106);
    _mm_storeu_si128((__m128i*)&ctx->gpr[107], s107);
    _mm_storeu_si128((__m128i*)&ctx->gpr[108], s108);
    _mm_storeu_si128((__m128i*)&ctx->gpr[109], s109);
    _mm_storeu_si128((__m128i*)&ctx->gpr[110], s110);
    _mm_storeu_si128((__m128i*)&ctx->gpr[116], s116);
    _mm_storeu_si128((__m128i*)&ctx->gpr[120], s120);
    /* 0xB238 tail: publish gpr[73] and hand off to the indirect dispatcher. */
    ctx->pc = (uint32_t)_mm_cvtsi128_si32(s73);
    g_spu_trampoline_fn = spu_indirect_branch;
}
