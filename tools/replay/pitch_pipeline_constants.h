/*
 * Pitch vertex pipeline constants, extracted 2026-11-01 from real captures of
 * jobmod_c690c2f7b09a (PS3 SPU image 6) taken in the same pitch frame.
 *
 * Sources (all under /tmp/opencode/fncap_full/):
 *   0x06D68  position unpack (12/12/11, 35 bits/vertex), leaf
 *   0x0A798  end of the 4x4 affine transform loop (register-state snapshot)
 *   0x0AF98  normalize / tangent orthonormalize + attribute store
 *   0x10B88  stream dispatch block (tails into 0x7AC0)
 *   0x10F78  0x2A command handler (stream index 2 in this capture)
 *
 * IMPORTANT (verified by replay, see notes/pitch_0x2A_pipeline.md):
 *   0x10F78 is NOT the position path in this capture.  It runs
 *   0x49D4 -> 0xEA70(empty list) -> 0x10C70 x4 -> 0x109C0 -> 0x7840 family.
 *   The 12/12/11 position path is 0x6D68, and it is the one that writes
 *   LS 0x1F7C0..0x203BF (0xC00 bytes / 192 float4 vertices).
 */
#ifndef PITCH_PIPELINE_CONSTANTS_H
#define PITCH_PIPELINE_CONSTANTS_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * A. Position unpack: SPU 0x6D68  (spu_recomp.c:5372)
 *
 * Per vertex, MSB-first bitstream, 35 bits = x:12 y:12 z:11.
 * 192 vertices, 24 loop iterations x 8 vertices; 128 bytes written / iter.
 * The dequant is bit-exact:
 *
 *   a_bits = 0x3F800000 + B[lane] + (F << 2)      (uint32 wrap)
 *   a      = ftz(a_bits)
 *   r      = ftz(fma(a, ftz(A[lane]), ftz(C[lane])))
 *
 * ftz(x) = ((x & 0x7F800000) == 0) ? 0 : x   (SPU_FAST_FLOAT denormal flush)
 * F is the unsigned packed field; lane 3 has no field (F=0) and produces the
 * constant that is replicated into w.
 * ------------------------------------------------------------------------- */
#define PITCH_POS_IN_BASE        0x17350u   /* byte base of the packed stream  */
#define PITCH_POS_OUT_BASE       0x1F7C0u   /* first 16-byte AoS slot          */
#define PITCH_POS_VERTS          192u
#define PITCH_POS_STRIDE_BITS    35u
#define PITCH_POS_BITS_X         12u
#define PITCH_POS_BITS_Y         12u
#define PITCH_POS_BITS_Z         11u
#define PITCH_POS_OUT_STRIDE     16u        /* bytes/vertex (x,y,z,w)          */
#define PITCH_POS_OUT_BYTES      (PITCH_POS_VERTS * PITCH_POS_OUT_STRIDE) /* 0xC00 */

/* Captured coefficient GPRs (0x06D68).  Raw 32-bit lane values. */
static const uint32_t pitch_pos_A[4] = { 0x45000400u, 0x45000200u, 0x45000400u, 0xC5004400u }; /* sr11 */
static const uint32_t pitch_pos_B[4] = { 0x00002F28u, 0x00008048u, 0x000036D8u, 0x44800801u }; /* sr10 */
static const uint32_t pitch_pos_C[4] = { 0xC5004400u, 0xC5008200u, 0xC5004400u, 0x00002F28u }; /* sr12 */

/* Verified checksum: FNV-1a64 of LS 0x1F7C0..0x203BF after 0x6D68. */
#define PITCH_POS_FNV            0xb98ec2c0561b2bbfu

/* -------------------------------------------------------------------------
 * B. Affine transform (SPU 0xA798, tail of the skin/transform loop)
 *
 * The transform reads a 3x4 affine matrix (three float4 rows, row-major,
 * translation in .w) from LS 0x29540 in the 0x0A798 capture:
 *     effective address = (sr3 + sr115) & 0x3FFFF with sr3=0x29360,
 *     sr115[0]=0x1B401E0 -> low 18 bits 0x1E0 -> 0x29540
 * Rows are 16 bytes apart.  Rotation part is orthonormal * 0.9499.
 * ------------------------------------------------------------------------- */
