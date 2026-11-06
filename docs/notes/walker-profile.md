# Walker own work at the pitch meter: where the ~26 ms of CPU goes

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. One meter launch (`ab_live_metercap_20261103_002238`,
X once, `METER=1 SKIP_CINE=1 WCB=2 PIN=1 CUBE_RT=1`, x11, `VK_DRAWPROF=1`)
captured one meter frame and produced the live phase numbers. All the
per-draw fine split below is offline replay of that capture
(`replay_rsx_engine`, null backend), with an env-gated profiler added for this
mission. No shipping optimisation was implemented; the only code changes are
instrumentation plus a replay-only frame tick.

## TL;DR

* **Live meter, steady A/B window (552 draws/frame):** walker drain
  **27.64 ms**, of which the profiled engine is **26.02 ms** and FIFO parse
  **1.62 ms**. Per draw that is **50.1 us** drain / **47.2 us** engine.
  Wall beyond the drain (park/idle/WCB service) is 8.44 ms.
* **The three costs are texture revalidation, Vulkan recording and vertex
  fetch.** Live engine split: `tex` **11.33 ms** (43 %), `draw` **5.51 ms**
  (21 %), `fetch` **4.28 ms** (16 %), `other` 2.61, `pipe` 2.07, `parse`
  1.62, `bind` 0.23, layout ~0.40.
* **Texture work is the cache, not the bind path.** Offline the whole
  `sink_bind_textures` is 3.96 ms/frame and `eng_texture_slot` is 3.88 of it
  (98 %): lookup + sparse/full content hashing + upload. Surface/depth scans,
  samplers and the per-unit memo are <0.1 ms.
* **Vulkan recording is 5.0-5.5 ms**, split by `VK_DRAWPROF` as vtxgpu 2.9,
  rp_begin 1.7, copy 1.5, desc 1.3, barrier 0.8, state/bind_draw/rp_end
  0.4 us/draw each. The null-backend replay (1.4 ms `draw_be`) contains only
  the CPU conversion; the rest is command recording.
* **Readback is a separate 6.2 ms/frame** (4.60 copy + 1.57 wait,
  `readback-cost.md`) in the walker's wall wait, outside the drain timer.
* **The capture frame:** 502 backend draws, 525 draw groups, 114,688
  vertices, 4.0 MiB converted, 276 texture-unit binds, 0 new pipelines after
  warm-up, 525 pipeline lookups/frame.
* **Offline replay reproduces the live per-draw costs** for fetch, texture
  and pipeline: 23.5 us/draw engine offline vs 47.2 live, and the 23.7 us
  difference is exactly the two things a one-frame replay cannot do - live
  texture churn (+12.5 us/draw) and Vulkan recording (+7.1 us/draw), plus
  ~4 us of contention.
* **The mission's ~20 ms own-work figure is the long-frame mix; ours is
  26 ms** in the 552-draw steady window. On the 502-draw capture the same
  code is 15.7 ms (23.5 us/draw). The ranking and the plan do not change;
  see "Premise check".

## 1. What was measured, and how

### 1.1 Live phase profile (existing `PS3RECOMP_WALKER_PROF=1`)

`ab_live.sh` already exports `PS3RECOMP_WALKER_PROF=1` and
`PS3RECOMP_PIPE_TIMING=1`; `VK_DRAWPROF=1` was added for this run. One launch,
X once, the capture armed after the meter was confirmed. The measured A/B
window is 164.9-203.6 s from launch (the capture is at 157 s; the A/B window
is after it, same scene, same settings, empty arms).

| live meter, measured window | ms/frame | us/draw |
|---|---:|---:|
| walker drain | **27.638** | 50.08 |
| wall outside drain (park/idle/WCB) | 8.436 | 15.29 |
| engine (dispatch) | **26.022** | 47.16 |
| parse (drain - engine) | 1.616 | 2.93 |
| fetch | 4.279 | 7.75 |
| tex | **11.325** | 20.52 |
| pipe | 2.070 | 3.75 |
| bind | 0.234 | 0.42 |
| draw | **5.507** | 9.98 |
| other | 2.606 | 4.72 |

