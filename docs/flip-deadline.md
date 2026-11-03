# Flip deadline: the tick is starved by the walker drain, and completing earlier loses

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two windowed meter launches, standard
`play_windowed.sh`/`ab_live.sh` configuration (all five batch-1 switches on,
`METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11`, 16 ms display tick,
`PS3RECOMP_FLIP_ON_DONE=0`):

* **diagnosis**: `ab_live_flipdiag_20261103_150246`, one arm, 40 s binary
  timeline triggered 6 s after the pitch meter was confirmed (`N=1 SEG=80`,
  1,289 frames analysed).
* **A/B**: `ab_live_prepflip_20261103_151553`, `N=6`, 10 s segments, 2 s
  dropped, arms rotated A/B. A = status write kept (today),
  B = `PS3RECOMP_PREPARE_FLIP_KEEP_STATUS=1` (do not let
  `cellGcmSetPrepareFlip` complete the outstanding flip).

## TL;DR

* **The deadline is real and common, but it is not a completion edge we can
  move for free.** In the 40 s meter window, **369 of 1,289 frames (28.6 %)**
  left `Gfx_DisplayFlip` by its own `2 x 16683 us = 33.37 ms` deadline; 920
  (71.4 %) left by the flip handler. The deadline frames are a very tight
  **34.15 ms** (p90 34.31, max 35.16).
* **The flip was requested and presentable long before the deadline.** For
  deadline frames the present was queued **p50 0.35 ms**, p90 10.56 ms after
  the request (368/369 before the deadline), and the flip is an immediate
  (`cellGcmSetFlipImmediate`); the walker "executes" it at the request
  (`req_to_fin = 0` for all 8,672 flips in the run). No `cellGcmTickFlip`
  completion and no flip handler ran for the frame's own request sequence
  before the deadline.
* **The completion beat is delayed by the walker's own drain.** The display
  tick (`cellGcmTickVBlank`/`TickFlip`) and `cellGcm_rsx_process_fifo()` run on
  the same `rsx walker` thread. In a deadline frame one drain starts ~4.7 ms
  after the request and runs **23.3 ms**, followed by a put wait and a second
  **10.8 ms** drain; the next completion beat only lands **p50 7.5 ms after
  the deadline** (and belongs to the next request). At the deadline the walker
  drain was active in **80.2 %** of deadline frames, a c690 job in **59.9 %**,
  a put wait in 19.8 %, the walker idle wait in 18.2 %; the present fence and
  `vkQueuePresentKHR` were active in **0 %** (the present had already
  finished).
* **Completing earlier does not win.** The existing early-completion switch
  (`PS3RECOMP_FLIP_ON_DONE=1`) was already measured at **34.06 -> 26.00 fps**
  in [`flip-quantisation.md`](flip-quantisation.md); it makes the guest block
  on the next buffer's ref hand-off. The new status-preserving candidate also
  lost: **32.54 -> 28.21 fps**, p50 **34.15 -> 37.13 ms**, walker **17.75 ->
  21.84 ms**, on every one of six paired segments, and it did not remove the
  deadline frames (823 -> 776 per 60 s arm, a higher fraction because B is
  slower). **Neither switch is enabled.**
* **No >200 ms frames at the meter with batch 1 on.** The 40 s window has
  max **83.96 ms**. The whole run has 18 gaps >200 ms, all before the meter
  (menus/asset loads and the match load), not the batch-1 A/B's 921 ms
  segment.

## 1. What `Gfx_DisplayFlip` waits on

`Gfx_DisplayFlip` (`0x000692dc`, Ghidra) loops: wait for a queued frame
(`DAT_012450a0`), wait for its buffer ref (`DAT_01245068`), `cellGcmResetFlipStatus()`,
`cellGcmSetFlipImmediate(bufferId)`, then:

```c
DAT_011203e0 = DAT_011203dc;                 /* the frame being waited for   */
*(u64 *)(iVar4 + 0x1245190) = sys_time_get_system_time();
...
while (DAT_011203e0 != 0) {
    now = sys_time_get_system_time();
    if (now - start < (DAT_011203f8 << 1))   /* 2 x 16683 us = 33,366 us     */
        Cond_Wait(&DAT_012450d8, 17000);     /* us; see below                */
    else
        DAT_011203e0 = 0;                    /* give up: the deadline branch */
}
```

* `Cond_Wait` (`0x009a3060`) is `sys_lwcond_wait(mutex + 0x28, timeout, 0)`;
  the mutex base is `0x12450d8`, so the flip-done lwcond is **`0x1245100`**.
  The timeout is **17,000 us** (`Cond_Wait(&DAT_012450d8, 17000)`).
