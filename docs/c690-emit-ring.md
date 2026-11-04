# c690 0x21 emit/ring: 100 % compute, no DMA, no atomics, no wait

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. One windowed meter launch
(`ab_live_op21meas_20261104_043452`): standard `play_windowed.sh`/`ab_live.sh`
configuration (`PIN=0`, `WCB=2`, x11, the four cheap wins, the shader disk
cache, the hitch recorder, `SPURS_PM_FAST_RESCHED=8`, `CUBE_RT=1`, `PACE=1`)
plus `METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`, N=3 x 10 s (2 s dropped) and
`PS3RECOMP_C690_OP21=1`. Governor `performance`.

This follows [`edge-native-step5.md`](edge-native-step5.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`c690-function_map`](c690_function_map.md),
[`walker-cuts-2.md`](walker-cuts-2.md) and
[`c690-dispatch-cuts.md`](c690-dispatch-cuts.md).

## TL;DR

* **0x21 is compute, not a wait.** In 619,067 calls it issued **0** MFC
  transfers (0 GET, 0 PUT, 0 slow-path), **0** GETLLAR/PUTLLC, **0**
  tag-status reads, **0** atomic-status reads and **0** decrementer reads.
  The 64.99 s of inclusive 0x21 time splits **99.9 %** between two lifted
  sub-selector bodies and 0.2 % dispatch; there is no blocking event to
  correlate with the walker.
* **The time is in two emit/format kernels.** 0x13080 is a sub-selector
  dispatcher. Selector 1 (`0x13140 -> 0xBB80`) is 15.7 % of calls but
  **58.2 %** of the time (37.84 s, **390.4 us/call**); selector 7
  (`0x131A0 -> 0xBB98 / 0xA3E0 / 0xA3F8`) is 84.3 % of calls and **41.5 %**
  of the time (27.01 s, **51.7 us/call**). Both run the same
  float-transform/pack family (`fm`, `fma`, `shufb`, 0xBCB4/0xA410 and the
  0xBED0/0xBF00/0xC018 kernels), the first with a larger batch.
* **The old "emit/ring packet" name is misleading.** The measured 0x21 path
  never calls the ring allocation (0xE0A8), never reserves a line, and never
  DMAs. The ring allocation and its GETLLAR/PUTLLC live in the inter-job gap
  (where `PS3RECOMP_C690_GAP` already sees them), not in this command.
* **Walker/ring/DMA/atomic levers are ruled out for 0x21.** Publishing ring
  space or labels earlier, a cheaper reservation, a larger ring or a faster
  DMA path cannot remove any of this time: none of those events occurs.
* **The only applicable lever is native emit, and it is not small.** A native
  0x13140/0x131A0 kernel is a ~1 KB SPU float/shuffle body with a format
  switch, not a packet writer. Even at an optimistic 2x on all of 0x21 it
  saves ~5.2 ms of the 24.4 ms critical-lane busy (burst 35.0 -> ~29.8 ms),
  which stays on the 34.15 ms p50 floor; the measured parity of the existing
  native stages (0.99-1.53x) points to ~1-2 ms, inside the A/B noise. Nothing
  was implemented; no live key was added and the second launch was not spent.
* **Measurement only.** `PS3RECOMP_C690_OP21` is default off; no
  `spu_context` field was added and no game file changed.

## 1. Static: what LS 0x13080 actually is

The lifted handler `0x13080` starts by loading five words from the command
payload (`r3+0x150`, `+0x160`, `+0x154`...), tests two of them, and then
dispatches through an **8-entry pointer table embedded at LS 0x1311C**:

| selector | target | measured role |
|---:|---|---|
| 0 | `0x13178` | write one word at `[r80+0xA0]` and return |
| 1 | `0x13140` -> `0xBB80` | `0xBBB0` -> `0xBCB4` transform/pack body (large batch) |
| 2, 4, 5, 6 | `0x13198` | `stop` (not taken on this workload) |
| 3 | `0x131F8` -> `0xA3E0` | `0xA410` transform/pack body |
| 7 | `0x131A0` -> `0xBB98` / `0xA3E0` / `0xA3F8` | transform/pack body (small batch) |

