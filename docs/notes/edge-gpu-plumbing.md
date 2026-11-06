# c690 Edge on the GPU — 0x2A plumbing (chain accounting, design, offline proof)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. **Offline only: no game launch.** The data is the
60 s clean meter window of the existing `ab_live_copy1_20261104_120526` run
(already reported in [`c690-copy-unpack.md`](c690-copy-unpack.md)), the
captured meter jobs from phase 2a/2b, the lifted c690 module, and a new
offline harness that runs the SPU path, the GPU 0x2A kernel, the ring copy
and the `replay_rsx_engine` walker.

This follows [`edge-gpu-phase1.md`](edge-gpu-phase1.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md) …
[`step5`](edge-gpu-phase2b-step5.md),
[`c690-copy-unpack.md`](c690-copy-unpack.md),
[`c690-emit-ring.md`](c690-emit-ring.md) and
[`c690_function_map.md`](c690_function_map.md).

## TL;DR

* **The rest of the map is control work, not GPU work.** The
  `0x14/0x0E/0x0F/0x18..0x1D/0x23` chain is **20.75 us/job, 11.21 %** of the
  c690 wall in the clean 60 s window (907,390 jobs, 167.96 s wall), not
  ~26 us/13 %. The per-command averages sum to 29.98 us, but 0x14/0x0F/0x23
  run only 0.18 times/job and 0x18/0x19 only 0.75; weighting by calls/job
  gives the lower number. **0x1A is the only DMA handler** (6.56 M small MFC
  GETB copies, 793 ms, 0.4 % of the wall); **0x1E is a DMA tag wait** (no
  payload); every other handler is SPU/LS ring/list/stream control. None is
  a GPU compute kernel. The GPU-able part of the map remains the geometry
  tail `0x21/0x25/0x26/0x2A`.
* **The 0x2A plumbing is proven offline.** For the captured 36-byte pitch
  job (`fncap21b/0x10F78`) the SPU assembler writes **196 records (7,056
  bytes)** at LS `0x2F870`; the exported planar inputs are **12,544 bytes**,
  the GPU batch is one dispatch, and the GPU records are **0 differing
  bytes** from the SPU records. Applying the real submit PUT mapping
  (`LS 0x2F000..0x320D0 -> EA 0x11765D00`, from corpus job
  `corpus_edge3s1/job_000000`) gives **0 differing ring bytes**. A captured
  meter frame replayed through `replay_rsx_engine` gives the **same output
  hash** for the SPU and GPU record bytes.
* **Do not wire it live yet.** The meter's captured 0x2A tail is a different
  format: **0 of 128** sampled 0x2A pairs are the validated 36-byte class;
  they are a 32-byte record class (0x2F8A0/0x2F210 style, 32-byte stride).
  A 36-byte-only kernel therefore has a fallback far above the 5 % gate.
  The walker binding is also not small: the CPU vertex fetch and the
  GPU-unpack staging copy both read the ring from guest memory, and the
  index list is decoded on the CPU. **Stop after the offline proof; the
  wiring plan and honest estimate are in §6.** Nothing was enabled, no
  `spu_context` field was added, and `build-play` is unchanged.

## 1. The rest of the map: 0x14/0x0E/0x0F/0x18..0x1D/0x23

### 1.1 Where the numbers come from

The clean 60 s meter window is the delta of the periodic `[c690-stats]`
blocks at elapsed 164.2 s and 224.2 s of
`ab_live_copy1_20261104_120526.log` (the same window as
[`c690-copy-unpack.md`](c690-copy-unpack.md) §1.1): 907,390 jobs,
167,957,693 us of c690 lane wall. The complete 41-entry opcode map is in
`runtime/spu/c690_stats.c` (`s_op_map`); the per-command inclusive time is
the table the report prints. No new launch was made.

### 1.2 The chain

