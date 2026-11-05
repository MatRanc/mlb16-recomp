# Menu flip fix: complete the display tick from our own request state, age-bounded

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. This implements the proposal in
[`menu-34ms.md`](menu-34ms.md) behind the live key
`PS3RECOMP_FLIP_TICK_OWN_STATE` (**default off**). One automated pitch-meter
A/B was run; the menu measurement is the user's manual test at the end.

Follows [`menu-34ms.md`](menu-34ms.md), [`meter-p50-pin.md`](meter-p50-pin.md),
[`walker-cuts-2.md`](walker-cuts-2.md), [`frame-pacing.md`](frame-pacing.md)
and [`targets-and-backlog.md`](targets-and-backlog.md).

## TL;DR

* **Implemented, default off.** With `PS3RECOMP_FLIP_TICK_OWN_STATE=1`,
  `cellGcmTickFlip` completes an outstanding **direct** flip using our own
  `s_flip_direct_pending` flag and the request's timestamp, not the guest's
  `s_flip_status` word. It completes only while the request is younger than
  the guest's first 17 ms wait (`GCM_FLIP_TICK_AGE_US = 17000`). With the key
  off, the old status gate runs unchanged.
* **State machine unit-tested.** `test_gcm_tick_in_drain` now covers
  request -> `cellGcmSetPrepareFlip` clobbers the status to DONE -> tick
  completes exactly once; a request older than 17 ms is not completed and
  stays outstanding; no double completion; no completion with no request.
  All 30 video fixtures pass.
* **Pitch meter is not regressed (one launch, N=6 x 10 s, 2 s dropped).**
  p50 **34.15 ms in both arms**; p90 38.01 -> **37.27**, p99 41.00 ->
  **40.89**; fps 34.75 -> 36.71 (mixed per-segment sign, one A segment has a
  host stall). No window freeze in the measured bands, 0 dropped presents.
  Scene thumbnails: A/B diff 2.97/255, the same as A/A 3.44 and B/B 2.89.
* **Two-timeout frame count moved up, not down:** 0.327 -> 0.374 per frame
  (11.4 -> 13.7 per second). This is the meter's producer/ref-bound mode, not
  a frame-time regression; it is sampled every 2 s (see caveats).
* **Menu number is still the user's.** Expected: the 34 ms mode (two
  timeouts) collapses to one-tick frames, ~55-60 fps in start screen / main
  menu / team select. Nothing is default-on; default waits for the user's
  test and a medium review.

## 1. Change

### 1.1 Live key

`PS3_LIVE_FLIP_TICK_OWN_STATE` / `PS3RECOMP_FLIP_TICK_OWN_STATE`, appended at
the end of the live-ctl enum and name table (`ps3_live_ctl.h:92`,
`ps3_live_ctl.c`). Default 0 everywhere; `play_windowed.sh` does not set it.

### 1.2 Tick gate

`gcm_flip_complete_locked(GCM_FLIP_GATE_TICK)` (`cellGcmSys.c:1063`) becomes:

```c
if (own_state_key) {
    age = gcm_us_now() - s_flip_req_us;
    if (s_flip_direct_pending && s_flip_req_us &&
        age < GCM_FLIP_TICK_AGE_US && gcm_flip_direct_take())
        seq = s_flip_req_seq;
} else {                     /* unchanged */
    if (s_flip_status == CELL_GCM_FLIP_STATUS_WAITING &&
        gcm_flip_executed_take())
        seq = s_flip_req_seq;
}
```

* `s_flip_req_us` is stamped in `gcm_flip_request` under `FLIP_LOCK`
  (`cellGcmSys.c:4202`), next to the request sequence.
* `gcm_flip_direct_take()` (`cellGcmSys.c:203`) is the old direct-pending
  half of `gcm_flip_executed_take`, so the completion consumes the request
  (no double completion) and the FIFO-marker bit cannot be consumed by this
  gate.
* `GCM_FLIP_TICK_AGE_US` (`cellGcmSys.c:219`) is the guest's own first
  wait (`Cond_Wait(..., 17000)` in `Gfx_DisplayFlip`), not a new tuning
  constant.
