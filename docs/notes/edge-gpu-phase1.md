# c690 Edge on the GPU — phase 1 (offline data flow, design, prototype status)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. **Offline only: no game launches.** Static analysis
of the EBOOT (Ghidra) and of our lifted c690 module, our own runtime/walker
code, the existing corpora, and one offline Vulkan compute microbenchmark.

This follows [`c690-offload-decision.md`](c690-offload-decision.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690_function_map.md`](c690_function_map.md),
[`edge-native-step1.md`](edge-native-step1.md) … [`step5`](edge-native-step5.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`frame-pacing-floor.md`](frame-pacing-floor.md) and
[`nhl-lessons.md`](nhl-lessons.md) §1.6.

## TL;DR

* **No CPU code reads post-transform vertices or post-cull counts.** The five
  2 MB mesh rings and the index list are read only by the RSX walker (our
  code) when it decodes the draw; the guest PPU only polls per-SPU job-list
  completion counters and copies vertex/index **addresses** into RSX methods.
  The job's own 0x21 output *is* read by later SPU commands in the same job
  (cull 0x25/0x26, index 0x27, assembler 0x2A), so "move only 0x21" needs a
  GPU→LS readback on the job's critical path.
* **The output-to-consumer hand-off is already the inFAMOUS shape**: the job
  writes the data first, then patches a self-JUMP command "hole" last; the
  walker cannot execute the draw until that hole is patched, and the ring
  labels stay unchanged. A GPU producer can keep that contract: write the
  vertex/index data, then release the slot (or an indirect command), with no
  CPU read of the payload.
* **The 0x21 kernel is 390 µs/call for selector 1** (58.2 % of all 0x21
  time; 0x21 is 42.9 % of the c690 wall). A per-job CPU fence is now
  **32.8 µs p50** (was 41 µs), but 429 jobs/frame × that is ~14 ms/frame if
  serialised, and the SPU cannot continue past 0x21 without the result.
  Option (iii) (GPU 0x21 + LS readback) is therefore fence-bound and cannot
  reach the target; option (ii) (GPU compute + small command-block readback)
  works only if the heavy data stays GPU-resident and the readback is batched
  **per frame/burst**, not per job.
* **Recommendation: option (i) for phase 2**, a GPU-resident geometric tail
  (unpack → transform/pack → cull → assemble → final ring), with the SPU
  keeping ring allocation, the command slot/indirect list and the fallback.
  Kill criterion in §5. The first two-week spike is one pitch format
  (0x21 sel 1, 384 vertices) plus the walker binding a GPU ring buffer.
* **Prototype status:** the offline harness and captured state exist (a real
  0x13140 entry, 384 vertices / 96 iterations, output
  `0x1F750..0x20F4B`; both the A730 and BB80 format paths captured from the
  existing corpus). The Vulkan fence/dispatch cost is measured. A bit-exact
  GPU shader for the BB80 pack was **not** finished: the offline corpus only
  witnessed the A730 variant, and forcing the BB80 format bit gives
  unvalidated inputs. The exact one-run capture command for the user is in
  §6. Nothing was enabled or added to the default build.

## 1. What the job produces

### 1.1 Producer

The c690 job (module `jobmod_c690c2f7b09a`, LS base 0x4000, entry LS
0x135B8) is a command-stream consumer. Per pitch job the measured order is
`04,07,08,0A*N,0C,13,1F,21,[25|26],27,2A` (19-21 layouts in the corpus).
The important stages for the GPU question:

| stage | LS | role | measured share of job wall |
|---|---|---|---:|
| 0x0A / 0x10B88 | 0x12478 | bind stream, unpack/convert (12/12/11 SNORM etc.) | 7.5 % |
| 0x13 | 0x11A60 | 16-byte copy | 9.7 % |
| **0x21 sel 1/7** | **0x13080 → 0x13140/0x131A0** | **float transform/pack (390 / 52 µs per call)** | **42.9 %** |
| 0x25/0x26 | 0x13520/0x13518 → 0x132E0 | screen cull → kept-index list | 12.7 % |
| 0x27 | 0x11790 | index copy + output sizing | 1.5 % |
| 0x2A | 0x10F78 | 36-byte vertex assembler at LS 0x2F870 | 15.7 % |

Shares and per-call means from [`c690-emit-ring.md`](c690-emit-ring.md) /
[`edge-native-step5.md`](edge-native-step5.md); 0x21 was measured to issue
**0 DMA, 0 GETLLAR/PUTLLC, 0 tag/atomic reads, 0 decrementer reads** in
619,067 calls. It is pure LS compute.

### 1.2 Outputs and who reads them

| output | where it lands (guest) | consumer | when |
|---|---|---|---|
| transformed/packed vertex records | five 2 MB mesh rings, EA `0x11900000`–`0x12300000`, RSX IO `0x00900000`–`0x012fffff` | **RSX walker only** (`rsx_draw_engine.c` fetch/index/decode, Vulkan GPU-unpack) | when the FIFO draw executes |
| kept-index list / draw count | index buffer inside the same rings; count in the command slot | **RSX walker only** (`rsx_draw_engine.c:5006-5024`) | same |
| 0x90-byte per-job command slot | PPU command ring, RSX IO `0x013xxxxx`–`0x014xxxxx` | **walker** as ordinary FIFO words (`cellGcmSys.c:3518-3545`) | as soon as it is patched |
| ring labels 72-76 (`0x480`–`0x4c0`) and the buffer ref | guest labels / GCM context | SPU reads the label for free space; the walker executes the slot's `RELEASE`; the PPU uses the ref for ring reuse | per job / per frame |
| per-SPU job-list counters | job-list header `+0x40..+0x4e` | **guest PPU** (`WwsJobList_SpinUntilDone`, `FUN_009fb898`) | before the next job-list reuse |

Static PPU evidence (Ghidra, EBOOT):

* `FUN_000730c8` (ICE draw builder) reads only descriptor **counts and EAs**
  (`*puVar27`, `puVar27[1]`, `puVar27+6`) and writes RSX methods; it never
  dereferences a vertex/index buffer.
* `FUN_00070e14` (per-stream descriptor) reads only stream metadata.
* `FUN_0006f0f8` allocates the mesh pipeline, the two job lists, the
  0x380000-byte command buffer and the per-SPU ring bases
  (`[mesh+0x18+4*i]`, `[mesh+0x38+4*i]`, stride `[mesh+0x58]`), and stores
  those **EAs** in the job header (`[job+0x20]` ring start, `[job+0x22]` label
  EA). `FUN_0006f6b0` kicks the lanes with `cellSpursReadyCountStore`.
* `FUN_0006ed24` → `FUN_009fbe10`/`WwsJobList_SpinUntilDone` reads only the
  per-SPU `ushort` counters and `cellSpursGetWorkloadInfo`. It never reads
  geometry.

Runtime evidence:

* Every SPU output PUT is synchronous and goes through
  `gcm_note_spu_put` (`cellGcmSys.c:1962-1981`); it maps the EA, traces it,
  records an SPU block and wakes the walker, but does **not** decode payload.
* An unfilled command hole is a self-JUMP; the walker parks
  (`cellGcmSys.c:3287-3331`) and cannot execute the draw until the job
  overwrites it. The job writes the data first and the slot last, so the
  walker sees data before commands **provided the guest protocol is kept**.
  `GCM_SEMA_ACQUIRE` is off by default; the park is the ordering edge.
* On the walker's vertex path the CPU either decodes the vertices
  (`rsx_vertex_compact.c` / `read_vp_vertex`) or, on the GPU-unpack path,
  bulk-copies the raw bytes into a staging buffer and dispatches a compute
  shader (`rsx_vulkan_backend.c:5131-5196`). The index list is always read
  and decoded on the CPU (`rsx_draw_engine.c:5006-5024`).

**Answer to the key question:** no guest PPU or HLE code reads post-transform
vertex data or post-cull counts. The only CPU consumers are the walker's
fetch/decode (and the GPU-unpack staging copy). The PPU reads only job
completion counters, and the job-list wait happens **before the next job-list
reuse**, not before the draw. The walker sees the slot only after the data is
written.

### 1.3 Why "move only 0x21" is not enough

The 0x21 output is written to LS (`0x1F750..0x20F4B` in the captured sample),
and the *same job* immediately runs 0x25/0x26 (cull), 0x27 (index) and 0x2A
(assembler) on it. Those stages are 30 % of the job wall. A GPU 0x21 that
leaves its result in VRAM would have to read it back into LS before the SPU
can continue; that is one fence + transfer per job on the job's critical
path. This is the central difference from the inFAMOUS SSAO/lighting case,
where the gated SPU function is a sink (the frame's consumer runs later on the
same RSX queue and can read shared GPU images).

## 2. Design comparison

All estimates use the meter's measured shape: 429 jobs/frame, five lanes,
critical-lane busy 24.39 ms, inter-job gaps 10.61 ms, burst wall 35.00 ms,
frame p50 34.15 ms ([`pin-and-burst-path.md`](pin-and-burst-path.md),
[`frame-pacing-floor.md`](frame-pacing-floor.md)). 0x21 is ~10.5 ms of the
critical lane ([`c690-emit-ring.md`](c690-emit-ring.md)).

### (i) inFAMOUS-style gate, walker consumes GPU buffers directly

**Shape.** GPU pass runs the geometric tail for a job: read the job's packed
attribute stream / planar arrays, transform, pack, cull, assemble the 36-byte
records and the index list into a Vulkan buffer that mirrors the mesh ring.
The SPU keeps the job ABI, the ring allocation and the command hole; if the
GPU pass succeeds, the heavy SPU stages are skipped. The walker recognizes
draws whose EAs fall in the five ring ranges and binds the GPU buffer
instead of re-reading guest memory. If the GPU pass fails, the SPU runs the
lifted tail (fallback), exactly the inFAMOUS `spu_gate_function` pattern.

**Sync points.** One GPU submission per job (or one batched submission per
frame/burst). The command slot is patched only after the GPU writes are
visible; use a timeline semaphore / external fence or a same-queue ordering
barrier. The walker waits on the slot park as today; it additionally waits on
the GPU timeline value before recording the draw. **No payload readback.**

**Latency vs flip deadline.** The GPU pass is off the SPU critical lane; the
only new latency is GPU execution plus one queue wait, and it can be recorded
before the walker reaches the draw. The 10.5 ms of 0x21 plus 0x2A/cull/0x13/
0x0A (up to ~21.6 ms of the 24.39 ms critical lane) leaves the lane; the
burst becomes mostly the inter-job gaps plus dispatch. Best case burst
~15-20 ms, frame p50 ~21-25 ms. This is the only option that can approach the
21 ms target.

**Exactness.** GPU FP32 is RNE; SPU is RZ+FTZ. Expect positions ~1e-5,
packed normals ~1 LSB, and rare borderline cull triangles (NHL's accepted
profile: 94.5 % exact normals, 5/216 segments with one borderline triangle).
A bit-exact mode needs RZ emulation (double or integer) and would cost most
of the gain; accept ~1 LSB for positions/packed data and make the cull
conservative (union) or log/fallback on borderline cases.

**Fallback.** Per-job layout key (descriptor words, batch range, format
signature). Unsupported layouts run the lifted tail. The GPU buffer is
double-buffered per ring slot so a mid-frame switch cannot expose partial
data.

**Effort.** The hard part is the format coverage, not the Vulkan plumbing.
One pitch format (sel 1, 384 vertices) is a 2-week spike; the full observed
format set (0x0A/0x13/0x21 sel 1/7/0x25/0x26/0x27/0x2A) is 4-8 weeks; full
coverage and validation 10-16 weeks. This is **smaller than** the old
"whole 2-4 month job" estimate because the PPU side and the ring/slot
protocol stay as they are, and the walker already has a GPU vertex path.

### (ii) GPU compute + readback of only the small command block

**Shape.** Same GPU tail as (i), but the GPU writes the vertex/index data into
a GPU ring and the CPU reads back only the per-job command block (counts,
offsets, draw state; ~0x90 bytes), then patches the guest hole / builds the
draw.

**Sync points.** A fence (or timeline semaphore) per batch. If batched per
frame/burst, the SPU cannot consume the results until the batch completes;
the cull/assembler must either also be on the GPU (then it is (i) with an
extra readback) or wait for the whole batch.

**Latency vs flip deadline.** The small readback itself is cheap
(~0.1 ms/frame for 429 × 0x90 bytes). The problem is the **fence**:
`record+submit+wait` is 32.8 µs p50 / 37.9 µs p99 (10k iterations, this
host). Per job that is 429 × 32.8 µs = **14.1 ms/frame** if serialised, and
~2.8 ms on the critical lane (86 jobs). Batched once per frame it is ~33 µs
plus ~1.9 µs/dispatch × 429 ≈ 0.8 ms of CPU recording, but the batch end
lands at the frame's GPU tail and serialises the SPU tail behind it.

**Exactness.** As (i) for the payload; the readback is exact bytes.

**Fallback.** Same as (i).

**Effort.** (i) plus an IPC/readback path and the indirect/patched-slot
handling: +2-4 weeks. It is only worth doing if the walker can consume an
indirect draw list and the guest command slot can be replaced, which is
exactly (i)'s harder half.

**Verdict.** Acceptable only with per-frame batching and a GPU-resident
payload; at that point it is (i) plus overhead. Per-job it is dead by
arithmetic.

### (iii) Keep cull + command emission on SPU/CPU, move only 0x21

**Shape.** GPU runs 0x13140/0x131A0 kernels; the result is copied back into
LS; the SPU continues with 0x25/0x27/0x2A and the epilogue.

**Sync points.** One submit+wait+copy per job (the SPU cannot proceed without
the LS result). Sync is on the job's critical path.

**Latency vs flip deadline.** 0x21 sel 1 is 390 µs lifted; the fence p50 is
32.8 µs plus a 6 KB copy and dispatch overhead. Best case saves ~350 µs/job
on the lane but adds ~2.8 ms/lane/frame of fence wait (86 jobs), so the
critical lane falls from 24.39 to ~21.6 ms and the burst from 35.0 to
~32 ms. That is a 3-4 fps-ish move at best, not 21 ms. If the GPU pass is
batched per frame, the SPU tail cannot start until the batch ends; that is
worse. This mirrors the old Design A conclusion, with a slightly cheaper
fence.

**Exactness.** The LS copy is exact; only the GPU math differs (~1 LSB).

**Fallback.** Straightforward: if the GPU copy/fence fails, run lifted 0x21.

**Effort.** 3-5 weeks for the two selectors and format coverage; the least
work, but it does not reach the target. **No-go.**

### What changed versus `c690-offload-decision.md`

1. **Consumer map is now complete.** The old doc knew the post-transform
   consumers were SPU stages and "everything else index/metadata/packets".
   We now know the final rings and index list have **exactly one** consumer,
   the RSX walker, and the PPU only polls job counters. That makes a
   no-readback GPU consumer possible in a way the old doc did not assume.
2. **The old Design A fence number was 41 µs p50; it is 32.8 µs now.** The
   per-job fence arithmetic still kills option (iii): 14 ms/frame serialised.
3. **The old Design B (whole job, 2-4 months) is no longer the only
   60 path.** A GPU geometric tail plus the existing ring/slot protocol is a
   smaller, testable subset. The PPU, job ABI, ring allocation and labels do
   not move.
4. **The 0x21 kernel is the right target but not a standalone one.** It is
   42.9 % of the wall and pure compute, but its LS result is consumed by the
   same job's SPU cull/assembler. A standalone 0x21 move is fence-bound.
5. **A GPU vertex path already exists in the walker** (`PS3RECOMP_VK_VTX_UNPACK`
   compute unpack, `rsx_vulkan_backend.c:4539-4671`, `5149-5196`). The new
   work is producing the ring bytes on the GPU and binding them, not building
   a compute backend from scratch.

## 3. Prototype: what exists, what it measured

### 3.1 Offline capture of a real 0x13140 call

The existing corpus (`recomp-work/corpus_edge1_20261103_012604`) has full
LS/GPR stage pairs at 0xFDA0 entry/exit but no 0x13140 pair. From a stage
image we reconstructed the 0x13080 command context (r3 = 0x1F000, selector
byte forced to 1) and ran the lifted 0x13080 handler offline. It reached
0x13140 and produced a valid invocation:

* entry GPRs: `r3=0x1F158`, `r4=0`, `r5=0x25B90`, `r80=0x1F000`;
* 384 vertices, 4 per iteration, 96 iterations;
* source arrays `0x23E10..0x2BFxx` unchanged;
* output `0x1F750..0x20F4B` (6140 bytes; 384 × 16-byte records).

Both format sub-branches were exercised:

| `r6` bit 4 | path | intermediate snapshots |
|---|---|---|
| clear | 0x13140 → 0xA3C0/A410 → A730/A798/A808/A8A0/A940 → B830 | A730, A798, A808, A8A0, A940, B830 (+BB20) |
| set | 0x13140 → 0xBB80/0xBB98 → BBB0/BCB4 → BED0/C420/C398/C0F8/C290/C018/BE00 | all seven |

The two paths write the same output range. Full LS/GPR snapshots for both
are in `/tmp/opencode/c690gpu/outa2` and `outb2` (research artifacts; the
input LS is the corpus stage image, so this is a *hybrid* capture: real game
arrays, offline-reconstructed command context).

The lifted 0x13140 sel-1 cost is the live measurement from
[`c690-emit-ring.md`](c690-emit-ring.md): **390.4 µs/call** (37.84 s over
96,930 calls); sel 7 is 51.7 µs. 0x21 is 42.9 % of the c690 wall.

### 3.2 Vulkan compute cost (offline)

`mlb16-recomp/tools/replay/test_vk_sync`, RTX 3060 Ti, headless, this host:

| operation | p50 | p90 | p99 | mean |
|---|---:|---:|---:|---:|
| record+submit+wait | **32.8 µs** | 34.1 | 37.9 | 35.2 |
| submit-only | 2.2 µs | 2.7 | 11.2 | 4.9 |
| batched, N=8 | 5.1 µs/dispatch | 5.3 | 5.7 | 5.2 |
| batched, N=64 | **1.9 µs/dispatch** | 1.9 | 2.0 | 2.0 |

These are the numbers behind the option comparison. They are transport
costs, not the kernel; the captured 0x13140 workload is 384 vertices ×
4-lane float math and is negligible on a 3060 Ti.

### 3.3 Why there is no GPU-vs-SPU byte diff yet

The offline corpus only witnessed the **A730** format variant. The heavy
`0xBB80` branch can be forced by setting bit 4 of `r6`, and we captured its
input/output, but that forced state is not a captured game-format descriptor:
the constants it selects (`0x14490..0x14500`) and the pack path were not
validated against a real job. Writing a bit-exact shader from it would be
unvalidated. The BB80 body is also a ~800-line lifted dispatcher/pack chain,
not a small fixed formula, so a shader needs its real input class first.

**No shader was signed off. Nothing was default-on.** The design conclusion
does not depend on the missing diff; the capture command is in §6.

## 4. Recommendation for phase 2

Take **option (i)**: GPU-resident geometric tail, SPU keeps the job ABI, ring
allocation, command hole and fallback, walker binds the GPU ring buffer and
uses the existing slot park for ordering.

Phase 2a (2-week spike, one format):

1. Capture one real BB80 job (§6) and build the GPU kernel for
   `0x13140 → 0xBB80/0xBB98 → BED0/BF00/C018` (or the A730 path if it is the
   pitch's dominant format) on that input.
2. Add an offline diff: full LS output, 384 records, exact / max error / LSB
   histogram; accept ≤1 LSB and zero cull changes.
3. Wire the walker to bind a GPU ring buffer for the five mesh-ring EAs; keep
   the SPU tail as the fallback.
4. One meter A/B (this is the first game launch; phase 1 is offline only).

Phase 2b (if 2a passes): extend to 0x0A/0x13 unpack, 0x25/0x26 cull, 0x27
index and 0x2A assembly; batch the GPU pass per burst; replace the per-job
command readback with an indirect draw list.

### Kill criterion

Stop if any of these is true at the end of 2a:

* GPU output differs by > 1 LSB on > 0.1 % of packed values, or changes any
  kept-index list;
* fallback rate > 5 % of c690 jobs (layout/format misses);
* measured burst wall with the spike on is not ≤ 30 ms, or p50 is not
  ≤ 30 ms, in the A/B (i.e. the GPU pass does not beat the noise already
  seen in the native-stage A/Bs);
* the added GPU/CPU sync costs > 2 ms/frame on the critical lane.

Go to 2b only if the spike shows burst wall ≤ 28 ms and p50 ≤ 30 ms. The
21 ms target needs the full tail plus the existing gap/dispatch work; do not
promise it from 0x21 alone.

## 5. Why the premise still holds

The premise "move Edge work to the GPU" is **not** contradicted: the CPU does
not need the post-transform data, and the walker is ours to change. What is
contradicted is the narrow "gate only the 0x21 SPU function" version: the
same job's SPU cull/assembler consumes 0x21's output in LS, so that version
is fence-bound. The GPU pass must cover the whole geometric tail (or at
least 0x21 + cull + assemble), not one stage.

## 6. Exact capture command for the user (one game run)

This is the only missing input; no other game launch is needed for phase 1.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
mkdir -p /tmp/opencode/fncap21
# Captures the first dispatch of each listed lifted function (LS + GPRs + pc).
EXTRA="SPU_FN_CAPTURE=0x13140,0xBB80,0xBED0,0xC018,0xA730,0xB830:/tmp/opencode/fncap21" \
  ab-tools/measure_fast.sh fncap21.log
```

Then, at the pitch meter, let it settle for ~10 s (the meter scene is stable;
do not start a throw). The files appear as
`/tmp/opencode/fncap21/0x13140/{ls.bin,gpr.bin,pc.txt}` etc. Send that
directory back; the offline harness in §7 is already wired for
`ls.bin`/`gpr.bin`. If `/tmp/fncap21/0xBB80` is absent, the first 0x13140 was
the A730 variant; run the same command once more at the meter and the first
BB80 dispatch will be captured (or use the `0x13140` pair and force bit 4 of
`r6` as the phase-1 harness did, and validate against the real `0xBB80`
input when it arrives).

## 7. Reproduction (offline)

```sh
# 1. extract the module image from our EBOOT (EA 0x01161300 -> file 0x1151300)
python3 - <<'EOF'
d=open('/home/m/Documents/mlb16-online/EBOOT.elf','rb').read()
open('/tmp/opencode/c690gpu/module.bin','wb').write(d[0x1151300:0x1151300+0x10860])
EOF

# 2. the harnesses (research only, not committed)
#    /tmp/opencode/c690gpu/run_13140.c   - 0x13080 -> 0x13140 capture
#    /tmp/opencode/c690gpu/replay_full.c - full-job replay attempt (blocked on
#                                          the low-LS SPURS runtime, see below)
cd /tmp/opencode/c690gpu
clang -O2 -march=native -mavx2 -mfma -ffp-contract=off \
  -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu \
  -I /home/m/Documents/mlb16-online/ps3recomp-play/include \
  -o run_13140 run_13140.c reg_c690.c \
  /home/m/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu/job_c690_native.c -lm
./run_13140 direct out5 outbb2 0x10 0x13140   # BB80 path
./run_13140 direct out5 outa2 0x0  0x13140   # A730 path

# 3. Vulkan transport numbers
cd /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay
./test_vk_sync
```

The full-job replay attempt (`replay_full.c`) starts at 0x135B8 with the
module image and a corpus job file, but the job body calls the **unlifted
low-LS SPURS runtime** at 0x1690 after 0xE750; without that runtime it stops
there. Replaying a whole job offline needs the low-LS interpreter or a
0x13140 stage capture, which is why §6 asks for the capture.

## 8. Clean room

This is our own static analysis of the EBOOT (Ghidra), our lifted c690 module
and our runtime/walker code. The stage-split/corpus/differential method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2
PC port (MIT, ThiagoFraga761 and contributors, `docs/NOTES.md`,
`work/port/native_passes.c`), used as a design reference only; no code was
copied. No RPCS3 or other GPL emulator source was read or cited.