| op | handler | calls | calls/job | us/call | us/job | share | what it does | class |
|---|---:|---:|---:|---:|---:|---:|---|---|
| 0x0E | `0x11AA8` | 678,040 | 0.747 | 0.78 | 0.58 | 0.32 % | ring ticket + 112-byte header/copy through `0x11990` | SPU/LS |
| 0x0F | `0x11A90` | 98,961 | 0.109 | 0.74 | 0.08 | 0.04 % | ring ticket + 64-byte header/copy through `0x11990` | SPU/LS |
| 0x14 | `0x11CD8` | 165,814 | 0.183 | 0.96 | 0.18 | 0.09 % | stream/descriptor setup, LS copy (`0x11D54/0x11DC0`) | SPU/LS |
| 0x18 | `0x053C0` | 677,340 | 0.746 | 3.86 | 2.88 | 1.56 % | stream/list builder; four `0x5680` block copies + `0x5BC0` | SPU/LS |
| 0x19 | `0x04F00` | 677,340 | 0.746 | 5.44 | 4.06 | 2.19 % | list/stream walk; indirect to `0x49B0/0x4BC4/0x4BD4/0x4F98/0x4FB0/0x50FC/0x5F80` | SPU/LS |
| 0x1A | `0x129F0` | 985,850 | 1.086 | 1.23 | 1.34 | 0.72 % | **MFC GETB (`0x44`)** of a small guest range into LS; 6.56 M copies/60 s, 793 ms | **DMA** |
| 0x1B | `0x0F520` | 985,850 | 1.086 | 4.17 | 4.53 | 2.45 % | ring allocator `0xE6B8` + packet builder `0xF5D0/0xF600/0xF628/0xF638` | SPU/LS |
| 0x1C | `0x11BC0` | 985,851 | 1.086 | 0.74 | 0.81 | 0.44 % | ring ticket + small header copy through `0x11990` | SPU/LS |
| 0x1D | `0x0F680` | 985,851 | 1.086 | 4.11 | 4.46 | 2.41 % | ring allocator + tree/list state machine `0xF6F8..0xFA14`, `0x6780`, `0x4100`; two stop paths | SPU/LS |
| 0x1E | `0x12A60` | 985,850 | 1.086 | 0.42 | 0.46 | 0.25 % | **MFC tag wait**: `WrTagMask` / `WrTagUpdate` / `RdTagStat` loop | **DMA wait** |
| 0x23 | `0x12DC8` | 165,814 | 0.183 | 7.53 | 1.38 | 0.74 % | stream/list/extra-block state machine `0x8C80`, `0x12E10..0x13004`; stop path | SPU/LS |
| **sum** | | | | | **20.75** | **11.21 %** | | |

On the documented 24.39 ms critical-lane basis that is **2.73 ms/frame**;
on this run's own 20.2 ms lane basis it is **2.26 ms/frame**.

### 1.3 The premise correction

The step-5/copy-unpack narrative quoted "~26 us/job, ~13 % of the wall".
The run's data does not support that as a per-job number:

* the **per-command average sum** of the eleven rows is **29.98 us**;
* the **calls/job weighting** is what matters: 0x14/0x0F/0x23 occur only
  0.183/0.109/0.183 times per job, and 0x18/0x19 only 0.746. Weighted, the
  chain is **20.75 us/job** and **11.21 %** of the c690 wall.
* 0x1A's 6.56 M copies are 0.4 % of the wall in the all-DMA table; 0x1E
  issues no copies (it waits on tags). The chain has no hidden long DMA.

So the chain is real but **~1.8 ms/frame smaller than the quoted 26 us/13 %**
on the 24.39 ms basis, and it is not a GPU target: it is serialized ring,
list, stream and tag control. A native CPU rewrite of individual handlers
could shave some of the 20.75 us, but the per-handler costs are 0.4-7.5 us
and the handlers are control-flow heavy, so there is no GPU-compute win.

## 2. 0x2A plumbing: design

### 2.1 What the SPU must still do

The 0x2A handler (`0x10F78`) is not just the vertex loop. The SPU must keep:

* **Parser and command boundary** (`0x10700`, `s_op_map`): the job's command
  stream, the opcode dispatch and the next-command state.
* **The handler prologue/epilogue bookkeeping**: the LS allocator `0x49D4`,
  the output cursor `ctx+0xD0`, the context words `ctx+0xA0/0x260/0x2A0/0x2D0`,
  the allocator counters at `0x1F300/0x1F340/0x1F430`, the scratch block at
  `0x28FB0` and the stack. The GPU only replaces the record writes.
* **The ring ticket / command slot**: the ring allocator (`0xE6B8`) and the
  command/self-JUMP hole. The SPU writes the slot **last**, after the data is
  visible, so the walker's park is still the ordering edge.
* **The submit epilogue PUT** that copies LS `[ring_lsa, ring_lsa+size)` to
  the ring EA (`0xDF40` bulk-copy path; the real PUT in corpus job
  `corpus_edge3s1/job_000000` is `LS 0x2F000..0x320D0 -> EA 0x11765D00`).
  In the alias design this copy is skipped for the record range and the
  walker binds the GPU buffer instead.
