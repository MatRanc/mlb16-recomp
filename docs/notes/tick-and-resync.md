# The jump-park resync and the display-tick ceiling

Date: 2026-11-02 (evening). Branch `exp/live-ab` in `ps3recomp-play` (off
`play/multiscene`), built as `recomp-work/build-play`. Windowed pitch runs with
`PS3RECOMP_VK_WINDOW=1`, `VK_DUMMY_BLACK=1`,
`PS3RECOMP_VK_PRESENT_MODE=fifo`, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`,
`PS3RECOMP_WALKER_PROF=1`, `GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1`,
`GCM_FLIPCOUNT=1`, `PS3RECOMP_PIN=1`, MangoHud CSV, and the scripted pitch nav.
All runs used `SDL_VIDEODRIVER=x11`.

## TL;DR

* **The frametime PNG was not lying about the arms; it was being read as game
  fps.** MangoHud's `frametime` is the *host present interval* (the window
  presents at 55-100 fps here), not the guest `[flip]` rate. The shaded A/B
  bands were also drawn from each switch, including the 2 s the report
  discards, so a band contained the previous arm's tail. The labels were
  nevertheless correct: `[live-ctl]` + `[flip]` prove A = baseline
  (20-36 fps) and B = `EXP_NO_DRAW` (41-42 fps) in the reference run. The plot
  now shades the measured window, detects MangoHud's nanosecond `elapsed`
  column instead of falling back to cumulative frametime, prints the band's
  guest fps, and `ab_live_report.py` has a live-ctl/arm self-check (PASS on
  all re-rendered runs).
* **The jump-park resync now has a walk-through fix behind
  `PS3RECOMP_WALKER_RESYNC_FIX=1` (default off).** Instead of teleporting
  `get` to `put` and losing every draw, clear and flip marker in between, it
  rewinds `get` to the first unread offset below `put` and drains that range
  through the normal walker (state, fences, flips and the draw sink included).
  Offline replay gate is unchanged (`0f91a96c79a7aadf`); a park-reuse fixture
  shows the skipped probe executed with the fix and dropped without it.
* **FAST_WAKE still does not survive the pitch, even with the fix.** It is not
  the resync alone: the FAST_WAKE arm runs at **~9 fps** (walker 55 ms, wait
  50 ms per frame) before the wedge, and flips stop on the 3rd B segment. The
  fix fired once (`walking 0x0000C03C..0x0000C190`) 10 s after the last flip
  and did not restart them. 4d was therefore skipped.
* **The tick is the cap only when the walker's draw work is gone.**
  `EXP_TICK_US` is a live switch (default 16000 = today's 16 ms).
  * WCB off, both arms `EXP_NO_DRAW=1`: A (16 ms) **40.89 fps** (range
    37.1-42.8); B (8 ms) **44.25 fps** but bimodal: 54.0, 60.4, 55.8 in three
    segments and 28.7, 30.8, 35.9 in the others. The 16 ms tick is a real
    ceiling (B exceeds 41.5 by 45 %), but 8 ms is not a free 2x: the doubled
    vblank/flip callback rate also costs guest time in some segments.
  * WCB=2, real drawing: A **33.83 fps**, B **33.71 fps**, paired
    **0.12 fps** (95 % CI -5.96..+6.20). With real draws the walker (20 ms)
    is the limiter; the tick is irrelevant. This run is A/A-level.
* **Changing the tick changes game speed** for flip/vblank-driven logic: it
  scales `cellGcmGetVBlankCount`, the vblank handler argument and the flip
  completion/handler rate. The current 16.000 ms tick is 62.5 Hz, already
  4.3 % fast versus the PS3's 59.94 Hz.

## 1. A/B bands: what was actually wrong

`recomp-work/ab-tools/ab_live_report.py` used to write each band as

```
switch_wall .. switch_wall + SEG
```

but the segment it reports is `[guest_at + DROP, guest_at + SEG]`. The first
two seconds after a switch therefore shaded the *previous* arm's tail (and the
0.15 s the control file takes to apply). On the reference NO_DRAW run the A
band (baseline) opened on B's fast tail and the B band opened on A's slow tail,
which is the visual "reversal".

The other half is the metric. `frametime_plot.py` draws one point per time
bucket using the **maximum** MangoHud frametime in the bucket, and MangoHud's
frametime is the host present interval. In the reference run the window
presents at 70-78 fps in both arms, while the guest flips at 20-42 fps; the
present series and the guest series are not the same measurement. Bucket
maxima of present time gave A 31.1 ms vs B 38.9 ms even though guest fps was
A 20-36 vs B 41-42.

Verification, with the log's own events (reference run
`ab_live_ceil_nodraw_20261102_142204`):

| check | result |
|---|---|
| `[live-ctl] flip=15461 EXP_NO_DRAW=1 (was 0)` at 14:26:45 | B arm starts after the A switch at 14:26:35 |
| `[flip]` counter 270231 ms -> 280605 ms (A window) | 36.02 fps; walker 17.27 ms (draws run) |
| same counter 282605 -> 290605 ms (B window) | 40.98 fps; walker 2.13 ms (`EXP_NO_DRAW`) |
| every A/B segment | self-check PASS (12/12) |

Changes:

* `ab_live_report.py` writes the **measured** window (`wall + DROP`, width
  `SEG - DROP`) and appends the band's guest fps as a fourth field.
* `frametime_plot.py` reads `start end label [guest_fps]`, prints the guest
  fps beside the A/B label, and `check_segments()` validates ordering,
  overlap, empty bands and reports host present time separately from guest
  fps. `choose_time()` now recognises a nanosecond `elapsed` column (current
  MangoHud on Linux) instead of silently using cumulative frametime.
* **Self-check:** after building the segments, `ab_live_report.py` re-parses
  the log's `[live-ctl]` changes, converts the log's time-of-day to wall time,
  and asserts the control state at the start and middle of every measured
  window equals the arm body logged for that arm. It prints
  `A/B band self-check ... PASS` or the exact mismatching key.
* `ab_live.sh` now defaults **WCB=2** (the normal visual configuration); arms
  that must not read back pass `WCB=0`.

### Two harness bugs found while doing this

1. **A key set by the control file never reverts.** `ps3_live_ctl.c` remembers
   `s_file_set[key]`; a later control file that does not name the key leaves
   the old value in place. The old `ARM_A=""` / `ARM_B="EXP_FAST_WAKE=1"`
   start-screen run therefore ran *both* arms as FAST_WAKE after the first B.
   `ab_live.sh` now warns when B names a key A does not, and the report's
   self-check fails such a run. Arms must name every key in both directions
   (e.g. `EXP_FAST_WAKE=0` vs `=1`).
2. **`EXP_NO_DRAW=1` cannot be live at boot.** The control file is written
   before launch, so arm A = NO_DRAW boots black, and the closed-loop nav has
   nothing to classify (one 4b attempt timed out with an empty path). New
   `BOOT_ARM` env (default `ARM_A`) is written at launch only; the first switch
   applies `ARM_A`. 4b used `BOOT_ARM="EXP_NO_DRAW=0 EXP_TICK_US=16000"`.

## 2. When the walker jumps, parks and resyncs

Code references are to `ps3recomp-play/libs/video/cellGcmSys.c` on
`exp/live-ab` after this change.

A park is a JUMP whose target is its own address: `0x20000000 | get`, or
`get | 1` for the other JUMP encoding. It is the "stop the RSX here" idiom;
the title overwrites the word with a JUMP to the next segment when that
segment is ready.

1. **Jump.** In the JUMP branch (`:2765`-`2809`) a non-self target is followed
   immediately: `s_fifo_getoff = tgt`.
2. **Park.** If `tgt == get` (`:2777`) the walker stops the pass without
   consuming the word (`s_park_pass = 1`, `s_walker_park_get = get`,
   `s_tr_parked = 1`, `break`). The next pass re-reads the word, so a patched
   park is followed normally.
3. **Grace.** If `put == get` the title has not moved on; the walker just
   waits. If `put != get` the title has moved its write head elsewhere and the
   park may be abandoned. `park_off/park_put/park_since` are recorded; after
   `GCM_PARK_GRACE_MS` (default **10 000 ms**) with the same park and the same
   `put`, and only after re-reading the word to see it is still the park, the
   walker calls `gcm_fifo_resync_why("jump-park", &get, put)`.
4. **Resync.** `gcm_fifo_resync_why()` (`:1714`) picks
   `land = gcm_spu_block_for_put(put)` if an SPU block ends just before `put`,
   otherwise `land = put`, and `from = get <= land ? get : s_last_ring_off`.
   It sweeps `[from, land)` for:
   * SET_REFERENCE fences (`gcm_ref_push_at`), so `cellGcmFinish` wakes;
   * label releases: NV406E `0x64/0x6C` and NV4097
     `0x01A4/0x1D6C/0x1D70/0x1D74`, replayed through `rsx_process_method`;
   * flip markers are counted so a later flip is not waited on forever.

   It then sets `get = land`. **Draws, clears, inline/immediate geometry and
   the vertex/texture state in that range are not replayed.** The comment says
   so: "Draws in the range are still lost." A skipped flip marker plus a
   skipped set-begin-end is the team-select 3D logo (and any frame whose
   flip lives in the skipped range).

### Why FAST_WAKE reaches the resync

`EXP_FAST_WAKE` (`cellGcm_fifo_kick_wait`, `:1955`) replaces the 4 ms idle
block with `WaitForSingleObject(ev, 0)` / `sched_yield()`. The walker now
re-enters `cellGcm_rsx_process_fifo` continuously instead of sleeping between
passes. The FIFO lock is held for a whole drain, so the producer that writes
the next segment and patches the park (PPU or an SPU PUT hook) competes for it
at a much higher rate. At the pitch the measured signature is a fast-wake
walker time of 55 ms/frame (vs 17 ms baseline) and a 10 s park; then the
resync fires. The 4 ms block was not dead time for this title: it was the
producer's window. (This is the mechanism the old doc inferred; this run adds
the per-frame walker/wait numbers.)

### The fix: walk the range instead of teleporting over it

`gcm_fifo_resync_walk()` (`:1673`), used at the jump-park (`:2789`) and
stuck-get (`:3255`) sites when the env switch is on:

* `from = get <= put ? get : s_last_ring_off` (the first offset below `put`
  the walker has not executed);
* if `from` is itself a self-JUMP, step over that one word (the abandoned
  park is a no-op once the title has moved `put`);
* if `from < put`, set `get = from`, clear the park state and `continue` the
  normal drain loop. The loop then executes every method up to `put` with the
  real `s_state`, the fences, flip markers and the draw sink. If `from >= put`
  there is nothing to walk and it sets `get = put`.

This is smaller than replaying the range in a side path (which would need a
copy of the dispatch and a state runtime) and it cannot double-execute:
`s_last_ring_off` is updated to `get + 4` only for offsets the walker actually
consumed below `put`.

Default off: `PS3RECOMP_WALKER_RESYNC_FIX=1` (environment, read once into a
static). The unmapped-JUMP/CALL and circling resyncs are deliberately not
changed: those targets are not walkable (or the walker is in a different ring
entirely).

## 3. The display tick and what hangs off it

`templates/project/main.cpp` (`frame_clock`) runs one tick loop. Each tick:

1. `cellGcmTickVBlank()` — `s_vblank_count++`, sets the pending vblank bit;
2. `cellGcmTickFlip()` — if a flip marker was executed and the status is
   `WAITING`, sets the pending flip bit (the flip is *completed* here);
3. presents a pending flip (`rsx_backend_present`);
4. `cellGcm_rsx_process_fifo()` — drains and publishes one queued fence;
5. `next_tick += 16` (now `+= tick_period_qpc()`).

Consumers of the tick, statically:

| consumer | affected by the period |
|---|---|
| `cellGcmGetVBlankCount`, vblank handler argument (`s_vblank_count`) | yes: counts ticks |
| `cellGcmGetFlipStatus`, `cellGcmSetWaitFlip`, flip handler | yes: completion is tick-gated |
| presenting (`cellGcm_take_flip_pending`) | yes: at most one present per tick (the outer loop can also present) |
| FIFO drain, `gcm_ref_publish_one` (one fence per drain) | yes: once per tick, plus the outer pass |
| window pump | outer loop / `PS3RECOMP_PUMP_MS`, not the tick |
| guest `sys_timer_usleep`, `gettimeofday` | no: wall clock |

So changing the period changes **game speed** for any logic that steps per
flip or counts vblanks. It does not change wall-clock timers. At the pitch
the meter screen is static, so the logs show no animation change, but the
NO_DRAW flip rate rose from 41 to 54-60 in the fast B segments: per-flip
simulation would run up to ~1.5x. Also note today's period is 16.000 ms =
62.5 Hz, not 59.94 Hz.

### `EXP_TICK_US`

A live switch (numeric, `PS3RECOMP_CTL` key `EXP_TICK_US`, default 16000).
`main.cpp` keeps the tick on QPC (`QueryPerformanceFrequency/Counter`; on
Linux the monotonic ns clock, Windows the 10 MHz counter) so sub-millisecond
periods are possible, and reads the value every tick. `ps3_live_ctl_tick_us()`
is deliberately a **strong** symbol: the runner's call is what pulls
`ps3_live_ctl.c` out of the runtime archive. (Making every entry point weak
lets fixtures link, but a weak reference does not extract an archive member;
an early build of this change called address 0 in `frame_clock` and dumped
core. The accessor is the fix.)

## 4. Runs

All at the pitch meter (`X` once at the pitch), `N=6`, 10 s segments, 2 s
dropped, PIN on, auto-settle. "Run count" is game launches.

### 4a. RESYNC_FIX=1, baseline vs FAST_WAKE (WCB=0)

| segment | A baseline fps | B FAST_WAKE fps |
|---|---:|---:|
| 0 | 36.79 | 9.05 |
| 1 | 37.13 | 9.84 |
| 2 | 32.08 | 8.73 (wedge, 13 prof samples) |
| mean | **35.34 +/- 7.01** (n=3) | **~9** |

The third B segment stopped flipping at guest **314 481 ms**. 10.2 s later the
fix fired:

```
[cellGcmSys] resync-fix: walking 0x0000C03C..0x0000C190 instead of skipping to put 0x0000C190
[walker-prof] at=324832ms ... walker=3030.880ms wait=7016.295ms ... draws=985 parse=2991.052ms
```

The walk-through happened, but no flip followed. The script's no-flip guard
stopped the arm (exit 7). **Does not survive**; 4d was skipped. The initial
4a attempt crashed at boot in `frame_clock` on the weak-symbol bug described
above; the retry used the fixed binary.

### 4b. WCB=0, NO_DRAW vs NO_DRAW + 8 ms tick

`BOOT_ARM` drew the boot; both arms are `EXP_NO_DRAW=1`, so the image is the
last pitch frame.

| segment | A 16 ms | B 8 ms |
|---|---:|---:|
| 0 | 37.14 | 28.66 |
| 1 | 40.76 | 30.78 |
| 2 | 41.42 | **54.00** |
| 3 | 41.58 | 35.91 |
| 4 | 42.81 | **60.39** |
| 5 | 41.66 | **55.76** |
| mean | **40.89 +/- 2.05** | **44.25 +/- 14.71** |

Paired difference -3.36 fps (CI -16.65..+9.94). The mean does not resolve the
change because B is bimodal; the important number is that B reaches
**54-60 fps**, well above the 41.5 fps 16 ms ceiling, in half its segments.
The 16 ms tick is a cap; 8 ms removes it, but the cost is variable (the
doubled callback rate is not free). A is the usual NO_DRAW ceiling
(41-43 fps), so the control is good. `tick_nodraw`'s first attempt is the
black-boot nav failure described above.

### 4c. WCB=2, baseline vs 8 ms tick (real drawing)

| | A 16 ms | B 8 ms |
|---|---:|---:|
| mean | **33.83 +/- 6.44** | **33.71 +/- 6.99** |
| range | 24.01-38.19 | 20.36-37.88 |

Paired **0.12 fps** (95 % CI -5.96..+6.20), A/A-level PASS. The walker own
time (20.4 vs 21.5 ms) dominates; the tick does not matter while draws run.
The slow middle segments (segments 2-4, 20-30 fps) are the draw-heavy phase,
and both arms see them.

## 5. What a proper timing fix needs

The tick currently does three unrelated jobs: it is the 60 Hz vblank clock,
the flip-completion edge, and the drain/fence cadence. A real fix should
separate them:

1. **Keep vblank at 59.94 Hz.** Accumulate `qpf * 1001 / 60000` (as
   `wave/main.cpp` already does) instead of 16.000 ms. This alone removes the
   4.3 % fast clock and is independent of any fps target.
2. **Complete a flip where the walker executes it.** After the drain reaches a
   flip marker (or a direct flip), mark the status `DONE` and hand the flip
   bit to the serialized handler pump (`ppu_gcm_pump`) immediately. This
   removes the up-to-one-tick latency without touching the vblank count. It
   is *not* hardware-accurate (the RSX completes at vsync), so it belongs
   behind an explicit uncapped-mode switch.
3. **Decouple the drain/fence publication from the tick.** Run the drain on
   the kick event and a ~1 ms timer (as `wave/main.cpp` does), so fences do
   not wait for a vblank. Keep vblank callback delivery at 59.94 Hz.
4. **Preserve handler ordering.** The flip and vblank bits must stay
   serialized by one pump, and the `s_flip_tick_seq == s_flip_req_seq` rule
   (do not complete a newer flip with an older tick) must survive.
5. **Decide the game-speed policy.** A title that does one simulation step
   per flip will run faster when flips complete faster. The options are:
   keep flip completion at vsync and only present/render ahead (no guest fps
   gain), add per-title delta-time/fixed-step handling (needs guest
   knowledge), or accept the speed change behind an opt-in flag. There is no
   general way to get both >60 fps and unchanged per-flip simulation.

The tick experiment's result bounds the payoff: with real drawing the walker
is the limiter, so the timing work only matters after the draw work is gone
(the native draw fast path).

## Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools

# 4a: resync fix + FAST_WAKE (wedges; see above)
TAG=resync_fix_fastwake N=6 PIN=1 WCB=0 PS3RECOMP_WALKER_RESYNC_FIX=1 \
  ARM_A="EXP_FAST_WAKE=0" ARM_B="EXP_FAST_WAKE=1" ./ab_live.sh

# 4b: tick ceiling without draw work (boot must draw)
TAG=tick_nodraw N=6 PIN=1 WCB=0 \
  BOOT_ARM=$'EXP_NO_DRAW=0\nEXP_TICK_US=16000' \
  ARM_A=$'EXP_NO_DRAW=1\nEXP_TICK_US=16000' \
  ARM_B=$'EXP_NO_DRAW=1\nEXP_TICK_US=8000' ./ab_live.sh

# 4c: tick with real drawing (WCB=2 is the new default)
TAG=tick_wcb2 N=6 PIN=1 WCB=2 \
  ARM_A="EXP_TICK_US=16000" ARM_B="EXP_TICK_US=8000" ./ab_live.sh

# offline: replay gate (fix must not change the captured pitch frame)
cd /home/m/Documents/mlb16-online/recomp-work
build-play/ps3recomp_sdk/replay_rsx_engine walker_frame --frames 20 --warmup 5
PS3RECOMP_WALKER_RESYNC_FIX=1 build-play/ps3recomp_sdk/replay_rsx_engine \
  walker_frame --frames 20 --warmup 5
```

