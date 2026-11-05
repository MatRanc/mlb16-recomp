# c690 0x2A record classes: decode, per-class GPU kernels, exactness gate, coverage

Date: 2026-11-05. **Offline only: zero game launches.** Corpus: the regenerated
pitch-meter tail spool `recomp-work/captures/gpu2a/fncap2b2w3` (192 0x2A pairs;
`PS3RECOMP_C690_TAIL_CAPTURE` at the `0x10F78` function exit) and the control
spool `fncap2b2w2` (64 pairs, all `0x00A00004`). Work is in `mlb16-recomp` only;
the `ps3recomp-gpu` walker-binding worktree, `build-gpu` and the existing
`c690_2a_32b.*` files were not touched.

This follows [`edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md) (its Blockers §2),
[`edge-gpu-2a-wiring.md`](edge-gpu-2a-wiring.md),
[`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md) …
[`step5`](edge-gpu-phase2b-step5.md), [`edge-exactness.md`](edge-exactness.md)
and [`c690_function_map.md`](c690_function_map.md).

## TL;DR

* **The meter 0x2A output is not four classes, it is ten record layouts plus
  three non-record outputs.** The 192 pairs split into 60 position-first
  32-byte, 17 twenty-byte, 17 position-only (the 48-byte family is four
  12-byte positions in a row), 19 attribute-first (nine 32-byte / ten
  28-byte), 13 forty-four-byte, 5 twenty-eight-byte position-first, plus 31
  first-in-job pairs that call the low-LS SPURS runtime and 30 outputs that
  copy no positions at all (five 64-byte, two 40-byte, one 16-byte, one
  4-byte, one 112-byte and the attribute/index-only remainder).
* **Every replayable vertex layout is decoded and kerneled.** Seven class
  kernels (one shared `.inc`) reproduce **131/131 classified pairs, 45,440
  records**: positions **0 differing bytes**, packed fields **max 1 LSB**,
  bytes the class does not write **identical to the SPU image**, and **0
  writes outside the record range**. The 9 previously unclassified 32-byte
  "packed offset" pairs are included: they use the same record layout; the
  source entry index is recovered from the record and exported.
* **Coverage is 131/192 meter 0x2A pairs = 68.2 %** (up from 60/192 = 31.2 %
  with the 32-byte kernel alone). The remaining fallback is **61/192 =
  31.8 %**: 31 first-in-job pairs (16.1 %) whose handler enters the low-LS
  SPURS runtime (`0x1690`, `0x2F18`, …) and whose output contains no source
  vertex copies; 19 pairs (9.9 %) that emit no positions (attribute/index
  streams); and 11 rare layouts (5.7 %: 64, 40, 16, 4 and 112-byte).
* **The 5 % gate is still not met, and the premise in the previous docs was
  wrong in two ways.** `0x00A00004` is 50/192 = 26.0 % of pairs, not 51 %;
  31 of those 50 are the first-in-job runtime path above, so only 19 (9.9 %)
  are vertex records (nine 32-byte + ten 28-byte, now kerneled). The stride
  classifier's "other 98" mixed those 50 with five other descriptors and
  three output kinds.
* **No live key or walker binding was added** (that is the parallel
  `ps3recomp-gpu` mission). This document gives the exact kernel interface so
  it can add the classes without redesign; see §7.

## 1. Corpus and method

The spool stores one IN and one OUT full 256 KiB Local Store + 128 GPRs per
sampled command. For each pair the harness reads:

* `r3 = gpr[3]`, the source stream table at `LS[r3+0x260]` (eight words; each
  is a 16-byte-entry planar array base), the output cursor at `LS[r3+0xD0]`
  (IN and OUT), and the format word at `LS[0x1F690]`;
* the record stride `S` from `delta = end - out_base` and the class layout;
* the source entry for each record by matching the record's position bytes
  against the stream arrays (the pre-pass may list a vertex more than once;
  the correct entry is the one that also reproduces the packed fields).

The reference is the captured OUT (the lifted SPU output); the kernel runs from
the captured IN. A class is accepted for a pair only when every field of every
record can be reproduced from the IN image, so classification is itself a
decode test.

## 2. Record layouts

