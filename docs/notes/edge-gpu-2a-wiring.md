# c690 Edge 0x2A: the meter is not one 32-byte class (coverage stop, no kernel, no wiring)

> **Update 2026-11-05:** §5.1 (index pre-pass contract) and §5.2 (32-byte
> kernel exactness) are done; see [`edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md).
> The 32-byte class is a single-offset gather (u16 byte offsets, or implicit
> `k*16`); the kernel is exact on 51/51 clean pairs (20,312 records), but the
> class is only 31.2 % of the regenerated meter 0x2A corpus and the live key /
> walker binding are still absent, so no §6 A/B was run.


Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. **Offline only: zero game launches.** The data is
the phase-2b meter tail spools `fncap2b2w` and `fncap2b3a730` (64 pairs each;
`PS3RECOMP_C690_TAIL_CAPTURE`), the step-2 `fncap2b2` command-level spool
(32 pairs), `recomp-work/captures/fncap21` (the phase-2a 36-byte pitch job),
the lifted c690 module, and the existing offline harness.

This follows [`edge-gpu-plumbing.md`](edge-gpu-plumbing.md) (whose §5 is this
mission), [`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md) …
[`step5`](edge-gpu-phase2b-step5.md) and
[`edge-exactness.md`](edge-exactness.md).

## TL;DR

* **The mission premise is wrong as a coverage statement.** The meter's 0x2A
  output is **not one 32-byte record class**. Over the two meter spools
  (128 sampled pairs) the record stride is **36-byte 0, position-first
  32-byte 55 (43.0 %), 20-byte 12 (9.4 %), 48-byte 16 (12.5 %), other 45
  (35.2 %)**. The descriptor at LS `0x1F690` selects the layout. A 36-byte
  kernel plus a position-first 32-byte kernel would still leave **57.0 % of
  sampled 0x2A pairs** on the SPU; at 0x2A's clean 44.3 % share of c690 jobs
  that is **~25 % of c690 jobs**, five times the 5 % fallback gate.
* **The 32-byte layout was decoded, but it is not a stride change.** A
  32-byte record is `[pos.xyz f32][normal 11:11:10][4 x s16][2 x f16]` plus a
  4-byte index/attribute pair that the SPU's index pre-pass writes into the
  same record. The assembler builds an index/attribute table first (e.g.
  `0x2B580` in the pair-65 trace) and then assembles each record with
  overlapping 16-byte and 4-byte stores. A GPU kernel must either replicate
  that pre-pass or consume its exported index list; that is the ~1–2 week
  job predicted in `edge-gpu-plumbing.md` §5, not a pack formula.
* **No GPU kernel, no walker binding, no live key was added.** Building the
  kernel on the "all 32-byte" premise would have produced a 43 %-coverage
  kernel and an A/B that measures the fallback. Per the mission's stop rule
  ("if the premise is wrong, stop and report with evidence") the work stops
  at the coverage measurement. §4 gives the design and the per-path handling
  that the next mission needs; §6 gives the exact live A/B plan.
* **Nothing is default-on.** `build-play` was not rebuilt, no `spu_context`
  field was added, and the play scripts are untouched. The only committed
  code change is the coverage classifier in
  `tools/replay/test_c690_2a_plumbing.py` (`--coverage-only`).

## 1. Method

The tail spool records one IN and one OUT full 256 KiB LS + all 128 GPRs per
sampled 0x2A command (`runtime/spu/c690_corpus.c`, `C69STG01`). For each
0x2A pair the classifier reads:

* `r3 = gpr[3]`, the source table at `LS[r3+0x260]` (four 16-byte-stride
  arrays), the output cursor at `LS[r3+0xD0]` in the IN image, and the end
  cursor at the same address in the OUT image;
* `delta = end - out_base`, and the command descriptor at LS `0x1F690`.

The classifier is stride-based, not descriptor-based: the assembler is a
gather, so **every record of a real class starts with the first 12 bytes of
some 16-byte-aligned source entry**. A candidate stride `S` scores the share
of records whose first 12 bytes are such a prefix; `S` is accepted only at
100 %. A wrong stride fails on the first records. The 36-byte class keeps its
strict phase-2a layout test (`[uv.xy][colour u8x4][pos.xyz][nrm.xyz]`).

To decode the 32-byte layout and the pack rules, a private research build of
the lifted module (not committed) traced every `spu_ls_read128` /
`spu_ls_write128` on one captured IN image, then perturbed one source word at
a time (one 16-byte source entry, one word) and diffed the 32-byte record.
This is the same differential method as phase 2a, at word granularity.

## 2. Coverage at the meter

`python3 tools/replay/test_c690_2a_plumbing.py --coverage-only --spool …`:

