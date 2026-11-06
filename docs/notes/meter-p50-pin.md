# What pins the meter p50 at 34.15 ms: the c690 producer, not the flip/vblank model

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play` (native c690 dispatch `SPURS_PM_NATIVE_DISPATCH=6` and
`SPURS_PM_FAST_RESCHED=64` are the play defaults). Two meter launches:

* `ab_live_p50pin_20261104_152125` — one diagnostic 40 s arm at the pitch
  meter with a 25 s full `PS3RECOMP_TIMELINE` (212 MB, deleted after analysis),
  `METER=1 SKIP_CINE=1 PIN=0 WCB=2 VIDEODRIVER=x11`, governor `performance`,
  `PITCH_THRESH=35`, one X.
* `ab_live_p50fix_20261104_153029` — one A/B launch, `N=4`, 10 s segments,
  2 s dropped per segment, arms alternated A/B: A =
  `PS3RECOMP_FLIP_DEADLINE_US=0` (today), B =
  `PS3RECOMP_FLIP_DEADLINE_US=33366` (cap the flip-done wait to the guest's
  exact 2 x 16683 us deadline). Same configuration, native dispatch on.

This follows [`frame-pacing-floor.md`](frame-pacing-floor.md),
[`flip-quantisation.md`](flip-quantisation.md),
[`flip-deadline.md`](flip-deadline.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`c690-serialisation.md`](c690-serialisation.md),
[`c690-native-dispatch.md`](c690-native-dispatch.md) and
[`targets-and-backlog.md`](../targets-and-backlog.md).

## TL;DR

* **34.15 ms is not a flip/vblank quantisation and not a 30 fps cap.** The
  title's `Gfx_DisplayFlip` (`0x000692dc`) gives up after
  `DAT_011203f8 << 1`, and `FUN_00064b68` sets that word to **0x412b =
  16683 us** in the 720p mode (`2 x 16683 = 33.366 ms`). With the timeline
  recorded, the five c690 lanes finish p50 **32.97 ms** after the flip
  request and the frame is **34.15 ms**; the extra **~1.2 ms** is the walker
  drain / ref-publication tail, not a rounding grid. Fast frames exist at
  11-18 ms and the EBOOT never calls `cellGcmSetFlipMode`; there is no fixed
  30 fps pacing.
* **Capping the flip-done wait to the guest's exact deadline does not move
  p50.** The A/B (off vs `PS3RECOMP_FLIP_DEADLINE_US=33366`) keeps p50 at
  **34.15 vs 34.15 ms**, p90 37.93 vs 38.45, p99 41.19 vs 43.04. The B arm
  also caught three unrelated 0.7-0.9 s present/pump stalls; the percentiles
  are the robust numbers. If the deadline were the limiter, the cap would
  have removed ~0.8 ms; it did not. The frame ends when the producer's next
  ref is published.
* **Where the time goes with native dispatch on** (853 timeline frames,
  25 s): frame p50 **34.15 ms**, c690 lane union p50 **32.01 ms**, first c690
  job p50 **0.10 ms**, last c690 job p50 **32.97 ms** (p90 34.81), walker
  drain p50 22.39 ms, walker put-wait p50 11.36 ms. In the hard 32-34 ms
  class (285 frames) the union is **32.78 ms** and the last job **33.08 ms**
  in a **34.15 ms** frame; 263 of those 285 frames leave on the two 17 ms
  timeouts, but the producer has already ended within ~1.1 ms of the frame
  end.
* **Native dispatch bought the tail, not the median.** From
  [`c690-native-dispatch.md`](c690-native-dispatch.md): fps 34.00 -> 35.33,
  p90 43.20 -> 39.21, p99 46.67 -> 42.66, 40-50 ms share 24.3 % -> 7.1 %,
  **p50 34.15 both**, gap p50 70.3 -> 53.8 us. It removed coordinator
  round-trip gaps; it did not remove job compute. The median frame is still
  set by the same c690 compute plus the walker/ref tail.
* **Edge is not finished for the median.** The remaining p50 lever is c690
  job compute: the diagnostic's opcode histogram is **0x21 emit/ring 25.0 %,
  0x22 whole-body 21.2 %, 0x2A assembler 13.5 %, 0x0A 6.5 %, 0x25/0x26 cull
  7.6 + 3.5 %** (cumulative meter run; the meter-only shares in
  [`edge-native-step5.md`](edge-native-step5.md) are the same shape). The
  parked GPU/native items are exactly these classes. A ~2x cut of 0x21+0x22+
  0x2A would remove ~5-7 ms of the burst and move p50 to ~28-29 ms.
* **Recommendation:** if the meter p50 is the target, continue Edge compute:
  unpark the 0x22 whole-body native (the largest single class) and/or the
  0x21 emit kernels before the renderer. If the user's priority is the
  real-play 40-80 ms frames, the renderer/walker is the lever and the meter
  median stays at 34 ms; Edge's remaining value there is only the p90/p99/
  40-50 tail, which native dispatch has already cut.

## 1. Static: the deadline is 2 x 16683 us, and it is not a frame timer

`Gfx_DisplayFlip` (`0x000692dc`, Ghidra) loops:

1. wait for a queued frame (`Cond_Wait(&DAT_012450a0, 9000)`);
2. wait for the buffer ref (`Cond_Wait(&DAT_01245068, 9000)`);
3. `cellGcmResetFlipStatus(); cellGcmSetFlipImmediate(bufferId);`
4. wait for the flip handler with an outer deadline:
   `now - start < (DAT_011203f8 << 1)`; on each pass it calls
   `Cond_Wait(&DAT_012450d8, 17000)`.

`FUN_00064b68` sets `DAT_011203f8 = 0x412b` (16683) unless the height is
0x240, where it is 20000. In the 720p meter it is therefore 16683 us and the
outer deadline is **33.366 ms**. Two host `Cond_Wait(17000)` rounds are
**34.0 ms**, and scheduling overhead puts the observed deadline frames at
**34.13-34.15 ms**. The vblank handler `FUN_00069288` writes the same value to
`DAT_011203f0`, which has no reader; this is not a frame-interval setting.

The title does not call `cellGcmSetFlipMode` and does not import
`cellGcmGetVBlankCount`, so there is no game-side 30 fps interval. The
deadline is a fallback for a late flip handler. Fast frames prove the title
can run faster: the diagnostic has 31 % of frames below 20 ms (p10 12.51 ms),
and the A/B has the same fast mode.

The question in the mission was whether 34.15 is a quantisation of *our*
flip/vblank emulation (for example the 16 ms display tick). The answer is no:
the tick is not the completion edge in these frames (the tick and the drain
share `rsx walker`, but the frame is set by the producer), and changing the
deadline to the guest's exact 33.366 ms is neutral (section 3). The 34.15 is
real producer work that lands on the deadline.

## 2. Diagnostic timeline with native dispatch on

Run `ab_live_p50pin_20261104_152125`: 853 frame ends in the 25 s window
(guest 152.45-176.92 s), 663 with c690 work. Frame boundary is
`PS3_TR_GFX_FRAME_END` (the `cellGcmSetFlipImmediate` request from
`Gfx_DisplayFlip`); c690 spans are the five `spurs pm` lanes.

| term | p10 | p50 | p90 | mean |
|---|---:|---:|---:|---:|
| frame (request -> next request) | 12.51 | **34.15** | 38.67 | 28.68 |
| c690 lane union | 0.00 | **32.01** | 34.18 | 22.22 |
| first c690 job (from frame start) | -0.08 | 0.10 | 0.18 | 0.14 |
| last c690 job (from frame start) | 1.66 | **32.97** | 34.81 | 28.93 |
| walker drain (union) | 2.01 | 22.39 | 28.85 | 17.60 |
| walker put-wait (union) | 4.92 | 11.36 | 15.61 | 11.11 |

Per frame class:

| frame class | n | frame p50 | c690 union p50 | last c690 p50 | two-timeout frames |
|---|---:|---:|---:|---:|---:|
| <20 ms | 271 | 14.13 | 0.00 | 0.79 | 0 |
| 20-32 ms | 11 | 21.10 | 0.69 | 1.32 | 0 |
| **32-34 ms** | **285** | **34.15** | **32.78** | **33.08** | **263** |
| 34-38 ms | 165 | 36.77 | 33.02 | 33.28 | 19 |
| 38-42 ms | 113 | 39.24 | 34.11 | 34.31 | 0 |
| >42 ms | 8 | 44.42 | 34.90 | 35.22 | 0 |

The 34.15 mode is the 32-34 ms class. The c690 burst starts at +0.1 ms, ends
at +33.1 ms, and the frame ends at +34.15 ms: about **1.1 ms of walker/ref
tail** after the last job. The 22 frames in this class that leave by a signal
rather than the deadline have the same last-job time, which is why the
`Gfx_DisplayFlip` completion edge is not the variable that sets the class.

The deadline frames are not "the Edge burst already finished, then the game
waits for a grid": 163 of 299 hard frames (54.5 %) have their last c690 job
before the guest's true 33.366 ms deadline, and the frame still ends at
34.15. The rest have c690 spilling to 33-35 ms. Both cases are producer-bound
at the frame end, and the A/B in section 3 confirms that moving the deadline
does not help.

## 3. A/B: cap the flip-done wait to the guest's exact deadline

`PS3RECOMP_FLIP_DEADLINE_US` is a live numeric key (default **0**). When
non-zero, `sys_lwcond_wait_real` (`runtime/ppu/ppu_sysprx.cpp`) uses
`runtime/ppu/flip_deadline.h` on the flip-done lwcond (`0x1245100`, 17000 us
waits) to cap the second timeout to the remaining time to that deadline
(plus a 200 us margin for the guest's earlier start). A third wait after the
deadline returns `ETIMEDOUT` at once, so jitter cannot turn it into a 50 ms
timeout. The state machine is unit-tested
(`tests/sync_stress/test_flip_deadline.c`, `ctest -R flip_deadline` passes);
no `spu_context` fields were added and the shared-field ABI check passes.

Run `ab_live_p50fix_20261104_153029`, 4 x 8 s measured per arm (2 s dropped
per 10 s segment):

| metric | A (off) | B (33366) |
|---|---:|---:|
| frames | 1,125 | 956 |
| fps | **35.16** | 29.88* |
| p50 ms | **34.15** | **34.15** |
| p90 ms | 37.93 | 38.45 |
| p99 ms | 41.19 | 43.04 |
| mean ms | 28.43 | 32.07 |
| 40-50 ms share | 2.8 % | 3.9 % |
| 50-80 ms share | 0.1 % | 0.1 % |

\* B has three unrelated present/pump stalls (window `max pump gap` 717, 898,
898 and 827 ms in segments 5 and 7; `longest vkQueuePresentKHR` 1-3 ms, 0
no-slot/not-ready drops). The other B segments run 36.50 and 33.75 fps. p50,
p90 and p99 are unchanged by the stalls.

Per-segment p50: A 34.15 / 34.25 / 34.14 / 34.15, B 34.15 / 34.17 / 34.14 /
34.15. The cap moved the **wait exit** to the guest's deadline but the
**frame boundary** did not move: the display thread then waits for the next
frame's buffer ref, which is published by the same c690 + walker producer at
~34.1 ms.

This is the decisive negative result: the 17 ms wait overshoot is real
(34.15 vs 33.366), but it is not what pins the frame. The pin is the producer
ending at ~33.1 ms plus the ~1.1 ms walker/ref tail.

## 4. Why native dispatch did not move the median

[`c690-native-dispatch.md`](c690-native-dispatch.md) measured the native
dispatch path with the same play defaults:

| metric | native off | native on |
|---|---:|---:|
| p50 ms | 34.15 | 34.15 |
| p90 ms | 43.20 | 39.21 |
| p99 ms | 46.67 | 42.66 |
| 40-50 ms share | 24.3 % | 7.1 % |
| gap p50 us | 70.3 | 53.8 |
| gap p90 us | 104.2 | 69.8 |
| lane busy | 0.589 | 0.613 |

The native path removes the worker -> coordinator -> worker round trip on the
c690 rerun. That is a **per-job dispatch gap**, and it is what the 40-50 ms
tail was made of. The median job compute is unchanged (the same lifted SPU
code runs; native dispatch only re-enters the PM in place). The timeline
above shows the median frame ending ~33 ms after the burst starts, so a gap
cut of ~1-2 ms cannot move it; the 40-50 frames fall to 34-38 instead.

Equivalently, the mission's premise ("more Edge speed cannot move p50 if the
pin is something else") is answered in the negative for the current build:
the pin *is* Edge compute plus the walker tail. Native dispatch finished the
tail of the Edge work; it did not finish the median.

## 5. What the remaining Edge classes are

Diagnostic `[c690-stats]` opcode shares (cumulative over the meter run; the
meter-only ranking in [`edge-native-step5.md`](edge-native-step5.md) is the
same shape):

| class | share of c690 inclusive time |
|---|---:|
| 0x21 emit/ring (handler 0x13080) | 25.0 % |
| 0x22 whole-body (0x10190) | 21.2 % |
| 0x2A assembler (0x10F78) | 13.5 % |
| 0x25/0x26 cull | 7.6 + 3.5 % |
| 0x0A unpack/pack | 6.5 % |
| everything else | 22.7 % |

The parked work in [`edge-exactness.md`](edge-exactness.md) is exactly this:
GPU 0x2A (four record classes; only the 36-byte and part of the 32-byte
classes have exact kernels), GPU 0x21 sel7 / 0x25/0x26 cull, the whole-body
0x22 native body, and extra host lanes (`wip/c690-extra-lanes`). None is
default-on; all remain behind keys or offline only.

## 6. Recommendation (Edge vs renderer)

* **Meter p50 target:** continue Edge compute. The median is the c690
  producer, not the flip path or the game's pacing. The largest single lever
  is 0x22 whole-body (21.2 %), then 0x21 emit (25.0 %) and 0x2A (13.5 %). A
  2x cut of those three is ~5-7 ms of the burst, i.e. p50 ~28-29 ms. The
  renderer/walker does not touch this: the meter's walker drain is p50
  22.39 ms, smaller than the c690 burst.
* **Real-play 40-80 ms target:** the renderer/walker is the lever. The
  replay capture ([`c690-serialisation.md`](c690-serialisation.md)) has the
  c690 batch ending at +24-32 ms while the walker drains 66-77 ms; the meter
  A/Bs cannot shorten those frames. Edge tail work is already done
  (native dispatch: 40-50 share 24.3 -> 7.1 %); further Edge median work
  would not remove the real-play long frames.
* **Do not** chase the last 0.8 ms of the 17 ms wait: the A/B shows capping
  it to the guest's exact 33.366 ms is neutral. The key stays default off and
  is an experiment only.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# 1) diagnostic timeline at the meter, native dispatch on
rm -f /tmp/p50pin_tl.bin /tmp/p50pin_tl.go
( for i in $(seq 1 600); do f=$(ls -t ab_live_p50pin_*.log 2>/dev/null | head -1);
    [ -n "$f" ] && grep -q "pitch meter confirmed" "$f" && { sleep 2; touch /tmp/p50pin_tl.go; break; }
    sleep 1; done ) &
TAG=p50pin N=1 SEG=40 DROP=0 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=800 SETTLE=10 PITCH_THRESH=35 \
  PS3RECOMP_TIMELINE=/tmp/p50pin_tl.bin PS3RECOMP_TIMELINE_HITCH_US=0 \
  PS3RECOMP_TIMELINE_TRIGGER=/tmp/p50pin_tl.go PS3RECOMP_TIMELINE_SECS=25 \
  PS3RECOMP_TIMELINE_RING_MB=64 PS3RECOMP_TIMELINE_FLUSH_MB=16 \
  ./ab-tools/ab_live.sh

# 2) deadline-cap A/B
cd build-play && ninja MyGameRecomp && cd ..
TAG=p50fix N=4 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=800 SETTLE=10 PITCH_THRESH=35 \
  ARMS='A=PS3RECOMP_FLIP_DEADLINE_US=0|B=PS3RECOMP_FLIP_DEADLINE_US=33366' \
  BOOT_ARM='PS3RECOMP_FLIP_DEADLINE_US=0' ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_p50fix_*.log \
  --marks ab_live_p50fix_*.marks --seg 10 --drop 2
```

The 212 MB timeline and the captures are in `/tmp` / the run folders and are
not committed (see the repo rule on `recomp-work`).

## 8. Caveats

* One diagnostic launch and one A/B launch, one meter state each. The A/B is
  4 x 8 s per arm; the p50 is exact to 0.01 ms and is identical in every
  paired segment, which is stronger than the ~0.4 fps run-to-run drift.
* The B arm's present/pump stalls (0.7-0.9 s) are host window stalls, not
  engine drops; they inflate B's mean/std and fps but do not touch the
  percentiles. They are why the pooled B fps is not the right comparison.
* The opcode shares are cumulative over the run; they are used only for
  ranking. The meter-only ranking is in `edge-native-step5.md`.
* The timeline is the same scene and build as the A/B but not the same
  instant; the A/B's p50 matches the timeline's p50 exactly.
* The title's deadline word is 20000 us in the 0x240 video mode, so the cap
  is a live numeric value, not a hard-coded 33366.

## 9. Clean room

All of this is our own static analysis of the EBOOT (Ghidra), our own lifted
SPU code and runtime trace stream, and our own logs. No RPCS3 or other GPL
emulator source was read or cited.