* `Gfx_FlipHandler` (`0x000691b0`) advances the consumer index
  (`DAT_011203dc++`), clears `DAT_011203e0`, and signals with
  `FUN_009a311c` -> `sys_lwcond_signal_all(0x1245100)`.
* In our runtime (`PS3_LWCOND_REAL=1`, now the standard configuration) the
  guest's 17 ms wait is a real host condition variable: `lwc_wait()` converts
  us to ms rounded up (`SleepConditionVariableCS`), returns `LWC_TIMEDOUT` on
  expiry (guest `ETIMEDOUT`, `0x8001000B`), and a signal marks the FIFO waiter
  record. Infinite waits still have the 10 ms fallback wake, but this wait is
  timed, so it has none.
* `sys_time_get_system_time()` is the host monotonic clock in **microseconds**,
  so the outer deadline is wall-time, not a count of ticks.
* The deadline is therefore reached after **two** 17 ms cond timeouts (the
  loop re-checks the 33.37 ms deadline between them). That is why the observed
  deadline frames are 34.1-34.2 ms, not 33.37 ms exactly.

The lwcond counters were extended to report the frame-level split
(`deadline_frames`): a deadline frame is two consecutive 17 ms timeouts on
`0x1245100` (the second one is the branch that clears `DAT_011203e0`). The
old `deadline=` field remains the raw 17 ms timeout count for compatibility.

## 2. Per-flip timeline (40 s window, 1,289 frames)

Events added to `PS3RECOMP_TIMELINE` for this run:

| event | meaning |
|---|---|
| `gcm.flip_req_seq` | flip request: `a` = frame (flip count), `b` = request sequence |
| `gcm.flip_handler` | guest flip handler delivered: `a` = completed sequence, `b` = latest request |
| `flip.wait_exit` | flip-done lwcond exit: `a` = 0 signal / 1 timeout / 2 fallback / 3 poll, `b` = elapsed us |

(Present queued, `gcm.flip_done` and `rsx.drain` already existed.)

| stage | p50 | p90 | p99 | mean |
|---|---:|---:|---:|---:|
| frame (`request -> next request`) | 34.16 ms | 41.48 | 47.05 | 30.99 |
| request -> present queued | 2.45 ms | 10.85 | 16.09 | 3.97 |
| request -> `cellGcmTickFlip` completion | 6.75 ms | 17.35 | 20.86 | 8.23 |
| request -> handler delivered | 6.84 ms | 17.48 | 20.92 | 8.55 |
| handler -> flip-done wait exit | 0.02 ms | 0.15 | 1.54 | 0.07 |

The whole post-tick chain is sub-millisecond: the handler is delivered ~0.1 ms
after the tick completion (the GCM interrupt thread pumps every 250 us), and
the guest's wait exits ~0.02 ms after the handler signals. **When the tick
runs, the completion is not the problem.** The problem is that the tick does
not run inside the deadline frame.

## 3. Deadline frames by cause

369 frames (28.6 %) left by the deadline. All of them:

| quantity | value |
|---|---:|
| present queued before the deadline | **368 / 369** (p50 0.35 ms, p90 10.56 ms after request) |
| `cellGcmTickFlip` completion for this sequence before the deadline | **0 / 369** |
| guest flip handler for this sequence before the deadline | **0 / 369** |
| two 17 ms timeouts | 369 / 369 |
| frame duration p50 / p90 / p99 | 34.15 / 34.31 / 35.06 ms |
| next completion beat after the deadline | p50 7.54 ms, p90 15.51 ms |

What was still running at the deadline instant (request + 33.366 ms):

| active span at the deadline | deadline frames |
|---|---:|
| walker drain (`rsx.drain`) | **296 / 369 (80.2 %)** |
| c690 job (`c690.job`, any lane) | **221 / 369 (59.9 %)** |
| walker put wait (`rsx.put_wait`) | 73 / 369 (19.8 %) |
| walker idle wait (`rsx.idle_wait`) | 67 / 369 (18.2 %) |
| present fence (`present.fence_wait`) | **0 / 369** |
| `vkQueuePresentKHR` | **0 / 369** |

Inside the deadline frame itself (union of spans clipped to
`request -> next request`):

| span | p50 | p90 | mean |
|---|---:|---:|---:|
| c690 lane union | 33.20 ms | 33.87 | 32.90 |
| walker drain | 22.32 ms | 29.19 | 22.70 |
| present fence | 7.37 ms | 10.93 | 5.55 |

The present is early and the GPU fence is already done; the c690 burst and the
walker drain fill the frame. This is not "the flip was not executed by the
walker": for an immediate flip the request *is* the execution
(`req_to_fin` is 0 for every flip). It is "our completion beat was still
inside a drain when the guest's 33.37 ms expired".