| spool | 0x2A pairs | 36-byte | 32-byte | 20-byte | 48-byte | other |
|---|---:|---:|---:|---:|---:|---:|
| `fncap2b2w` | 64 | 0 | 26 (40.6 %) | 6 (9.4 %) | 9 (14.1 %) | 23 (35.9 %) |
| `fncap2b3a730` | 64 | 0 | 29 (45.3 %) | 6 (9.4 %) | 7 (10.9 %) | 22 (34.4 %) |
| **meter total** | **128** | **0** | **55 (43.0 %)** | **12 (9.4 %)** | **16 (12.5 %)** | **45 (35.2 %)** |
| `fncap2b2` (command-level check) | 32 | 0 | 17 (53.1 %) | 2 (6.2 %) | 4 (12.5 %) | 9 (28.1 %) |

Consequences for the fallback gate:

* the phase-2a 36-byte kernel covers **0/128** meter 0x2A pairs — it is a
  validation kernel, not a meter fallback reducer;
* a position-first 32-byte kernel covers **55/128 (43.0 %)**;
* with both kernels the remaining sampled 0x2A fallback is **73/128 =
  57.0 %**. The clean meter histogram puts 0x2A at **44.3 % of c690 jobs**
  (`fncap2b2w` `tail_hist.txt`; the `2b3a730` window is distorted by the
  `0xA7` leaf capture). That is **~25 % of c690 jobs** in fallback, versus
  the **<= 5 %** gate.

The descriptor at LS `0x1F690` tracks the class: `0x007C0004` and
`0x00940004` are position-first 32-byte; `0x00C40008` is the 20-byte family
(with some non-multiple deltas, see below); `0x00600008` is 48-byte;
`0x00A00004` and `0x00EC0004` are mixed/other. The descriptor alone is not a
sufficient key because different `0x00C40008` pairs do not all divide by 20
(e.g. delta 3904, 1600, 3072), so the stride/prefix test is what is reported.

## 3. The 32-byte record layout (position-first class)

Decoded from perturbing one captured 32-byte job (`fncap2b2w` seq 60, source
table `pos=0x1F780 nrm=0x21F00 tan=0x24680 uv=0x26E00`) and confirmed on the
`fncap2b3a730` seq 65 job:

| bytes | contents | source | exactness rule |
|---|---|---|---|
| 0..11 | `pos.x, pos.y, pos.z` f32 | table[0] entry | **exact copy** (positions exact) |
| 12..15 | normal packed: x 11 bits (bit 0), y 11 (bit 11), z 10 (bit 22), two's complement | table[1] xyz | <= 1 LSB |
| 16..23 | 4 x s16 big-endian (`enc0<<16|enc1`, `enc2<<16|enc3`) | table[2] xyzw | <= 1 LSB |
| 24..27 | 2 x f16 big-endian (`f16(u)<<16|f16(v)`) | table[3] xy | <= 1 LSB |
| 28..31 | two u16 attribute/index values | SPU index pre-pass | **exact, SPU-written** |

The pack rules recovered from the probes (the `-1` is part of the SPU
conversion, not rounding noise):

```
enc_fixed(v, scale, bits) = clamp(trunc(v * scale) - 1, -(1<<(bits-1)), (1<<(bits-1))-1)
normal.x = enc_fixed(x, 1024, 11)   normal.y = enc_fixed(y, 1024, 11)
normal.z = enc_fixed(z,  512, 10)
tangent  = enc_fixed(c, 32768, 16) for each of the four components
uv       = f16(u), f16(v)
```

For seq 60 record 0 the source normal `(-0.459279, -0.879585, -0.208354)`
encodes to `x=-471, y=-901, z=-108` = `0xE523DE29`, the captured word; with
`floor()` instead of `trunc()` it is off by one. Positions match the source
triple for **all** records of every accepted 32-byte pair (the classifier's
1.0 score).

### 3.1 Why this is not yet a full-pair kernel

The first record of each pair matches the four-planar-array model, but a
whole-pair differential does not: the normal/tangent/uv source entry is not
always at `table[k] + pos_index`. The handler first builds an
index/attribute table in LS (`0x2B580…` in the seq-65 trace, `0x27350…` in
seq 11) and then assembles the record with several overlapping stores,
including the 4-byte index/attribute pair at bytes 28..31. The index pre-pass
is the missing piece; the pack formulas above are proven, the gather/index
contract is not. Reproducing the pre-pass (or exporting its table) is the
next unit of work.

## 4. Walker binding design (implemented: no; design per §2.7)

The binding only makes sense once a kernel produces the ring range for the
class the draw actually reads. It was not implemented because the kernel
coverage fails the 5 % gate and an offline hash proof would be vacuous. The
design, with every path from `edge-gpu-plumbing.md` §2.7:

* **Live key.** `PS3RECOMP_C690_GPU_2A` (default **0**). With the key off the
  walker takes the existing path byte-for-byte: no allocation, no map
  lookup, no extra branch in the draw's vertex/index resolve beyond one
  predicted-not-taken global. The key is per job: an unrecognised layout key
  runs the lifted `0x10F78` body and writes guest memory as today.