* **The per-job fallback**: if the layout/format key is not the validated
  36-byte class, run the lifted `0x10F78` body exactly as today.

### 2.2 What the SPU skips

The per-vertex gather/pack loop `0x7840 -> 0x78C0 -> 0x7960/0x7998` and the
colour pack `0x7D40/0x7D7C/0x7DA8/0x7E58/0x7E98` — i.e. the writes to the
36-byte records `[UV0.xy][COLOR0 u8x4][POSITION.xyz][NORMAL.xyz]`. The SPU
still writes the non-record bytes (cursor, trailer, index/count table) that
the handler produces around the records.

### 2.3 What it exports per job

* **Four planar arrays**: normal at `ctx+0x260[0]`, position at `[1]`, colour
  at `[2]`, uv0 at `[3]`, each `count * 16` bytes, big-endian 32-bit lanes.
  For the captured pitch job: 196 records, **12,544 bytes**.
* **A job table**: the four compact offsets, `out_base`, `count`. The GPU
  kernel needs nothing else from the 256 KiB LS.
* **Where**: a per-lane export buffer in guest memory (main), double-buffered
  per ring slot. In the offline harness the compact buffer is 19,600 bytes
  (12,544 arrays + 7,056 output); in a live burst it is one small PUT per job
  (or a direct GPU read of the SPU LS if the LS is shared). The SPU's export
  is `count * 16 * 4` bytes, not a full LS image.

### 2.4 How the GPU result lands at the ring EA

Two equivalent shapes; the first is the option (i) target:

* **Alias the ring range.** The GPU writes the 36-byte records into a
  device-local buffer that mirrors the ring range. The walker binds that
  buffer for draws whose EAs fall in the ring range and does not read the
  guest ring for those draws. The SPU's submit PUT is skipped for the record
  range; it still writes the non-record bookkeeping.
* **Per-burst device-local copy.** The GPU writes the records into a
  device-local buffer; at the burst fence the buffer is copied (GPU copy or
  CPU) into the guest ring, and the walker continues to read guest memory.
  This keeps the existing walker read path but adds a per-burst copy; it is
  a fallback shape, not the preferred one.

The ring copy is a pure byte copy of LS `[ring_lsa, ring_lsa+size)`. The GPU
records must be byte-identical to the SPU records at `out_base`; the harness
proves this for the validated class.

### 2.5 Sync point

* **One GPU submission per burst** (429 jobs), not per job: the 0x2A kernel is
  0.23-0.27 ms per 429-job burst device-local (0.54-0.62 us/job), so a
  per-job fence would be 14 ms/frame and is ruled out.
* **Ordering edge**: the SPU's hole patch / slot release is the existing
  park edge. The SPU must not patch the slot until the GPU writes are
  visible. Two valid orders:
  1. **Burst fence before the hole patch**: the SPU waits for the burst
     fence, then patches the slot. This puts the burst GPU time at the end of
     the SPU lane tail (the kernel is ~0.3 ms, acceptable), or
  2. **Walker timeline wait**: the walker, when it reaches a draw that reads
     the GPU ring, waits on the burst's timeline semaphore before recording
     the draw. This keeps the GPU off the SPU lane and makes the wait
     per-draw, not per-job.
* **Flip deadline**: the hard bound is the walker's draw for the ring slot
  before the flip. The GPU pass must complete by then; the offline harness
  uses one dispatch + fence, which is the conservative shape.
* **No per-job fence and no payload readback.**

### 2.6 Fallback per job

The layout key is the source table order, the output record size (36 vs 32
bytes), the descriptor/format word and the count. If the key is not the
validated 36-byte class, the SPU runs the lifted `0x10F78` body. The GPU
buffer is double-buffered per ring slot so a mid-frame switch cannot expose
partial data. The fallback is per job, not per frame.

### 2.7 Paths that break option (i)

These are the places where the walker or CPU reads the ring contents; each
must be changed or the GPU buffer cannot replace guest memory:

* **CPU vertex fetch**: `rsx_draw_engine.c` decodes vertices on the CPU
  (`rsx_vertex_compact.c`, `read_vp_vertex`) or bulk-copies the raw bytes
  into a staging buffer for the GPU unpack (`rsx_vulkan_backend.c:5131-5196`).
  Both read guest memory. The walker needs a path that reads the GPU buffer
  (or the GPU unpack must read the GPU buffer directly).
