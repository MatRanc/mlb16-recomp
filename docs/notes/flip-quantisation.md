# Flip quantisation: the 33 ms mode is the title's own 2-vblank deadline, not the display tick

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. One windowed meter launch:
`recomp-work/ab_live_flipdone_20261103_135620`, pitch meter (X once),
`METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11`, `N=6` per arm, 10 s
segments, 2 s dropped, arms rotated through `PS3RECOMP_CTL`
(A, B, C, A, ...). Three arms: today, the new switch, and a 4 ms tick with the
switch off. One game launch.

## TL;DR

* **Completing the flip where the present is queued (`PS3RECOMP_FLIP_ON_DONE=1`)
  does not remove a 16 ms rounding; it makes the meter slower.** Guest fps
  **34.06 -> 26.00**, per-flip p50 **33.89 -> 38.34 ms**, walker **17.36 ->
  24.85 ms**. It loses on every one of the six paired segments.
* **A 4 ms tick with the old completion (`EXP_TICK_US=4000`) is also slower**
  (28.52 fps, p50 36.83). The 32-34 ms mode is not "17-30 ms of work rounded up
  to two ticks".
* **The static chain found a better explanation.** The title's own
  `Gfx_DisplayFlip` waits for the flip with a deadline of
  `DAT_011203f8 << 1` = **2 x 16683 us = 33.37 ms** and then gives up
  (`DAT_011203e0 = 0`). The observed p50 is 33.89 ms and 39 % of A's frames sit
  in 32-36 ms. The 32-34 ms mode is that deadline, not our display tick.
* **Contractually, immediate completion is the right emulation.**
  `cellGcmSetFlipMode` has no call site in the EBOOT, and `Gfx_DisplayFlip`
  calls `cellGcmSetFlipImmediate`. Immediate means the flip is done when the
  RSX executes it, not at the next vsync. B therefore does the *more correct*
  thing and exposes the real wait: the guest's buffer-ref hand-off, which costs
  ~38 ms per frame at this scene.
* The switch is implemented, default **off**, and with a 59.94 Hz vblank tick
  when enabled. It is **not** enabled in `play_windowed.sh`: B does not win.

## 1. What the histogram said

`ab_live_jobgap5_20261103_133749.log` (`[fliplog]` `fin` deltas, measured
windows): a spike at **32-34 ms** (2,907 frames), almost nothing at 26-32 and
36-38, and a cluster at 12-16 ms. p50 was 34.0 ms in every A/B while p90/p99
moved. RPCS3 shows ~20 ms at the same scene. The working hypothesis was that
the 16 ms display tick completes the flip, so 17-30 ms of work rounds to
~33 ms and p50 can never move.

The A arm of this run reproduces that distribution (p50 33.89, 39 % in
32-36 ms, 1.1 % in 24-32 ms).

## 2. Static: how a flip completes today

All references are to `libs/video/cellGcmSys.c` on `exp/live-ab`.

1. **Request.** `Gfx_DisplayFlip` (`0x000692dc`, Ghidra) loops: wait for the
   flip queue producer, wait for the buffer ref, `cellGcmResetFlipStatus()`,
   `cellGcmSetFlipImmediate(bufferId)`, then wait for the flip to complete.
   `cellGcmSetFlipImmediate` is `cellGcmSetFlipCommand` in HLE
   (`:5061`, `:3952`) -> `gcm_flip_request(bufferId, in_fifo=0)` (`:3838`).
   That stores `s_flip_status = WAITING`, increments `s_flip_req_seq`, sets
   `s_flip_pending = 1` and `s_flip_direct_pending = 1`.
2. **Present.** The frame clock (`templates/project/main.cpp`, `frame_clock`)
   takes `s_flip_pending` and calls `present_guest_frame()` ->
   `rsx_backend_present()` before the next drain. On the outer pass it also
   presents when `cellGcm_flip_request_count` moves. Measured `fin -> pres`
   p50 is **3.4 ms** (A), so the present is not tick-gated.