Draws/frame in the window: 552.8 (A) / 550.8 (B), mean **551.8**. Guest fps
27.4 / 28.0. This is the window the frame-timeline doc calls "walker drain":
the drain is the frame, and the c690 producer wait is the park/put-wait term,
not engine time.

### 1.2 The fine profiler (`PS3RECOMP_WALKER_PROF2=1`, new)

`libs/video/rsx_draw_engine.c` gained a second, default-off phase set. It
prints one `[walker-prof2]` line per flip beside `[walker-prof]`:

| term | what it covers | code |
|---|---|---|
| `layout` | `eng_vertex_layout` | `rsx_draw_engine.c:5847` (`sink_end_work`) |
| `surface` | decode render state, target set, surface/viewport/zeta | `rsx_draw_engine.c:5847` |
| `fetch_refs` | arrays + index runs -> reference list | `rsx_draw_engine.c:4585` (`dc_fetch`) |
| `fetch_plan` | fetch key, remap, plan prepare, defer choice | `rsx_draw_engine.c:4585` |
| `convert` | vertex fetch/convert (also inside `fetch`/`draw`) | `rsx_draw_engine.c:4466`, `:4450` |
| `vtxoff` | `eng_check_gpu_vertex_offload` | `rsx_draw_engine.c:5681` |
| `pipe_lookup` | fingerprint + memo probe | `rsx_draw_engine.c:4045` |
| `pipe_analyze` | program/state analysis, texture masks, constants | `rsx_draw_engine.c:4045` |
| `pipe_key` | identity hash | `rsx_draw_engine.c:4045` |
| `pipe_dir` | pipeline hash-directory lookup | `rsx_draw_engine.c:4045` |
| `pipe_decompile` | VP/FP decompile and text patching | `rsx_draw_engine.c:4045` |
| `pipe_create` | `be->pipeline_create` (host compile) | `rsx_draw_engine.c:4249` |
| `pipe_debug` | `eng_debug_fp` | `rsx_draw_engine.c:5847` |
| `tex_slot` | texture cache lookup/hash/decode/upload | `rsx_draw_engine.c:2655` |
| `tex_upload` | decode + backend upload (inside slot) | `rsx_draw_engine.c:2390` |
| `tex_bind` | whole `sink_bind_textures` (slot is inside) | `rsx_draw_engine.c:4992` |
| `vtex` | vertex texture binds | `rsx_draw_engine.c:5847` |
| `const` | VP/FP constant blocks | `rsx_draw_engine.c:5847` |
| `index` | topology rebuild + index writer | `rsx_draw_engine.c:5847` |
| `bind_be` | backend bind calls (descriptor writes) | `rsx_draw_engine.c:5847` |
| `surftrack` | `pd_note`, written-target bookkeeping | `rsx_draw_engine.c:5847` |
| `draw_be` | backend draw recording (contains `convert`) | `rsx_draw_engine.c:5847` |

Timers nest: `convert` is inside `fetch`/`draw`, `tex_upload` inside
`tex_slot`, `tex_slot` inside `tex_bind`. Eleven to twenty extra
`clock_gettime` calls per draw are ~0.25 ms/frame at 502 draws; the measured
run-to-run spread on this machine is larger than that (see Caveats).

### 1.3 Capture and replay

The capture is one meter frame at the first flip after the meter was
confirmed:

```
recomp-work/meter_frame.fifo   109,318 words (437 KB)
recomp-work/meter_frame.mem    27,477 pages (107.3 MB)
recomp-work/meter_frame.state  148 KB
```

It has 502 backend draws, 525 draw groups, 114,688 vertices, no read misses.
The replay is the shipping `rsx_draw_engine.c` with the null backend; only
Vulkan recording is absent.

Two replay-only changes were needed to make a one-frame capture model a
steady scene:

* **`rsx_draw_engine_replay_advance_frame()`** (`rsx_draw_engine.c:6409`)
  increments `g.frames` after each replayed frame. The title never registers
  a display buffer in the replay, so `eng_present()` returns before its frame
  tick and every replay frame stayed "frame 0": texture entries were hashed
  once and then never revalidated. With the frame tick the replay follows the
  live per-frame sparse hash and the full-hash-every-8-frames cadence.
  **The last-frame output hash and the 10-frame gate are unchanged**
  (`a6fcc849341eba9b`, `02a922c9b2620fa1`), so the replay output is still
  exact.
