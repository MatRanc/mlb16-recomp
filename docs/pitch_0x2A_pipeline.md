# Pitch 0x2A handler (SPU 0x10F78) — offline replay & pipeline constants

Date: 2026-11-01. **No game launch and no PPU/SPU execution in the emulator were
used**; everything below is offline replay of the lifted SPU code on captures.

Lift: `recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`
Captures: `/tmp/opencode/fncap_full/{0x10F78,0x06D68,0x10B88,0x0A798,0x0AF98}`

---

## 1. Does 0x10F78 replay offline?  YES

Harness: `mlb16-recomp/tools/replay/test_job_c690_10f78_replay.c`
(linked against the real lifted image + `job_c690_native.c`; the runtime
channel/DMA ABI is replaced by an instrumented model, and indirect branches
resolve through a generated LS-address→lifted-function table).

Run on `/tmp/opencode/fncap_full/0x10F78` (r3 = 0x1F000, r4 = 0x2A000000):

```
LS FNV (input)   : 0x7c3c94626cf4d27d
LS FNV (output)  : 0xa149994a2ec14296
GPR FNV (output) : 0xf4cd98cfd8c4e587
LS+GPR FNV       : 0xbfa9a9b96117bf38
DMA get=0 put=0 other=0  ch_ops=0  dispatches=195
Final pc=0x108E8  status=0x0  stop=0x0
```

It reaches its normal return (`bi $r0` → link 0x108E8 = the caller). **It issues
no MFC DMA and touches no mailbox channel**, so nothing had to be stubbed in
this instance.

### What it actually does (instrumented entry counts)

```
0x10F78  1
 0x049D4  1      LS allocator (rewrites context +0xA0/+0x340, bumps cursors)
 0x0EA70  1      staging; takes the EMPTY-LIST exit
 0x10C70  4      per-stream descriptor parse -> 0x109C0
 0x109C0  4
 0x07840  3  0x07D40 1  0x07D7C 1  0x07DA8 4  0x07E58 4  0x07E98 4
 0x078C0 12  0x07960 77  0x07998 77
```

EA70 path inside the function (probed): `EACC → EB10 → EBD8 → EC60 → return`.
At `0xEBD8` (`spu_recomp.c:21131` region) the guard is

```
if (sr81[0] == 0) { SPU_DIRECT 0xEC60 }   // 0xEBD8
```

`s r81` is the staging-list count; it is zero in this capture, so the DMA/mailbox
half of EA70 (`0xEBA0`: MFC_WrTagMask / WrTagUpdate / RdTagStat and the MFC
issue) is skipped. **Where it would diverge:** if that count were non-zero, the
handler would walk the 0x440-byte list and issue MFC GETs from guest EAs; those
EA payloads are *not* in an LS-only capture, so that path would need guest RAM
(or a zero-fill stub, which is what the harness supports via `FF_ZERO_GET`).

### Correction to the prior pipeline mapping

In this capture 0x10F78 is **not** the position-transform path. It never calls
0x6D68, 0xA798, 0xA9B0, 0xAF98, 0x7C18 or 0x8160. Its actual work is a
stream-2 attribute gather/convert driven by 0x10C70 → 0x109C0 → the
0x7840/0x78C0/0x7960/0x7998 family. Observed side effects:

| LS range | before → after | interpretation |
|---|---|---|
| 0x23400..0x2358F | zeros → `0008 0018 0028 …` | 16-bit vertex-id/index buffer, +0x10 step |
| 0x2F870..0x3139F | zeros → float data | stream-2 attribute output buffer |
| 0x1F0A0/0x1F0D0 | 0x23400 / 0x2F870 | output cursors (context) |
| 0x1F300/0x1F340/0x1F430 | counters | allocator bookkeeping |

Context fields rewritten to `+0xA0 = 0x23400`, `+0xD0 = 0x2F870`
(`spu_recomp.c:26272` reads `[r3+0x2A0]`/`[r3+0x2D0]`/`[r3+0x300]` in the
pre-EA70 prologue).

**The position output at LS 0x1F7C0 is untouched by this 0x10F78 instance** —
the bytes there are already the result of an earlier 0x6D68 call in the same
frame (the input capture is non-zero: `bee683563f5a875c…`).

