# Walker cuts 2: the remap-build memo, and why the flip tick cannot simply be moved

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh`/`ab_live.sh`
configuration (`TEX_DIRTY`, `PIPE_MEMO`, the four cheap wins, the shader disk
cache, the hitch recorder, the DXT lift; the c690 cull union merged but off)
plus `METER=1 SKIP_CINE=1`. Two game launches were used: the first never
confirmed the pitch (the harness's default `PITCH_THRESH=25` is below the
cube-render-target luma diff; `PITCH_THRESH=35` fixed it), the second ran the
A/B.

## TL;DR

* **Fresh offline profile (today's defaults, 502 draws/frame, null backend):**
  the largest remaining per-draw cost is **`fetch_plan` 2.42 ms** (remap build
  + plan prepare), then **`draw_be` 1.38** (CPU vertex convert 1.25), **`tex`
  0.97-1.6**, **`other` ~1.7**, **`pipe_lookup` 0.76**, **`parse` 0.93**. The
  instruction-level profile puts **`rsx_vertex_remap_build` at 22 %** of all
  samples, the single biggest function.
* **`PS3RECOMP_REMAP_FAST` (new, live key): a 1024-entry direct-mapped memo of
  the references seen most recently** answers a nearby duplicate without
  probing the open-addressing table. Offline **9.151 -> 8.642 ms/frame
  (-0.51, -5.6 %)**, `fetch_plan` **2.415 -> 1.954 ms**. Replay gates are
  bit-identical; the remap unit fixture now checks the memo against an O(n^2)
  first-use collapse.
* **Live A/B (one launch, N=6, 10 s segments, 2 s dropped): the walker drain
  falls 18.39 -> 18.12 ms (-0.27), but the frame does not move.** fps 32.19 vs
  32.12, p50 **34.16 vs 34.18 ms**, p90 41.20 vs 41.22, p99 46.26 vs 46.44,
  deadline frames 456 vs 455. The 34-36 ms bin is 531 vs 519 of 1,539/1,536
  frames. **This is the same result as `frame-timeline-2.md`: the walker is no
  longer the p50 lever.** The remap memo is exact and cheap, so it is kept on;
  it does not by itself buy frames.
* **Tick decoupling: implemented as `PS3RECOMP_TICK_IN_DRAIN` (live key) but
  DEFAULT OFF.** The drain now has a completion checkpoint that fires only
  when the flip's commands have executed *and* the present path has queued the
  image. That removes the "tick only on the 16 ms beat" starvation, but the
  guest then advances to the next buffer and waits for its ref (the
  `FLIP_ON_DONE` failure). The checkpoint as written is only ~4 ms later than
  `FLIP_ON_DONE`, so it does not avoid that wait; the design that would avoid
  it (complete at the *next* frame's flip marker, when the next buffer's fence
  is already executed) is written up in section 3 and not enabled.
* **The plan leftovers were checked before coding.** The readback cuts
  (`WCB_RECT`/`WCB_BATCH`/`WCB_SKIP`) are already in this branch, default off;
  at the meter RECT/SKIP do not fire and BATCH has no pending queue to merge
  (`exposure-and-readback-cuts.md`), so they are not a walker win. Vulkan
  recording, GPU vertex conversion and the parse/record split are not
  offline-measurable in the null-backend replay; they are left to a mission
  that can measure them live.

## 1. Fresh offline profile

### 1.1 Method

`replay_rsx_engine` on `recomp-work/meter_frame` (502 backend draws, 114,688
vertices, 4.0 MiB converted), `--frames 240 --warmup 20`,
`REPLAY_NULL_HASH=0`, today's defaults (`TEX_DIRTY=1 PIPE_MEMO=1
RSX_TEX_PIN=1`). `PS3RECOMP_WALKER_PROF2=1` for the phase split; the
instruction profile is `perf record -F 999` over 600 frames with the same
environment (the `[vdso]` 8 % is the profiler's own `clock_gettime`).

The replay is one static captured frame: it cannot model live texture churn
(why `tex` is a floor here, not the live 5.9 ms) and has no Vulkan recording.
Everything else is the shipping code.

### 1.2 Phase split (ms/frame, 502 draws)

| rank | term | ms/frame | note |
|---:|---|---:|---|
| 1 | `fetch_plan` | **2.42** | remap build + plan prepare |
| 2 | `draw_be` | 1.38 | contains `convert` 1.25 |
| 3 | `tex_bind` (`tex_slot`) | 0.97-1.60 | alternating hash-cadence frames |
| 4 | `other` | ~1.7 | group setup / state decode, coarse |
| 5 | `parse` | 0.93 | FIFO decode |
| 6 | `pipe_lookup` | 0.76 | pipeline-memo fingerprint (memo hits 525/525) |
| 7 | `layout` | 0.37 | vertex layout analysis |
| 8 | `fetch_refs` | 0.34 | reference list build |
| 9 | `index` | 0.18 | topology rebuild / index write |
| 10 | `const` | 0.12 | constant blocks |

`perf` by symbol:

| share | symbol |
|---:|---|
| 22.2 % | `rsx_vertex_remap_build` |
| 8.4 % | `eng_texture_content_hash` (live: `TEX_DIRTY` removes it) |
| 7.3 % | `eng_texture_sparse_hash` (live: removed) |
| 5.3 % | `eng_pipeline_get` (memo fingerprint) |
| 4.8 % | `rsx_loop_f32_all` |
| 4.5 % | `gcm_rsx_process_fifo_unlocked` |
| 3.6 % | `rsx_vertex_fetch_blocks` |
| 3.5 % | `sink_end_work` |

The remap build is the one big cost that is (a) offline-measurable, (b) live
too (it runs on every memo miss), and (c) not already covered by a landed cut.
That is the change below.

## 2. `PS3RECOMP_REMAP_FAST`

### 2.1 Design

`rsx_vertex_remap_build` (`libs/video/rsx_vertex_compact.c`) collapses repeated
`(vertex_id, base_index)` references with an open-addressing table of
`2 x count` 16-byte slots. Indexed meshes repeat vertices within a few
triangles, but the table is larger than L1 and every lookup is a random access.

The new path adds a **1024-entry direct-mapped memo** to `rsx_vertex_remap`:

```c
typedef struct { u64 key; u32 value; u32 generation; } micro_slot;
micro_slot micro[1024];  u32 micro_generation;
```

At the top of each build `micro_generation++` (0 clears the array). For each
occurrence the memo is probed with a cheap 32-bit fold of the key; a hit
answers the occurrence directly, a miss runs the existing table and stores the
answer in the memo. The memo stores only `(key -> unique index)`, never guest
bytes, so it cannot change an answer: the key is the same
`(base_index << 32) | vertex_id` the table uses, and a collision only costs a
table probe. The big table still gets every unique key, so a later distant
repeat is answered exactly as before.

`PS3RECOMP_REMAP_FAST` (live key `PS3_LIVE_REMAP_FAST`) gates it; default off
in the engine, on in the play scripts after this measurement.

### 2.2 Exactness

* `replay_rsx_engine` on `meter_frame`: last frame **a6fcc849341eba9b**, gate
  **ff2a220c28750bc3**, identical with the switch on and off.
* `walker_frame`: **416abee2c5bff47d / 3e6f6ad4ebb23ed6**, identical.
* `test_vertex_remap` now enables the switch for the whole fixture and adds a
  400-reference list (local and distant repeats, two base indices,
  non-monotonic order) checked against an O(n^2) first-use collapse, then
  rebuilds the same list on the same object. All 30 video fixtures pass.

### 2.3 Offline A/B

Three interleaved repeats, 240 measured frames each, `REPLAY_NULL_HASH=0`:

| repeat | off | on |
|---|---:|---:|
| 1 | 9.151 ms | 8.642 ms |
| 2 | 9.339 ms | 8.620 ms |
| 3 | 9.151 ms | 8.811 ms |
| median | **9.151** | **8.642** |

**-0.51 ms/frame (-5.6 %).** `fetch_plan` 2.415 -> 1.954 ms (-0.46); every
other phase is unchanged within noise. `perf` drops the remap build from 22.2 %
to 18.8 % of samples.

### 2.4 Live A/B

One launch (`ab_live_walk2b_20261104_001239`), `N=6 SEG=10 DROP=2`,
`METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11 PITCH_THRESH=35`,
`PS3_LWCOND_STATS=1`, arms rotated A/B through `PS3RECOMP_CTL`:

* A = `PS3RECOMP_REMAP_FAST=0`
* B = `PS3RECOMP_REMAP_FAST=1`

| per-flip | A off | B on |
|---|---:|---:|
| frames | 1,539 | 1,536 |
| fps | **32.19** | **32.12** |
| p50 ms | **34.16** | **34.18** |
| p90 ms | 41.20 | 41.22 |
| p99 ms | 46.26 | 46.44 |
| std ms | 16.56 | 10.52 |
| mean ms | 31.01 | 31.17 |
| walker drain ms | **18.39** | **18.12** |
| wait ms | 12.65 | 13.00 |
| deadline frames (60 s) | 456 | 455 |
| flip-done waits / signal | 2,114 / 1,084 | 2,120 / 1,079 |

The A std is inflated by one segment (seg 8) that contains a stall; the
per-segment p50 is 34.15-34.24 in both arms. Paired per-segment fps differences
are +1.00, 0.00, +0.13, -2.76, +2.00, -0.75 (mean +0.27, no consistent sign);
the walker difference is consistently about -0.3 ms except one segment.

2 ms histogram of the per-flip `fin` intervals (8-52 ms, 52+ = over):

| bin | 8 | 10 | 12 | 14 | 16 | 18 | 20 | 22 | 24 | 26 | 28 | 30 | 32 | 34 | 36 | 38 | 40 | 42 | 44 | 46 | 48 | 50 | 52 | 52+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 17 | 26 | 68 | 105 | 81 | 73 | 30 | 30 | 10 | 1 | 2 | 0 | 12 | **531** | 149 | 170 | 98 | 65 | 29 | 11 | 4 | 1 | 1 | 25 |
| B | 17 | 26 | 51 | 93 | 68 | 78 | 43 | 11 | 5 | 1 | 0 | 1 | 13 | **519** | 162 | 190 | 97 | 72 | 33 | 9 | 4 | 4 | 0 | 39 |

**Conclusion.** The cut does what it says (walker -0.27 ms, `fetch_plan`
-0.46 offline) and changes nothing about the frame. That is expected from
`frame-timeline-2.md`: the drain is 18 ms inside a 34 ms frame and the frame
ends on the c690 producer burst, so a 0.3 ms drain cut goes into `put_wait`.
Keeping it is still right (it is exact, cheap, and helps the load's walker
drain), but it is not a p50 lever.

## 3. Tick decoupling: what was built and why it is off

### 3.1 The implemented checkpoint

`PS3RECOMP_TICK_IN_DRAIN` (live key `PS3_LIVE_TICK_IN_DRAIN`) adds
`cellGcmFlipTickCheckpoint()` at the top of every FIFO drain pass. It marks a
completion only when all three hold:

1. the flip status is `WAITING`;
2. the present path has queued the image for this request sequence
   (`s_flip_presented_seq == s_flip_req_seq`, set by
   `cellGcm_take_flip_pending*`);
3. the drain has executed the flip (`gcm_flip_executed_take()`).

The handler still runs on the GCM interrupt thread (`GCM_HANDLER_THREAD=1`), so
this only moves the completion edge. `test_gcm_tick_in_drain` covers the
executed+presented case, the executed-but-not-presented case (must not consume
the executed bit), and the switch-off case. The replay gates do not exercise
this (cellGcmSys is not in the replay).

### 3.2 The side effect it does not avoid

`Gfx_DisplayFlip` (`0x000692dc`) does, per iteration:

```
wait for a queued frame (DAT_012450a0)
wait for the buffer ref: DAT_01245020[(consumer & 3)] == *(DAT_01123b00 + 8)
cellGcmResetFlipStatus(); cellGcmSetFlipImmediate(bufferId)
wait for the flip handler / 2 x 16683 us deadline
```

`DAT_01245020[slot]` is a fence value `Gfx_FrameEnd` writes into the frame's
command stream (`FUN_0006309c(&DAT_00f32644)`) and the RSX publishes at
`DAT_01123b00+8`. Completing the flip advances `DAT_011203dc`, so the display
thread immediately asks for the **next** buffer and waits for *its* fence.
If the walker has not executed that frame's fence yet, the guest blocks there.
That is exactly the `PS3RECOMP_FLIP_ON_DONE` failure measured in
`flip-deadline.md` (34.06 -> 26.00 fps). The checkpoint is only the ~4 ms
between the present and the next drain, not a frame, so it does not make the
next fence ready.

### 3.3 The design that would avoid it (not enabled)

The next buffer's fence is executed exactly when the drain reaches that frame's
flip marker (`0xFEAD` word), because `Gfx_FrameEnd` emits the fence immediately
before the marker. So a completion that is safe against the ref wait is:

> at a flip marker, after the drain has executed the marker's frame, complete
> the *previous* presented flip. The next buffer's fence is now published, so
> `Gfx_DisplayFlip` advances without blocking.

This pipelines one frame: the display thread flips frame N at marker N+1, and
its completion lands at marker N+2. The cost is one frame of latency, and the
completion is still bounded by the drain of the next frame. Whether that beats
the title's 2 x 16683 us deadline depends on the c690 burst, not on the tick.
Implementing it needs a marker-side hook in the drain (the checkpoint is
already there; it needs to run after `s_fifo_flips_reached++`, not at the top)
and a live A/B. It is left off and queued as the follow-up.

**The premise check matters here.** `flip-deadline.md` already shows the
deadline branch is *faster* than completing early when the ref wait is not
ready; `frame-timeline-2.md` shows the frame ends on the c690 burst. A tick
change can only win if it removes the ref wait, which is what 3.3 does; the
simple "complete from the drain" edge does not.

## 4. Plan leftovers that were not taken

| item | status |
|---|---|
| readback cuts (`WCB_RECT`/`BATCH`/`SKIP`) | already in this branch (commit `618ec118`), default off. `exposure-and-readback-cuts.md`: at the meter RECT/SKIP never fire (the exposure chain is redrawn every frame and the 32x32 page is read whole), BATCH has no pending queue (the mip reads are sequential faults), and the cuts-on run was neutral. The 512x512 NV3089 copy is a whole-surface copy once every ~3 frames (341 KB/frame), so RECT cannot shrink it. |
| Vulkan recording (5.5 ms live) | not measurable in the null-backend replay. The live `VK_DRAWPROF` split is still vtxgpu 1.53, rp_begin 0.89, copy 0.79, desc 0.68, barrier 0.42 ms/frame. Keeping render passes open across same-target draws is the biggest piece and needs a live-only mission. |
| GPU vertex conversion | the live summary already offloads almost every draw (m0 449k, m3 2.66M, remap 2.40M, other 186k of 3.3M calls), so the CPU `convert` left in the replay is mostly the null backend, not the live walker. |
| parse/record split | XL (4-8 weeks), and `parse` is 0.93 ms offline / ~2.5 ms live. Not started. |

## 5. Reproduce

```sh
cd recomp-work/build-play
# gates (must be identical with the switch on/off)
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
PS3RECOMP_REMAP_FAST=1 ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
# offline A/B (3 interleaved repeats)
for i in 1 2 3; do for on in 0 1; do
  env PS3RECOMP_REMAP_FAST=$on PS3RECOMP_TEX_DIRTY=1 PS3RECOMP_PIPE_MEMO=1 \
      PS3RECOMP_RSX_TEX_PIN=1 REPLAY_NULL_HASH=0 \
      ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 240 --warmup 20
