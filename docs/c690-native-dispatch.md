# c690 native dispatch: FAST_RESCHED 64, the dispatch-cut loss, and a fairness-gated lane hand-off

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Three meter launches: `fastresched1` (FAST_RESCHED
8/16/32/64), `native1` (first native attempt, staged-only), `native2` (native
key with the fairness fallback). All used `METER=1 SKIP_CINE=1 PIN=0 WCB=2`,
x11, governor `performance`, `PITCH_THRESH=35`, `PS3RECOMP_C690_GAP=1`, and
`SPURS_SCHED_TRACE=1` where noted. This follows
[`long-frames.md`](long-frames.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`c690-dispatch-cuts.md`](c690-dispatch-cuts.md),
[`c690-job-gaps-ab.md`](c690-job-gaps-ab.md) and
[`c690-job-gaps.md`](c690-job-gaps.md).

## TL;DR

* **`SPURS_PM_FAST_RESCHED=64` wins the 8/16/32/64 probe on the tail.** Over
  3 x 10 s per arm (2 s dropped), arm D (64) beats the current 8 on every
  paired segment: **+0.50 fps** mean (+0.62/+0.50/+0.38), p90 **43.53 vs
  44.63 ms**, p99 **46.84 vs 48.03 ms**, p50 identical (34.15 ms), 40-50 ms
  share 26.6 % vs 26.4 %. 16 is neutral, 32 is slightly worse. **64 is now the
  play default** in both `play_windowed.sh` copies and `ab_live.sh`.
* **Why the dispatch-cuts bundle lost ~2 fps.** `SPURS_PM_FAST_ACTIVE` is the
  only cut that changes *when* the fast path fires: it keeps a lane in a rerun
  loop at `active == maxContention`. At that steady state the coordinator
  round trip is the cross-workload arbitration point (DXT/Crowd/synth/MP3/
  Kinematics), so the bundle compressed the c690 burst (host 23.54 -> 9.19 ms,
  wall 34.25 -> 30.99 ms) but slowed the frame (walker wait +0.94 ms, engine
  +0.91 ms, fps 35.30 -> 33.29). `ONE_LOCK` and `GEN` only shave the fast
  path's own overhead and cannot add work.
* **Where a gap goes today.** The existing timelines still split the
  critical-lane 10.61 ms gap in a 35.00 ms burst as **tail 2.95 / host 5.03 /
  head 2.56 ms**; the host part is the worker -> coordinator -> worker CV round
  trip. Mailbox/signal/event/reservation/out-mailbox/MFC-tag waits are **0
  records**; `LOCKLINE_RETRY` is 0.3 % of gap time. The new 2 s live window
  measures the same thing globally: A (FAST_RESCHED=64) **gap p50 70.3, p90
  104.2, p99 1216 us**, lane busy 0.589.
* **The native path is implemented behind `SPURS_PM_NATIVE_DISPATCH` (wid
  bitmask, default 0) and it is a large win at the meter.** Two triggers:
  (a) a job the PM has already staged in its persistent local store, or
  (b) ready/signal work for the same workload **only while no other enabled
  workload wants this lane**. The PM still owns the WWS job-list protocol, so
  order, dependencies, `maxContention`, priorities, `WwsJobList_SpinUntilDone`
  counters, labels/signals and the workload state are unchanged. A/B
  (`native2`, 5 x 10 s per arm): **fps 34.00 -> 35.33**, p90 **43.20 ->
  39.21 ms**, p99 **46.67 -> 42.66 ms**, p50 34.15 ms both, 40-50 ms share
  **24.3 % -> 7.1 %**, gap p50 **70.3 -> 53.8 us**, gap p90 **104.2 -> 69.8
  us**, gap p99 **1216 -> 397 us**, lane busy **0.589 -> 0.613**.
* **The staged-job trigger never fires on this PM** (`nstage=0`): the wwsjob
  PM stores the job before exiting, so its pipeline words are `0xFFFFFFFF` at
  exit. The effective mode is (b), the fairness-gated rerun. The staged path
  is implemented and unit-tested for a PM that preloads.
* **Nothing default-on except FAST_RESCHED=64.** The native key stays default
  off per the mission; the A/B is a clear recommendation to enable it
  (`SPURS_PM_NATIVE_DISPATCH=6` for `MeshJobs0/1`).