The table entry is selected by `((byte of r4) & 7)`; the handler's two early
`stop` paths (`0x13240`/`0x13248`) were not taken in the measured run. The
selector targets are entered through `bi $r2` (indirect), so the existing
`c690_stats_on_lookup` hook sees each entry.

Both heavy targets converge on the same large body:

* `0xBB80` loads constants `0x144E0/0x14490/0x144A0/0x144D0`, `0xBB98` loads
  `0x14500/0x144B0/0x144C0/0x144F0`, both fall into `0xBBB0` -> `0xBCB4`.
* `0xBCB4` saves r80-r127, loads the format constants, computes a target from
  a pointer/byte pair and `bi $r13` into the `0xBED0/0xBF00/0xC018` kernel
  family. That code is float math: `fm`/`fma` on four lanes, `shufb` to
  de-interleave/interleave, `fa`/`xor`/`and` for the packing, then a loop
  (`brnz ... 0xBF00`, `bi $r102` at 0xC0F4) over the batch.
* `0xA3E0`/`0xA3F8` do the same via `0xA410`.

There is **no** call to `0xE0A8` (ring allocation), no `MFC_Cmd` write, no
`MFC_RdAtomicStat`, and no `MFC_RdTagStat` on the dynamic path. The static
reachability from `0xBB80` to the `0xDFD8`/`0xE020` MFC helpers exists in the
module, but the measured selector never takes it: the DMA-free kernel branch
is the one that runs.

## 2. Measurement

### 2.1 What was added

`runtime/spu/c690_stats.c` has a new measurement mode,
`PS3RECOMP_C690_OP21=1`, default off:

* On entry to handler `0x13080` (opcode 0x21) it opens a per-thread span and
  charges the time between sub-selector entries to the current selector.
  The span closes at the next command handler or the job end, so it uses the
  same command boundary as the existing inclusive timer.
* The per-thread counters cover GET/PUT DMA count and bytes, the time inside
  the synchronous MFC copy (`mfc_do_transfer_fast` and the slow-path fallback),
  GETLLAR/PUTLLC and failures, MFC tag-status reads, MFC atomic-status reads,
  `SPU_RdDec` reads, and the `spu_context.steps` delta per call.
* Reporting is cumulative (the existing 2 s periodic block plus atexit) and
  also prints a 2 s `[c690-op21-window]` delta line, so the last pre-exit
  window can be read without the boot/load history.
* Everything is inside the existing `g_c690_op21` predicted-not-taken tests;
  with the key off the runtime is unchanged.

### 2.2 Results (meter, cumulative at 188.4 s)

| metric | value |
|---|---:|
| c690 jobs | 864,420 |
| 0x21 calls | **619,067** (4.7 % of commands) |
| 0x21 inclusive time | **64,988,117 us = 43.4 %** of the 149,647,724 us command wall |
| per call | mean **104.98 us**, p50 **54 us**, p90 **112 us**, p99 **926 us**, max **3,086 us** |
| lifted entries per call | **222.5** (`ctx->steps`) |
| MFC GET / PUT | **0 / 0** (0.00 MiB, 0.00 MiB) |
| MFC slow path | **0** |
| DMA time | **0 us (0.0 %)** |
| GETLLAR / PUTLLC / failures | **0 / 0 / 0** |
| MFC tag-status / atomic-status reads | **0 / 0** |
| `SPU_RdDec` reads | **0** |

The 2 s window deltas are stable: the last window is 24,070 calls at
2,623,641 us (mean 109.0 us) with the same all-zero wait counters; the
previous 14 windows are within 108.7-110.9 us mean.

### 2.3 Sub-selector split (same 64.99 s)

| selector | calls | share of calls | inclusive us | share of 0x21 | avg us/call |
|---|---:|---:|---:|---:|---:|
| dispatch/other | 619,067 | 100 % | 144,597 | 0.2 % | 0.23 |
| `0x13140 -> 0xBB80` (sel 1) | 96,930 | **15.7 %** | **37,837,126** | **58.2 %** | **390.36** |
| `0x131A0 -> 0xBB98/A3E0` (sel 7) | 522,137 | **84.3 %** | 27,006,509 | **41.5 %** | **51.72** |