### Why the beat is late

The frame clock and the FIFO walker are the same host thread:

```
cellGcm_fifo_kick_wait(4 ms)
while (now >= next_tick) {
    cellGcmTickVBlank(); cellGcmTickFlip();
    present if pending;
    cellGcm_rsx_process_fifo();      /* may run for the whole c690 burst */
    next_tick += 16 ms;
}
... outer present + cellGcm_rsx_process_fifo() ...
```

One representative deadline frame (`n=5964`):

| time from request | event |
|---:|---|
| 0.000 ms | request (`SetFlipImmediate(0)`, sequence 11928) |
| 4.669 ms | present queued |
| 17.063 ms | first 17 ms timeout, wait again |
| 34.131 ms | second 17 ms timeout -> **deadline**, `DAT_011203e0 = 0` |
| 34.146 ms | next request (sequence 11930) |
| 39.19 ms | completion of the *next* request |

The walker spans around it: put wait 5.03 ms ending 0.28 ms before the
request, then a **23.33 ms drain** starting 4.70 ms after the request, then a
put wait and a **10.85 ms drain**. No `cellGcmTickFlip` is called between the
request and the next completion beat; the guest's two 17 ms waits therefore
both expire and it takes the deadline branch. The deadline is what lets the
guest keep the older buffer on screen and start the next frame without the
full buffer-ref hand-off, which is why it is *faster* than waiting for the
completion.

## 4. The status-write candidate (and why it is not the cause)

`cellGcmSetPrepareFlip` (`0x00066530` calls it from `Gfx_FrameEnd`) sets
`s_flip_status = DONE` in the HLE. It looked like a plausible clobber: the
title prepares up to three frames ahead, so the main thread can call it while
`Gfx_DisplayFlip` is waiting for an older flip, and a `DONE` status would make
the tick skip the completion.

A new default-off live key, `PS3RECOMP_PREPARE_FLIP_KEEP_STATUS` (enum
`PS3_LIVE_PREPARE_FLIP_KEEP_STATUS`), leaves the status alone when set. With
`PS3RECOMP_FLIP_PREPARE_STATS=1`, a two-second
`[flip-prepare] calls=... clobbered=... keep=...` line counts calls that saw
`WAITING` while the write was active. (The counter is a "saw WAITING with the
write enabled" count, not proof that the write caused a specific lost
completion.)

The A/B result rules the status write out as the main cause:

| arm | frames | fps | p50 | p90 | p99 | std | mean | walker | wait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A keep status write (today) | 1,562 | **32.54** | **34.15** | 40.50 | 47.84 | 18.82 | 30.72 | **17.75** | 12.91 |
| B `KEEP_STATUS=1` | 1,354 | 28.21 | 37.13 | 42.69 | 50.10 | 10.75 | 35.46 | 21.84 | 13.69 |

Six paired segments, A-B fps difference **+4.33 +/- 0.82** (every segment
A > B). Per-flip 2 ms histogram:

| bin (ms) | 8-10 | 10-12 | 12-14 | 14-16 | 16-18 | 18-20 | 20-22 | 22-24 | 24-26 | 26-28 | 28-30 | 30-32 | 32-34 | 34-36 | 36-38 | 38-40 | 40-42 | 42-44 | 44-46 | 46-48 | 48-50 | 50+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 17 | 41 | 57 | 106 | 94 | 74 | 40 | 8 | 7 | 0 | 1 | 1 | 15 | **563** | 156 | 162 | 84 | 46 | 26 | 15 | 6 | 13 |
| B | 0 | 0 | 10 | 37 | 47 | 51 | 33 | 25 | 9 | 4 | 1 | 0 | 31 | 333 | 210 | **249** | 146 | 76 | 42 | 24 | 10 | 15 |

Flip-done wait exits and frame-level deadlines over the six 10 s segments per
arm (60 s of arm time, drop included):

| arm | flip-done waits | exited by signal | 17 ms timeouts | deadline frames |
|---|---:|---:|---:|---:|
| A | 4,736 | 2,555 | 2,181 | **823** (13.7/s) |
| B | 4,710 | 2,585 | 2,125 | **776** (12.9/s) |

So B removes almost no deadline frames (and a larger fraction of its slower
frames still hit one) and costs 4.3 fps. The `SetPrepareFlip` status write is
not what keeps the tick from running during a drain; the drain does.