3. **Completion.** `cellGcmTickFlip()` (`:995`) runs on the display tick. It
   completes a waiting flip only if `gcm_flip_executed_take()` says the RSX
   executed it (for a direct flip: `s_flip_direct_pending`), stamps
   `s_flip_tick_seq`, and sets the pending flip bit. `ppu_gcm_pump()` then sets
   `s_flip_status = DONE` (if the seq still matches) and invokes the guest's
   flip handler, `Gfx_FlipHandler` (`0x000691b0`).
4. **What waits.** `Gfx_FlipHandler` advances its flip-queue consumer index
   (`DAT_011203dc`) and clears `DAT_011203e0`. `Gfx_DisplayFlip` blocks in
   `while (DAT_011203e0 != 0)` until that happens. `Gfx_FrameEnd` (`0x00066530`)
   is a separate, earlier stage: it calls `cellGcmSetPrepareFlip` and lets the
   main run up to three flips ahead (`DAT_01120410 + 1 < d8 - dc`).
   `cellGcmSetWaitFlip` / `cellGcmGetFlipStatus` poll the same status but this
   title's display thread is the consumer that matters.

So the tick enters at step 3, and the maximum added latency is one period
(16 ms). It is real, but it is not the p50 limiter (section 5).

### The PS3 contract

* `cellGcmSetFlipMode` (import thunk `0x00f29d74`) has **no call site** in the
  EBOOT: its only xrefs are the thunk itself and its descriptor (`0x010b171c`).
* `cellGcmSetFlipImmediate` (thunk `0x00f29f14`) is called from
  `Gfx_DisplayFlip` at `0x0006941c`. Immediate mode flips as soon as the RSX
  executes the command; the flip handler is the RSX's completion, not a vsync
  edge. So **completing at the present is contract-correct** for this title,
  and completing on a 60 Hz tick is the less accurate behaviour.

### The vblank clock

* `FUN_00069288` (registered through the `cellGcmSetVBlankHandler` call in
  `FUN_0006e2e8`) only does `DAT_011203e4++`, `DAT_011203e8 = now`,
  `DAT_011203f0 = now + DAT_011203f8`. `DAT_011203f0` has **no reader**
  (Ghidra xrefs: one write, nothing else), so it is a dead deadline.
* `DAT_011203f8` is 16683 us (20000 us in the `0x240` video mode), set by
  `FUN_00064b68`, and is read in exactly one live place: `Gfx_DisplayFlip`'s
  `DAT_011203f8 << 1` flip-wait deadline (**33.37 ms**). That is the number the
  32-34 ms mode sits on. The title does not import `cellGcmGetVBlankCount` and
  does not time animation from the vblank count.
* Keeping the vblank handler at 59.94 Hz is still the safe choice for any
  title that does use it, so the switch changes the default tick to 16683 us
  (see section 3). B's measured vblank rate is 59.8-60.3 Hz; A's is 62.3-62.5.

## 3. The switch

`PS3RECOMP_FLIP_ON_DONE` is a live control-file key (enum
`PS3_LIVE_FLIP_ON_DONE`, default 0). Code:

* `ps3_live_ctl_tick_us()` returns `EXP_TICK_US` when it is set; otherwise it
  returns **16683 us** when `FLIP_ON_DONE` is on and 16000 us when it is off.
  An explicit `EXP_TICK_US` always wins, so the A/B can isolate the period.
* `cellGcmSetFlipPresentPath(rsx_ok)` is called once after backend init.
  `cellGcmTickFlip()` returns without completing when the switch is on **and**
  a present path exists; headless/replay (no backend) keeps tick completion so
  a flip can never hang.
* `cellGcmFlipOnPresent()` is called by `present_guest_frame()` immediately
  after `rsx_backend_present()`. It runs the same locked
  `WAITING && gcm_flip_executed_take()` completion `cellGcmTickFlip` used to,
  so the seq rule ("an older tick must not complete a newer flip") and the
  serialized handler pump are unchanged.
* No `spu_context` fields were added; the ABI rule in
  [`freeze-20261103.md`](freeze-20261103.md) is untouched.

## 4. Run: A vs B vs a 4 ms tick