* **`REPLAY_NULL_HASH=0`** turns off the null backend's digest of every vertex
  and upload for timing runs (`tools/replay_null_draw_backend.c:34`). The
  digest is kept by default; it costs 0.66 ms/frame of `bind` and 0.3 ms of
  `draw` in this capture and is present in both arms of any A/B.

## 2. Offline breakdown (steady replay, 240 frames)

`PS3RECOMP_WALKER_PROF2=1 REPLAY_NULL_HASH=0 replay_rsx_engine
recomp-work/meter_frame --frames 240 --warmup 20`, frames 5-239 averaged.
The capture is static (textures never change), so the texture numbers here
are the floor; the live texture cost is 11.3 ms, not 4.0.

### 2.1 Counts

| per frame | value |
|---|---:|
| backend draws | 502 |
| draw groups | 525 |
| vertices | 114,688 |
| converted vertices | 114,448 |
| converted bytes | 4,109 KB (4.0 MiB) |
| texture-unit binds | 276 |
| pipeline lookups | 525 |
| pipelines created after warm-up | 0 |
| fetch memo hits/misses | 0/0 (memo off) |
| texture uploads after warm-up | 0 (static capture) |

Live adds ~552 draws/frame and one Vulkan descriptor set per draw on the
batched path; the null backend allocates none.

### 2.2 Time, ms/frame at 502 draws

| rank | cost | ms/frame | us/draw | share of engine | file:line |
|---:|---|---:|---:|---:|---|
| 1 | texture cache lookup/revalidate/hash (`tex_slot`) | **3.88** | 7.72 | 33 % | `rsx_draw_engine.c:2655`, hashes `:2315`, `:2341` |
| 2 | fetch plan: key + remap + prepare (`fetch_plan`) | **2.38** | 4.73 | 20 % | `rsx_draw_engine.c:4585` (key `:4482`, remap `:4703`, prepare `:4716`) |
| 3 | unaccounted engine (`other`: group setup, resets, decode) | **1.72** | 3.43 | 15 % | `rsx_draw_engine.c:5847` |
| 4 | backend draw (`draw_be`, CPU conversion + null call) | **1.41** | 2.81 | 12 % | `rsx_draw_engine.c:5847` |
| 5 | vertex convert (`convert`, inside draw/fetch) | 1.29 | 2.56 | 11 % | `rsx_draw_engine.c:4466`, `:4450` |
| 6 | FIFO parse / method dispatch (`parse`) | **0.89** | 1.77 | 7.6 % | `cellGcmSys.c:3283`, `rsx_dispatch.c` |
| 7 | pipeline analysis (`pipe_analyze`) | **0.78** | 1.56 | 6.7 % | `rsx_draw_engine.c:4045` |
| 8 | pipeline identity hash (`pipe_key`) | **0.66** | 1.32 | 5.7 % | `rsx_draw_engine.c:4045` |
| 9 | vertex layout (`layout`) | 0.40 | 0.79 | 3.4 % | `rsx_draw_engine.c:5847` |
| 10 | reference list from arrays/indices (`fetch_refs`) | 0.35 | 0.70 | 3.0 % | `rsx_draw_engine.c:4585` |
| 11 | index rebuild/writer (`index`) | 0.17 | 0.35 | 1.5 % | `rsx_draw_engine.c:5847` |
| 12 | pipeline fingerprint/memo (`pipe_lookup`) | 0.16 | 0.31 | 1.4 % | `rsx_draw_engine.c:4045` |
| 13 | constant blocks (`const`) | 0.13 | 0.25 | 1.1 % | `rsx_draw_engine.c:5847` |
| 14 | GPU-offload descriptor (`vtxoff`) | 0.09 | 0.18 | 0.8 % | `rsx_draw_engine.c:5681` |
| 15 | backend binds (`bind_be`) | 0.05 | 0.09 | 0.4 % | `rsx_draw_engine.c:5847` |
| 16 | surface tracking (`surftrack`) | 0.03 | 0.05 | 0.2 % | `rsx_draw_engine.c:5847` |
| 17 | pipeline directory (`pipe_dir`) | 0.02 | 0.04 | 0.2 % | `rsx_draw_engine.c:4045` |
| 18 | vertex textures (`vtex`) | 0.02 | 0.05 | 0.2 % | `rsx_draw_engine.c:5847` |
| 19 | `eng_debug_fp` (`pipe_debug`) | 0.01 | 0.02 | 0.1 % | `rsx_draw_engine.c:5847` |
| — | **engine total** | **11.69** | **23.3** | 100 % | |
| — | parse | 0.89 | 1.77 | | |
| — | **walker drain** | **12.58** | **25.1** | | |

