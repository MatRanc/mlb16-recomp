# c690 0x2A position-first 32-byte class: index pre-pass, GPU kernel, exactness gate

Date: 2026-11-05. Branch `exp/edge-gpu` in the `ps3recomp-gpu` worktree
(based on `exp/live-ab`), build `recomp-work/build-gpu` (capture build only;
the play build was not rebuilt). **Two pitch-meter launches, both capture
runs** (the 2-launch cap); no live A/B was run. Offline: the regenerated meter
tail spools, the lifted c690 module, and a new Vulkan compute kernel.

This follows [`edge-gpu-2a-wiring.md`](edge-gpu-2a-wiring.md) (§5.1/§5.2 and
§6), [`edge-gpu-plumbing.md`](edge-gpu-plumbing.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md) …
[`step5`](edge-gpu-phase2b-step5.md),
[`edge-exactness.md`](edge-exactness.md) and
[`meter-p50-pin.md`](meter-p50-pin.md).

## TL;DR

* **The index pre-pass contract is decoded.** The 0x2A handler builds (or is
  handed) a per-record **u16 byte offset** list. For the common format the
  list is at command payload `cmd[0]`; record *k* gathers **all four planar
  arrays** at `base[i] + cmd0[k]`. A second format is the same gather with the
  implicit list `offs[k] = k * 16`. The 4 bytes at record `+28..31` are
  SPU-written; the kernel leaves them untouched.
* **The 32-byte kernel is exact within policy on every clean pair.** On the
  regenerated meter corpus: **51/51 pairs, 20,312 records**, **0 differing
  position bytes**, **normal/tangent/UV max 1 LSB**, the SPU attribute word
  exact, **0 GPU writes outside record bytes 0..27**, and the ring bytes the
  walker reads differ only at the allowed packed fields
  (**position/attribute 0 differing**). The 36-byte kernel regression check
  is still byte-exact (196 records, 0 ring bytes).
* **Coverage is the blocker, and the live wiring does not exist.** The
  stride-sampled meter corpus is **60/192 32-byte (31.2 %)**, 1/192 36-byte,
  17/192 20-byte, 16/192 48-byte and 98/192 other. The 32-byte stage alone
  leaves **68.8 % of 0x2A pairs** on the SPU (gate <= 5 %). There is **no
  `PS3RECOMP_C690_GPU_2A` key and no walker binding** on `exp/live-ab`
  (`git grep` across all branches: zero hits), so §4 is still a design.
  Both are independent reasons not to launch §6.
* **No live A/B.** The two authorized launches were spent regenerating the
  corpus after the old spools were deleted (the first capture was the
  `0xA00004` "other" class only; the second used `TAIL_STRIDE=1000` to sample
  the meter window). The mission's stop rule applies: the premise "the 0x2A
  wiring exists" is false, and the §6 fallback gate (<= 5 %) is not met.
* **What the measured stage is worth.** 0x2A is 14.06 % of the c690 wall
  (3.43 ms of the documented 24.39 ms critical lane). Covering 31.2 % of its
  pairs is ~**1.07 ms** of lane time — not a p50 move on its own. The next
  GPU levers remain 0x21 sel7 **4.74 ms**, the culls **3.03 ms**, and the
  already-native 0x22 **5.80 ms**.

## 1. Why the corpus was regenerated

The phase-2b spools (`fncap2b2w`, `fncap2b3a730`) were deleted in the disk
cleanup. Two new pitch-meter captures were made with the step-2/3 hook
(function-exit 0x2A pairs, `PS3RECOMP_C690_TAIL_CAPTURE`, its own
non-consuming trigger, `METER=1 SKIP_CINE=1`, one X, governor
`performance`, build `build-gpu`):

| run | tag | pairs | result |
|---|---|---|---|
| 1 | `fncap2b2w2` | 64 per key, `STRIDE=1` | all 64 0x2A pairs are descriptor `0x00A00004` ("other"): the first 64 commands after the meter appears are one mesh batch. Kept as a control. |
| 2 | `fncap2b2w3` | 192 per key, `STRIDE=1000` | samples spread over ~20 s of meter; the 192 0x2A pairs are the mix in §2. This is the gate corpus. |

The old spools are game data and are not committed; the new ones live under
`recomp-work/captures/gpu2a/` (also not committed). The capture build used a
**copy** of the lifted module
(`recomp-work/captures/gpu2a/spu_capture/…`) patched with
`patch_cull_hooks.py --capture`, so the shared play module was not modified.

## 2. Coverage at the meter

`python3 tools/replay/test_c690_2a_plumbing.py --coverage-only --spool …`
on `fncap2b2w3`:

| class | pairs | share |
|---|---:|---:|
| position-first **32-byte** | **60** | **31.25 %** |
| 36-byte (the phase-2a pitch class) | 1 | 0.52 % |
| 20-byte | 17 | 8.85 % |
| 48-byte | 16 | 8.33 % |
| other (e.g. `0x00A00004`) | 98 | 51.04 % |
| **total** | **192** | |