* **Index list**: always read and decoded on the CPU
  (`rsx_draw_engine.c:5006-5024`). The 0x2A output is the vertex buffer, not
  the index list, but a future stage that moves the index list also hits
  this.
* **WCB readback**: `eng_guest_ptr` / `eng_wcb_access` can copy guest memory
  for targets. The mesh ring is a vertex buffer, so this is not on the 0x2A
  path today, but the mechanism is a risk if the ring range is ever a target.
* **SPU submit epilogue PUT**: if the SPU skips it, the guest ring memory is
  stale; the walker must not read guest memory for the GPU-covered range.
* **PPU**: no PPU code reads post-transform vertices (phase 1); the PPU only
  polls job counters. This side is safe.

## 3. Offline implementation

### 3.1 Files

* `tools/replay/test_c690_2a_plumbing.c` — the C harness. It runs the lifted
  SPU `0x10F78` on the captured IN LS/GPRs (reference), derives the source
  table and output cursor, exports the four arrays to a compact buffer,
  runs `c690_2a_assembler.comp` on it with one dispatch, compares the GPU
  records with the SPU records, applies the ring mapping to both LS images
  and compares the ring bytes, and writes the SPU/GPU records and ring
  images.
* `tools/replay/test_c690_2a_plumbing.py` — the orchestrator. It builds and
  runs the C harness, classifies the captured meter 0x2A pairs (36-byte vs
  other), patches the captured `meter_frame` guest memory with the SPU and
  GPU record bytes, and replays both through `replay_rsx_engine` to compare
  the output hashes.

Both are research tools. The capture spools and the lifted module are game
data and are not committed.

### 3.2 Exactness on the captured 36-byte pitch job

Input: `/tmp/opencode/fncap21b/0x10F78` (the phase-2a meter capture, real
pitch job, `r3=0x1F000`, source table
`nrm=0x1F6B0 pos=0x20330 col=0x20FB0 uv=0x21C30`).

| quantity | value |
|---|---:|
| output base | LS `0x2F870` |
| output cursor after the handler | `0x31400` |
| records (derived) | **196** (7,056 bytes) |
| exported arrays | 12,544 bytes |
| compact export buffer | 19,600 bytes |
| GPU vs SPU records | **0 differing bytes** |
| GPU writes outside the output | **0 bytes** |
| ring mapping | `LS 0x2F000..0x320D0 -> EA 0x11765D00` |
| ring bytes | **0 differing** |

The 196 records include the allocator's 16-byte-rounded tail (the phase-2a
"193 valid" was the draw count; the handler writes 196). The GPU kernel is
exact over all 196, so the full ring copy is exact.

### 3.3 Walker hash on the captured meter frame

`recomp-work/meter_frame` (the 109,318-word meter frame; 502 draws) was
replayed through `replay_rsx_engine` with the 7,056 record bytes patched into
a captured vertex array (`location=1`, `offset=0xc5a310`) that the walker
reads:

| arm | output hash | guest reads |
|---|---|---:|
| unpatched baseline | `a6fcc849341eba9b` | 11,175 hit / 0 miss |
| SPU record bytes | `d73c469bb3578448` | 11,175 hit / 0 miss |
| GPU record bytes | `d73c469bb3578448` | 11,175 hit / 0 miss |

The baseline differs (the patch is visible), and the SPU and GPU hashes are
identical. This is the offline stand-in for binding the GPU buffer: the
walker sees the same bytes and produces the same draw/state hash. It is a
guest-memory patch, not a Vulkan buffer binding; §2.7 lists the walker
changes a real binding needs.

### 3.4 Coverage of the meter's 0x2A jobs

The phase-2b meter tail spools (`fncap2b2w`, `fncap2b3a730`, 64 pairs each)
were classified by the source table and the first output record:

| spool | 0x2A pairs | 36-byte (validated) | other (32-byte) |
|---|---:|---:|---:|
| `fncap2b2w` | 64 | **0** | 64 |
| `fncap2b3a730` | 64 | **0** | 64 |

The captured meter 0x2A tail is a **32-byte record class** (e.g. output base
`0x2F8A0`, delta `16000` = 500 x 32; the first 12 bytes are the source
table[0] normal, not the 36-byte `[uv][color][pos][nrm]` layout). The
validated 36-byte class is the pitch job captured in phase 2a; the tail
spool's jobs are a different format. The 36-byte kernel is exact for its
class but does not cover the meter's sampled 0x2A format.

