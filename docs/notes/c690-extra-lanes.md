# c690 extra host lanes: the guest's maxContention is the ceiling, and the burst does not move

Date: 2026-11-05. Branch `exp/c690-lanes` in the new worktree
`ps3recomp-lanes` (from `exp/live-ab`, with the parked WIP `27f49913`
cherry-picked and fixed), build `recomp-work/build-lanes`. Code pushed to
`fork` only; `PS3RECOMP_C690_EXTRA_LANES` stays **default 0**. One pitch-meter
launch (the cap), three arms (0 / 2 / 4 hidden host lanes), 4 x 12 s per arm,
plus a 24 s `PS3RECOMP_TIMELINE` window covering the first A/B/C rotation.

This follows [`c690-serialisation.md`](c690-serialisation.md) (3.82-4.00 of 5
lanes concurrently, ~100-110 ms of lane time, last job +29-32 ms),
[`meter-p50-pin.md`](meter-p50-pin.md) (the 34.15 ms frame is the c690
producer), [`c690-native-dispatch.md`](c690-native-dispatch.md) (native
dispatch and the fairness gate), [`spu-codegen-audit.md`](spu-codegen-audit.md)
(the per-job compute is the remaining lever) and
[`targets-and-backlog.md`](../targets-and-backlog.md) (speed option (d)).

## TL;DR

* **The guest's `maxContention=5` is a hard ceiling on mesh concurrency, and
  the five physical lanes already provide it.** A hidden worker may only run a
  mesh workload when a physical row for that same workload is not already in
  use; with five lanes the total is still five, so the extra workers can only
  *replace a lane lost to another workload*, not add a sixth mesh instance.
* **Mechanically the implementation works and is live-switchable.** The helper
  threads run the same lifted `wwsjob` PM with the same per-wid job list, the
  same per-row state and the same lock-line bookkeeping; guest-visible
  `active`/`wklCurContention`/`ready`/`idle` are unchanged, and every
  `(wid, spu_num)` row has at most one PM instance. The counters show the
  helper doing ~2.5 k c690 jobs/s in the key-on arms.
* **The measured effect is zero (negative, if anything).** fps 36.60 (0) /
  36.25 (2) / 36.45 (4); p50 **34.15 ms in all three arms**; p90
  38.46 / 38.35 / 38.25; p99 42.24 / 42.10 / 41.88. In the 24 s timeline the
  per-frame c690 lanes-average is 4.04 / 4.05 / 4.25, the last job p50 is
  **32.59 / 32.29 / 32.99 ms** (B -0.30 ms, C +0.40 ms), and the lane union
  p50 is 32.26 / 32.01 / 32.41 ms. That is inside the run-to-run noise.
* **Why the helper does not add a lane:** with key on, worker 0's c690 rate
  drops from ~3,028 to ~1,945 jobs/s while a new worker appears at ~2,579
  jobs/s; the aggregate is ~15.3-15.9 k jobs/s in every arm. The helper takes
  the work the physical lane lost while another workload used its row; it does
  not shorten the frame because that other workload is still on the frame's
  chain.
* **The lane-6 wedge is a guest deadlock, not a host crash.** `WwsJobList_
  SpinUntilDone` waits for a completed *prefix* of the job list: it takes the
  minimum of six 16-bit per-lane cursors (`+0x42..+0x4c`) and the list counter
  (`+0x4e`) and waits for `numJobs` (plus chain lengths). The mesh stage
  initialises only lane 0's cursor to 0; lanes 1-5 start at `0xffff`
  ("not participating"). The title's MeshJobs0/1 say `maxContention=5` and
  `priority[5]=0`, i.e. the workload contract is five concurrent streams even
  though the instance has six rows. `SPURS_LANE6` made a sixth stream active
  (and raised the guest-visible cap); the wait then stopped completing (the
  flip counter stopped, the PPU parked, and the FIFO later hit a bad word the
  resync could only rescue once). The safe invariant is therefore **total mesh
  instances <= `maxContention`, with one PM context per `(wid, spu_num)`**.