## Runs and frametime PNGs

All under `recomp-work/`:

| run | log | frametime PNG |
|---|---|---|
| 4a resync fix vs FAST_WAKE (partial: wedged) | `ab_live_resync_fix_fastwake_retry_20261102_153319.log` | `ab_live_resync_fix_fastwake_retry_20261102_153319_ft.png` |
| 4b NO_DRAW vs 8 ms | `ab_live_tick_nodraw_retry_20261102_155606.log` | `ab_live_tick_nodraw_retry_20261102_155606_ft.png` |
| 4c WCB=2 vs 8 ms | `ab_live_tick_wcb2_20261102_160301.log` | `ab_live_tick_wcb2_20261102_160301_ft.png` |
| reference NO_DRAW (re-rendered) | `ab_live_ceil_nodraw_20261102_142204.log` | `ab_live_ceil_nodraw_20261102_142204_ft.png` (old shading: `..._ft_oldbands.png`) |

Read the PNGs as: the green line is the **host present** interval; the A/B
number under each band is the **guest** `[flip]` rate from the report.

* 4a: A bands are the normal 20-30 ms present trace; B bands are a 35-90 ms
  mess, and the third B flattens at ~40 ms with no flips.
* 4b: A bands are the steady ~41 fps NO_DRAW trace; B alternates between
  clearly lower present times and spikes, matching the bimodal guest fps.