With the 32-byte kernel the remaining 0x2A fallback is **132/192 = 68.75 %**
of sampled pairs (73.4 % if only the 51 clean pairs are counted). The earlier
128-pair phase-2b corpus measured 43.0 % 32-byte; the difference is sampling:
`STRIDE=1` catches one mesh batch, `STRIDE=1000` samples the window. The
honest statement is **31-43 % depending on the window**, still far above the
5 % gate.

## 3. Index pre-pass contract (§5.1)

For each captured pair the harness reads the command payload words
`cmd[0..7]` (LS `r3+0x150`), the source table at `r3+0x260`, and the output
cursor at `r3+0xD0`. Two layouts reproduce the records exactly:

* **Format A (offset list).** `cmd[0]` points at a u16 array, one entry per
  record; the entry is a **byte offset into all four planar arrays**. Record
  *k* is `pos[cmd0[k]]`, `nrm[cmd0[k]]`, `tan[cmd0[k]]`, `uv[cmd0[k]]`.
  Example (`seq 453000`, 400 records): `cmd0[0..5] = 0x27c0, 0x27e0, 0x2780,
  0x2790, 0x27a0, 0x27b0`, and the records' position triples are exactly
  `array0 + those offsets`.
* **Format B (implicit list).** The same gather with `offs[k] = k * 16`; the
  cursor table at `cmd[0]` is not the record index list for this sub-format.
  Example (`seq 1293000`, 312 records): every record position is `array0[k]`,
  and all four arrays match at `k*16` within the pack rules.

This corrects the phase-2b reading: the normal/tangent/UV entries **are** at
the same offset as the position for both formats. The previous "not always at
`table[k] + pos_index`" was an element-index vs byte-offset confusion.

The pre-pass itself is **kept on the SPU** (the design in
`edge-gpu-2a-wiring.md` §4). The kernel consumes the exported offset list and
the SPU's 4-byte attribute word; it does not rebuild the pre-pass.

### 3.1 Attribute bytes 28..31

The 4 bytes at `+28..31` are not produced by the gather/pack. They are written
by the SPU path in the same 16-byte store that carries the tangent/UV (the
trace at `pc=0x7540/0x76b8/0x7e98` shows the word already in the source
register). The contract therefore treats them as **SPU-written**: the job
carries the captured word and the kernel leaves `+28..31` untouched. This is
the §5.2 rule, and the gate verifies it byte-for-byte.

### 3.2 The 9 unclassified sub-variant pairs

Of the 60 records that pass the stride classifier, **51** match A or B
exactly. Nine (`seq 154000, 875000, 941000, 977000, 1108000, 1464000,
1595000, 1696000, 1796000`) have the right 32-byte stride and the right
position prefixes, but their offset list is not a contiguous u16/u32 table at
`cmd[0]` or the identity list (for `seq 941000` it is at LS `0x280b0`; for the
others it is stored in a different packed form). They are a **separate
sub-variant of the same record layout**, not a different class; they are
excluded from the kernel gate and are the first extension for the next
mission.

## 4. Kernel (§5.2)

New files:

* `tools/replay/c690_2a_32b.comp` — one invocation per record. Inputs: the
  full LS image, an exported u16 offset list, the output LS image. Push
  constants: the four array bases, `out_base`, `count`. It writes only
  `+0..27`:
  * `+0..11` position f32 xyz, exact copy;
  * `+12..15` normal 11:11:10, `clamp(trunc(v*scale) - 1)`;
  * `+16..23` 4 x s16 tangent, `clamp(trunc(v*32768) - 1)`;
  * `+24..27` 2 x f16 UV (`u` high half, `v` low);
  * `+28..31` untouched.
* `tools/replay/test_c690_2a_32b.c` — Vulkan runner and gate. It seeds the
  output with the SPU attribute word, dispatches once, diffs the 28 GPU bytes
  against the captured SPU records, counts writes outside the record fields,
  and builds the ring image as "SPU keeps every non-record byte; GPU supplies
  0..27" before comparing the walker's ring range.
* `tools/replay/test_c690_2a_32b.py` — parses the spool, classifies A/B,
  exports the job, runs the C gate, aggregates.

The pack rules were re-derived from the corpus: `trunc` toward zero, then
`-1`, then clamp to the field range. This is within 1 LSB for every packed
value; it is bit-exact for ~75 % of normals and ~61 % of tangents/UVs, and
1 LSB for the rest. Positions are exact.

## 5. Exactness gate

`python3 tools/replay/test_c690_2a_32b.py --spool …/fncap2b2w3`:

| quantity | result |
|---|---|
| 32-byte pairs classified A/B | **51** (15 A, 36 B) |
| records | **20,312** |
| position bytes differing | **0** |
| normal packed | max **1 LSB**, 0 records > 1 |
| tangent s16 | max **1 LSB**, 0 records > 1 |
| UV f16 | max **1 LSB**, 0 records > 1 |
| SPU attr word (+28..31) | **0** differing records |
| GPU writes outside record bytes 0..27 | **0** bytes |
| ring bytes, position/attribute fields | **0** differing |
| ring bytes, packed normal/UV fields | 1,151 bytes (allowed <= 1 LSB) |
| ring bytes outside records | **0** differing |
| pass | **51/51** |