`pos` is three big-endian f32 copied bit-exactly. `normal` is one 32-bit word
`x:11 y:11 z:10` (two's complement). `s16xN` are N big-endian s16. `f16x2` is
two big-endian f16 (u in the high half, v in the low). "SPU" marks bytes the
SPU pre-pass writes; the kernel never touches them.

| class | bytes | fields |
|---|---:|---|
| `pos` | 12 | `pos@0` |
| `20b` | 20 | `pos@0`, `f16x2@12`, `s16x2@16` (scale 8192) |
| `28b_pos` | 28 | `pos@0`, `normal@12`, `s16x4@16` (32768), `f16x2@24` |
| `32b_pos` | 32 | `pos@0`, `normal@12`, `s16x4@16` (32768), `f16x2@24`, **SPU attr@28** (4 bytes) |
| `32b_attr` | 32 | **SPU prefix@0** (8 bytes), three `normal@8,12,16`, `pos@20` |
| `28b_attr` | 28 | **SPU prefix@0** (4 bytes), three `normal@4,8,12`, `pos@16` |
| `44b` | 44 | `pos@0`, `normal@12`, `s16x2@16` (1024), `s16x4@20` (32768), `s16x2@28` (16384), `s16x3@32` (1024), `s16x3@38` (1024) |

Pack rules (re-derived and gated on this corpus):

```
normal(v, scale, bits) = clamp(trunc(v * scale) - 1, -(1<<(bits-1)), (1<<(bits-1))-1)
  x,y: scale 1024, 11 bits;  z: scale 512, 10 bits
s16(v, scale)          = clamp(trunc(v * scale) - 1, -32768, 32767)
f16(v)                 = truncate the f16 mantissa; denormals/zero -> signed zero
```

The f16 rule is new: the previous 32-byte kernel used round-to-nearest-even and
passed at <= 1 LSB, but several 20/28/44-byte classes carry UV values in the
f16 denormal range where RNE and the SPU differ by 0x200 in the raw encoding.
The shared kernel truncates the mantissa and flushes denormals to signed zero,
which matches the SPU on every corpus pair (the RNE form is also within 1 LSB
on the non-denormal values, so the 32-byte result is unchanged).

### 2.1 The 48-byte family is a position stream

A `0x600008` output the stride classifier called "48-byte" is four consecutive
12-byte positions (source entries `4k..4k+3`) with no other field. The kernel
therefore treats it as a 12-byte position stream: `count = delta/12`, source
index `k`, and copies `pos` only. The same kernel covers the 14 `0x600008`
pairs the old classifier called 48-byte and the 3 `0x1000000` 12-byte pairs
(17 pairs, 7,388 records).

## 3. Index pre-pass contract

The SPU keeps the pre-pass (the design in `edge-gpu-2a-wiring.md` §4). For
these kernels it exports, per record, a **u32 source entry index** (entry =
16-byte-aligned source slot). Every class gathers all its fields at that same
entry index; this is the corrected reading of the earlier "not always at the
same offset" confusion: the same index is used, but the pre-pass may list a
vertex several times, and the duplicate with the matching attributes is the
one that was written.

* **A/B 32-byte formats (51 pairs).** Format A's u16 byte-offset list is at
  `cmd[0]`; format B is the identity `k*16`. Both are converted to entry
  indices `offset/16`.
* **The 9 packed-offset 32-byte pairs** (`seq 154000, 875000, 941000, 977000,
  1108000, 1464000, 1595000, 1696000, 1796000`). `seq 941000` stores the u16
  byte-offset list at IN LS `0x280b0` (not at `cmd[0]`); the other eight store
  a topology order that is not a plain u16/u32 table at any searched address.
  They are the **same 32-byte record layout**: the kernel gate recovers the
  entry index from each record's position and passes it explicitly, so they
  are gated (all 60/60 position-first pairs). The live binding must export the
  pre-pass's index list in this u32 form; it must not assume `cmd[0]`.
* **Attribute-first 32/28-byte.** The position is not stream slot 0: the
  stream table lists the three 11:11:10 sources and the position last (the
  position may be any of the table entries; the gate searches all of them).
  The index list is the pre-pass's topology list, recovered per record.

## 4. Kernels

New files in `tools/replay/`:

* `c690_2a_records.inc` — the shared writer: LS big-endian load/store, the
  pack rules above, one invocation per record, and the class switch.
* `c690_2a_pos.comp`, `c690_2a_20b.comp`, `c690_2a_28b_pos.comp`,
  `c690_2a_32b_pos.comp`, `c690_2a_32b_attr.comp`,
  `c690_2a_28b_attr.comp`, `c690_2a_44b.comp` — one class each
  (`#define C690_CLASS_x` + the shared `.inc`).
* `test_c690_2a_classes.c` — Vulkan runner. Uploads the captured IN LS, the
  seed image (IN plus every SPU-written byte), the u32 index list and the
  seven stream bases; one `vkCmdDispatch`; writes the resulting LS image.
* `test_c690_2a_classes.py` — corpus parser, class classifier, index/base
  recovery, job builder, exactness gate and coverage report.

The kernel writes only the class's fields. Bytes outside those fields are
whatever the seed image holds, so the gate proves "SPU-written bytes
untouched" for the prefix/attribute ranges and "no GPU writes outside records"
for the whole 256 KiB image.

## 5. Exactness gate

`python3 tools/replay/test_c690_2a_classes.py --spool …/fncap2b2w3`:

| class | pairs | records | positions | packed | result |
|---|---:|---:|---|---|---|
| `pos` (12-byte stream; 48-byte family) | 17 | 7,388 | 0 bytes | exact | 17/17 |
| `20b` | 17 | 5,004 | 0 bytes | <= 1 LSB | 17/17 |
| `28b_pos` | 5 | 700 | 0 bytes | <= 1 LSB | 5/5 |
| `32b_pos` (incl. the 9 sub-variant) | 60 | 22,296 | 0 bytes | <= 1 LSB | 60/60 |
| `32b_attr` | 9 | 3,412 | 0 bytes | <= 1 LSB | 9/9 |
| `28b_attr` | 10 | 4,120 | 0 bytes | <= 1 LSB | 10/10 |
| `44b` | 13 | 2,520 | 0 bytes | <= 1 LSB | 13/13 |
| **total** | **131** | **45,440** | **0** | **max 1 LSB** | **131/131** |

Also zero: GPU writes outside the record range, uncovered record bytes that
differ from the SPU, and SPU-written prefix/attribute bytes (they are seeded
and left untouched).

## 6. Coverage and remaining fallback

| bucket | pairs | share | why no kernel |
|---|---:|---:|---|
| kerneled (7 classes) | **131** | **68.2 %** | — |
| first-in-job / low-LS runtime | 31 | 16.1 % | `ob_base == 0` pairs: the handler enters low-LS SPURS code (`0x1690`, `0x2F18`, …); offline replay aborts there, and the written block contains **no source 4-byte words** (it is not a vertex gather). |
| no positions written | 19 | 9.9 % | index/attribute-only outputs (e.g. `0x600008` deltas not divisible by 12); nothing to assemble from the planar arrays. |
| rare layouts | 11 | 5.7 % | five 64-byte, two 40-byte, one 16-byte (pos+normal), one 4-byte, one 112-byte; the 64/40/112 fields are further mixtures (the 64-byte sample is a 44-byte record plus a second 20-byte group). |

Fallback with these kernels is **61/192 = 31.8 %** of 0x2A pairs. That is far
above the <= 5 % gate. The largest single missing piece is the **first-in-job
runtime path (16.1 %)**, which cannot be moved by a record kernel until the
low-LS runtime is modelled or the SPU pre-pass exports its result; the next
is the attribute/index-only output class (9.9 %).

The previous documents' "0x00A00004 = 51 %" needs correcting: the descriptor
is 26.0 % of pairs, and only 9.9 % of pairs are the two attribute-first vertex
layouts from that descriptor. The 0.23-0.27 ms per 429-job burst device-local
cost model from phase 2b step 1 is unchanged; the new kernels are the same
one-invocation-per-record shape.

## 7. Interface for the walker binding

Per 0x2A job, the binding must provide:

1. **Inputs.** The 256 KiB IN Local Store image (source arrays and the
   SPU-written bytes) and up to seven stream bases copied from
   `LS[r3+0x260]` in the class's field-slot order (the gate derives the slot
   order by reproducing each field; the SPU's format context knows it).
2. **Index list.** A device-local u32 array, one source **entry index** per
   record, in output order. This is the only pre-pass product the kernel
   needs; it replaces the u16 offset list for A and makes the 9 sub-variant
   pairs expressible. The SPU builds it today, so exporting it is a copy, not
   a reimplementation.
3. **Output range.** `[out_base, out_base + count * record_size)` in the mesh
   ring (`out_base` from `LS[r3+0xD0]`, or the ring base `0x2F000` when the
   cursor is 0). The kernel writes only the fields in §2; the SPU must write
   the class prefix/attribute bytes listed there (and the command hole/label,
   as today). The range map is the same `(ring EA, size) -> VkBuffer+offset`
   the binding already publishes.
4. **Per-class constants.** `count`, `record_size`, the seven stream bases and
   the class selector. No per-record payload readback.

The only class the current binding exports is A/B 32-byte; adding the other
six is a class selector, the extra stream bases, and the u32 index list.

## 8. Reproduction (offline)

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp

# classification only (no Vulkan)
python3 tools/replay/test_c690_2a_classes.py \
  --spool /home/m/Documents/mlb16-online/recomp-work/captures/gpu2a/fncap2b2w3 --no-vk

# full gate (one dispatch per pair, 131 pairs / 45,440 records)
python3 tools/replay/test_c690_2a_classes.py \
  --spool /home/m/Documents/mlb16-online/recomp-work/captures/gpu2a/fncap2b2w3

# the 51-pair 32-byte kernel is unchanged
python3 tools/replay/test_c690_2a_32b.py \
  --spool /home/m/Documents/mlb16-online/recomp-work/captures/gpu2a/fncap2b2w3
```

The spool is game data and is not committed.

## 9. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code, our own captures and our own kernels. The
stage/corpus/differential method follows NHL Legacy Native (MIT, (c) 2026
HeckYouMan) with attribution; no NHL code was copied. The SPU-gate/shared-GPU
image pattern is from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and
contributors), design reference only. No RPCS3 or other GPL emulator source
was read or cited.