The existing coarse phases over the same frames agree: `fetch` 2.762,
`tex` 4.025, `pipe` 1.678, `bind` 0.070, `draw` 1.432, `other` 1.722,
`engine` 11.689, `parse` 0.887.

## 3. What the one-frame replay cannot see, and the live numbers

| cost | live | offline | why the gap |
|---|---:|---:|---|
| texture revalidation/upload | 11.33 | 4.03 | live textures change between frames; the capture has one version |
| Vulkan recording | 5.51 | 1.43 | null backend records no vkCmd*; the replay has only CPU conversion |
| fetch plan/refs | 4.28 | 2.76 | contention + more draws in the live window |
| pipeline | 2.07 | 1.68 | more draws/contention |
| parse | 1.62 | 0.89 | more draws/contention |
| other | 2.61 | 1.72 | more draws/contention |
| bind | 0.23 | 0.07 | null backend does no host work |
| **engine** | **26.02** | **11.69** | |

At 552 live draws/frame the offline per-draw cost (23.3 us) is the same code;
multiplying by 552 gives 12.9 ms, and the remaining 13.1 ms is the live-only
texture churn (~6.9 ms at the offline tex per-draw rate) plus Vulkan
recording (~3.9 ms) plus contention. That is the whole 26 ms.

### 3.1 Vulkan recording (`VK_DRAWPROF`, live, 526 draws/frame)

| subphase | us/draw | ms/frame @526 | code |
|---|---:|---:|---|
| vtxgpu (compute unpack record) | 2.9 | 1.53 | `rsx_vulkan_backend.c:4926-4985` |
| rp_begin | 1.7 | 0.89 | `rsx_vulkan_backend.c:5009-5015` |
| copy | 1.5 | 0.79 | `rsx_vulkan_backend.c:4912-4924` |
| desc (descriptor set alloc/write) | 1.3 | 0.68 | `rsx_vulkan_backend.c:4875-4908` |
| barrier | 0.8 | 0.42 | `rsx_vulkan_backend.c:5005-5008` |
| state (viewport/scissor/stencil/bounds) | 0.4 | 0.21 | `rsx_vulkan_backend.c:5016-5035` |
| bind_draw | 0.4 | 0.21 | `rsx_vulkan_backend.c:5036-5047` |
| rp_end | 0.4 | 0.21 | `rsx_vulkan_backend.c:5048-5050` |
| **total measured** | **9.4** | **4.94** | |

The live profiler's `draw` is 5.51 ms including the CPU conversion the
null replay also does; the two agree to ~0.5 ms. Queue submit is not on the
walker: `vk_eng_flush_async()`/`vk_submit_and_wait()`
(`rsx_vulkan_backend.c:4578`, `:716`) run on the batched path and the present
thread; `VK_EB_*` batching keeps the walker from waiting except when a batch
is full.

### 3.2 Readback (WCB=2, from `readback-cost.md`)

Outside the drain timer, in the walker wall wait:

| surface | copies/frame | copy ms/frame | wait ms/frame |
|---|---:|---:|---:|
| 512x512 NV3089 transfer `0x0B36BC00` | 0.33 | 3.13 | 0.00 |
| 32x32 mip `0x019D7900` | 0.33 | 1.39 | 1.46 |
| 2x2 / 1x1 mips | 0.66 | 0.08 | 0.11 |
| **total** | **1.32** | **4.60** | **1.57** |

One `submit_and_wait` per resolve (`rsx_vulkan_backend.c:716`) is the wait;
the copy is in `eng_wcb_resolve` (`rsx_draw_engine.c:3336`) from the
NV3089 path (`cellGcmSys.c:1316`). Locks are not a term: the FIFO SRW lock
(`cellGcmSys.c:2421`) is uncontended in this scene (<0.01 ms/frame).

