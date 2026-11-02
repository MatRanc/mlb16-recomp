# In-run live A/B, the draw-work ceiling, and why the pitch is not stationary

Date: 2026-11-02. Branch `exp/live-ab` in the `ps3recomp-play` worktree (off
`play/multiscene`), built as `recomp-work/build-play`. All runs are windowed
pitch runs with `PS3RECOMP_VK_WINDOW=1`, `VK_DUMMY_BLACK=1`,
`PS3RECOMP_VK_PRESENT_MODE=fifo`, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`,
`PS3RECOMP_WALKER_PROF=1`, MangoHud CSV, and scripted nav. Dates/times are the
machine's local clock.

## TL;DR

* **The in-run harness works, but only after the pitch settles.** The pitch
  screen is not stationary for the first 1-3 minutes: the guest streams
  assets, and the walker mean swings 14-45 ms/frame. A 10 s wait after nav
  lands inside that warm-up. `ab_live.sh` now waits until the walker mean has
  been <= 20 ms for 8 s; the A/A noise then falls from ~5 fps (PIN off) to
  ~2 fps (PIN on).
* **A/A (both arms identical, 6 x 10 s per arm):** PIN off diff **-0.31 fps**
  (95% CI -5.56..+4.95, segment std 4.2/4.9); PIN on diff **-0.11 fps**
  (95% CI -1.73..+1.51, std 2.2/1.7). Both estimates are inside the ~0.5 fps
  target; only the pinned run has a CI tight enough to resolve ~1.6 fps.
* **`EXP_NO_DRAW=1` is the answer to "is the walker the limit?"** Baseline
  **30.26 fps** (walker own 24.94 ms, wait 10.76 ms) vs NO_DRAW **41.22 fps**
  (walker own **2.15 ms**, wait 22.11 ms), paired diff **-10.96 fps**
  (CI -17.69..-4.23). Every segment is faster with NO_DRAW, and the NO_DRAW
  arm is far steadier (std 0.6 vs 6.5 fps). **Walker draw work is the frame
  limiter.** With it removed, the PPU/guest side settles at ~41.5 fps.
* **The draw source is mixed, not just the SPU slots.** Under WALKER_PROF the
  baseline spends 3.8-34.6 ms/frame in draws fed from the PPU main ring and
  4.4-11.1 ms/frame in draws fed from the five SPU mesh rings (EA
  0x11900000-0x122fffff); the mesh share is ~20-75% of draw time by segment.
  A native-renderer fast path should cover both, with the mesh slots a large
  first target.
* **`EXP_FAST_WAKE=1` is not viable at the pitch.** It wedged twice at the
  same segment (5th B): the FIFO hit a jump-park resync, the frame's commands
  were dropped, and flips stopped. It is a busy poll replacing the 4 ms idle
  block; it lets the walker re-read an unpatched park. At the start screen it
  changed nothing (46.5 vs 46.5 fps) because the 16 ms tick, not the 4 ms
  sleep, is the pacer.
* **Pipeline creation is not the scene-change stutter.** There is no
  `VkPipelineCache` (every create passes `VK_NULL_HANDLE`), but a whole run
  makes only ~340-430 pipelines and spends **30-41 ms total** (p50 29-36 us,
  max 0.4-6.9 ms). The start-screen transition stutter is orders larger.
* **The display is tick-quantized at 16 ms.** At the title, draw frames wait
  14-19 ms and idle frames wait 28-33 ms (one vs two 16 ms ticks); walker own
  CPU is 0.1-0.6 ms. Gameplay driven by vblank/flip callbacks is therefore
  tied to the 60 Hz tick; a >60 fps target needs a tick/timing change.

## What was added

### Live control file (`PS3RECOMP_CTL`)

`libs/video/ps3_live_ctl.c` parses `NAME=value` lines and is polled from the
walker thread (`cellGcm_rsx_process_fifo_unlocked`, once per drain pass, which
is at least once per flip). The only cost is a `stat()` comparing
mtime+size+inode; a change is applied to per-key atomics and logged:

```
[live-ctl] flip=421 PS3RECOMP_WALKER_FAST=1 (was 0)
```

Switch names are the environment variables themselves; the environment seeds
them at first use, so a run without a control file is unchanged. Live keys:

| key | effect |
|---|---|
| `PS3RECOMP_WALKER_FAST` | fetch-plan/remap and pipeline memos; changing it flushes both |
| `PS3RECOMP_WALKER_WAKE` | wake the parked walker on an SPU PUT in its range |
| `EXP_NO_DRAW` | return from the draw sink before fetch/texture/pipeline/backend recording |
| `EXP_FAST_WAKE` | walker re-polls instead of the 4 ms block; lwcond rechecks yield |

The engine's hot paths still read cached ints; `cellGcmSys` applies the live
values once per drain pass through `rsx_draw_engine_set_walker_fast/no_draw`.

### Thread names and opt-in pinning (`PS3RECOMP_PIN=1`)

`include/ps3emu/thread_name.h` now sets the Linux/macOS task name (not just the
trace track). `runtime/platform/thread_affinity.c` reads sysfs topology and
prints the mapping once. On this Ryzen 5 2600X:

```
[pin] PS3RECOMP_PIN=1 topology online=0-11: walker cpu 0 (core 0, L3 group 0),
ppu main cpu 1 (core 1, L3 group 0), other cpus 2-5,8-11;
masks walker=0x1 ppu=0x2 other=0xf3c (walker/ppu siblings idle)
```

The walker gets one hardware thread with its sibling idle, `ppu main` another
core of the same L3/CCX, and every other named thread (PPU, SPURS, present)
shares the rest. `ps3_thread_apply_affinity_self` reuses `apply_affinity` in
`runtime/platform/win32_compat.c`.

### `recomp-work/ab-tools/ab_live.sh`

One run, one window, one game instance:

1. preflight: no user `MyGameRecomp`, governor `performance`, ISO mounted;
2. boot, scripted nav to the pitch (or park at the press-start screen);
3. **auto-settle** until the mean of the last 20 `[walker-prof]` reports is
   <= 20 ms for 8 s (`AUTO_SETTLE=0 SETTLE=10` reproduces the original fixed
   10 s wait);
4. alternate A/B every 10 s, N=6 segments per arm, rewriting the control
   file atomically; drop the first 2 s of each segment;
5. a 5 s no-flip guard (also catches the jump-park resync);
6. report per-segment fps from `[flip]`, per-arm mean and 95% CI, the paired
   difference with CI, walker own/wait ms from `[walker-prof]`, and a
   MangoHud frametime PNG with A/B bands via
   `mlb16-recomp/tools/frametime_plot.py --segments`.

`PS3RECOMP_VK_PRESENT_STATS=1` is kept; the report prints the number of stats
seconds with `shown=0` (window freeze) and the no-slot drop count.

### `EXP_NO_DRAW` and the draw-source counter

`sink_end` returns immediately (`rsx_draw_engine.c:4690`) when the switch is
on: no fetch plan, no vertex conversion, no texture decode, no pipeline, no
backend/Vulkan recording. `sink_begin`, the draw/index/inline/immediate
callbacks, `sink_clear` and `sink_flip` still run, so the method stream,
fences, labels and flips are unchanged; the image is the last presented
frame.

Fences do not depend on the draw engine: `gcm_ref_push_at`
(`cellGcmSys.c:1800`) and `gcm_ref_publish_one` (`:1830`) are in the walker's
guest-memory path, and the `NV4097` back-end/texture-read labels are written
at dispatch in `rsx_commands.c:525-556`. WCB readback *does* need the real
draws (`eng_wcb_resolve`, `rsx_draw_engine.c:2460`), so the NO_DRAW ceiling
run used WCB off in **both** arms. The NO_DRAW arm kept flipping at 41.5 fps
and stayed at the pitch.

The draw-source counter is always on under `PS3RECOMP_WALKER_PROF`: a group
whose active vertex arrays resolve into the five 2 MB SPU mesh rings (EA
0x11900000-0x122fffff, i.e. the 0x90-byte slots' ring data) is `mesh`; every
other group -- PPU-built main ring, inline/immediate geometry -- is `main`.
The report adds `src_main/src_mesh` (draw packets) and `mainms/meshms` (the
sink time, per frame).

### Pipeline creation timing

Every `vkCreateGraphicsPipelines`/`vkCreateComputePipelines` in
`rsx_vulkan_backend.c` (lines 1142, 1485, 3726, 4065) is timed and reported
as `pipecrt`/`pipems` in the next `[walker-prof]` line; with
`PS3RECOMP_PIPE_TIMING=1` each create also logs
`[pipe-create] kind <us> at <monotonic ms>`.

## Measurement validity: the pitch is not stationary

The first A/A run (native Wayland, FIFO) froze the window from 13:42:00 on:
`blits 0, shown 0, dropped ~70 no-slot/s` for the rest of the run, MangoHud
logged only the first 7 s, and the guest fps drifted. That is the failure in
`docs/window-freeze-fix.md`, not a measurement of the arms. The retry (and all
later runs) used `SDL_VIDEODRIVER=x11` (Xwayland), the documented fallback:
2-5 frozen seconds, 11-128 no-slot drops over the whole run, CSV complete.

Even with a clean window, the first ~1-3 minutes at the pitch are a warm-up:
the guest opens `CACHE_C8372833` (171 MB), `HeadFaceCacheFile`,
`MLBPS3_GD.WAD`, `CROWD.SKX`, `PA.SKX`, ... and the walker mean goes from
~14 ms to 25-45 ms and back. A/A segments starting 10 s after nav therefore
swing 16-38 fps (PIN off retry: A 21.61, B 22.60; std 5.8/5.9) and even a
fast/idle phase can be caught by a fixed threshold. The harness now waits for
the idle walker mean.

### A/A results (settled)

| run | arm A fps | arm B fps | paired diff (95% CI) | segment std A/B | range A/B |
|---|---:|---:|---:|---:|---|
| PIN off | 29.62 +/- 4.42 | 29.92 +/- 5.14 | **-0.31 +/- 5.25** [-5.56, +4.95] | 4.21 / 4.89 | 24.32-36.98 / 24.39-37.66 |
| PIN on  | 29.20 +/- 2.27 | 29.31 +/- 1.83 | **-0.11 +/- 1.62** [-1.73, +1.51] | 2.16 / 1.74 | 25.79-32.15 / 27.12-32.15 |

Both point estimates satisfy "within ~0.5 fps"; both CIs include 0. Pinning
plus the later settle reduces the segment spread by ~2.5x. The honest limit:
the best pair resolves ~1.6 fps, not 0.5 fps, with 6 x 10 s per arm. A 3 fps
change needs either the pinned configuration and many more segments, or a
quieter scene than the pitch.

Earlier fixed-10 s A/A attempts (same arms, 10 s after nav) were
PIN off **-1.00 +/- 6.53** (std 5.76/5.87) and PIN on **-0.80 +/- 5.07** (std
5.02/6.83): the warm-up dominates, not the switch.

## Ceiling 1: `EXP_NO_DRAW` -- walker draw work is the limit

Run `ab_live_ceil_nodraw_20261102_142204`, Xwayland, PIN on, WCB off, N=6.
Baseline arm A vs NO_DRAW arm B; every pair is faster with NO_DRAW:

| segment | A fps | B fps | A walker/wait ms | B walker/wait ms |
|---:|---:|---:|---:|---:|
| 0 | 36.02 | 40.98 | 17.27 / 10.50 | 2.13 / 22.26 |
| 1 | 35.06 | 41.34 | 18.33 / 10.47 | 2.27 / 21.92 |
| 2 | 32.84 | 41.67 | 20.78 / 9.74 | 1.54 / 22.45 |
| 3 | 25.05 | 40.17 | 28.42 / 11.53 | 3.74 / 21.15 |
| 4 | 19.56 | 41.50 | 45.43 / 11.61 | 1.68 / 22.42 |
| 5 | 33.01 | 41.66 | 19.38 / 10.71 | 1.53 / 22.47 |
| **mean** | **30.26 +/- 6.83** | **41.22 +/- 0.60** | **24.94 / 10.76** | **2.15 / 22.11** |

Paired difference **-10.96 fps (95% CI -17.69..-4.23)**. The NO_DRAW arm's
own walker CPU falls to 1.5-3.7 ms; its wait rises to ~22 ms because the
walker is now idle waiting for the PPU, which becomes the pacer at ~41.5 fps.
The baseline's low segments are the heavy draw phases (draws up to 1127
packets/frame, walker up to 45 ms); NO_DRAW removes both the cost and its
variability.

Draw-source split in the baseline arm (`mainms` = PPU main ring, `meshms` =
SPU mesh rings), same segments:

| segment | src_main draws | src_mesh draws | main ms | mesh ms |
|---:|---:|---:|---:|---:|
| 0 | 899 | 1273 | 3.82 | 10.89 |
| 1 | 1486 | 1274 | 5.69 | 10.13 |
| 2 | 2579 | 484 | 14.29 | 4.38 |
| 3 | 1467 | 1217 | 14.47 | 11.09 |
| 4 | 6055 | 928 | 34.62 | 7.27 |
| 5 | 1328 | 553 | 12.42 | 5.10 |

The mesh slots are a large, sometimes dominant, part of the draw cost, but
the PPU-built main-ring draws are also substantial. **A native-renderer fast
path for the mesh slots is the next project; it should also cover the main
ring for the full ceiling.**

## Ceiling 2: `EXP_FAST_WAKE` -- not viable at the pitch

Two runs (planned + one retry), Xwayland, PIN on, WCB off. Both stopped at
the same place: the 5th B (FAST_WAKE) segment, at guest ms **300.79** and
**300.97**, `[flip]` stopped advancing. The log shows:

```
[FIFODUMP] +0 io=007C1780 w=207C1780   <-- bad word
[cellGcmSys] FIFO resync (jump-park) 0x007C1780 -> put 0x0000F370
[cellGcmSys] resync rescued 2 fence(s) and 0 label write(s) ...
[RSX vulkan] window: 105469 pumps ... blits 0, shown 0 ...
```

The busy poll re-reads a self-JUMP park before the PPU patches it; the park
grace expires and the resync drops the frame's commands (including the
flip). The 4 ms block is not dead time: it gives the producer time to patch.
The same failure twice is the stop condition; the both-NO_DRAW-and-FAST_WAKE
arm was therefore not run.

At the start screen FAST_WAKE did not wedge, but it did not help either. In
the stable intervals (segments 2-5) both arms are **46.4-46.7 fps**; the
paired start-screen difference is not usable because the title screen
auto-advanced at ~113 s (fps 46 -> 27).

### Where the start-screen time goes (static + measured)

The title screen runs ~42.9 fps over 51-62 s. The per-frame walker reports
show two frame shapes (e.g. at 51.5-51.8 s):

* draw frame: `draws=8`, walker **0.29-0.59 ms**, wait **13.6-19.0 ms**;
* idle frame: `draws=0`, walker **0.13 ms**, wait **28.0-33.5 ms**.

The wait is the frame clock's 16 ms tick. `cellGcmTickFlip`
(`cellGcmSys.c`) only completes a flip when a tick runs; `main.cpp:263` does
`next_tick += 16` and the outer loop calls `cellGcm_fifo_kick_wait(idle_ms)`
(`:243`). A request that misses the tick waits one tick (~14-19 ms); one that
just misses two tick boundaries waits two (~28-33 ms). That is the 18/34 ms
alternation and the ~43-46 fps average. The 4 ms idle sleep and the
`lwcond` 1 ms recheck are inside that; removing them (`EXP_FAST_WAKE`) cannot
beat the tick, which is why the start-screen A/B is flat.

## Pipeline cache: there is none, and it is not the stutter

Every create passes `VK_NULL_HANDLE` as the cache
(`rsx_vulkan_backend.c:1142, 1485, 3726, 4065`); the file has no
`VkPipelineCache`. Timing over the runs:

* ~340-430 creations per run, **30-41 ms total**;
* mean 74-123 us, p50 **29-36 us**, max 0.4-6.9 ms (one 28.8 ms boot create
  in the start-screen run);
* at the pitch `pipecrt=0`; the `[walker-prof] pipe=` spikes (up to ~5 ms in
  the heavy frames) are memo/decompile work, not `vkCreate*Pipelines`.

The start-screen transition (46 -> 27 fps) and the pitch warm-up are not
explained by a ~0.03 s total. A `VkPipelineCache` would save that 0.03 s of
warm-up, not the stutter; do not implement it as a performance fix.

## Game timing: the 16 ms tick

`main.cpp` runs a 16 ms tick (`next_tick += 16`). `cellGcmTickVBlank` bumps
the vblank count and `cellGcmTickFlip` completes a flip only on a tick. The
guest's vblank/flip callbacks and flip status therefore advance at 60 Hz at
most, regardless of how fast the walker drains. The start-screen 18/34 ms
alternation is the direct measurement of that quantization. Gameplay or
animation that consumes vblank/flip callbacks is tied to the tick; a >60 fps
target would need a smaller tick (or off-tick flip/flip-completion delivery)
and a check of the guest's fixed-step assumptions. The outer loop does drain
at the 4 ms kick cadence and can present between ticks, but the flip
*completion* the guest waits on is tick-gated.

## Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools

# A/A (PIN on, settled); PIN=0 for the unpinned arm
TAG=aa_pin1_settled N=6 PIN=1 WCB=2 \
  ARM_A="PS3RECOMP_WALKER_FAST=0" ARM_B="PS3RECOMP_WALKER_FAST=0" ./ab_live.sh

# NO_DRAW ceiling (WCB off: readback needs the real draws)
TAG=ceil_nodraw N=6 PIN=1 WCB=0 \
  ARM_A="EXP_NO_DRAW=0" ARM_B="EXP_NO_DRAW=1" ./ab_live.sh

# FAST_WAKE (from the pitch this wedges; see above)
TAG=ceil_fastwake N=6 PIN=1 WCB=0 \
  ARM_A="" ARM_B="EXP_FAST_WAKE=1" ./ab_live.sh

# start screen, baseline vs FAST_WAKE
TAG=start_fastwake START_SCREEN=1 N=4 PIN=1 WCB=0 \
  ARM_A="" ARM_B="EXP_FAST_WAKE=1" ./ab_live.sh
```