* 4c: A and B look the same within the draw-heavy noise; both spend the first
  bands in the warm-up/draw-heavy 20-30 fps phase and settle at ~37 fps.
* reference NO_DRAW: now annotated A 36/35/33/25/20/33, B 41/41/42/40/42/42.

## Caveats

* The pitch still warms up for 1-3 min, and segments 2-4 of 4c caught a
  slower phase in both arms; compare paired segments, not the absolute mean.
* 4b's B arm is bimodal, so its mean is not the result; the maximum (60.4 fps)
  and the fact that three of six segments exceed 41.5 fps are.
* FAST_WAKE's ~9 fps is measured with `RESYNC_FIX=1`; the fix fires only once
  (at the wedge), so it is not the cause. The old FAST_WAKE start-screen A/B
  was invalid (arm A did not name `EXP_FAST_WAKE=0`; see harness bug 1).
* The replay capture `recomp-work/walker_frame` contains no park/resync, so
  the replay gate only proves the fix's default path is inert. The park-reuse
  fixture (with the skip probe) is the positive test; it was built from
  `libs/video/tests/test_gcm_park_reuse.c` with the missing weak-symbol stubs
  in `/tmp` because the fixture in this checkout does not link the newer
  walker symbols.
* One game instance at a time; the 4a first attempt and the 4b first attempt
  are the two failed launches (weak-symbol crash, black-boot nav), each
  retried once.
