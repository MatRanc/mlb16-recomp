# Frame pacing: the present pairs are two guest flips handed over back to back; pace the window at the arrival cadence

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. Windowed meter runs: `ab_live.sh`, pitch meter
(X once), `METER=1`, `WCB=2`, `PIN=1`, `PS3RECOMP_RSX_CUBE_RT=1`, x11,
`PS3RECOMP_VK_PRESENT_NEW_ONLY=1` (now `ab_live.sh`'s default). One game
launch per run, 12 segments (6 per arm, 10 s each, 2 s dropped).

## TL;DR

* **The pairs are two guest frames handed to the window back to back, not
  re-presents and not the FIFO.** With `PRESENT_NEW_ONLY=1` every flip is
  presented exactly once (`flips not shown: 0`, no duplicate `show` lines).
  The `[fliplog]` `pres` timestamps (the frame clock's handoff to the window)
  already arrive 1-4 ms apart: the guest runs up to three flips ahead of the
  display, the drain catches up, and two flips are decoded and presented in
  one pass. The FIFO/present thread then reports them at 1 ms and 47 ms.
* **`PS3RECOMP_VK_PACE=1` (live key, default off) removes the 1 ms clamp with
  no guest-fps cost.** The present thread holds each queued present until its
  slot in an arrival-cadence schedule. Present intervals at the meter
  (`[fliplog] show`, measured windows): p50 **46.46 -> 38.43 ms**, p90
  **58.30 -> 51.60**, std **24.28 -> 14.90**, intervals < 8 ms
  **32.2 % -> 0.0 %**, > 40 ms **61.8 % -> 45.2 %**. MangoHud's CSV says the
  same (p50 **45.71 -> 39.04 ms**, std **23.80 -> 15.96**, < 8 ms
  **28.8 % -> 3.2 %**). Guest fps is unchanged: A **29.30 +/- 0.86**, B
  **29.07 +/- 0.76**, paired A-B **+0.22 +/- 0.57** (95 % CI -0.35..+0.79),
  A/A check **PASS**.
* **No input latency beyond one frame.** The pacer delays only the
  `vkQueuePresentKHR` call, never the frame's GPU work (the swapchain blit is
  submitted when the frame ends) and never the walker. A frame that is late
  is presented at once, and the schedule is capped at one interval of wait;
  `pres -> show` p90 at 30 fps is 17 ms in the B arm (5 ms in A), i.e. at most
  one frame.
* **Cube probes: an incomplete probe now binds the last complete image for
  that address; only a probe that was never complete keeps the dummy.** In
  the pregame 368 log-deduplicated frames sample a cube whose address had
  been complete before and is temporarily incomplete; those used to bind the
  black dummy and now bind the last image. The remaining 1,435 `cube` census
  misses are addresses that were never complete (a probe being built for the
  first time). The 2 s thumbnails show no within-scene bright/dark pair.
* **`ab_live.sh` now exports `PS3RECOMP_VK_PRESENT_NEW_ONLY=1` by default**
  and `play_windowed.sh` (both copies) sets `PS3RECOMP_VK_PACE=1` by default
  (`PS3RECOMP_VK_PACE=0` restores the old timing).

## 1. Static: where a flip's time goes

All on the branch above:

* `gcm_flip_request` (`libs/video/cellGcmSys.c:3715`) is the one place a flip
  is counted (`s_flip_frame_count`, the `PRESENT_NEW_ONLY` edge). MLB's
  libgcm is statically linked and writes `0xFEADxxxx` words into its ring;
  the drain decodes them (`cellGcmSys.c:2852`) and reaches
  `cellGcmSetFlipCommand` there. The log has no `[flipword]` lines from
  `_cellGcmSetFlipCommand`, so the title does not use the HLE marker path and
  **request and walker finish are the same drain instant**.
* The frame clock (`templates/project/main.cpp`, `frame_clock`) is the RSX
  walker. On a pending flip it calls `present_guest_frame()` *before* the
  next drain, so the presented batch is exactly the completed frame; the
  outer pass also presents when `cellGcm_flip_request_count` moves.
* `rsx_vulkan_backend_present()` records the swapchain blit and submits it;
  `vk_present_window` (`libs/video/rsx_vulkan_backend.c:2353`) applies the
  `PRESENT_NEW_ONLY` edge, acquires a swapchain image, records/submits the
  blit into it, and hands the image index to the present thread. The present
  thread (`rsx_vulkan_backend.c:2187`) waits for the blit fence and calls
  `vkQueuePresentKHR` (FIFO).

The four observable stages are therefore:

| stage | where | meaning |
|---|---|---|
| `req` | `gcm_flip_request` | guest flip counted; for this title the drain decode |
| `fin` | same for this title (drain decode of `0xFEAD`) | the walker executed it |
| `pres` | `vk_present_window`, after the new-frame gate | handed to the window blit |
| `show` | present thread after `vkQueuePresentKHR` | the display/MangoHud saw it |

### The per-flip log (`PS3RECOMP_FLIP_LOG=1`, default off)

`cellGcmSys.c:1104-1160`: a 1024-entry ring indexed by flip seq, one line per
presented flip when `show` is stamped, plus `skipped` lines when a slot is
reused by a flip that was never presented:

```
[fliplog] n=5673 req=72195412401 fin=72195412401 pres=72195412401 show=72195429556
          req_to_fin=0 fin_to_pres=17024 pres_to_show=... show_gap=...
```

(Real lines are one line; wrapped here.) `show_gap` is the interval since the
previous presented flip. `cellGcm_flip_log_present/shown` are the two hooks
the Vulkan backend calls.

### Where the pairs come from

In the measured A arm (PACE=0, NEW_ONLY=1) 32 % of present intervals are
< 8 ms and 62 % are > 40 ms, while the mean is 34.5 ms. The per-flip log
shows the same pairs already at `pres`, e.g.:

```
n=5673 pres=72195412401 show=72195429556
n=5674 pres=72195429425 show=72195429723   (pres gap 17 ms, show gap 0.17 ms)
n=5709 pres=72197612420 show=72197612764
n=5710 pres=72197613349 show=72197613906   (pres gap 0.93 ms, show gap 1.1 ms)
```

So:

* it is **not** the present thread re-presenting: `PRESENT_NEW_ONLY` presents
  each new flip once (`flips not shown: 0`), and the duplicates the frame
  clock's outer pass used to make are gone;
* it is **not** primarily FIFO queueing: `pres -> show` is p50 4.2 ms
  (p90 5.0 ms) in A, so the present thread is not sitting on frames;
* it **is** the walker handing two guest flips to the window back to back.
  The guest is allowed three display flips ahead and one render frame in
  flight (`docs/notes/cube-rt-and-frames-in-flight.md`); after a slow drain it
  catches up and two decoded flips reach `present_guest_frame()` in the same
  pass or a tick apart. MangoHud then sees the pair as ~1 ms + ~47 ms.

## 2. The fix: `PS3RECOMP_VK_PACE=1`

Live key `PS3RECOMP_PRESENT_PACE` in `libs/video/ps3_live_ctl.{c,h}` (name
`PS3RECOMP_VK_PACE`, seeded from the environment, default 0). A switch in the
control file flips it mid-run like the cube and tick keys.

`vk_pace_frame_due` (`rsx_vulkan_backend.c:2153`), called on the walker thread
for each new frame that passes the `PRESENT_NEW_ONLY` gate:

1. keeps an EWMA of the **arrival** interval of new guest frames (`d`,
   clamped to 2-100 ms, asymmetric: faster cadence converges in ~4 frames,
   a slow patch in ~16). It is an arrival clock, not the present spacing --
   measuring our own held presents would feed back and ratchet (the first
   attempt did exactly that and ran B down to 6 frames/s; that run is kept in
   the log but not used);
2. advances a schedule by one interval per frame (`due = max(arrival,
   previous due)`), capped at `arrival + interval`, and abandoned
   (`due = arrival`) when two presents are already queued, so a backlog can
   never compound;
3. stores the due time in the present queue job.

The present thread sleeps until that due time and only then calls
`vkQueuePresentKHR`. The swapchain blit was already recorded and submitted
when the frame ended, so the GPU work and the walker never wait on the pacer;
a frame that arrives late is due at once. Worst-case added display latency is
one interval, bounded by the cap and the backlog guard.

Why the cost is ~zero: the pacer sleeps on the present thread, which already
does the blocking `vkQueuePresentKHR`; the walker and guest are untouched.
The paired fps difference is +0.22 (CI covers 0), and the walker is 27.81 vs
27.98 ms.

## 3. Run: A/B on the meter (PACE=0 vs PACE=1)

Both arms `PS3RECOMP_RSX_CUBE_RT=1`; boot/pre-meter with PACE=0, CUBE_RT=1.
`TAG=pacing_ab2`, image `ab_live_pacing_ab2_20261102_195223`.

```sh
cd recomp-work/ab-tools
TAG=pacing_ab2 METER=1 WCB=2 PIN=1 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
ARM_A=$'PS3RECOMP_VK_PACE=0\nPS3RECOMP_RSX_CUBE_RT=1' \
ARM_B=$'PS3RECOMP_VK_PACE=1\nPS3RECOMP_RSX_CUBE_RT=1' \
BOOT_ARM=$'PS3RECOMP_VK_PACE=0\nPS3RECOMP_RSX_CUBE_RT=1' \
PS3RECOMP_FLIP_LOG=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh
```

The run's own report (`ab_live_report.py`, guest `[flip]` lines):

| arm | fps | walker ms | wait ms |
|---|---:|---:|---:|
| A PACE=0 | **29.30 +/- 0.86** (std 0.82, 28.30-30.39) | 27.81 | 6.32 |
| B PACE=1 | **29.07 +/- 0.76** (std 0.72, 27.81-29.90) | 27.98 | 6.26 |

Paired A-B **+0.22 fps**, 95 % CI **[-0.35, +0.79]**, A/A check **PASS**;
the live-ctl band self-check passes for all 12 segments.

### Present intervals (`tools/flip_pace_stats.py`, measured windows)

From the per-flip log's `show` timestamps:

| metric | A PACE=0 (n=1386) | B PACE=1 (n=1374) |
|---|---:|---:|
| p50 | 46.46 ms | **38.43 ms** |
| p90 | 58.30 ms | **51.60 ms** |
| p99 | 68.57 ms | **61.18 ms** |
| mean | 34.48 ms | 34.72 ms |
| stddev | 24.28 ms | **14.90 ms** |
| intervals < 8 ms | 32.2 % | **0.0 %** |
| intervals > 40 ms | 61.8 % | **45.2 %** |

From MangoHud's CSV over the same wall windows:

| metric | A PACE=0 (n=1391) | B PACE=1 (n=1355) |
|---|---:|---:|
| p50 | 45.71 ms | **39.04 ms** |
| p90 | 57.67 ms | **52.42 ms** |
| p99 | 68.39 ms | **61.20 ms** |
| mean | 34.53 ms | 34.68 ms |
| stddev | 23.80 ms | **15.96 ms** |
| intervals < 8 ms | 28.8 % | **3.2 %** |
| intervals > 40 ms | 59.9 % | **47.3 %** |

The means are equal because the guest produces the same number of frames; the
pacer only redistributes their display times. The log and CSV agree within a
few tenths except the < 8 ms share (a log line per guest flip, a CSV row per
window present; a few `show` gaps below 8 ms remain where the next frame
arrived inside the one-interval cap).

Stage breakdown (per-flip log, measured windows):

| stage | A | B |
|---|---:|---:|
| `req -> fin` | 0.00 ms (same drain instant) | 0.00 ms |
| `fin -> pres` | p50 4.23 / p90 20.02 / mean 7.59 ms | p50 4.24 / p90 19.83 / mean 7.56 ms |
| `pres -> show` | p50 4.21 / p90 5.01 / mean 2.94 ms | p50 4.67 / p90 17.00 / mean 7.71 ms |

`fin -> pres` is where the two frames bunch; `pres -> show` growing in B is
the one-interval hold the pacer adds to the second frame of a pair.

### Images

* `recomp-work/ab_live_pacing_ab2_20261102_195223_ft.png` -- MangoHud
  frametime for the whole run, shaded A (blue) / B (orange) bands with the
  guest `[flip]` fps printed in each band. Read it as: the first ~250 s are
  boot/menus/pregame (frames ~30 ms, with the pregame cinematics' own
  60-150 ms spikes at 100-250 s); the measurement bands start at 255 s and
  stay on the meter. In the bands A shows the characteristic pairing (low
  spikes down to ~1 ms between ~46 ms frames); B's trace is visibly tighter
  (fewer sub-8 ms spikes, lower p90/std) at the same ~29-30 fps. The dashed
  lines are 16.7 ms (60 Hz) and 33.3 ms (30 fps). Y is clipped at 149 ms.
* `recomp-work/ab_live_pacing_ab2_20261102_195223_intervals.png` -- the
  same present intervals as a histogram from the per-flip log. A has a
  spike at 0-2 ms (the pairs) plus one at 48 ms; B has no 0-2 ms bar and
  spreads the intervals over 12-60 ms.

## 4. Cube probes: bind the last complete image

### Code

* Engine (`libs/video/rsx_draw_engine.c:4296-4335`): when a cube unit's base
  address is a colour target and fewer than six faces are usable, the unit
  no longer falls straight to the dummy. It calls the new optional
  `cube_target_last` hook; only if that returns 0 (no complete image was
  ever built at this address) does it bind the dummy and note a `cube` miss.
  A served fallback is not a miss.
* Header (`rsx_draw_engine.h:195-213`): `cube_target_view` now also carries
  `location/offset`, so the backend can key its cache by the guest address;
  `cube_target_last(user, location, offset, fmt, w, h, remap, rsx_fmt)` is
  the fallback hook.
* Vulkan (`rsx_vulkan_backend.c:3297-3377`): `vk_cube_rt` entries store the
  address, and `vk_eng_cube_target_last` returns the newest complete cube for
  that address/format/size/crossbar, bumping its LRU tick so a live fallback
  is not evicted while draws still sample it. The image is copied from the
  six faces when the probe completes; a face being recreated (serial 0) or
  not yet declared no longer forces black for the same address.

### Unit test

`libs/video/tests/test_rsx_draw_engine.c` (`test_cube_render_target`): after a
complete six-face cube is bound, one face target is recreated in another
format (so the set is temporarily incomplete). The engine must (a) not
rebuild the backend image, (b) ask for the last complete cube at that
address, (c) bind that handle instead of 0, and (d) not count a cube miss. A
five-face cube at an address that never completed still binds the dummy and
counts one miss. The stub backend implements the address-keyed lookup, so
the test covers the engine contract end to end.

`python3 tools/test_video_fixtures.py`: `test_rsx_draw_engine` and
`test_rsx_tex_miss` pass. (Two unrelated fixtures, `test_gcm_fifo_user_command`
and `test_gcm_flip_wrap`, fail to link on this checkout on missing
`rsx_wprof_*` / `rsx_draw_engine_*` symbols from other modules; this change
does not touch them.)

### Pregame evidence (same run, boot -> batter intro)

The run's thumbnails (`SCENE_SHOTS=1`, one every 2 s) cover boot, menus,
team select and the batter intro before the meter. The pregame contact sheet
is `recomp-work/ab_live_pacing_ab2_20261102_195223/pregame_sheet.png`
(105 shots, time and mean luma under each).

* **Alternation:** scanning every consecutive 2 s pair for "same structure
  (mean abs pixel diff < 3/255) but luma ratio > 1.4" finds **0 pairs**. The
  luma series is flat inside each scene (e.g. 30.5 for 30 s, 35.1 for 8 s)
  and steps only at real cuts. The per-frame bright/dark alternation the user
  saw is not sampled by a 2 s thumbnail, but the mechanism behind it is in the
  next two bullets.
* **Incomplete after complete:** parsing `[cube-rt]` lines per address, 20
  addresses are logged incomplete at least once *after* their first complete
  frame (e.g. `0x0DC20C00` 83 frames, `0x0DD85800` 76, `0x0DBA6C00` 46; 368
  frames in total). Those
  binds now use the last complete image instead of the dummy.
* **Cube misses:** the run's census totals **1,435** `cube` misses, all
  before the meter; they are only at addresses that were never complete
  (`0x0DC02400`, `0x0DB69C00`, and two frames of `0x0DE42800` before its first
  complete image). The earlier census run (`cube_rt_b_magenta`, different nav
  session) counted 6,481 before the meter and would have bound the dummy for
  every incomplete frame; the addresses that alternate complete/incomplete
  are the ones the fallback covers.
* **Definition of done for this symptom:** the cube probe no longer flashes
  black for an address that has ever been complete. A probe that has never
  been complete still reads the dummy (black with `VK_DUMMY_BLACK=1`),
  which is the old behaviour and cannot be fixed without inventing faces.

## 5. Reproduce

```sh
# build
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp

# unit
cd /home/m/Documents/mlb16-online/ps3recomp-play && python3 tools/test_video_fixtures.py

# the A/B above (one launch; the live ctl flips PACE every segment)
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=pacing_ab2 METER=1 WCB=2 PIN=1 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
ARM_A=$'PS3RECOMP_VK_PACE=0\nPS3RECOMP_RSX_CUBE_RT=1' \
ARM_B=$'PS3RECOMP_VK_PACE=1\nPS3RECOMP_RSX_CUBE_RT=1' \
BOOT_ARM=$'PS3RECOMP_VK_PACE=0\nPS3RECOMP_RSX_CUBE_RT=1' \
PS3RECOMP_FLIP_LOG=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh

# per-arm intervals from the log and the CSV
R=/home/m/Documents/mlb16-online/recomp-work/ab_live_pacing_ab2_20261102_195223
python3 /home/m/Documents/mlb16-online/mlb16-recomp/tools/flip_pace_stats.py \
  $R.log $R.marks --csv "$(ls $R/*.csv | head -1)"
```

The first attempt (`ab_live_pacing_ab1_20261102_194415`) used a present-spacing
EWMA in the present thread; it ratcheted (B fell to 6-21 presents/s while the
guest stayed at ~30 fps). Its B arm is the motivation for the arrival-cadence
design and is not used for the comparison.

## Caveats

* One run per arm pair (plus the discarded first attempt). The meter drifts
  ~0.5 fps run to run; the A/A check passed here, and the interval effect is
  much larger than that drift.
* `req == fin` for this title because its libgcm is statically linked and the
  drain decodes the `0xFEAD` flip word. A title using the HLE
  `_cellGcmSetFlipCommand` marker would show a non-zero `req -> fin`; the log
  supports it either way.
* The pacer cannot invent frames: when the guest produces two flips 1 ms
  apart, B still has some 12-20 ms intervals and some > 40 ms ones. What it
  removes is the sub-8 ms clamp at the *display* (0 % from 32 %), at the cost
  of at most one interval of present latency on the second frame of a pair.
* `PS3RECOMP_VK_PACE` only affects the windowed path; headless/replay runs do
  not present a window.
* The census counts are per run and the pregame nav path differs between the
  old magenta run and this one, so 6,481 -> 1,435 is directional evidence,
  not a paired comparison. The per-address rule (a served fallback is not a
  miss, a never-complete probe is) is what the code guarantees.