The harness defaults to `SDL_VIDEODRIVER=x11`; set `VIDEODRIVER=wayland` to
reproduce the native-Wayland path (it froze here). `AUTO_SETTLE=0 SETTLE=10`
reproduces the original 10 s fixed wait; `AUTO_SETTLE=1` (default) waits for
the idle walker mean.

## Runs and frametime PNGs

Valid runs (all under `recomp-work/`):

| run | log | frametime PNG |
|---|---|---|
| A/A PIN off (settled) | `ab_live_aa_pin0_settled_20261102_140417.log` | `ab_live_aa_pin0_settled_20261102_140417_ft.png` |
| A/A PIN on (settled) | `ab_live_aa_pin1_settled_20261102_141235.log` | `ab_live_aa_pin1_settled_20261102_141235_ft.png` |
| NO_DRAW ceiling | `ab_live_ceil_nodraw_20261102_142204.log` | `ab_live_ceil_nodraw_20261102_142204_ft.png` |
| start screen | `ab_live_start_fastwake_20261102_143955.log` | `ab_live_start_fastwake_20261102_143955_ft.png` |

Also produced, but invalid for the reasons in the text:

* `ab_live_aa_pin0_20261102_134150_ft.png` (native-Wayland freeze)
* `ab_live_aa_pin0_retry_20261102_134942_ft.png` (10 s wait, warm-up drift)
* `ab_live_aa_pin1_20261102_135722_ft.png` (10 s wait, warm-up drift)
* FAST_WAKE failures: `ab_live_ceil_fastwake_20261102_142848.log`,
  `ab_live_ceil_fastwake_retry_20261102_143426.log` (no report/PNG; the
  script stopped on the no-flip guard)