## 1. Task 1: `SPURS_PM_FAST_RESCHED` 8 / 16 / 32 / 64

One launch (`ab_live_fastresched1_20261104_133730`), 12 alternating segments
A/B/C/D x 3, 10 s each, first 2 s dropped. The key is a live control-file
value, so no rebuild. `PS3RECOMP_C690_GAP=1` was on in every arm (its ~9 %
lane-sample cost is common to all arms).

### 1.1 Frames and shares (pooled over 3 segments per arm)

| arm | value | frames | fps | p50 | p90 | p99 | 40-50 | 50-80 | >80 | >=40 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 8 | 793 | 33.04 | 34.15 | 44.63 | 48.03 | 26.4 % | 0.3 % | 0.1 % | 26.7 % |
| B | 16 | 794 | 33.08 | 34.15 | 43.91 | 47.98 | 25.2 % | 0.3 % | 0.0 % | 25.4 % |
| C | 32 | 781 | 32.54 | 34.15 | 43.93 | 47.17 | 30.2 % | 0.0 % | 0.0 % | 30.2 % |
| D | 64 | 805 | **33.54** | 34.15 | **43.53** | **46.84** | 26.6 % | 0.0 % | 0.0 % | 26.6 % |

Paired against A at the same rotation index:

| pair | segment | fps delta | p90 delta | p99 delta |
|---|---|---:|---:|---:|
| B-A | 1 / 5 / 9 | -0.12 / -0.12 / +0.38 | -0.46 / -0.99 / -0.54 | +1.00 / -3.61 / -1.19 |
| C-A | 2 / 6 / 10 | -0.38 / -0.38 / -0.75 | -0.53 / -0.89 / -0.59 | +0.42 / +0.07 / -1.95 |
| D-A | 3 / 7 / 11 | **+0.62 / +0.50 / +0.38** | **-1.10 / -1.16 / -0.79** | **-0.23 / -2.71 / -2.75** |

D wins all three pairs on fps, p90 and p99; p50 is 34.15 ms in every arm. B is
neutral (its segment 5 is a present/scene outlier: lane busy 0.473 vs 0.58-0.59
elsewhere, 40-50 share 21.2 %). C is consistently a little worse and is not a
candidate. The run-to-run spread for PIN=0 is ~0.37 fps
([`pin-and-burst-path.md`](pin-and-burst-path.md) §2.4), so +0.50 fps alone is
borderline; the **p90 win on all three pairs** is the stronger evidence.

### 1.2 c690 gap (global 2 s window; per lane needs the timeline)

| arm | value | gap p50 us | gap p90 us | gap p99 us | lane busy | jobs/s (5 lanes) |
|---|---:|---:|---:|---:|---:|---:|
| A | 8 | 73.7 | 108.7 | 1220 | 0.584 | 8,737 |
| B | 16 | 73.6 | 107.2 | 1204 | 0.550* | 8,244* |
| C | 32 | 73.6 | 106.8 | 1167 | 0.585 | 8,750 |
| D | 64 | 73.7 | **106.8** | **1172** | **0.589** | **8,836** |

`*` B's segment 5 outlier. The 2 s window is global, not per lane; the
per-lane split needs a `PS3RECOMP_TIMELINE` dump (the pinburst analysis in
[`pin-and-burst-path.md`](pin-and-burst-path.md) is that view). The live
numbers show the same direction as the fps tail: 64 leaves the typical gap
alone and trims the p90/p99 tail.

**Decision:** `SPURS_PM_FAST_RESCHED=64` is the play default in
`recomp-work/play_windowed.sh`, `mlb16-recomp/tools/play_windowed.sh` and
`recomp-work/ab-tools/ab_live.sh`. It is a live key, so an override still
works.

## 2. Why `c690-dispatch-cuts` lost ~2 fps

The earlier mission measured a three-key bundle:
`SPURS_PM_FAST_ACTIVE` + `SPURS_PM_FAST_ONE_LOCK` + `SPURS_PM_FAST_GEN`
([`c690-dispatch-cuts.md`](c690-dispatch-cuts.md)). The bundle did exactly what
it was built to do:

| metric | A (today) | B (bundle) |
|---|---:|---:|
| fast rerun share | 34.5 % | **85.5 %** |
| host phase per burst (5 lanes) | 23.54 ms | **9.19 ms** (-61 %) |
| burst wall | 34.25 ms | **30.99 ms** (-9.5 %) |
| lane occupancy | 70.3 % | **76.6 %** |
| guest fps | **35.30** | 33.29 |
| walker wait | **11.32 ms** | 12.26 ms |
| walker engine | **16.47 ms** | 17.36 ms |
| <=16 ms frames | **25.6 %** | 21.4 % |
| 36-38 ms frames | **8.9 %** | 13.5 % |

**Which cut:** `FAST_ACTIVE` is the only one that changes when the fast path
fires. `ONE_LOCK` and `GEN` are overhead-only: they shave lock acquisitions or
skip a registry lock on a rerun that would happen anyway; they cannot add
guest work. So the loss is attributable to rerunning at
`active == maxContention`.

**What got slower:** not the c690 work. Throughput was flat (14,622 -> 14,826
jobs/s, +1.4 %; mean job 177.0 -> 174.7 us). The c690 burst got *shorter*, but
the frame got *longer*: walker wait +0.94 ms, walker engine +0.91 ms, draw
+0.34 ms, fewer sub-16 ms frames and more 36-38 ms frames. The p50 stayed on
the 34.15 ms two-vsync floor.

**Mechanism:** at `maxContention` the coordinator round trip is the
arbitration point for the seven workloads on six lanes. A fast rerun keeps the
lane for up to eight quanta; `FAST_ACTIVE` removed the coordinator's chance to
hand it to DXT, Crowd, synth, MP3 or Kinematics, which the same frame also
needs. The c690 work finishes sooner; the frame finishes later. The earlier
`FAST_RESCHED` win did not contradict this because it fired only when
`active < maxContention` -- i.e. when there was no contention to arbitrate.