done; done
# fixtures
python3 tools/test_video_fixtures.py
```

Live A/B (one launch; `PITCH_THRESH=35` is required with the cube render
targets):

```sh
cd recomp-work/ab-tools
export PS3_LWCOND_STATS=1
TAG=walk2b PITCH_THRESH=35 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  ARMS='A=PS3RECOMP_REMAP_FAST=0|B=PS3RECOMP_REMAP_FAST=1' \
  BOOT_ARM='PS3RECOMP_REMAP_FAST=0' ./ab_live.sh
python3 pin_ab.py --log ../ab_live_walk2b_<ts>.log \
  --marks ../ab_live_walk2b_<ts>.marks --seg 10 --drop 2
```

## Caveats

* **One A/B launch** (the first launch never reached the meter: the harness's
  `PITCH_THRESH=25` is below the cube-RT pitch luma diff, which is ~30; 35 is
  the value `readback-cost.md` already documented). The second launch reached
  the meter on the first X and ran all 12 segments.
* The offline replay is one static frame; its `tex` and `convert` numbers are
  floors. The remap cut is measured on the same code live, so its delta is not
  a replay artefact.
* The live walker is 18.1-18.4 ms here, not the 26 ms of the older profile;
  `TEX_DIRTY` and `PIPE_MEMO` account for most of the difference, and the
  draw count (502 offline vs ~770 live in the older runs) differs between
  captures.
* `TICK_IN_DRAIN` is exactness-tested but has **not** been run live; it is
  default off. The marker-side variant in 3.3 is a design, not a measurement.