static const float pitch_transform_matrix[12] = {  /* rows: xyz | translation */
    +0.20980275f, -0.77823815f, +0.50281787f, +0.96878993f,
    +0.92172539f, +0.12283876f, -0.19445337f, +3.58617234f,
    +0.09428138f, +0.53080124f, +0.78221333f, -0.55416942f,
};

/* Same-image LS addresses that bound the transform chain:
 *   0x0A798 -> tail-call through gpr[82] -> 0x0AF98 (normalize)
 *   0x0A798 loads rows at 0x29540 / 0x29550 / 0x29560
 *   output-stream bases in the 0x0A798 capture: r8/r9/r10 rotate through
 *   {0x1F7C0, 0x214C0, 0x231C0};  per-stream slots at +0x10/+0x20/+0x30.
 */
#define PITCH_TRANSFORM_LS       0x0A798u
#define PITCH_TRANSFORM_MATRIX_LS 0x29540u

/* -------------------------------------------------------------------------
 * C. Normalize (SPU 0xAF98 -> 0xB238, spu_recomp.c:13787/13961)
 *
 *   len2 = fma(v, v, acc)                 (three fmas: x*x + y*y + z*z)
 *   est  = frsqest(len2)                  (SPU reciprocal-sqrt estimate)
 *   rlen = fi(len2, est)                  (SPU FI refinement, one step)
 *   n    = v * rlen
 *
 * There is no constant table: 0xAF98 is parameterised by its register state.
 * Bit-exact emulation therefore needs the SPU frsqest LUT + FI fixed-point
 * step, not GLSL inversesqrt().
 * ------------------------------------------------------------------------- */
#define PITCH_NORMALIZE_LS       0x0AF98u
#define PITCH_NORMALIZE_END_LS   0xB238u

/* -------------------------------------------------------------------------
 * D. Pack (inside 0x7540 at label 0x7C18, and 0x8160)
 *
 *   0x7C18: q = fma(attr, sr9, sr10); q = cflts(q, 157);
 *           shufb/rotmi(-16) interleave -> snorm16 words
 *   0x8160: q = fma(attr, sr9, sr10); q = cflts(q, 142);
 *           shufb -> snorm8 bytes
 *
 * cflts(v, i8) scales by 2^(173 - i8): 157 -> 2^16, 142 -> 2^31.
 * The sr9/sr10 scale/offset vectors are loaded from job state and are not
 * present in the captures above; for [-1,1] attributes the conventional
 * snorm values are 32767/0 (16-bit) and 127/0 (8-bit).
 * ------------------------------------------------------------------------- */
#define PITCH_PACK_LABEL_LS      0x7C18u
#define PITCH_PACK_SNORM16_I8    157
#define PITCH_PACK_SNORM8_I8     142
#define PITCH_PACK_SNORM8_LS     0x8160u

/* -------------------------------------------------------------------------
 * E. 0x2A handler (0x10F78) observed stream-2 output addresses
 *
 * Context base r3 = 0x1F000.  The handler rewrites the context:
 *   +0x0A0 = 0x23400  index/vertex-id output buffer (16-bit, +0x10 step)
 *   +0x0D0 = 0x2F870  vertex attribute output buffer (float data)
 * The 0x1F0A0/0x1F0D0 fields in the *input* capture are the same pointers
 * before the allocator (0x49D4) bumps the cursors.
 * ------------------------------------------------------------------------- */
#define PITCH_CMD2A_CONTEXT_LS   0x1F000u
#define PITCH_CMD2A_INDEX_OUT    0x23400u
#define PITCH_CMD2A_ATTR_OUT     0x2F870u
#define PITCH_CMD2A_FNV_LS       0xa149994a2ec14296ull  /* whole 256 KB LS */
#define PITCH_CMD2A_FNV_LSGPR    0xbfa9a9b96117bf38ull

#endif /* PITCH_PIPELINE_CONSTANTS_H */