The 12/12/11 position path is 0x6D68 (leaf), which writes exactly
`0x1F7C0..0x203BF` (0xC00 = 192 × float4).

---

## 2. Position output: bit-exact verification

Run the lifted 0x6D68 on `fncap_full/0x06D68`:

```
LS FNV        : 0x72a45be7eabc1101
GPR FNV       : 0xec9394371de4e862
LS+GPR FNV    : 0xc4158fc3e73e7496
Position 0x1F7C0[0xC00] FNV-1a64 : 0xb98ec2c0561b2bbf
```

* Native AVX2/FMA kernel (`tools/replay/native_6d68.c`): **0 differing bytes /
  quads / GPRs, pc equal** (`test_job_c690_6d68_native`). The tool prints
  `RESULT: FAIL` only because it hard-codes the old `/tmp/opencode/fncap`
  checksum `0xfc2e9569dc2feb11`; the actual diff line is all zeros.
* Vulkan compute `c690_vert.comp` (RTX 3060 Ti, compute only):
  CPU region FNV = GPU region FNV = `0xb98ec2c0561b2bbf`, **0 differing
  bytes, 0 writes outside the region**.
* New independent CPU reference (`tools/replay/pitch_pipeline_ref.c`,
  stage A): same FNV, **0 diff bytes → BIT-EXACT**.

`0x06D68` position format: base byte `0x17350`, 192 vertices, 35 bits/vertex
(12/12/11), output `0x1F7C0`, stride 16, 24 iterations × 8 vertices = 0xC00.

---

## 3. Constants

Header: `mlb16-recomp/tools/replay/pitch_pipeline_constants.h`.

### A. Position unpack (0x6D68, `spu_recomp.c:5372`)

```
in_base = 0x17350, out_base = 0x1F7C0, 192 verts, 35 bits (12/12/11)
a_bits = 0x3F800000 + B[lane] + (F << 2)       (uint32 wrap)
r      = ftz(fma(ftz(a_bits), ftz(A[lane]), ftz(C[lane])))
```
| vector | lane0 | lane1 | lane2 | lane3 |
|---|---|---|---|---|
| A = sr11 | 0x45000400 | 0x45000200 | 0x45000400 | 0xC5004400 |
| B = sr10 | 0x00002F28 | 0x00008048 | 0x000036D8 | 0x44800801 |
| C = sr12 | 0xC5004400 | 0xC5008200 | 0xC5004400 | 0x00002F28 |

### B. Transform (0xA798, `spu_recomp.c:12614`)

The block is the tail of the transform loop; it loads a **3×4 affine matrix**
from LS `0x29540` (effective address `(sr3 + sr115) & 0x3FFFF`,
sr3=0x29360, sr115[0]=0x1B401E0 → low bits 0x1E0). Rows are 16 bytes apart,
translation in `.w`:

```
row0  +0.20980275  -0.77823815  +0.50281787  +0.96878993
row1  +0.92172539  +0.12283876  -0.19445337  +3.58617234
row2  +0.09428138  +0.53080124  +0.78221333  -0.55416942
```
The 3×3 part is orthonormal × 0.9499. The block ends with
`ctx->pc = gpr[82]; spu_indirect_branch` (gpr[82]=0xAF98), i.e. it chains
straight into normalize. Output-stream bases in this capture rotate through
`{0x1F7C0, 0x214C0, 0x231C0}` (`r8/r9/r10`).

### C. Normalize (0xAF98 → 0xB238, `spu_recomp.c:13787/13961`)

```
len2 = fma(x,x, fma(y,y, fma(z,z, acc)))   ; spu_fma, SPU_FAST_FLOAT
est  = frsqest(len2)                       ; spu_recomp.c:13801
rlen = fi(len2, est)                       ; spu_recomp.c:13810
n    = v * rlen
```
No scalar constant table: it is parameterised by register state. Bit-exact
emulation needs the SPU `frsqest` LUT + fixed-point `FI` refinement, not
`inversesqrt()`.

### D. Pack (0x7540 at label 0x7C18; 0x8160)