* **Remaining levers are unchanged:** per-job compute/codegen, or a
  host-side dispatcher that fills *intra-lane* gaps (a different, larger
  design). More host workers behind the 5-stream contract cannot shrink the
  burst.

## 1. What the guest's job model requires

The evidence is our own EBOOT (Ghidra) and our own lifted PM; no external
emulator source was read.

* `WwsJobList_SpinUntilDone` (`0x009fb898`) and its polling form
  `FUN_009fad08` take `min(counters +0x42,+0x44,...,+0x4c, list counter
  +0x4e)` and wait until it reaches `numJobs` (`+0x40`), adding the chain
  lengths of the last job. The six counters are per-lane **progress cursors**
  over the ordered job array: the minimum is the completed prefix.
* `FUN_009fb1f8` (called by `WwsJobList` init `0x009fc088`) publishes
  `+0x40=0`, `+0x42=0`, `+0x44..+0x4e=0xffff`; `FUN_009facb8` resets the same
  after a completed list. Only lane 0 starts as a live cursor; the PM arms a
  lane's cursor when it starts and advances it with the global job index.
* `FUN_0006f0f8` (the mesh pipeline allocator) creates the two job lists
  ("MeshJobs0/1"), the per-SPU ring bases (`[mesh+0x18+4*i]`,
  `[mesh+0x38+4*i]`, stride `[mesh+0x58]`) and the 0x380000-byte command
  buffer for `i < DAT_0124ad80`. `DAT_0124ad80` is the instance's `nSpus`
  (`cellSpursGetInfo`), i.e. **6** in this title.
* The title then configures MeshJobs0/1 with **`maxContention=5`** and
  **`priority[5]=0`** (`bm_1.log`), so lane 5 is ineligible for the mesh
  workloads even though its row/ring exists. `Mesh_SetRingStartsAndKick`
  publishes the list and `cellSpursReadyCountStore(..., 1)` kicks it.
* `SPURS_LANE6` overrode both checks and counted the sixth lane in
  `active`/`wpCurContention` (so the guest saw a sixth active lane). Within
  ~10 s the PPU was parked in the job-list wait, the flip counter stopped and
  the FIFO produced a self-referential bad word at `0x01420590` before the
  resync. The failure mode is a **guest completion-prefix deadlock**; it is
  not a host thread-limit or a memory crash. We did not isolate which
  sub-invariant (list budget vs ring/command slot) breaks first, so the
  design below simply never creates a sixth stream.

The requirements for hidden workers are therefore:

| requirement | how the design satisfies it |
|---|---|
| job-chain order and list counters | the PM still claims/stores every job; a helper is just another PM re-entry on a row whose same wid is not already running |
| sync/barrier commands | untouched: the PM's own lock-line protocol runs the same |
| list/ring reservations | per `(wid, spu_num)` row; one instance per row |
| atomic reservations on shared counters | guest counters advance exactly as today; hidden lanes use the same per-row cursor |
| guarding/fairness | per-wid total <= `maxContention`; helpers do not consume a physical lane from another workload |
| `maxContention` semantics | never exceeded (5); the guest-visible contention byte is not written by a helper |
| per-lane LS state | the resident PM context is per `(wid, spu_num)`, so two threads never enter it concurrently |
| PM's view of idle SPUs | `active`, `ready`, `idle` and `curcont` are only updated by the normal path; a helper leaves them alone |

## 2. The design

The WIP's shape is kept: `SPURS_EXTRA_LANES_MAX = 4` helper slots per
instance, created once at scheduler start and parked on the condition
variable. A live control-file key `PS3RECOMP_C690_EXTRA_LANES` (0-4, default
0) says how many may hold a job. In the coordinator, after the physical lanes
have taken what they can, a helper slot may be dispatched when all of these
hold:

1. the key names it (`extra_index < limit`) and the native-dispatch key names
   the workload (`SPURS_PM_NATIVE_DISPATCH`, play default 6 for wids 1/2);
2. the workload is the c690 mesh protocol: runnable, enabled, in use, not
   shutting down, `maxContention == 5`, and it has ready work or a signal;