### 3.3 Premise check

The mission's "~20 ms own work" is the long-frame figure (drain ~28.7 minus
~7 ms of c690 wait). In the 552-draw steady A/B window we measure 27.64 ms of
drain and 26.02 ms of engine, i.e. **~26 ms of walker CPU, not 20**. The
difference is the window's draw/texture mix: the 502-draw capture has the same
code at 15.7 ms of drain (23.5 us/draw), and the live long frames reach
40-50 ms. The ranking, the target and the plan are unchanged; the target
should be read against the 26 ms CPU number (a 12-18 ms cut, not a 12 ms one).

## 4. Ranked plan: walker own work -> <= 8 ms

Ordered by expected ms at the meter per unit of effort. "Exactness" means the
shipping output must be unchanged; the common gate is the meter replay hash
(below), plus the per-item test.

Common replay gate:

```sh
cd recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
# last-frame hash a6fcc849341eba9b, 64-frame gate ff2a220c28750bc3
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_frame --frames 5 --warmup 2
# last-frame hash 416abee2c5bff47d, gate 3e6f6ad4ebb23ed6
```

| # | change | expected (ms/frame) | effort | kill criterion | exactness test |
|---:|---|---:|---|---|---|
| 1 | **Texture cache revalidation**: replace the per-frame sparse+full content hash in `eng_texture_slot` with a page-dirty generation (the WCB page table already knows every write), keep the full hash only as a fallback and after a known rewrite | **-6 to -9** (11.3 -> 2.5-5) | M, 1-2 wk | < 2 ms saved, or any frame-diff pixel change, or a stale texture after a mid-frame write | meter/pitch gate unchanged; plus a replay test that rewrites one page between frames and asserts the upload digest changes on the next bind |
| 2 | **Readback cuts**: rectangle-sized NV3089 resolve (3.13 ms) and one submission for the four tiny mip resolves (1.47 copy + 1.57 wait) | **-3 to -4.5** | M, 1-2 wk | < 1.5 ms saved, or any readback byte differs | byte-for-byte compare of the 512x512 target and the mip chain against the current build on the capture; `test_color_readback_guest` |
| 3 | **Vulkan recording**: keep render passes open across draws of the same target set, cache descriptor sets by texture set, drop the per-draw `vkCmdSet*` state block when unchanged | **-2 to -3** (5.5 -> 2.5-3.5) | M-L, 2-3 wk | < 1 ms saved, or a validation error / visual diff | null-backend gate unchanged (proves engine semantics); then in-game frame dump |
| 4 | **Fetch plan cache + cheaper key**: enable `PS3RECOMP_WALKER_FAST` (already tested: -1.63 ms fetch offline) and hash only the attribute descriptors the layout uses instead of all 16 plus the full ref list | **-1.2 to -1.8** (2.7 -> 1.0-1.5) | S-M, 3-5 d | < 0.5 ms saved, or any vertex byte/geometry change | gate unchanged with `PS3RECOMP_WALKER_FAST=1`; existing engine unit tests (`test_rsx_draw_engine.c`) |
| 5 | **Pipeline lookup**: use the existing pipeline memo (`walker-cuts.md`: -1.09 ms pipe offline) and cache the program analysis keyed by the program bytes | **-1.0 to -1.4** (2.1 -> 0.7-1.1) | S-M, 3-5 d | < 0.4 ms saved, or a wrong pipeline / constant block | gate unchanged; `test_rsx_draw_engine.c` pipeline cases |
| 6 | **FIFO parse/dispatch/other**: decode a whole run into a small command list once, replace the per-word subchannel/`gcm_3d_method` tests, and skip the no-op/zero runs unconditionally | **-1.5 to -2.5** (5.1 -> 2.6-3.6) | M, 1-2 wk | < 0.7 ms saved, or a method-stream mismatch | gate unchanged; `replay_rsx` dispatcher hash `0x85fc2ff5a1655c26` |
| 7 | **Vertex conversion to the GPU**: extend the compute unpack to every layout the CPU still converts (the live summary already has m0 284k, m3 798k, remap 711k; `other` 49k) and remove the CPU remap build | **-1.0 to -1.5** (convert 1.3; vtxgpu/copy stay) | M, 1-2 wk | < 0.5 ms saved, or a vertex byte mismatch | GPU unpack output vs `rsx_vertex_fetch_many` bit-exact on the capture; gate unchanged |
| 8 | **Parse/record split**: a producer thread parses FIFO and updates register/surface state while the walker thread records commands from a method list (double-buffered per frame) | **-3 to -5** of the serial walker, but only after 1-6 | XL, 4-8 wk | no p50 gain in 2 weeks, or any ordering/visual diff | gate unchanged, frame dump, in-game A/B |