One launch (`ab_live_flipdone_20261103_135620`), meter confirmed 183 s after
launch, 18 measured segments. Arms:

| arm | control file |
|---|---|
| A | `PS3RECOMP_FLIP_ON_DONE=0`, `EXP_TICK_US=16000` (today) |
| B | `PS3RECOMP_FLIP_ON_DONE=1`, `EXP_TICK_US=16683` (present completion, 59.94 Hz vblank) |
| C | `PS3RECOMP_FLIP_ON_DONE=0`, `EXP_TICK_US=4000` (finer tick only) |

Pooled over six 8 s windows per arm (`pin_ab.py`, `[fliplog]` `fin` deltas):

| arm | frames | fps | p50 ms | p90 ms | p99 ms | std ms | mean ms | walker ms | wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A today | 1635 | **34.06** | **33.89** | 39.70 | 44.99 | 10.23 | 29.36 | **17.36** | 11.88 |
| B flip-on-done | 1248 | 26.00 | 38.34 | 43.18 | 53.43 | **5.83** | 38.51 | 24.85 | 13.61 |
| C tick 4 ms | 1369 | 28.52 | 36.83 | 42.26 | 48.98 | 23.40* | 35.03 | 22.15 | 13.01 |

`*` C's pooled std is one hitch in segment 11 (std 55.42); its other segments
are 8.2-10.6 ms. B is worse than A on **every** segment (A 33.0-34.75 fps,
B 25.3-26.4, C 26.5-29.8).

### Per-flip histogram, 2 ms bins

Counts per arm, measured windows:

| bin (ms) | <12 | 12-16 | 16-20 | 20-24 | 24-32 | 32-36 | 36-40 | 40-44 | >=44 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A today | 82 | 209 | 169 | 76 | 18 | **642** | 292 | 124 | 23 |
| B flip-on-done | 9 | 10 | 8 | 9 | 0 | 210 | **612** | 303 | 87 |
| C tick 4 ms | 69 | 53 | 50 | 30 | 8 | 371 | **507** | 209 | 72 |

A is bimodal: a fast mode below 20 ms and a hard mode at 32-36 ms, with
24-32 almost empty. B removes the fast mode entirely and centres on 36-40.
C keeps some fast frames but moves the hard mode up to 36-40. **Neither
decoupling nor a finer tick produces frames below 32 ms.**

### Stage breakdown (measured windows)

| stage | A | B | C |
|---|---:|---:|---:|
| `fin -> pres` p50 / p90 / mean | 3.40 / 11.70 / 4.40 ms | 4.67 / 14.49 / 6.90 ms | 4.23 / 14.23 / 6.68 ms |
| `pres -> show` p50 / p90 / mean | 9.72 / 14.08 / 9.20 ms | 8.56 / 14.20 / 7.64 ms | 9.70 / 16.56 / 9.19 ms |

The present already lands ~3.4 ms after the guest flip request in A. B
completes the flip at that present, i.e. it removes **most of the tick
latency** (the tick could add up to 16 ms) and still gets slower. That is the
decisive number: the completion edge is not the frame-rate limiter.

### Vblank cadence (from `[flip]` `vc=`)

| arm | measured vblank rate |
|---|---|
| A (16000 us) | 62.3-62.5 Hz |
| B (16683 us) | 59.8-60.3 Hz |
| C (4000 us) | 248.9-251.3 Hz |

So B keeps a real 59.94 Hz vblank counter/handler while decoupling flip
completion, as intended.

## 5. Why B loses

* `fin -> pres` in A is 3.4 ms, so the flip is **requested and handed to the
  display** almost immediately; the only thing the tick delays is the
  completion edge. Moving that edge to the present (B) removes up to 16 ms of
  wait from `Gfx_DisplayFlip`, yet the frame interval goes **up** by 4.5 ms at
  p50.