* **GPU-produced ring range.** The GPU writes records into a device-local
  buffer that mirrors `[ring_lsa, ring_lsa+size)`; the SPU still allocates
  the ring slot, builds the non-record bookkeeping, and writes the command
  hole/slot last. A range map `(ring EA, size) -> VkBuffer + offset` is
  published at the burst fence, double-buffered per ring slot.
* **CPU vertex fetch** (`rsx_draw_engine.c` decode + `rsx_vertex_compact.c`
  / `read_vp_vertex`, and the bulk staging copy in
  `rsx_vulkan_backend.c`): add a range resolver at the point the draw
  resolves its vertex array. If the draw's vertex reads fall entirely inside
  a live GPU range, bind the device-local buffer and skip both the CPU
  decode and the staging copy. If not (partial overlap, fallback job, key
  off), use the existing guest-memory path unchanged.
* **Index list** (`rsx_draw_engine.c` CPU decode): the 0x2A output is the
  vertex buffer, not the index list. It stays CPU-decoded in this design;
  the range resolver only changes the vertex source. A future index kernel
  would add a second resolver, not reuse this one.
* **WCB readback** (`eng_guest_ptr` / `eng_wcb_access`): the mesh ring is a
  vertex buffer, not a render target. The resolver must refuse to alias a
  range that is ever a WCB source/target; keep the guest copy for those.
* **SPU submit epilogue PUT** (`0xDF40`): skip the PUT only for the
  record range, keep the header/trailer/index bytes. The walker resolves the
  ring EA to the GPU buffer instead of reading the (now stale) guest range.
* **PPU**: no PPU code reads post-transform vertices (phase 1); the PPU only
  polls job counters. No change.
* **Sync.** One dispatch per burst, one timeline semaphore, a walker wait
  only at draws that resolve a live GPU range. No per-job fence and no
  payload readback. The SPU must not release the slot before the burst
  fence (either wait at the tail, or let the walker wait on the timeline).

Offline gate for the next mission: replay the captured meter frame through
`replay_rsx_engine` with the key off and with the key on over a **patched
GPU ring buffer**, and require the same draw/state hash. The current
`test_c690_2a_plumbing.py` already does this with a guest-memory patch; the
next version must feed the same bytes through the resolver instead of
patching `.mem`, so the proof exercises the new path.

## 5. What the next mission must build first

1. **Index-pre-pass contract.** Decode the table built at `0x2B580`/`0x27350`
   and either replicate it in the GPU kernel or export it from the SPU (the
   SPU could keep the pre-pass; it is not the record pack). Gate: the same
   stride classifier reports 100 % of the accepted 32-byte pair records with
   the exported index list.
2. **32-byte kernel exactness.** Write the records from the exported index
   list: positions exact, normal/tangent/uv <= 1 LSB, bytes 28..31 untouched
   (SPU-written). Gate: 64/64 `fncap2b2w` + 64/64 `fncap2b3a730` pairs,
   max 1 LSB on the packed fields, 0 differing position bytes, and the ring
   bytes as in `test_c690_2a_plumbing.c`.
3. **20-byte and 48-byte classes** if the gate is still to be met. They are
   separate layouts (the 20-byte family writes
   `[pos.xyz f32][w][0xE000E000]`; the 48-byte family is still
   unclassified). Without them the fallback stays above 50 % of 0x2A.
4. **Then** the walker binding (§4) and the live A/B (§6).

## 6. Exact live A/B plan for the next mission

Only after §5.2 (and ideally §5.3) passes:

```sh
# A: key off (default), B: key on; same build, 5 x 10 s meter segments,
# 2 s dropped; PIN=0, WCB=2, x11.
cd /home/m/Documents/mlb16-online/recomp-work
TAG=gpu2a32 N=5 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 PITCH_THRESH=35 LIMIT=420 BOOT=40 \
  ARMS='A= B=PS3RECOMP_C690_GPU_2A=1' ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_gpu2a32_*.log \
  --marks ab_live_gpu2a32_*.marks --seg 10 --drop 2
```

Report p50/p90/p99 and the 40-50 ms share; the pass conditions are the usual
ones (p50 unchanged or lower, p90/p99 no worse, 40-50 ms share down) **and**
the `[c690-stats]` tail capture showing the per-job fallback rate at <= 5 %.
Capture one meter `replay_rsx_engine` frame in the same run and require the
same draw/state hash with the key on and off. No game launch is authorized
until the offline gates above pass.

## 7. Reproduction (offline)

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp
python3 tools/replay/test_c690_2a_plumbing.py --coverage-only \
  --spool /tmp/opencode/fncap2b2w \
  --spool /tmp/opencode/fncap2b3a730 \
  --spool /tmp/opencode/fncap2b2
```

The spools are game data and are not committed. The lifted module and the
private tracer are also not committed. The 36-byte harness is unchanged:

```sh
python3 tools/replay/test_c690_2a_plumbing.py \
  --capture /tmp/opencode/fncap21b/0x10F78 --no-replay
```

## 8. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code and our own captures. The stage/corpus/differential
method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern is
from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors), design
reference only. No RPCS3 or other GPL emulator source was read or cited.