The early-completion switch (`FLIP_ON_DONE`) is the other candidate, and it
was already measured: **34.06 -> 26.00 fps**, p50 33.89 -> 38.34 ms, walker
17.36 -> 24.85 ms. It does complete the flip at the present, but that makes
the display thread immediately ask for the next buffer and block on its ref
hand-off, which is longer than the deadline.

## 5. Decision

* `PS3RECOMP_PREPARE_FLIP_KEEP_STATUS` is a live key, **default off**, and is
  not added to `play_windowed.sh` or `ab_live.sh`. The code default is off.
* `PS3RECOMP_FLIP_ON_DONE` stays default off (unchanged).
* The deadline is the title's pacing choice when our tick is late: it is
  faster than exposing the buffer-ref wait. The p50 lever is the c690 burst
  (see [`frame-timeline-2.md`](frame-timeline-2.md)), not the completion edge.
* A real fix would have to decouple the display tick from the walker drain (a
  separate tick thread or a tick checkpoint inside the drain) *and* keep the
  guest from immediately hitting the ref wait. That is a larger change; it is
  added to the work queue as a follow-up, not enabled here.

## 6. Frames >200 ms with batch 1 on

* Meter window (40 s, 1,289 frames): max **83.96 ms**, **0 frames >200 ms**.
* Whole run (8,671 `fin` intervals): 18 gaps >200 ms, all before the meter:
  * 16 gaps of **237-654 ms** at 15:03:08-15:03:15 during the boot/menu
    scene, interleaved with `[fs] open/read` and first-read-on-fd lines;
  * one **5,152 ms** gap at 15:04:53 and one **510 ms** gap at 15:05:05
    during the match load after the cinematic-skip X (`[fs]` reads of
    `MLBPS3_GD.WAD`/`HeadFaceCache`, texture-cache opens, `cellSpurs
    AddWorkload`, `MapMainMemory`).
* None of these is the lwcond timeout path: the only `[LWCOND] ... fallback
  wake` lines are the boot handshake at `0x1246A98` at 15:02:46. The batch-1
  A/B's 921 ms segment did not recur at the meter.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd ../ab-tools

# diagnosis (one launch)
export PS3RECOMP_TIMELINE=/tmp/tl_flipdiag.bin
export PS3RECOMP_TIMELINE_TRIGGER=/tmp/tl_flipdiag.go
export PS3RECOMP_TIMELINE_SECS=40 PS3RECOMP_TIMELINE_RING_MB=32
export PS3RECOMP_TIMELINE_FLUSH_MB=8 PS3_LWCOND_STATS=1
TAG=flipdiag N=1 SEG=80 DROP=0 SETTLE=10 METER=1 SKIP_CINE=1 PIN=1 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=800 ARMS='A=' ./ab_live.sh &
# when "pitch meter confirmed" appears, wait 6 s, touch /tmp/tl_flipdiag.go

# A/B (one launch)
export PS3_LWCOND_STATS=1 PS3RECOMP_FLIP_PREPARE_STATS=1
TAG=prepflip N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  ARMS='A=PS3RECOMP_PREPARE_FLIP_KEEP_STATUS=0|B=PS3RECOMP_PREPARE_FLIP_KEEP_STATUS=1' \
  BOOT_ARM='PS3RECOMP_PREPARE_FLIP_KEEP_STATUS=0' ./ab_live.sh
python3 pin_ab.py --log ../ab_live_prepflip_<ts>.log \
  --marks ../ab_live_prepflip_<ts>.marks --seg 10 --drop 2
```

Timeline decoding: `PSTL` header, 32-byte records (`t0,t1,a,b,ev,pad`).
Events used here: `gcm.flip_req_seq` (53), `gcm.flip_handler` (54),
`flip.wait_exit` (55), `gcm.flip_done` (29), `present.queued` (49),
`rsx.drain` (13), `rsx.put_wait` (15), `rsx.idle_wait` (43), `c690.job` (33),
`present.fence_wait` (48), `present.vkqueue` (50).

## Caveats

* One diagnosis launch and one A/B launch; one meter state. The A/B effect
  (+4.3 fps) is far above the ~0.5 fps run-to-run drift and is consistent on
  all six segments, but it is one run.
* The timeline does not record every `cellGcmTickFlip` call, only completions.
  The "tick is starved by the drain" conclusion is from the drain spans, the
  missing completion, and the next completion landing after the drain; the
  status-preserving A/B rules out the `SetPrepareFlip` write but not a
  different tick-scheduling race.
* `deadline_frames` is counted as two consecutive 17 ms timeouts on
  `0x1245100`; a third timeout would not be counted again. In this scene every
  deadline frame leaves after exactly two (34.1-34.3 ms).
* The >200 ms gaps are characterised from the log (file reads, workload
  creation); no timeline was recorded outside the meter window.