* Guest-visible behaviour is unchanged: the completion still only marks a
  pending flip (`GCM_PENDING_SET(2)`); `ppu_gcm_pump` still writes DONE only
  when the completed sequence is still the newest request, and the handler is
  still delivered in the same order on the GCM interrupt thread. Counters
  (`s_flip_request_count`, `s_flip_frame_count`, `s_flip_completed_seq`,
  `s_flip_tick_seq`) follow the existing completion path.

### 1.3 Why the age bound

It makes the change a no-op for a flip whose first wait has already expired:
at the meter the walker drain usually delays the tick past 17 ms, so those
ticks are suppressed exactly as the clobbered-status gate suppressed them.
When a meter tick does land inside the bound it completes early; the frame is
still bounded by the c690 producer and the next buffer's ref, which is why p50
does not move. This is the difference from
`PS3RECOMP_PREPARE_FLIP_KEEP_STATUS` (no age bound; measured -4.3 fps at the
meter).

## 2. Unit test

`libs/video/tests/test_gcm_tick_in_drain.c` (the existing GCM fixture run by
`tools/test_video_fixtures.py`), key off for all older cases and on for the
new block:

1. `request_direct()` -> `cellGcmSetPrepareFlip(NULL, 0)` (the title's
   prepare-ahead) writes `s_flip_status = DONE` -> `cellGcmTickFlip()` +
   `ppu_gcm_pump()` completes **once** and leaves DONE.
2. Immediate second tick: no double completion, no second handler call.
3. `s_flip_req_us = now - 20000`: late request is **not** completed, the
   status stays WAITING and the direct-pending flag stays set.
4. `s_flip_req_us = now - 16999`: inside the bound it completes once.
5. No direct request (stale timestamp, status WAITING): the tick completes
   nothing.

`python3 tools/test_video_fixtures.py`: **30/30 pass** (including
`test_gcm_tick_in_drain`). No `spu_context` field was added; the shared-field
ABI check is unaffected.

## 3. Pitch-meter A/B

One launch: `ab_live_menufix_20261104_201037`, `METER=1 SKIP_CINE=1 PIN=0
WCB=2 VIDEODRIVER=x11 PITCH_THRESH=35`, `PS3_LWCOND_STATS=1`, `N=6 SEG=10
DROP=2`, arms alternated A/B, boot off:

* A = `PS3RECOMP_FLIP_TICK_OWN_STATE=0`
* B = `PS3RECOMP_FLIP_TICK_OWN_STATE=1`

Pitch meter confirmed 157 s after launch, first X only. `pin_ab.py`:

| per-flip | A off | B on |
|---|---:|---:|
| frames | 1,668 | 1,762 |
| fps | 34.75 | 36.71 |
| p50 ms | **34.15** | **34.15** |
| p90 ms | 38.01 | 37.27 |
| p99 ms | 41.00 | 40.89 |
| std ms | 17.50 | 11.35 |
| mean ms | 28.80 | 26.85 |
| walker ms | 9.64 | 8.80 |
| wait ms | 15.82 | 15.16 |

Per-segment p50 is 34.14-34.16 in every one of the 12 segments. Per-segment
fps differences (B-A) are +0.88, +4.37, +2.50, -2.88, +0.62, +6.26: the sign
is mixed and one A segment (10) contains a host stall (std 36.04), so the fps
difference is not a claim of a meter win. A p50/p90/p99 and the thumbnail
comparison are the robust numbers.

Two-timeout frames (`[lwcond-stats] deadline_frames`, two consecutive 17 ms
flip-done timeouts) over the six 8 s measured windows per arm:

| arm | deadline frames / 48 s | per second | per frame |
|---|---:|---:|---:|
| A | 546 | 11.4 | 0.327 |
| B | 659 | 13.7 | 0.374 |

Both an interpolation of the cumulative counter and a midpoint-assignment of
each 2 s sample give the same direction (+14 %). It is reported, not used as
a win: the metric is sampled every 2 s and the arms alternate every 10 s, and
an early completion can shift the next frame's tick phase. p50/p90/p99 and
fps are the frame-level result and none regressed.

Window health: no frozen window in the measured bands, 0 no-slot / 0
not-ready drops, longest `vkQueuePresentKHR` 3 ms, max pump gap 42 ms (boot /
load lines only). No hang, no crash.

Scene thumbnails (2 s shots, 640x360, mean abs luma diff 0-255):