The weighted mean is `0.157*390.4 + 0.843*51.7 = 104.9 us`, exactly the
measured per-call mean. The p50 (54 us) is the common 0x131A0 call; the mean
is pulled up by the 15.7 % selector-1 calls.

### 2.4 Frame context (same run, 3 x 8 s measured)

| fps | p50 | p90 | p99 | walker | wait |
|---:|---:|---:|---:|---:|---:|
| 34.71 | 34.16 ms | 39.38 ms | 42.56 ms | 17.21 ms | 11.47 ms |

The lane job wall is 149.6 s over 188.4 s (summed over five lanes), and 0x21
is 43.4 % of it. At the documented 35.00 ms big burst / 24.39 ms critical-lane
busy, 0x21 is **~10.5 ms of the critical lane**; selector 1 is ~6.1 ms and
selector 7 ~4.4 ms of that.

## 3. Answer: compute vs wait

**0x21 is 99.9 % compute and 0 % wait.** The two selector bodies account for
all of the inclusive time; the runtime counters show no MFC transfer, no line
reservation, no tag/atomic status read and no decrementer read in 619,067
calls. There is nothing to correlate with the walker drain: the command does
not wait for a ring pointer, a label, a fence, a lock line or a DMA.

The name "emit/ring packet" came from the lifted function map, not from a
measured data flow. What the command actually does here is format data in
local store: the selector-1 path runs the same float-transform/pack body as
selector 7 but on a larger batch, which is why it is 7.5x slower per call
(390 vs 52 us) while being only 15.7 % of calls. That is batch-size scaling,
not a stall: a stall would not be stable at 390 us across every 2 s window and
would show up in one of the wait counters.

## 4. Fix proposal

| lever | verdict | estimate |
|---|---|---|
| walker publishes ring space/labels earlier | **ruled out** | no wait in 0x21 |
| cheaper GETLLAR/PUTLLC reservation | **ruled out** | 0 atomics in 0x21 |
| larger ring | **ruled out** | 0 ring accesses in 0x21 |
| faster DMA emulation for this pattern | **ruled out** | 0 MFC transfers in 0x21 |
| native emit (0x13140/0x131A0 kernels) | only applicable lever; **not small/exact** | 1.3x on both: ~2.9 ms of critical-lane busy -> burst ~32 ms; 2x: ~5.2 ms -> ~29.8 ms; 3x: ~7.0 ms -> ~28 ms |
| dispatch/overlap (unchanged) | still the only route below 25 ms | needs ~10 ms, i.e. ~3.5x on all of 0x21 plus the other levers |

A native emit would be a bit-exact reimplementation of a ~1 KB SPU
float/shuffle body with a format switch and a variable loop count, not a ring
or packet writer. The existing offline gates put comparable native stages at
0.99-1.53x, so the realistic burst saving is ~1-2 ms, below the ~1 ms A/B
noise, and p50 stays on the 34.15 ms two-vsync floor. That is why nothing was
implemented, no live key was added, and the second launch was not used.

## 5. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work
TAG=op21meas N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 PITCH_THRESH=35 LIMIT=420 \
  ARMS='A=' BOOT_ARM='' PS3RECOMP_C690_OP21=1 ./ab-tools/ab_live.sh
# final report: the last [c690-op21] block before the exit in
# ab_live_op21meas_*.log; the 2 s deltas are [c690-op21-window] lines.
```

The static addresses are in
`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c` (function
`spu_func_00013080` and its table), cross-checked against the raw module at
EBOOT file offset `0x1151300` (EA `0x01161300`, LS base `0x4000`).

## 6. Clean room

This is our own static analysis of the lifted c690 module and the raw SPU
bytes extracted from our own EBOOT, plus our runtime's own counters. The
stage/corpus method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. No RPCS3 or other GPL emulator source was
read or cited.