```
0x7C18: q = fma(attr, sr9, sr10); q = cflts(q, 157); shufb + rotmi(-16)  -> snorm16
0x8160: q = fma(attr, sr9, sr10); q = cflts(q, 142); shufb              -> snorm8
```
`cflts(v,i8)` scales by `2^(173-i8)`: 157 → 2^16, 142 → 2^31
(`spu_recomp.c:6610` and `:7048/7972`). The `sr9/sr10` scale/offset vectors are
loaded from job state and are not in these captures; conventional snorm values
are 32767/0 and 127/0.

### Output buffers / strides

| stream | LS base | stride | evidence |
|---|---|---|---|
| positions (0x6D68) | 0x1F7C0 | 16 B × 192 = 0xC00 | verified |
| 0x2A stream 2 attr (0x10F78) | 0x2F870 | 16 B | replay diff |
| 0x2A stream 2 index (0x10F78) | 0x23400 | 2 B, +0x10 pattern | replay diff |
| transform stage (0xA798) | 0x1F7C0 / 0x214C0 / 0x231C0 | 16 B | GPR r8–r10 |

---

## 4. CPU reference & shader

* `mlb16-recomp/tools/replay/pitch_pipeline_ref.c`
  Implements A (bit-exact, validated), B (captured matrix), C (SPU
  `frsqest`+`FI`), D (`cflts 157` snorm16). Run:
  `/tmp/opencode/pitch_ref /tmp/opencode/fncap_full/0x06D68`
  → `stage A diff bytes: 0 … PASS (stage A bit-exact)`.
* `ps3recomp-probe/libs/video/vulkan/c690_pitch_pipeline.comp`
  Compute-shader extension of `c690_vert.comp` implementing A–D with push
  constants for the matrix and pack scale/offset. Compiles clean with
  `glslangValidator -V`.
* Golden 0x10F78 LS: `/tmp/opencode/out_10f78_probe.bin`
  (input LS / output LS checksums in §1). Golden 0x6D68 LS:
  `/tmp/opencode/out_06D68.bin`.

### Shader spec (what the GLSL must implement)

1. Read the MSB-first 35-bit stream at `in_base` and dequant with the exact
   `0x3F800000 + B + 4F` / `fma(a,A,C)` / `ftz` sequence (stage A, bit-exact).
2. Apply the 3×4 affine matrix (row-major, translation in `.w`) to `xyz`.
3. Normalize with the SPU sequence (`frsqest` + `FI`); `inversesqrt()` is only
   an approximation.
4. Pack the result: `q = trunc((v*sr9 + sr10) * 2^16)` then take the high 16
   bits for snorm16 (or the `cflts 142` path + byte shuffle for snorm8), and
   store big-endian 32-bit lanes into 16-byte AoS slots.

---

## Reproduce

```sh
# 0x10F78 replay (needs a recomp-work/runtime -> ps3recomp-native/runtime
# symlink because spu_recomp.c uses a ../../runtime include)
cd mlb16-recomp/tools/replay
gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I~/Documents/mlb16-online/ps3recomp-native/include \
    -I~/Documents/mlb16-online/ps3recomp-native/runtime/spu \
    -o /tmp/opencode/test_10f78 test_job_c690_10f78_replay.c reg_10f78.c \
    ~/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
    ~/Documents/mlb16-online/ps3recomp-native/runtime/spu/job_c690_native.c -lm
/tmp/opencode/test_10f78 /tmp/opencode/fncap_full/0x10F78

# stage A reference
gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I.../include -I.../runtime/spu -o /tmp/opencode/pitch_ref \
    pitch_pipeline_ref.c -lm
/tmp/opencode/pitch_ref /tmp/opencode/fncap_full/0x06D68
```

`reg_10f78.c` is generated by `gen_c690_reg.py` from `spu_recomp.h` (648
lifted functions).

## Caveats

* The 0x2A instance replayed here is stream index 2 and does not exercise the
  position path. The position/normalize/pack results above come from the
  separate captured stages (0x06D68, 0x0A798, 0x0AF98) of the same frame.
* Stage A is the only stage with a clean input→output capture; B/C/D are
  reconstructed from the register-state snapshots and the SPU instruction
  semantics, so they are documented but not claimed byte-exact end-to-end.
* The `RESULT: FAIL` lines in the older native/shader tests are hard-coded
  baseline checksums for a different capture directory; their diff counters
  are zero.