3. `active[wid] + extra_active[wid] < maxContention` (5);
4. a physical row `d < nSpus` has priority > 0 and `lane_wid[d]` does **not**
   already contain this `wid`.

It consumes exactly one ready unit (or the signal bit) under the same
lock-line critical section as the coordinator, sets the helper slot's
`spu_num = d`, records `lane_wid[d] |= wid`, increments `extra_active[wid]`,
and wakes the slot. The worker then calls `spu_run_policy_module` exactly like
a physical lane. On exit it clears its bit and `extra_active` and never writes
`curcont`.

The WIP needed four fixes to be safe, all in `libs/spurs/cellSpurs.c`:

* **Exclusive rows across both paths.** The normal coordinator now skips a
  row whose `lane_wid` already names the same wid (a helper owns it); the
  helper already skipped it. Without this, a normal dispatch could enter the
  same `(wid, spu_num)` resident PM context and job-list row as a helper.
* **Wide enough lane masks.** `lane_wid` is `u16`, not `u8`: wids 8-15
  aliased wids 0-7 in the WIP's 8-bit mask.
* **Deterministic live retirement.** Each helper has an `extra_index`; after
  every PM run it re-checks the live key and exits when the key no longer
  names its index. Switching the key to 0 parks all helpers after the job in
  flight, which is what makes the in-run A/B meaningful.
* **Stop/teardown.** `cellSpursShutdown`/`Finalize` now treats
  `extra_active` as busy and joins `nspus + extra_count` threads (the WIP
  would have stopped while a helper was running). The helper threads are
  created up front so the key can be raised live; with the key 0 they are
  dormant and dispatch nothing.

No `spu_context` field was added. The key is appended at the end of the live
control table (enum and name array).

## 3. Offline gates

| gate | result |
|---|---|
| `tools/test_spurs_scheduler.py` (existing cases + new) | **PASS** |
| hidden-lane semantics (free row, one ready unit, guest `active`/`curcont` untouched, cap 5, key 0 parks) | **PASS** |
| N-worker case: 4-lane workload squats rows 0-3, then 5 mesh instances (1 physical + 4 hidden) on rows 0-4, **no duplicate `(wid,row)`**, guest `curcont=1`, `extra_active=4` | **PASS** |
| scheduler fixture under ASan+UBSan (`detect_leaks=1`) | **0 errors, 0 leaks** |
| scheduler fixture under TSan | 5 reports, all `finish` in the fixture writing manufactured guest state with a plain store while the coordinator reads it lock-free; **none in the helper dispatch/worker path**. This is the fixture's guest-memory model, not a new race (the same pattern is in the pre-existing consumer/held cases). |
| `tools/test_video_fixtures.py` | **33/33** on a quiet machine (`test_gcm_tick_in_drain` failed once under load, the documented flake; passes on rerun) |
| `check_spu_context_abi.sh` | **PASS** (shared field offsets agree; no `spu_context` change) |
| `ninja -C recomp-work/build-lanes MyGameRecomp` | builds |

The N-worker fixture is the gate that matters for the job model: it starts a
four-lane squatter (a DXT-like workload) so the mesh workload has only one
physical row, then lets the helpers take four more. Every concurrently
running `(wid, lane)` pair is unique, and the guest-visible contention byte
stays at 1.

## 4. Live pitch-meter A/B

`BUILD=build-lanes`, `METER=1 SKIP_CINE=1 WCB=2 PIN=0`,
`VIDEODRIVER=x11`, `PITCH_THRESH=35`, `SETTLE=15`, one X, governor
`performance`. `N=4`, `SEG=12`, `DROP=2`, arms rotated
A=`...EXTRA_LANES=0`, B=`...=2`, C=`...=4`; boot arm A. The run completed all
12 segments (07:14:51-07:17:17, ~146 s of measured windows), rc 0, no `STOP`,
no flip gap.

| arm | frames | fps | p50 | p90 | p99 | mean | walker | wait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A (0) | 1,464 | **36.60** | **34.15** | 38.46 | 42.24 | 27.36 | 31.02 | 13.50 |
| B (2) | 1,450 | 36.25 | **34.15** | 38.35 | 42.10 | 27.62 | 28.68 | 13.61 |
| C (4) | 1,458 | 36.45 | **34.15** | 38.25 | 41.88 | 27.44 | 23.19 | 14.94 |