## Caveats

* The pitch is not a fixed workload. Even after auto-settle, per-segment fps
  spreads 2-5 fps; the A/A CI is the honest resolution (1.6 fps best case).
  Do not read a 1-2 fps arm difference from one 6 x 10 s run.
* The auto-settle threshold is a heuristic: the PIN-off run started in a
  brief fast phase and then drifted; the PIN-on run waited longer and was
  stable. A settle based on the walker mean over 20 s would be stricter.
* The NO_DRAW baseline arm was noisy (19.6-36.0) while NO_DRAW was steady
  (40.2-41.7). The paired difference is still positive in every segment, but
  the absolute baseline mean depends on which warm-up phase was sampled.
* `EXP_NO_DRAW` was run with WCB off in both arms; it does not test WCB
  readback or visuals.
* `EXP_FAST_WAKE` was stopped after the same wedge twice (rule: same failure
  twice -> stop). The start-screen run is a different scene and did not
  wedge, but its title screen auto-advanced.
* Pipeline timings run on the walker thread and include driver time; the
  `at` field in `[pipe-create]` is the process monotonic clock, not the
  guest-relative flip clock.
* One game instance at a time; the Xwayland run was used because native
  Wayland froze again despite `PS3RECOMP_VK_PRESENT_MODE=fifo`.