| pair | n | mean | min | max |
|---|---:|---:|---:|---:|
| A/A | 15 | 3.44 | 1.66 | 4.59 |
| B/B | 15 | 2.89 | 1.33 | 3.70 |
| A/B | 36 | 2.97 | 1.25 | 4.28 |

The B arm is visually inside the A/A control, so no thumbnail regression.

## 4. User menu test (the number this mission is for)

Record the start screen, main menu and team select with the 17 ms hitch
recorder, key off and key on. Run them in the same session/order if possible;
~2 min per scene is enough. One game at a time.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
mkdir -p captures
TS=$(date +%Y%m%d_%H%M%S)

# key OFF (baseline, today's menus)
PS3RECOMP_FLIP_TICK_OWN_STATE=0 \
PS3RECOMP_TIMELINE="$PWD/captures/menus_own_off_$TS.pstl" \
PS3RECOMP_TIMELINE_HITCH_US=17000 \
PS3RECOMP_TIMELINE_RING_MB=64 PS3RECOMP_TIMELINE_FLUSH_MB=16 \
./play_windowed.sh

# key ON (the fix)
PS3RECOMP_FLIP_TICK_OWN_STATE=1 \
PS3RECOMP_TIMELINE="$PWD/captures/menus_own_on_$TS.pstl" \
PS3RECOMP_TIMELINE_HITCH_US=17000 \
PS3RECOMP_TIMELINE_RING_MB=64 PS3RECOMP_TIMELINE_FLUSH_MB=16 \
./play_windowed.sh
```

Live-ctl fallback (one launch, flip the key mid-run instead of relaunching):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
CTL=/tmp/mlb16_flip_own.ctl
printf 'PS3RECOMP_FLIP_TICK_OWN_STATE=0\n' > "$CTL"
PS3RECOMP_CTL="$CTL" \
PS3RECOMP_TIMELINE="$PWD/captures/menus_own_live_$(date +%Y%m%d_%H%M%S).pstl" \
PS3RECOMP_TIMELINE_HITCH_US=17000 \
PS3RECOMP_TIMELINE_RING_MB=64 PS3RECOMP_TIMELINE_FLUSH_MB=16 \
./play_windowed.sh
# when the window says [live-ctl] applied, switch it:
printf 'PS3RECOMP_FLIP_TICK_OWN_STATE=1\n' > "$CTL"
```

What to look for:

* Menus at **~55-60 fps** instead of ~42; the 34 ms stutter mode gone.
* No judder / backward steps; menu animations and audio at normal speed.
* No hang on screen changes (start -> main menu -> team select) or at match
  load; the start press and team wheel should stay responsive.
* If it does not improve, keep the OFF and ON recordings: the capture shows
  whether the completion now fires inside the first 17 ms wait
  (`gcm.flip_done` / `gcm.flip_handler` before the first timeout) or the tick
  is still late.
* The key is default off, so nothing changes for normal play until you decide
  (then a medium review before default).

Analyse the recordings with `python3 mlb16-recomp/tools/hitch_table.py
<file>.pstl --bins --top 20`; the menu-34ms.md §6 script separates the
34 ms and 17-20 ms classes and counts `flip.wait_exit` kinds.

## 5. Caveats

* One automated launch, one meter state. The A arm's segment 10 contains a
  host stall (std 36.04); the p50 is identical in every paired segment, which
  is stronger than the run-to-run fps drift.
* `deadline_frames` is a cumulative counter logged every 2 s; the per-arm
  split uses time interpolation and is good to roughly +/-10 %, not to the
  frame. The direction was the same with two estimators.
* The own-state gate only handles a **direct** flip (the path this title uses:
  `cellGcmSetFlipImmediate` -> `gcm_flip_request(..., in_fifo=0)`). A FIFO
  marker flip is left to the legacy gate; the key is experimental and default
  off, so no other title changes.
* The menu estimate (55-60 fps, one flip per vblank, ~56 with the buffer-ref
  tail) is from [`menu-34ms.md`](menu-34ms.md) §5 and is not measured here.
* No capture or `recomp-work` file is committed.

## 6. Clean room

All of this is our own static analysis of the EBOOT (Ghidra), our own runtime
code, trace stream and logs. No RPCS3 or other GPL emulator source was read or
cited.