The p90/p99 differences are ~0.2-0.4 ms and do not survive the known ~0.4 fps
run-to-run drift; the p50 is exact in every paired segment (34.14-34.15).
Per-segment fps: A 38.00/36.80/35.80/35.80, B 37.10/34.90/35.20/37.80,
C 35.30/35.10/38.40/37.00.

`[c690-stats]` per-worker c690 jobs/s (mean over the arm's segments):

| arm | w0 | w1 | w2 | w3 | w4 | helper (w6) |
|---|---:|---:|---:|---:|---:|---:|
| A (0) | 3,028 | 3,073 | 3,103 | 3,038 | 3,063 | 49* |
| B (2) | **1,945** | 2,829 | 2,832 | 2,624 | 2,947 | **2,580** |
| C (4) | **1,955** | 2,852 | 2,834 | 2,594 | 2,883 | **2,468** |

`*` transition-only work while the previous arm's helper finished its last
job. The aggregate is 15.3 / 15.7 / 15.9 k jobs/s: the helper replaces the
jobs physical row 0 stops doing in the key-on arms, it does not add a sixth
concurrent stream. Only the first helper ever runs because the cap allows at
most one when four physical lanes are already on the workload.

### 4.1 Timeline: lanes and the burst end

The 24 s timeline (`PS3RECOMP_TIMELINE`, `HITCH_US=0`, 64 MiB ring) was
triggered ~1 s after the first A switch, so its windows are approximately
A (first ~10 s), B (next 12 s), C (last ~2 s). Frame boundary is
`gfx.frame_end` (905 frames); c690 spans are event 33.

| window | frames | lanes avg | peak avg | peak p50 | union p50 | last job p50 | last job p90 | lane-ms/frame | jobs/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A (0) | 379 | 4.04 | 4.04 | 5 | 32.26 | **32.59** | 34.46 | 79.3 | 399 |
| B (2) | 448 | 4.05 | 3.71 | 5 | 32.01 | **32.29** | 34.23 | 81.6 | 409 |
| C (4) | 75 | 4.25 | 3.93 | 5 | 32.41 | **32.99** | 34.64 | 80.5 | 398 |

`lanes avg` is the number of distinct host threads that ran any c690 job in
the frame; `peak avg` is the mean of the per-frame maximum number of
overlapping c690 spans. The helper adds a sixth *thread* to some frames but
the overlapping mesh work stays at the same 4-of-5 level, consistent with the
per-worker rate table.

### 4.2 Thumbnails

2 s thumbnails (grey 160x90 mean-abs-diff): within-arm consecutive A 1.04,
B 1.21, C 0.96; A/A matching-segment 2.76; cross-arm transitions 0.83-2.77.
The differences are the static scene's own crowd/camera noise, not an Edge
output change (the helper runs the same PM/job code and writes the same
per-row ring bytes). No black/garbage frame, no visual divergence.

## 5. What limits the burst

* **The 5-stream guest contract, not the host thread count.** The workload
  already keeps ~4.0 of 5 physical lanes busy inside the burst; the missing
  fraction is per-lane job gaps and coordinator/native-dispatch turnover, not
  a host worker shortage. A helper may not use a row whose same wid is still
  active, so it cannot fill an intra-lane gap; it only starts when a physical
  row is busy with another workload.
* **When it does start, it steals rather than adds.** w0 drops by ~1,080
  jobs/s and the helper adds ~2,580; the extra throughput is ~2 %, and the
  frame does not move because the workload that owned the row is still on the
  frame's chain.
* **Going above `maxContention` is the lane-6 wedge.** The EBOOT wait can
  address six cursors, but the title only submits/accounts for five; the
  sixth stream deadlocks the producer. Any design that wants >5 must first
  solve that guest contract (different job accounting / ring budget), which
  is not a small change.
* **The remaining lever is the same as before:** cut per-job compute
  (codegen / native whole commands) or fill intra-lane gaps with a host-side
  job stealer that owns the PM's claim/store path. The extra-lane experiment
  answers speed option (d) in the negative for the "more workers behind the
  same 5 lanes" design.

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-play
git worktree add ../ps3recomp-lanes -b exp/c690-lanes exp/live-ab
cd ../ps3recomp-lanes
git cherry-pick 27f49913          # the parked WIP, then apply this doc's fixes
# build from a copy of build-play's cache, retargeted at the new tree
cp -a ../recomp-work/build-play ../recomp-work/build-lanes
cmake -S templates/project -B ../recomp-work/build-lanes \
  -DPS3RECOMP_DIR=$PWD -DSPU_DIR=../recomp-work/spu_lifted
ninja -C ../recomp-work/build-lanes -j4 MyGameRecomp

python3 tools/test_spurs_scheduler.py
SDK=$PWD BUILD_CACHE=../recomp-work/build-lanes/CMakeCache.txt \
  ../recomp-work/ab-tools/check_spu_context_abi.sh
python3 tools/test_video_fixtures.py
```

Live A/B (one launch):

```sh
cd ../recomp-work/ab-tools
TAG=lanes1 BUILD=build-lanes N=4 SEG=12 DROP=2 METER=1 SKIP_CINE=1 \
  WCB=2 PIN=0 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=15 PITCH_THRESH=35 \
  PS3RECOMP_C690_STATS=1 \
  PS3RECOMP_TIMELINE=../recomp-work/captures/lanes1/lanes1_tl.bin \
  PS3RECOMP_TIMELINE_HITCH_US=0 PS3RECOMP_TIMELINE_TRIGGER=/tmp/lanes1_tl.go \
  PS3RECOMP_TIMELINE_SECS=24 PS3RECOMP_TIMELINE_RING_MB=64 \
  PS3RECOMP_TIMELINE_FLUSH_MB=16 \
  ARMS='A=PS3RECOMP_C690_EXTRA_LANES=0|B=PS3RECOMP_C690_EXTRA_LANES=2|C=PS3RECOMP_C690_EXTRA_LANES=4' \
  BOOT_ARM='PS3RECOMP_C690_EXTRA_LANES=0' ./ab_live.sh
python3 pin_ab.py --log ../ab_live_lanes1_*.log --marks ../ab_live_lanes1_*.marks \
  --seg 12 --drop 2
```

The 199 MB timeline and the run folder are under `recomp-work/captures/` and
the usual `ab_live_*` names; they are not committed.

## 7. Caveats and deviations

* **One launch, three arms.** The A/B is 4 x 12 s per arm; the p50 is exact
  in every segment, but the fps/p90/p99 differences are near the noise floor.
  The timeline covers only the first rotation (A ~10 s, B 12 s, C ~2 s), so
  its C window is short.
* **The lane-6 failure is characterised, not fully isolated.** We prove the
  wait's six-cursor/prefix form and the five-stream title configuration, and
  we observed the guest deadlock/FIFO bad word. We did not pin down whether
  the list budget or the command-slot/ring budget breaks first; the safe
  design avoids both by never exceeding `maxContention`.
* The helper threads are created at instance start even when the key is 0.
  They are dormant (condition variable) and dispatch nothing; this is what
  lets an in-run A/B raise the count without restarting. The key remains
  default 0.
* Native dispatch is a prerequisite: `SPURS_PM_NATIVE_DISPATCH` must name the
  mesh wids (the play default is 6). With it off, a helper runs one PM quantum
  and returns to the coordinator, which still works but adds the round trips
  the native path removed.
* TSan's five reports are the fixture's direct guest-state stores (the same
  pattern as the pre-existing `consumer`/`held_policy` cases), not the helper
  code; ASan/UBSan are clean.

## 8. Clean room

This is our own static analysis of our own EBOOT (Ghidra), our own lifted
`wwsjob_pm` and c690 module, our own SPURS runtime, our own trace stream and
captures. The stage/corpus/differential method follows NHL Legacy Native
(MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied. No
RPCS3 or other GPL emulator source was read or cited.