## 4. Verdict

* **Step 3 is exact for the validated 36-byte class.** The SPU skip/export,
  the GPU batch, the ring copy and the walker hash all pass; the plumbing is
  proven offline.
* **Do not wire `build-play`.** The 36-byte kernel's fallback over the
  meter's sampled 0x2A jobs is **>5 %** (0/128), and the walker binding is
  not small/safe: the CPU vertex fetch and the GPU-unpack staging copy read
  the ring from guest memory, and the index list is CPU-decoded. The added
  sync (one burst fence, ~0.1-0.3 ms) is within the 1 ms/frame gate, but
  format coverage and the walker read path are the blockers.
* **Nothing is default-on.** No runtime key was added, no `spu_context`
  field was added (the ABI check is unchanged), and `build-play` is
  untouched.

## 5. Wiring plan (if the 32-byte class is taken on)

1. **Reverse-engineer the 32-byte record layout** from the captured tail
   pairs (the same differential method as the 36-byte kernel). The first
   12 bytes are the source table[0]; the remaining 20 bytes are a pack of
   the other arrays. Gate: 64/64 pairs byte-exact, then the whole meter
   tail.
2. **Add a walker binding** for the GPU ring buffer: a `rsx_draw_engine`
   path that resolves the ring EA range to the device-local buffer and
   feeds the vertex fetch/unpack from it, without the CPU staging copy.
   Gate: the `replay_rsx_engine` hash is identical with the binding on/off
   for a captured meter frame.
3. **Add the burst timeline**: one submission per burst, a timeline
   semaphore, and a walker wait only at draws that read the GPU ring. Gate:
   added sync <= 1 ms/frame.
4. **Live key, default OFF**: a `PS3RECOMP_C690_GPU_2A` live key; fallback
   per job on the layout key. One pitch-meter A/B only after 1-3 pass.

Honest estimate: the 32-byte kernel is ~1-2 weeks (it is a different pack
chain, not a one-line stride change); the walker binding is ~1-2 weeks
because the CPU fetch/staging path is the consumer; the burst timeline is
small. The 0x2A-only win is **~3.4 ms of the 24.39 ms critical lane**
(14.06 % of the c690 wall), not a frame-rate move on its own. The full tail
still needs the 0x21 format kernels (the A730/A3E0/BB98 classes, ~1-2 weeks
each) and the `0xD080` cull (~1-2 weeks). This plumbing is the reusable
piece for all of them.

## 6. Reproduction

```sh
# exactness + ring bytes (the captured 36-byte pitch job)
cd /home/m/Documents/mlb16-online
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I ps3recomp-play/runtime/spu -I ps3recomp-play/include \
  -I mlb16-recomp/tools/replay \
  -o /tmp/opencode/gpu2a/test_c690_2a_plumbing \
  mlb16-recomp/tools/replay/test_c690_2a_plumbing.c \
  mlb16-recomp/tools/replay/reg_10f78.c \
  recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  ps3recomp-play/runtime/spu/job_c690_native.c -l:libvulkan.so.1 -lm
/tmp/opencode/gpu2a/test_c690_2a_plumbing \
  /tmp/opencode/fncap21b/0x10F78 \
  mlb16-recomp/tools/replay/c690_2a_assembler.comp \
  /tmp/opencode/gpu2a/c690_2a.spv --out-prefix /tmp/opencode/plumb

# coverage + walker hash (meter frame + replay_rsx_engine)
cd mlb16-recomp
python3 tools/replay/test_c690_2a_plumbing.py \
  --spool /tmp/opencode/fncap2b2w --spool /tmp/opencode/fncap2b3a730
```

The spools are `/tmp/opencode/fncap21b` (phase 2a), `/tmp/opencode/fncap2b2w`
and `/tmp/opencode/fncap2b3a730` (phase 2b); they are game data and are not
committed. The ring mapping is the real PUT in
`recomp-work/corpus_edge3s1/job_000000.c69`:
`PUT ea=0x11765D00 lsa=0x2F000 size=0x30D0`.

## 7. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code and our own captures. The stage/corpus/differential
method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern
is from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors),
design reference only. No RPCS3 or other GPL emulator source was read or
cited.