**What we did instead:** keep the cheap case (the lane has the same
workload's job ready) but add the arbitration test at the rerun point: if any
other enabled workload has ready work or a signal for a lane this physical SPU
is eligible for, yield to the coordinator. Section 4 shows that this
fairness-gated version wins rather than loses.

## 3. Where a gap's time goes today

The existing per-lane timelines are still the authoritative decomposition
([`pin-and-burst-path.md`](pin-and-burst-path.md) §4.2,
[`c690-job-gaps.md`](c690-job-gaps.md) §3). For the critical lane of a
35.00 ms big burst:

| phase | ms | share of the 10.61 ms gap | what it is |
|---|---:|---:|---|
| tail | 2.95 | 27.8 % | job submit epilogue + output PUTs + PM post-job bookkeeping + exit-to-kernel |
| **host** | **5.03** | **47.4 %** | worker returns, coordinator rescans/wakes, `spu_run_policy_module` re-enters the PM |
| head | 2.56 | 24.1 % | PM ticket claim, descriptor DMA, code re-DMA, job context |

* **Not lock-line atomics.** GETLLAR/PUTLLC ~2.5 per gap with ~7 % retries;
  `LOCKLINE_RETRY` spans cover 87 ms of 26.4 s of gap time (0.3 %).
* **Not mailbox/tag/event waits.** `SPU_MBOX_WAIT`, `SIGNAL_WAIT`,
  `EVENT_WAIT`, `RESV_WAIT`, `OUT_MBOX_WAIT` and `TAG_WAIT` emit **0 records**
  in this build/workload.
* **Not the code re-DMA.** `SPU_CODE_DMA_MEMO` removed ~97 % of overlay
  reloads and was flat on frame time; the copy is cache-resident and
  overlapped across lanes.
* **The host phase is the CV round trip.** The PM is persistent and re-entrant;
  the worker/coordinator hand-off is what the native path removes.

The new live window reproduces the direction without a timeline: A
(FAST_RESCHED=64) gap p50 70.3 us, p90 104.2 us, p99 1216 us; B (native) 53.8 /
69.8 / 397 us. The typical gap falls by ~16.5 us and the p90 by ~34 us, which
is the host part; the residual is the PM's own tail/head.

## 4. Design: native dispatch for the c690 job list

### 4.1 What the guest observes

The PPU waits for a mesh job list in `Mesh_WaitPrevJobList`
(`FUN_0006ed24`) -> `WwsJobList_Wait` (`0x009fbd70`) ->
`WwsJobList_SpinUntilDone` (`0x009fb898`). We decompiled our own EBOOT to
confirm the contract (clean room: no external emulator source):

* the job-list descriptor's first word is the list base;
* for the mesh stage (`param_2 = 3`) the stage header is at
  `base + 0x30 + 0x40 = base + 0x70`: `numJobs` at `+0x40` (relative to the
  stage header) and the per-lane completion counters at `+0x42, +0x44, ...,
  +0x4c`, with `+0x4e` as the list counter;
* the PPU waits until the minimum of those counters reaches the submitted
  count, then re-reads the job array at
  `*(base + stage*4 + 0x30) + count*8` and adds the chain lengths (a job
  descriptor with a non-zero `+2` continues the chain);
* `sync(1)` (`lwsync`) is used before each read.

The job code's submit epilogue increments the per-lane counter after the
output PUT. The PM's ticket/claim/staging code decides the order. **None of
that is ours to rewrite in the first implementation.**

### 4.2 The native path

The PM (`wwsjob_pm`, image 6) is lifted C and runs on the `spurs pm` worker.
The native path keeps the same PM and the same local store; it only avoids the
host round trip. On a PM return, the worker may run the PM again in place
when all of these hold:

1. the live key `SPURS_PM_NATIVE_DISPATCH` names this workload id (bitmask);
2. the workload is the title's c690 mesh workload: `slot->data` (job-list EA)
   is non-zero and the guest's `maxContention` is 5;
3. the slot still matches the dispatch (`spurs_pm_static_ok`, with the
   already-active lane allowed: `active <= maxContention`);
4. either
   * **staged:** the PM's persistent local store has a job that was loaded but
     not stored (`spurs_policy_pm_pipeline_staged`: `loadJobNum != -1 &&
     loadJobNum != storeJobNum`, with `runJobNum` consistent), or
   * **ready/signal:** the guest still publishes ready work or a signal for
     this workload, and the ready-level admission test passes with the
     already-active lane allowed;
5. **fairness:** no other enabled workload has ready work or a signal for a
   lane this physical SPU is eligible for and below its `maxContention`;
6. the PM has run fewer than 256 quanta in this residency (a bound against a
   stale-ready livelock).

When the path is taken, the worker mirrors exactly the coordinator's
bookkeeping under one lock-line critical section: consume one ready unit (count
mode) or clear the signal bit, write `wklCurrentContention`, reset `idle`, and
emit the same `spurs.kick` trace instant. For a **staged** job it leaves the
ready count untouched: the job was already claimed under an earlier unit, and
the coordinator would not have a new unit to consume either. Then it runs the
PM again without releasing the slot or waking the coordinator.

**Fallback:** any failed test returns to the existing coordinator path. That
covers an unrecognised wid, no job list, a different `maxContention`, a lost
priority slot, a state change, no staged/ready work, a competing workload, a
missing resident PM context (a fixture without `spurs_policy.c` links the
lookup weak as NULL), or the 256-quantum bound.

### 4.3 Exactness argument

* The PM image, data and local store are unchanged; the PM is persistent and
  re-entrant, and the worker only re-enters it.
* The PM still performs every ticket claim, descriptor/code DMA, job dispatch,
  output PUT and completion-counter update. The PPU's
  `WwsJobList_SpinUntilDone` sees the same counters and the same job order.
* `maxContention`, priorities, ready count and signal bits are read from the
  same guest memory and written with the same bookkeeping as the coordinator.
* The only guest-visible difference is *when* the lane yields. The fairness
  test preserves the arbitration the other workloads need; that is exactly
  the condition the `FAST_ACTIVE` bundle missed.
* The staged trigger does not decrement ready: the job was already claimed,
  so the guest's ready count must not move.

### 4.4 What the A/B says about the two triggers

The `[spurs-native]` diagnostic prints the first four exits of each c690 lane:

```
[spurs-native] wid=2 sn=3 staged=0 ready=1 sig=0 \
  pipe=FFFFFFF1,FFFFFFFF,FFFFFFFF,FFFFFFFF have=1
```

The three job numbers (`loadJobNum`, `runJobNum`, `storeJobNum`) are
`0xFFFFFFFF` at exit: **the wwsjob PM stores the job before yielding and does
not leave a loaded job behind.** The `nextLoadJobNum` word reads `0xFFFFFFF1`
at this offset, which is not a usable counter; the staged predicate uses the
three job numbers and is unaffected. Consequently `nstage=0` for the whole run:
the staged trigger never fires on this PM. The effective mode is the
fairness-gated ready/signal rerun. The staged trigger is still implemented and
unit-tested (a manufactured policy that leaves `loadJobNum=0`,
`storeJobNum=-1` is rerun without a coordinator dispatch); it will matter only
for a PM that pipelines across the exit.

## 5. Implementation

### 5.1 Code

| file | change |
|---|---|
| `libs/video/ps3_live_ctl.h/.c` | new live key `SPURS_PM_NATIVE_DISPATCH` (wid bitmask, default 0) |
| `libs/spurs/cellSpurs.c` | `spurs_pm_native_wids`, `spurs_pm_native_wid_ok`, `spurs_pm_native_other_ready`, `spurs_pm_native_try_rerun`; worker hook at `:2247`; `spurs_pm_static_ok`/`spurs_pm_ready_ok` gained `allow_active` |
| `runtime/spu/spurs_policy.c` | `spurs_policy_pm_pipeline_staged` (pure), `spurs_policy_pm_staged_pending` (resident lookup), `spurs_policy_pm_pipeline_words` (diagnostic) |
| `runtime/spu/spu_workload.h` | prototypes for the pure predicate and diagnostic |
| `tests/test_spurs_scheduler.c` | pure predicate shapes + end-to-end staged-job rerun (coordinator delta 1, native delta 1, ready 5 -> 3) |

Key entry points: `libs/spurs/cellSpurs.c:2151` (`spurs_pm_native_try_rerun`),
`:2117` (`spurs_pm_native_other_ready`), `:2247` (worker hook),
`runtime/spu/spurs_policy.c:161` (`spurs_policy_pm_pipeline_staged`), `:219`
(`spurs_policy_pm_staged_pending`).

### 5.2 Live key and A/B

`SPURS_PM_NATIVE_DISPATCH` is a bitmask of workload ids. `0` is off (the
default); `6` selects wids 1 and 2 (`MeshJobs0/1`). It is a control-file key,
so the A/B is one launch.

Run `native2` (`ab_live_native2_20261104_140242`): A =
`SPURS_PM_FAST_RESCHED=64;SPURS_PM_NATIVE_DISPATCH=0`, B = the same plus
`SPURS_PM_NATIVE_DISPATCH=6`, 5 x 10 s per arm, 2 s dropped.

| metric | A (native off) | B (native on) |
|---|---:|---:|
| frames | 1,360 | 1,413 |
| fps | 34.00 | **35.33** |
| p50 ms | 34.15 | 34.15 |
| p90 ms | 43.20 | **39.21** |
| p99 ms | 46.67 | **42.66** |
| mean ms | 29.38 | **28.29** |
| 40-50 ms share | 24.3 % | **7.1 %** |
| 50-80 / >80 | 0.0 / 0.0 % | 0.0 / 0.1 % |
| gap p50 us | 70.3 | **53.8** |
| gap p90 us | 104.2 | **69.8** |
| gap p99 us | 1216 | **397** |
| lane busy | 0.589 | **0.613** |
| c690 jobs/s (5 lanes) | 14,783 | **15,509** |

Per-segment fps (A/B alternating): 34.12/36.00, 34.25/36.00, 33.38/35.88,
34.00/37.12, 34.25/31.62. The last B segment has a present stall
(std 49.26 ms, 40-50 share 9.5 %); excluding it B averages ~36.3 fps. The p50
does not move; the gain is entirely the 40-50 ms population and the gap tail.

The `[spurs-sched]` counters at the end of the run: `fast=373222 native=746839
coord=1201262 ncall=779342 nstage=0 nready=5599 nother=26904`. The native path
fired 746,839 times; the fairness test yielded 26,904 times and the
no-ready/no-staged test 5,599 times. The sum matches `ncall`.

### 5.3 Offline checks

| check | result |
|---|---|
| `python3 tools/test_spurs_scheduler.py` | **PASS**, including the new staged predicate and the end-to-end native rerun (coordinator delta 1, native delta 1, ready 5 -> 3) |
| `test_spurs_api_limits`, `coordinator_kick`, `queue_size`, `teardown`, `task_signal` | **PASS** |
| `check_spu_context_abi.sh` | **PASS** (no shared `spu_context` fields changed) |
| `ninja MyGameRecomp` | builds |

## 6. Changes kept

* `SPURS_PM_FAST_RESCHED=64` is the play default in both `play_windowed.sh`
  copies and `ab_live.sh`.
* `SPURS_PM_NATIVE_DISPATCH` is implemented, default **0** (off), and is a
  live key. The A/B is a clear win; enabling it for play is a one-line change
  (`export SPURS_PM_NATIVE_DISPATCH="${SPURS_PM_NATIVE_DISPATCH:-6}"`), left to
  the user because the mission caps default-on changes at the FAST_RESCHED
  value.
* No `spu_context` fields were added; the ABI check passes.
* `recomp-work` and captures are not committed.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work
# Task 1: FAST_RESCHED 8/16/32/64
TAG=fastresched1 N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=800 SETTLE=10 PITCH_THRESH=35 \
  PS3RECOMP_C690_GAP=1 \
  ARMS='A=SPURS_PM_FAST_RESCHED=8|B=SPURS_PM_FAST_RESCHED=16|C=SPURS_PM_FAST_RESCHED=32|D=SPURS_PM_FAST_RESCHED=64' \
  BOOT_ARM='SPURS_PM_FAST_RESCHED=8' ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_fastresched1_*.log \
  --marks ab_live_fastresched1_*.marks --seg 10 --drop 2

# Task 2d: native dispatch A/B (rebuild first)
cd build-play && ninja MyGameRecomp && cd ..
TAG=native2 N=5 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=800 SETTLE=10 PITCH_THRESH=35 \
  PS3RECOMP_C690_GAP=1 SPURS_SCHED_TRACE=1 \
  ARMS='A=SPURS_PM_FAST_RESCHED=64;SPURS_PM_NATIVE_DISPATCH=0|B=SPURS_PM_FAST_RESCHED=64;SPURS_PM_NATIVE_DISPATCH=6' \
  BOOT_ARM='SPURS_PM_FAST_RESCHED=64;SPURS_PM_NATIVE_DISPATCH=0' \
  ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_native2_*.log \
  --marks ab_live_native2_*.marks --seg 10 --drop 2
```

The per-arm gap window comes from the `[c690-gap-window]` lines in the log
(global, 2 s); the per-lane split needs the timeline recipe in
[`pin-and-burst-path.md`](pin-and-burst-path.md) §7.

## 8. Caveats

* **One launch per A/B, one scene.** The meter is the standard parked scene;
  the multi-scene checks remain the user's windowed runs.
* **`native2` segment 9** has a present stall in B (std 49.26 ms, 31.62 fps).
  The pooled B still wins on every metric; excluding it, the win is larger.
  `fastresched1` arm B segment 5 is a smaller scene/present outlier (lane busy
  0.473); it does not change the D=64 decision.
* **The staged trigger did not fire** (`nstage=0`): the PM stores before
  exiting. The A/B therefore measures the fairness-gated ready/signal mode,
  not a direct job-list hand-off. A future PM that preloads across the exit
  would exercise the staged path; the unit test covers that shape.
* **`PS3RECOMP_C690_GAP=1` was on in both arms** (and in task 1). It costs a
  few percent of lane samples; the comparison is valid because both arms carry
  it, but the absolute fps is below a clean `C690_GAP=0` run.
* **The native key is off by default**, per the mission. The reported win is
  with `SPURS_PM_FAST_RESCHED=64` + `SPURS_PM_NATIVE_DISPATCH=6`.
* **The `nextLoadJobNum` word** reads `0xFFFFFFF1` at the exit diagnostic; the
  three job numbers are `0xFFFFFFFF`. The staged predicate uses the three job
  numbers, so the anomaly does not affect the decision. It is a diagnostic
  oddity worth resolving before a direct job-list dispatcher is attempted.
* **Per-lane gap is not in the live window.** The 2 s line is global; the
  per-lane split in §3 comes from the existing timelines.

## 9. Clean room

This is our own reverse engineering of the title's WWS job list and policy
module, our own EBOOT (Ghidra), our own lifted SPU code and our own trace
stream. The stage/corpus method follows NHL Legacy Native (MIT, (c) 2026
HeckYouMan) with attribution; no NHL code was copied. No RPCS3 or other GPL
emulator source was read or cited.