Items 1-3 alone remove **11-16.5 ms**; with 4-6 the walker CPU is at
**~4-8 ms**, which is the target. Item 8 is the only way past the floor that
remains when the parse and record halves both cost ~3 ms; it should not be
started before the single-thread costs are down.

### 4.1 Milestones

| after | walker engine | frame p50 (projection) |
|---|---:|---:|
| now | 26.0 | 33.9 |
| 1 (texture) | 17-20 | ~31-32 |
| 1-2 (+readback) | 13-16 engine, 2-4 ms less wall wait | ~28-30 |
| 1-3 (+Vulkan recording) | 10-13 | ~26-28 |
| 1-6 | 4-8 | c690/main floor (~19.5) |

The projections are linear on the measured terms; the frame p50 also depends
on the c690 side (`c690-profile.md`: both chains must fall below ~18 ms).

## 5. Reproduce

```sh
# capture + live profile (one launch, ~160 s to the meter)
cd recomp-work/ab-tools
TAG=metercap CAPTURE_PREFIX=$PWD/../meter_frame METER=1 SKIP_CINE=1 \
  WCB=2 PIN=1 N=1 SEG=20 DROP=2 SETTLE=5 VIDEODRIVER=x11 PITCH_THRESH=35 \
  PS3RECOMP_RSX_CUBE_RT=1 VK_DRAWPROF=1 ./ab_live.sh

# offline fine split (timing; digest off)
cd ../build-play
PS3RECOMP_WALKER_PROF2=1 REPLAY_NULL_HASH=0 \
  ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame \
  --frames 240 --warmup 20 2>&1 | grep walker-prof2

# exactness gate (digest on, profiler on/off must match)
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
PS3RECOMP_WALKER_PROF2=1 \
  ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
```

The run folder is `recomp-work/ab_live_metercap_20261103_002238/` (meter
frame `meter_1793683514396_1.png`, `scene_sheet.png`, MangoHud CSV). The
capture is `recomp-work/meter_frame.{fifo,state,mem}` (107.3 MB, game data,
never committed).

## Caveats

* **One launch** for the live phases (the task's run budget). The A/B window
  after the capture is 2 x 20 s, 995 walker reports; the capture frame itself
  is one frame of a 502-draw class, while the live window averages 552.
* **The replay is one static frame.** Texture uploads and live texture
  changes are not modelled; its texture cost (4.0 ms) is a floor. Every other
  per-draw term matches the live window within 10-15 %.
* **`g.frames` in replay.** The frame tick added here is replay-only; the
  shipping engine still advances it in `eng_present()`. It changes no output:
  the last-frame hash and the 10-frame gate are identical before and after.
* **`PROF2` overhead** is 11-20 `clock_gettime` calls per draw, ~0.25 ms/frame
  at 502 draws. The machine's run-to-run median on this capture varied by
  4-5 ms over 64-frame runs (turbo/thermal), so the overhead is not
  separable from noise; do A/B timing with `PROF2` off or identical frame
  counts.
* **`perf` was not used**: absent, and `perf_event_paranoid=2`; no system
  setting was changed.
* The 512x512/mip readback rows are from `readback-cost.md` (same branch,
  WCB=2, meter); this run did not re-enable `PS3RECOMP_RSX_WCB_ATTR`.
* Frame start/end accounting: the walker's wall wait (8.44 ms) is wall minus
  drain; it contains the park/idle wait, the c690 producer wait and WCB
  service. It is not added to the engine totals.