The ring difference is exactly the allowed packed-field drift; every
position/attribute byte the walker reads is identical.

Regression check on the validated 36-byte pitch job
(`recomp-work/captures/fncap21/0x10F78`): 196 records, **0 differing bytes**,
ring `LS 0x2F000..0x320D0 -> EA 0x11765D00`, **0 ring bytes**.

## 6. Why the live A/B was not run

Two independent blockers:

1. **The live wiring is absent.** `PS3RECOMP_C690_GPU_2A` does not exist in
   `ps3recomp-play` on any branch, and the walker binding in
   `edge-gpu-2a-wiring.md` §4 (GPU ring range, SPU export, timeline, fallback)
   is still design-only. An A/B would only measure the unchanged lifted path.
2. **The §6 fallback gate is not met.** The 32-byte class is 31.2 % of the
   sampled 0x2A pairs, so 68.8 % of 0x2A stays on the SPU; §6 requires
   <= 5 %. The 20/48-byte and the `0xA00004` "other" classes are not decoded.

The 2-launch cap was spent on the corpus, not on an A/B that cannot test the
change. `build-play` was not rebuilt; the game was not running during either
launch or build.

## 7. Time estimate and next stage

Using the clean meter map in [`edge-gpu-phase2b-step5.md`](edge-gpu-phase2b-step5.md)
(24.39 ms critical-lane basis):

| class | share | ms | GPU status |
|---|---:|---:|---|
| 0x21 sel7 (A3E0/BB98) | 19.41 % | 4.74 | no kernel |
| 0x22 tree/cull | 23.79 % | 5.80 | **CPU-native port exists**, not GPU |
| 0x2A all | 14.06 % | 3.43 | 32-byte 31.2 % now exact, rest no kernel |
| 0x25 + 0x26 cull | 12.41 % | 3.03 | no kernel |
| 0x21 sel1 | 3.31 % | 0.81 | no kernel |
| 0x13 copy + 0x0A unpack + 0x27 + 0x1F + setup | ~19 % | ~4.7 | CPU/memcpy/native, not GPU |

The measured 32-byte stage is **~1.07 ms** of the lane (31.2 % of 3.43 ms).
The remaining 0x2A classes are ~**2.36 ms**; the 0x21 sel7 is **4.74 ms**;
the culls **3.03 ms**. The full GPU tail (0x21 + culls + 0x2A) is the
**12.03 ms** already in step 5; the 32-byte kernel is the first measured
piece of it.

**Proposed next stage, in order:**

1. **Walker binding + live key** (`edge-gpu-2a-wiring.md` §4). This is the
   prerequisite for any measured GPU Edge stage. Gate: `replay_rsx_engine`
   draw/state hash identical with the binding on/off on a captured meter
   frame, the GPU range fed through the resolver (not a guest-memory patch),
   added sync <= 1 ms/frame, and the SPU writes the hole/label last.
2. **0x2A 20-byte + 48-byte + the 9 A/B sub-variant pairs** (and then the
   `0xA00004` "other" class), to bring the 0x2A fallback to <= 5 %. Gate:
   <= 5 % of sampled 0x2A pairs on the lifted path, per-class exactness
   (positions exact, packed <= 1 LSB, attr SPU-written), ring position/attr
   bytes exact.
3. **0x21 sel7 (A3E0/BB98)** — the largest single GPU slice (4.74 ms). Gate:
   64/64 meter pairs per class, positions exact, normals/UV <= 1 LSB,
   cull list unchanged, ring bytes.
4. **0x25/0x26 cull** — conservative policy (may keep extra triangles, never
   drop one). Gate: 128-pair replay, 0 removed vs the SPU, ring/count exact.
5. **0x22 whole-body** is the largest slice (5.80 ms) but already has a
   CPU-native port; verify/enable that before writing another GPU kernel.

## 8. Reproduction (offline)

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp

# coverage of the regenerated meter spool
python3 tools/replay/test_c690_2a_plumbing.py --coverage-only \
  --spool /home/m/Documents/mlb16-online/recomp-work/captures/gpu2a/fncap2b2w3

# 32-byte kernel gate (Vulkan, 51 pairs / 20,312 records)
python3 tools/replay/test_c690_2a_32b.py \
  --spool /home/m/Documents/mlb16-online/recomp-work/captures/gpu2a/fncap2b2w3

# 36-byte regression
python3 tools/replay/test_c690_2a_plumbing.py \
  --capture /home/m/Documents/mlb16-online/recomp-work/captures/fncap21/0x10F78 \
  --no-replay
```

The spools, the capture build and the lifted module are game data and are not
committed. The capture build used
`recomp-work/captures/gpu2a/spu_capture/` and `recomp-work/build-gpu`; the
shared `recomp-work/spu_lifted` and `build-play` were not modified.

## 9. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code, our own captures and our own kernel. The
stage/corpus/differential method follows NHL Legacy Native (MIT, (c) 2026
HeckYouMan) with attribution; no NHL code was copied. The SPU-gate/shared-GPU
image pattern is from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and
contributors), design reference only. No RPCS3 or other GPL emulator source
was read or cited.