* What replaces the wait is the guest's own **buffer-ref hand-off**: after the
  flip completes early, the display thread immediately wants the next queued
  buffer and blocks on its ref (the wait `Gfx_DisplayFlip` does before
  `cellGcmResetFlipStatus`). The walker cost rises **17.36 -> 24.85 ms**,
  which is that hand-off being pulled into the frame. B settles on a very
  tight ~38 ms frame (std 5.83 vs A's 10.23).
* A's 32-34 ms mode is consistent with the title's **2-vblank deadline**
  (2 x 16683 = 33.37 ms). A is faster than the real wait because at that
  deadline `Gfx_DisplayFlip` gives up (`DAT_011203e0 = 0`) and flips the next
  queued buffer even though the ref has not arrived. B removes the early
  completion that made the deadline relevant, so the genuine ~38 ms wait
  shows.
* C confirms the tick is not a rounding grid: a 4 ms tick does not create
  20-30 ms frames (only 0.6 % in 24-32 ms) and moves the hard mode **up** to
  36-40 ms. The old 8 ms result pointed the same way; this run adds the
  distribution.

Conclusion: the p50 lever is not the display tick or the flip completion
edge. On this scene it is the buffer-ref/producer hand-off after the flip
(already named in [`frame-timeline-2.md`](frame-timeline-2.md) as the c690
burst plus the ref wait). `FLIP_ON_DONE` is contract-correct but changes the
guest's pipeline for the worse here, so it stays off.

## 6. Visual / timing check

* 172 scene thumbnails over the whole run; the contact sheet is
  `ab_live_flipdone_20261103_135620/scene_sheet.png`. All three arms show the
  same intact pitch scene, no corruption, no black or partial frames. No
  resync, `[pm-end]`, crash or wedge line in the log; all 18 segments kept
  flipping.
* The meter is static, so a moving element cannot be timed per second. As a
  proxy, the mean absolute difference between consecutive 2 s thumbnails inside
  each arm is A 2.17, B 1.64, C 1.47 (0-255 luma): B's picture advances no
  faster than A's, consistent with B's lower fps, so there is no animation
  speed-up to guard against. The vblank handler stays at 59.94 Hz in B (the
  title's handler is a dead counter anyway).

## 7. Decision

* `PS3RECOMP_FLIP_ON_DONE` remains a live switch, **default off**.
* It is **not** added to either `play_windowed.sh` copy: B loses 8 fps at the
  meter and the premise (a tick-caused rounding) is not supported.
* `ab_live.sh`'s day-to-day configuration is unchanged (16 ms tick, tick-gated
  completion).

## 8. Reproduce

```sh
cd /home/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd ../ab-tools
TAG=flipdone N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=800 SETTLE=10 \
  ARMS='A=PS3RECOMP_FLIP_ON_DONE=0;EXP_TICK_US=16000|B=PS3RECOMP_FLIP_ON_DONE=1;EXP_TICK_US=16683|C=PS3RECOMP_FLIP_ON_DONE=0;EXP_TICK_US=4000' \
  BOOT_ARM='PS3RECOMP_FLIP_ON_DONE=0;EXP_TICK_US=16000' ./ab_live.sh

R=../ab_live_flipdone_<ts>
python3 pin_ab.py --log $R.log --marks $R.marks --seg 10 --drop 2
```

Offline: `python3 tools/test_video_fixtures.py` passes 28/29; the one failure
(`test_gcm_fifo_user_command`) is the pre-existing missing
`rsx_draw_engine_set_{fetch,pipe,remap}_memo` stubs, unrelated to this change.

## Caveats

* One launch, N=6 per arm, one meter state. B and C lose by 6-8 fps on every
  segment, far above the ~0.5 fps run-to-run drift, but this is one run.
* The vblank handler and the 2-vblank deadline are read statically from Ghidra;
  the link between the 33.37 ms deadline and the 32-34 ms mode is an inference
  from the timing (mode 33.9 ms, deadline 33.37 ms + loop cost), not a
  trace-verified cause. The A/B does not separate the deadline from the ref
  hand-off; it only shows the tick/completion edge is not the limiter.
* C's pooled std is inflated by one 55 ms hitch in segment 11.
* The meter is static, so the animation-speed check is a thumbnail-difference
  proxy, not a real moving-element speed measurement.
