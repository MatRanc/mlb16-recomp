# The pitch-meter scene: clean baseline, the guest cadence, and why NO_DRAW does not help here

Date: 2026-11-02 (evening). Branch `exp/live-ab` in the `ps3recomp-play`
worktree (off `play/multiscene`), built as `recomp-work/build-play`. Windowed
runs with `PS3RECOMP_VK_WINDOW=1`, `VK_DUMMY_BLACK=1`,
`PS3RECOMP_VK_PRESENT_MODE=fifo`, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`,
`PS3RECOMP_WALKER_PROF=1`, `PS3RECOMP_PIN=1`, MangoHud CSV, and
`SDL_VIDEODRIVER=x11`. All numbers are one game launch per run; the scene
thumbnails below show every run stayed on the meter.

## TL;DR

* **The pitch meter is a stable scene, but it is guest/display-cadence bound
  at ~30-34 fps, not draw bound.** The guest asks for a flip roughly every
  30 ms (two 16 ms ticks); the walker has 19-27 ms of work inside that budget.
* **`EXP_NO_DRAW` does not raise fps on the meter.** WCB=0 baseline **33.55
  fps** (walker 19.17 ms, wait 10.76) vs NO_DRAW **30.92 fps** (walker
  **3.78 ms**, wait **28.60**), paired **+2.63 fps** for the baseline
  (95 % CI +1.78..+3.48). Removing 15.4 ms/frame of walker draw work leaves
  the frame at two ticks and makes it slightly slower, not faster. The old
  pitch-wait NO_DRAW ceiling (~41 fps) does not reproduce on the meter.
* **A/A on the meter (WCB=2, N=8):** A **29.34 +/- 0.64 fps** (std 0.77),
  B **29.93 +/- 0.89** (std 1.07), paired **-0.59 +/- 1.09 fps**
  (95 % CI -1.67..+0.50). The CI is tighter than the old ~1.6 fps; the point
  estimate is 0.59 fps (one low first segment) and the CI includes 0.
* **`PS3RECOMP_WALKER_FAST=1` is slower on the meter:** A **29.18 fps** vs
  B **27.21 fps**, paired **1.98 fps** (95 % CI +1.01..+2.94). Walker
  27.36 -> 30.71 ms/frame, mostly fetch (4.07 -> 6.57) and tex
  (10.16 -> 11.39). Do not enable it as a cut.
* **The normal visual config (WCB=2) is slower than WCB=0** on this scene:
  29.3-29.9 vs 33.6 fps. The WCB readback path costs ~7.5 ms of walker time,
  concentrated in the main-ring draws and tex (see the phase table). That is
  a bigger immediate lever than a native mesh path if the same visuals are
  required.
* **Ranked draw work still exists** (mesh 11.6 ms, tex 9.8 ms, main 10.2 ms,
  fetch 4.0 ms, draw 4.7 ms per frame), but on the meter it buys **0 fps**
  until the guest cadence/tick is addressed. The draw fast path should be
  judged on a scene where the guest asks for the next flip sooner.

## Harness changes: nav ends on the meter, and the run is filmed

`recomp-work/ab-tools/ab_live.sh` and `measure_fast.sh` now source
`meter_lib.sh`:

1. Reach the pitch as before (`nav3.py` unchanged; it stops at the lineup and
   the scripts poll for `ref/pitch_wait.png`).
2. Press X **once** (`pad.txt` = `0x4000 6`, CROSS) and verify the meter from
   the frame dump against `ref/pitch_meter*.png`. The match region is
   `370,95,395,120` (640x360), the upper part of the meter arc; threshold 30
   mean abs diff. `pitch_wait.png` scores 93 in this region, so the check does
   not accept the pre-X screen. A second press is never written.
3. `METER=1` is the default; `METER_CAL=1` saves `meter_cal_plus{1,2,4,8,16,30}s`
   and `ref/pitch_meter_cal.png` when a reference is missing.
4. Settle with a fixed **10 s** after the X press (`AUTO_SETTLE=0` default).
   The calibration run shows the walker falls from 30-40 ms in the last
   seconds before X to 27.2 ms in +0..5 s, 27.4 in +5..10, 26.9 in +10..15,
   and stays at 25-29 ms for the next 80 s; 10 s is after the plateau.
5. `SCENE_SHOTS=1` (default) copies `frame.ppm` to `scene_<epoch-ms>.png`
   every 2 s from launch; `scene_sheet.py` writes `scene_sheet.png` with the
   measured A/B bands (the sheet keeps the 30 s before the first switch plus
   the measurement, not boot/nav).

The meter has a different visual with WCB off: `ref/pitch_meter_wcb0.png`
is the second reference. The first ceiling attempt stopped on the WCB=0
meter because only the WCB-on reference existed (a verification failure, not
a game failure); it was retried once with both references.

## Runs

All runs used the scripted home-team nav, X once, 10 s settle, 10 s segments,
2 s dropped.

### 1. A/A, WCB=2, N=8

| segment | A fps | B fps | A walker/wait ms | B walker/wait ms |
|---:|---:|---:|---:|---:|
| 0 | 28.13 | 31.36 | 27.73 / 7.62 | 24.73 / 7.11 |
| 1 | 29.36 | 28.91 | 26.42 / 7.03 | 27.33 / 7.87 |
| 2 | 30.00 | 30.01 | 25.33 / 7.27 | 26.27 / 7.13 |
| 3 | 29.20 | 31.04 | 26.75 / 7.42 | 25.03 / 7.07 |
| 4 | 30.41 | 30.66 | 25.82 / 7.25 | 25.45 / 7.28 |
| 5 | 29.84 | 30.06 | 26.25 / 7.37 | 25.93 / 7.20 |
| 6 | 28.45 | 28.64 | 28.58 / 7.46 | 26.64 / 8.29 |
| 7 | 29.34 | 28.72 | 26.96 / 7.14 | 27.51 / 7.42 |
| **mean** | **29.34 +/- 0.64** | **29.93 +/- 0.89** | **26.73 / 7.32** | **26.11 / 7.42** |

Segment std 0.77/1.07 fps. Paired difference **-0.59 +/- 1.09 fps**
(95 % CI -1.67..+0.50). The first A segment is the low one (28.13). The
control file names the same key/value in both arms and the live-ctl self-check
passes 16/16, so this is drift/noise, not an arm difference. The CI half-width
(1.09) is the clean-meter resolution; compare with the old pitch A/A's ~1.6.

### 2. WCB=0 ceiling: baseline vs `EXP_NO_DRAW=1`, N=6

`BOOT_ARM="EXP_NO_DRAW=0"` so the nav has a picture; both arms WCB=0 because
the NO_DRAW readback path needs real draws.

| segment | A baseline fps | B NO_DRAW fps | A walker/wait | B walker/wait |
|---:|---:|---:|---:|---:|
| 0 | 33.27 | 31.21 | 19.44 / 10.68 | 3.66 / 28.64 |
| 1 | 33.47 | 30.39 | 19.24 / 10.58 | 3.83 / 29.10 |
| 2 | 32.87 | 30.45 | 19.26 / 11.16 | 3.85 / 29.03 |
| 3 | 33.74 | 31.20 | 19.12 / 10.68 | 3.76 / 28.10 |
| 4 | 34.24 | 30.27 | 18.97 / 10.60 | 3.92 / 29.12 |
| 5 | 33.69 | 32.01 | 19.00 / 10.85 | 3.65 / 27.63 |
| **mean** | **33.55 +/- 0.49** | **30.92 +/- 0.71** | **19.17 / 10.76** | **3.78 / 28.60** |

Paired difference A-B **+2.63 fps** (95 % CI +1.78..+3.48): the baseline is
faster. The NO_DRAW walker is idle in a 28.6 ms wait; the frame interval is
~32 ms (two ticks), so the guest/display cadence, not draw work, sets the
rate. This is the opposite of the pitch-wait result (NO_DRAW 41.22 vs 30.26);
the meter is a different guest workload. The WCB=0 baseline is also faster
than the WCB=2 baseline (33.55 vs 29.34), see the next table.

### 3. WCB=2, `WALKER_FAST=0` vs `WALKER_FAST=1` (live), N=6

| segment | A fps | B fps | A walker ms | B walker ms |
|---:|---:|---:|---:|---:|
| 0 | 29.42 | 26.61 | 26.96 | 31.89 |
| 1 | 29.13 | 26.93 | 27.22 | 30.81 |
| 2 | 29.92 | 26.75 | 27.05 | 31.51 |
| 3 | 28.97 | 27.57 | 27.43 | 30.49 |
| 4 | 29.19 | 28.47 | 26.82 | 29.09 |
| 5 | 28.46 | 26.89 | 28.68 | 30.45 |
| **mean** | **29.18 +/- 0.51** | **27.21 +/- 0.73** | **27.36** | **30.71** |

Paired **1.98 fps** (95 % CI +1.01..+2.94). The fast arm is **slower**:
fetch 4.07 -> 6.57 ms, tex 10.16 -> 11.39 ms, mainms 10.75 -> 12.73,
meshms 11.55 -> 12.89. `WALKER_FAST` does not survive as a cut on the meter.

## Per-phase walker time on the meter (WCB=2 baseline)

From the N=8 A/A run (A arm; the N=6 fast-run A arm agrees):

| phase | ms/frame | note |
|---|---:|---|
| walker (drain) | 26.73 | own time; wait 7.32 |
| engine (dispatch) | 25.21 | sum of the phases below |
| parse | 1.50 | drain - dispatch |
| fetch | 4.01 | vertex fetch/remap |
| tex | 9.80 | texture decode/upload |
| pipe | 1.91 | pipeline memo/setup |
| bind | 0.21 | |
| draw | 4.75 | draw recording/submission |
| other | 4.54 | unaccounted inside dispatch |
| **source: main ring** | **10.20** | 987.6 draw packets/frame |
| **source: SPU mesh** | **11.57** | 1392.7 draw packets/frame |

For the WCB=0 baseline (ceiling run A) the same phases are: engine 17.68,
parse 1.49, fetch 4.04, **tex 4.29**, pipe 1.86, bind 0.21, draw 4.96,
other 2.33, mainms **4.25**, meshms **12.12**. The WCB readback path costs
~5.5 ms of tex and ~6 ms of main-ring sink time in the normal config; the
mesh source is essentially unchanged (11.6 vs 12.1).

## Ranked list for a draw fast path

The ms are walker time removable per frame at the meter; they do **not**
convert to fps here because the 28.6 ms NO_DRAW wait/two-tick cadence bounds
the frame. Treat this as headroom for a future cadence/tick change.

1. **Mesh-ring draw path** -- 11.57 ms/frame (1393 draw packets/frame). The
   largest source block; a native path for the five SPU mesh rings removes
   most of it. Expected save: up to 11.57 ms/frame at the meter.
2. **Texture decode/upload** -- 9.80 ms/frame (within the source blocks).
   WCB=0 drops this to 4.29 ms, so a large part is the WCB/main-ring texture
   path; a native decode + upload fast path is the next biggest phase.
3. **Main-ring draw path** -- 10.20 ms/frame (988 packets). The WCB=0
   baseline is 4.25 ms, so the normal visual config pays ~6 ms here for
   readback; if the visuals need WCB, this is a separate 6 ms target.
4. **Vertex fetch/remap** -- 4.01 ms/frame. The fetch plan is a per-group
   cost that a native vertex path can amortize.
5. **Draw recording/submission** -- 4.75 ms/frame.
6. **Pipeline memo/setup** -- 1.91 ms/frame; pipeline creates are negligible
   (`pipecrt=0` at the meter).
7. **Parse** -- 1.50 ms/frame; **other** -- 4.54 ms/frame.

Ordering by source, mesh first, then the main ring; ordering by phase, tex
first, then draw/fetch. Both orders overlap: tex/fetch/draw are costs inside
the source blocks, not additive with mainms/meshms.

The honest ceiling: `EXP_NO_DRAW` removes 19.17 - 3.78 = **15.4 ms** of
walker time (WCB=0) and changes fps by **-2.6**. A draw fast path that saves
10 ms on the meter would still be hidden by the ~28.6 ms wait. The next
experiment should test the guest cadence (e.g. `EXP_TICK_US=8000` with
NO_DRAW on the meter) before spending the native-renderer effort.

## Frametime PNGs and contact sheets

Read the green line as the host present interval and the number in each band
as the guest `[flip]` fps from the report. The contact sheets show the
pitch/meter scene for the whole measurement; the B bands are the same image
for NO_DRAW because that arm presents the last real frame.

* A/A WCB=2: `recomp-work/ab_live_aa_meter_20261102_162639_ft.png`;
  `.../ab_live_aa_meter_20261102_162639/scene_sheet.png`. Bands A 28/29/30/
  29/30/30/28/29, B 31/29/30/31/31/30/29/29.
* Ceiling WCB=0: `recomp-work/ab_live_ceil_nodraw_meter_retry_20261102_164417_ft.png`;
  `.../ab_live_ceil_nodraw_meter_retry_20261102_164417/scene_sheet.png`.
  A 33/33/33/34/34/34, B 31/30/30/31/30/32. The B present trace is slightly
  higher (slower), not faster.
* WALKER_FAST A/A WCB=2: `recomp-work/ab_live_fast_meter_20261102_165120_ft.png`;
  `.../ab_live_fast_meter_20261102_165120/scene_sheet.png`.
  A 29/29/30/29/29/28, B 27/27/27/28/28/27.
* Calibration: `recomp-work/ab_live_meter_cal_20261102_161911_ft.png`;
  `.../ab_live_meter_cal_20261102_161911/scene_sheet.png`; post-X frames
  `meter_cal_plus{1,2,4,8,16,30}s.png`.

## Launch check (task 5)

The current `recomp-work/play_windowed.sh` (which now defaults to
`SDL_VIDEODRIVER=x11`, HUD on) launched `build-play` and reached the title:
`[flip] 2940 at 74101 ms`, steady **42.3-42.6 fps**, window `shown=63-68`
with **0 no-slot drops**, no `frame_clock` / address-0 / SIGSEGV or core.
Log: `recomp-work/play_20261102_170429.log`.

An earlier launch at 16:59 used the pre-change launcher (native Wayland) and
reproduced the documented present freeze at 16:59:39 (`shown=0`, 61-67
no-slot drops/s) while flips continued; that is the window path, not the
address-0 crash. The Xwayland default is the clean path.

## Caveats

* One run per arm pair; the A/A point estimate is 0.59 fps (CI includes 0) and
  the first A segment is low. Do not read a sub-1 fps arm difference from one
  run.
* The NO_DRAW arm is a stale image (last real frame); the meter is static, so
  the contact sheet still proves the scene.
* The meter visual differs with WCB off; two reference crops are required.
  The first ceiling attempt stopped on that verification mismatch and was
  retried once.
* The "30 Hz" statement is the measured flip interval (~30-32 ms) and wait
  (~28.6 ms NO_DRAW, ~10.8 baseline), not a claim about a named guest timer.
  A tick/cadence experiment is the next step; do not build the native fast
  path on the strength of this scene alone.
* PIN on, x11 (Xwayland); the native-Wayland launcher freeze is separate.
