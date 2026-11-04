# Frame-pacing floor: the 34.15 ms p50 is the c690 producer, not the flip/present path

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`, code commit `84e5dd06` plus the changes below. One
windowed meter launch (`ab_live_pacingfloor_20261104_033642`), standard
`play_windowed.sh`/`ab_live.sh` configuration (`PIN=0`, `WCB=2`, x11,
`PS3RECOMP_VK_PRESENT_MODE=fifo`, `PACE=1`, `PRESENT_NEW_ONLY=1`) plus
`METER=1 SKIP_CINE=1 PITCH_THRESH=35`, `PS3_LWCOND_STATS=1`, and a 90 s binary
timeline (774 MiB, deleted after analysis). N=4 x 10 s per arm, 2 s dropped.

This follows [`flip-deadline.md`](flip-deadline.md),
[`flip-quantisation.md`](flip-quantisation.md),
[`walker-cuts-2.md`](walker-cuts-2.md) and
[`c690-dispatch-cuts.md`](c690-dispatch-cuts.md).

## TL;DR

* **The flip/present/tick path does not quantise the frame.** In the measured
  window a hard frame is `c690 lane union 32.81 ms` inside a `34.28 ms` frame;
  the next frame's fence (`SET_REFERENCE`) is published **0.26 ms before the
  frame ends**. The frame is producer-bound, not completion-bound. `dur - c690
  union` is **3.2 ms**.
* **34.15 ms is the guest's own 2 x 17 ms deadline, but it is not the cause.**
  `Gfx_DisplayFlip` waits on the flip-done lwcond with a 2 x 16683 us deadline;
  our host `Cond_Wait` rounds 17000 us up to 17 ms, so the deadline branch
  exits at two timeouts (34.13 ms). It fires because the next buffer's fence
  lands at ~34.4 ms, not because work finished early and was rounded up.
* **The distribution has no 20-32 ms population.** Arm A: 22.7 % of flips are
  8-16 ms, 7.5 % 16-20 ms, **0.5 % 20-32 ms**, 40.3 % 34-36 ms. Frames either
  contain the c690 burst (hard, ~34) or not (fast, ~14-18). There is no group
  of frames whose work is done at 20-30 ms and which then waits for 34.15.
* **Completing earlier loses.** The fixed tick-in-drain checkpoint (B) and the
  marker-side/fence completion (C) both reproduce the `FLIP_ON_DONE` failure:
  the display thread advances into the buffer-ref wait, the fast mode
  disappears, and fps falls **33.91 -> 25.47 / 24.56** (p50 34.15 -> 38.05 /
  38.12) on every paired segment. Neither is enabled.
* **Present never blocks the guest.** `vkQueuePresentKHR` runs on a dedicated
  thread; at the meter its longest call was 10 ms once, otherwise 0-2 ms. A
  driver stall at 03:40:25 blocked the present thread for 1,139 ms and dropped
  7 presents, while the guest kept flipping at 34 ms. FIFO/mailbox is a window
  property only (not live-switchable: it is fixed at swapchain creation), so
  present mode is not a guest-frame lever.
* **The real lever stays c690 / walker.** RPCS3's ~20 ms is a faster producer,
  not a different flip edge. Making the c690 burst ~20 ms is what would move
  this floor; a completion-edge change can at best recover the 0.26 ms of
  fence-to-frame slack, which is below run-to-run noise.

## 1. Static: what `Gfx_DisplayFlip` actually waits on

`Gfx_DisplayFlip` (`0x000692dc`, Ghidra) loops per frame:

1. wait for a queued frame: `Cond_Wait(&DAT_012450a0, 9000)` until
   `DAT_011203dc != DAT_011203d8`;
2. wait for **this** frame's buffer ref: `Cond_Wait(&DAT_01245068, 9000)`
   while `DAT_01245020[consumer & 3] != *(u32 *)(DAT_01123b00 + 8)`;
3. `cellGcmResetFlipStatus(); cellGcmSetFlipImmediate(bufferId);`
4. wait for the flip handler / deadline: set `DAT_011203e0 = consumer`, then
   loop `Cond_Wait(&DAT_012450d8, 17000)` until `DAT_011203e0 == 0`, with a
   `sys_time_get_system_time()` check of `DAT_011203f8 << 1` = 2 x 16683 us.

`Gfx_FlipHandler` (`0x000691b0`) advances `DAT_011203dc`, clears
`DAT_011203e0` and signals the lwcond at `0x1245100`. Our host lwcond
(`PS3_LWCOND_REAL=1`) converts 17000 us to **17 ms**, so the deadline branch is
two timeouts: 17.06 + 17.06 = 34.13 ms. That is the 34.15 ms p50.

`Gfx_FrameEnd` (`0x00066530`) writes the frame fence with
`FUN_0006309c(&DAT_00f32644)` (a `SET_REFERENCE`) immediately before it
publishes the producer, stores the same value in `DAT_01245020[slot]`, and
calls `cellGcmSetPrepareFlip`. The display thread's ref test in step 2 is for
that value.

### The `0xFEAD` marker does not exist for this title

`walker-cuts-2.md` section 3.3 proposed completing the previous flip at "the
next frame's flip marker (`0xFEAD`)" because `Gfx_FrameEnd` emits the fence
immediately before it. The timeline and the per-flip log show this title has
**no in-FIFO flip marker**:

* 0 `[flipword] ... FIFO flip words decoded` lines in the whole run;
* every `[fliplog]` line has `req_to_fin=0` (non-FIFO request; the HLE
  `cellGcmSetFlipImmediate` import is what `Gfx_DisplayFlip` calls at
  `0x6941c`);
* `Gfx_FrameEnd` writes `0xfeed0001`/`0x4e000/0xcafebabe/0x4eb00`, not
  `0xFEAD`.

The analog of the marker-side design for this title is therefore **the fence
publication**, not a FIFO word: when a `SET_REFERENCE` newer than the one the
flip's request saw is published, the next frame's buffer ref is ready. That is
the `PS3RECOMP_FLIP_FENCE_DONE` key implemented here.

## 2. Present: a dedicated thread, never on the guest's critical path

`templates/project/main.cpp` calls `rsx_backend_present()` on the walker; the
Vulkan backend records and submits the swapchain blit, then hands the image to
`vk_present_thread_main`. `vkAcquireNextImageKHR` uses a 0 ms timeout unless
`PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS` is set, and the present queue is bounded
(`VK_PRESENT_SLOTS`); when full or not-ready the frame is dropped, not waited
for.

At the meter (`[RSX vulkan] window:` lines, 03:39:47-03:41:50):

* longest `vkQueuePresentKHR` 0-2 ms, one 10 ms outlier;
* a driver stall at 03:40:25 blocked the present thread for **1,139 ms** and
  dropped 7 no-slot presents. The guest kept flipping at its normal 34 ms
  (`[fliplog]` p99 43 ms in that arm), so the stall did not reach the guest;
* no `no-slot`/`not-ready` drops outside that stall.

`PS3RECOMP_VK_PRESENT_MODE` is read only when the swapchain is created, so it
cannot be A/B'd live. It is also irrelevant to the guest frame: the mode
changes when the presentation engine releases an acquired image, which only
affects our own present thread / window. The earlier `FLIP_ON_DONE` and
`TICK_IN_DRAIN` results already showed that the present fence is done long
before the guest's wait (`flip-deadline.md` section 3: present fence 0/369
active at the deadline).

## 3. Timeline: one hard frame, edge by edge

Representative A-arm deadline frame (guest 219788 ms, `PS3RECOMP_TIMELINE`,
`0x1245090` = ref lwcond, `0x1245100` = flip-done lwcond):

| t from request (ms) | edge |
|---:|---|
| 0.000 | flip request (`SetFlipImmediate`, req seq) |
| 0.002-17.064 | flip-done wait, **timeout** 17.061 ms |
| 4.356 | present queued (`[fliplog] pres`) |
| 10.18-33.74 | walker drain (one pass; producer-bound) |
| 17.067-34.128 | flip-done wait, **timeout** 17.060 ms |
| 18.858 (+9.06) / 27.925 (+9.06) | ref lwcond waits (main thread, same lwcond) |
| 33.738 | `SET_REFERENCE` executed and published (fence value 9152) |
| 34.128 | second timeout -> **deadline branch**, `DAT_011203e0 = 0` |
| 34.145 | next flip request (frame boundary) |
| ~36.7 (p50 over deadline frames) | `cellGcmTickFlip` completion |
| ~40.1 (p50) | guest flip handler delivered |

The completion lands **after** the frame has already ended on the deadline.
The fence at 33.738 is the next buffer's; it is published 0.4 ms before the
deadline in this frame. There is no earlier edge that would let the guest
advance without then blocking on that same fence.

### What sets each frame class (A arm, 1,027 timeline frames)

| class | n | frame dur p50 | c690 union p50 | drain end p50 | last fence p50 |
|---|---:|---:|---:|---:|---:|
| fast (<25 ms) | 328 | 14.15 | 0.00 | 14.11 | 13.91 |
| mid (25-32) | 4 | - | - | - | - |
| hard (32-36) | 425 | 34.15 | **32.81** | 34.15 | 34.41 |
| slow (36-100) | 274 | 38.76 | 34.24 | 38.76 | 38.57 |

For the hard frames, `dur - c690 union = 3.2 ms` and `dur - last fence =
0.26 ms`. The fast frames have no c690 work at all and are bounded by the
main-thread per-frame work (~14 ms). The 20-32 ms band is empty (4 frames of
1,027): the c690 burst is an all-or-nothing ~33 ms block, which is why p50
sits on the two-vsync deadline. **The two-vsync number is the size of our c690
producer burst, not a rounding grid in the flip path.**

## 4. What was built (all default off)

Review fixes from `ec4c75cf` first, then the marker-side experiment:

* **`presented_seq` is the queued image's sequence.** `s_flip_pending_seq` is
  stored where `s_flip_pending` is set (direct request or FIFO marker, under
  `FLIP_LOCK`), and `cellGcm_take_flip_pending[_synced]` copies it into
  `s_flip_presented_seq`. The old take-time `s_flip_req_seq` could name a
  newer request that was never presented.
* **One completion helper.** `gcm_flip_complete_locked(gate)` /
  `gcm_flip_complete(gate)` serve the display tick, the drain checkpoint and
  the present path (the three copies had already drifted). The tick and
  present gates keep their old conditions; the drain gate keeps
  `presented_seq == req_seq && executed`.
* **Fence-side marker completion (`PS3RECOMP_FLIP_FENCE_DONE`, live key).**
  `gcm_flip_request` records the ref value the guest had already seen
  (`s_flip_req_ref`); `gcm_ref_publish_one` calls
  `cellGcmFlipFenceCheckpoint(val)`. The gate completes the last presented,
  not-yet-completed image when a **newer** fence is published. Status is not
  consulted: this title's `SetPrepareFlip` writes `DONE` ahead of the display
  thread and the guest's wait is on the handler.
* **Tests.** `libs/video/tests/test_gcm_tick_in_drain.c` now covers the
  take-time identity (direct and FIFO-marker path), the tick-in-drain
  presented/executed guards, and the fence gate (same fence = no completion,
  newer fence = one completion, tick does not race it). All 30 video fixtures
  pass; the `spu_context` ABI check passes. No `spu_context` field was added.

## 5. Live A/B

One launch, arms rotated A/B/C, N=4 x 10 s, 2 s dropped, `PIN=0`:

| arm | control file | frames | fps | p50 | p90 | p99 | std* | mean | walker | wait |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A today | `TICK_IN_DRAIN=0 FLIP_FENCE_DONE=0` | 1,085 | **33.91** | **34.15** | 39.30 | 43.16 | 11.03 | 29.17 | **17.42** | **11.26** |
| B tick-in-drain | `TICK_IN_DRAIN=1` | 815 | 25.47 | 38.05 | 40.92 | 43.94 | 29.89 | 39.27 | 24.56 | 14.92 |
| C fence | `FLIP_FENCE_DONE=1` | 786 | 24.56 | 38.12 | 41.69 | 70.57 | 63.51 | 40.69 | 24.50 | 16.69 |

\* B/C pooled std is inflated by two present-thread stalls (the 1,139 ms one in
A, and one in C's last segment); p50/p90/p99 and the per-segment fps are the
robust numbers. A > B and A > C on every paired segment (A-B per segment
+8.00, +9.74, +8.50, +7.50 fps).

2 ms per-flip histogram (8-46 ms, `+` = over):

| bin (ms) | 8-10 | 10-12 | 12-14 | 14-16 | 16-18 | 18-20 | 20-22 | 22-24 | 30-32 | 32-34 | 34-36 | 36-38 | 38-40 | 40-42 | 42-44 | 44+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 55 | 43 | 68 | 80 | 62 | 20 | 3 | 1 | 1 | 4 | **437** | 117 | 123 | 51 | 12 | 8 |
| B | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 14 | 133 | **252** | **262** | 116 | 30 | 8 |
| C | 3 | 2 | 3 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 114 | **254** | 228 | 114 | 43 | 24 |

Flip-done deadline frames (two 17 ms timeouts, cumulative `[lwcond-stats]`
deltas over the four 8 s measured windows per arm):

| arm | deadline frames / 32 s | per second |
|---|---:|---:|
| A | 431 | 13.5 |
| B | 16 | 0.5 |
| C | 749 | 23.4 |

B shows the mechanism directly: it removes almost all deadlines but is
**8.4 fps slower**, because the guest now waits on the buffer ref
(`P50` per-frame ref-wait sum rises from 28.2 to 48.9 ms; the fast mode
disappears). C is worse: with the tick no longer the completion edge, the
fence lands at ~38 ms, i.e. **after** the deadline, so the guest times out
anyway and then receives a stale handler; 94 % of C frames take the deadline.

### Why the present thread did not explain anything

The one long present (1,139 ms) is a driver/host stall, not FIFO backpressure:
it happened while the guest kept producing at its normal rate and no present
was on the guest's path. It is excluded from the p50/p90/p99 comparison by
using segments and percentiles.

## 6. Why RPCS3 can show ~20 ms at the same scene

The guest contract is the same: it waits for the next frame's fence before it
flips, and the frame cannot be shorter than the producer. RPCS3's producer
(its SPU/RSX scheduling) finishes the meter frame in ~20 ms; ours takes ~33 ms
of c690 lane time plus ~1.5 ms of walker/fence tail. Matching that number means
cutting the c690 burst (native unpack/pack/skin/normalize, lane occupancy,
or the walker drain), which is already the documented speed priority. There is
no flip/present change that turns a 33 ms producer into a 20 ms frame.

## 7. Decision

* **Nothing enabled.** A (today) is the winner; `TICK_IN_DRAIN` and
  `FLIP_FENCE_DONE` remain default 0 in both `play_windowed.sh` copies and
  `ab_live.sh`.
* The review fixes (presented-sequence identity, shared completion helper,
  tests) are kept.
* The marker-side design is implemented for this title as the fence-publication
  key; it loses with the tick disabled, and an additive variant could recover
  at most the measured **0.26 ms** fence-to-frame slack (below the ~0.5 fps
  run-to-run drift) while risking the same ref-wait failure, so it was not
  given a second launch.
* The floor is not the game's vblank pacing: the guest expects a flip handler,
  falls back to its 2 x 16683 us deadline, and the deadline is *faster* than
  the real ref wait. It is our c690 producer burst that lands on ~2 vsync
  periods.

## 8. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work
rm -f /tmp/pacing_tl.bin /tmp/pacing_tl.go
( for i in $(seq 1 900); do f=$(ls -t ab_live_pacingfloor_*.log 2>/dev/null | head -1);
    [ -n "$f" ] && grep -q "pitch meter confirmed" "$f" && { sleep 2; touch /tmp/pacing_tl.go; break; }
    sleep 1; done ) &
TAG=pacingfloor N=4 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 PITCH_THRESH=35 LIMIT=880 \
  ARMS='A=PS3RECOMP_TICK_IN_DRAIN=0;PS3RECOMP_FLIP_FENCE_DONE=0|B=PS3RECOMP_TICK_IN_DRAIN=1;PS3RECOMP_FLIP_FENCE_DONE=0|C=PS3RECOMP_TICK_IN_DRAIN=0;PS3RECOMP_FLIP_FENCE_DONE=1' \
  BOOT_ARM='A=PS3RECOMP_TICK_IN_DRAIN=0;PS3RECOMP_FLIP_FENCE_DONE=0' \
  PS3_LWCOND_STATS=1 PS3RECOMP_TIMELINE=/tmp/pacing_tl.bin \
  PS3RECOMP_TIMELINE_TRIGGER=/tmp/pacing_tl.go PS3RECOMP_TIMELINE_SECS=90 \
  PS3RECOMP_TIMELINE_RING_MB=64 PS3RECOMP_TIMELINE_FLUSH_MB=16 \
  PS3RECOMP_TIMELINE_HITCH_US=0 ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_pacingfloor_*.log \
  --marks ab_live_pacingfloor_*.marks --seg 10 --drop 2
```

The timeline decoder keeps events 13, 15, 17, 29, 33, 37, 38, 41, 43, 49,
53, 54, 55; map ns to guest ms with the `[flip]`/`[fliplog]` anchor as the
other PSTL scripts do. The 774 MiB file was deleted after analysis.

## 9. Clean room

All of this is our own static analysis of the EBOOT (Ghidra) and our runtime's
own trace stream. No RPCS3 or other GPL emulator source was read or cited.
