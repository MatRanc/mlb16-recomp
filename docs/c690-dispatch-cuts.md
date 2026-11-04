# c690 per-job dispatch turnover: what the gap is, three cuts, and why they lose

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`, code commit `84e5dd06`. One meter launch
(`ab_live_dispatch1_20261104_025208`): `METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`,
`PIN=0 WCB=2`, x11, governor `performance`, N=6 x 10 s per arm, 2 s dropped.
This follows [`pin-and-burst-path.md`](pin-and-burst-path.md),
[`c690-job-gaps.md`](c690-job-gaps.md),
[`c690-job-gaps-ab.md`](c690-job-gaps-ab.md),
[`c690-profile.md`](c690-profile.md) and
[`boot-spu-hitches.md`](boot-spu-hitches.md).

## TL;DR

* **The gap split in this build is still tail + host + head.** Big bursts in
  arm A: 34.25 ms wall, 70.3 % lane occupancy, 29.4 % of lane-wall in
  inter-job gaps, split **28.5 % tail / 46.8 % host / 24.7 % head**. The host
  part is the SPURS worker -> coordinator -> worker round trip.
* **The policy module is lifted, not interpreted.** Image 6 (`wwsjob_pm`,
  12,268 B) runs as generated C on the `spurs pm` workers; the per-job head and
  tail are PM/runtime code, and no wait event or single runtime helper covers
  them. The 67 KiB job-module re-DMA is real but tiny: ~0.9 GB/s across five
  lanes, a few microseconds per job per lane, and the memo only removes the
  host copy/verify, not the engine's GET. That is why `SPU_CODE_DMA_MEMO` was
  neutral.
* **Three exact, default-off cuts were implemented and unit-tested:**
  `SPURS_PM_FAST_ACTIVE` (a lane already counted may rerun at maxContention),
  `SPURS_PM_FAST_ONE_LOCK` (check and consume under one lock-line
  acquisition), `SPURS_PM_FAST_GEN` (trust the dispatch-time workload snapshot
  while the registry epoch is unchanged).
* **Live A/B: the bundle cuts the turnover, but loses frames.** The fast-rerun
  share rises 34.5 % -> 85.5 %. In the timeline window the host phase falls
  **23.54 -> 9.19 ms per burst (-61 %)**, all-lane gap share 29.4 -> 22.6 %,
  lane occupancy 70.3 -> 76.6 %, and burst wall **34.25 -> 30.99 ms
  (-9.5 %)**. But guest fps is **35.30 -> 33.29** (paired A-B
  **+2.00 +/- 1.52 fps**, 95 % CI [0.48, 3.53]) and walker wait rises
  11.32 -> 12.26 ms. p50 frame time is identical (34.15 ms, the two-vsync
  present/hand-off floor).
* **Why:** at maxContention the coordinator round trip is not pure overhead.
  It is the arbitration point for the seven workloads (MeshJobs0/1, DXT,
  Crowd, synth, MP3, Kinematics) over six lanes. Keeping a lane in a fast
  rerun loop for up to eight quanta compresses the c690 burst but delays the
  other workloads the frame also needs. The burst gets faster; the frame does
  not.
* **Keeping nothing on.** All three keys stay default off in both
  `play_windowed.sh` copies and `ab_live.sh`; `SPURS_PM_FAST_RESCHED=8`
  remains as before.
* **Sixth lane: no safe version.** The title sets `nSpus=6`,
  `MeshJobs0/1 maxContention=5` and `priority[5]=0`; the five mesh instances
  are a guest protocol, not a runtime limit. The prior live probe of
  `SPURS_LANE6=1` wedged the guest in ~10 s. It also takes lane 5 from DXT and
  the audio/synth workloads. C was not run: B has no winner and the lane-6
  wedge is already a documented hard failure.

## 1. Where the 25 us/job goes in our runtime

The gap is measured exactly as in `pin-and-burst-path.md` section 3: policy
spans (`ev 4`) and c690 job spans (`ev 33`) per `spurs pm` thread, paired on
the same lane, bursts split at a 1 ms union gap. This run's timeline is 95 s
(25.4 M events), covering the first ten measured segments (five per arm).

| big bursts (>=20 ms) | A (today) | B (three cuts) |
|---|---:|---:|
| bursts | 766 | 710 |
| wall mean / p50 ms | 34.25 / 34.15 | **30.99 / 30.96** |
| lane occupancy | 70.3 % | **76.6 %** |
| jobs per burst | 678 | 676 |
| per-lane busy ms/burst | 24.07 | 23.75 |
| inter-job gap, % of lane-wall | 29.4 % | **22.6 %** |
| tail / host / head share | 28.5 / **46.8** / 24.7 % | 40.3 / **26.2** / 33.4 % |
| tail / host / head ms per burst (all lanes) | 14.32 / **23.54** / 12.42 | 14.14 / **9.19** / 11.72 |
| serial tail (<=1 lane busy) | 0.034 ms | 0.031 ms |

The absolute host phase falls **61 %** and the all-lane gap falls 35 %; the
tail and head barely move (-1 % and -6 %). That is the signature of removing
the worker/coordinator hand-off, not job compute or the PM's own staging.

What each candidate part is, in our code:

| part | evidence | verdict |
|---|---|---|
| policy module between jobs | image 6 is the lifted `wwsjob_pm` (12,268 B); lanes are 47 % job code, 3 % PM in the existing leaf profile; no interpreter spans | PM tail/head is generated C + runtime, not a lock or a syscall |
| job descriptor DMA | ~1.1 KiB input per job (`c690-profile.md`); normal MFC GET | small |
| 67 KiB job code re-DMA | image 204 is 67,648 B; `code_get` 70 KiB/job in the old window; our path does a synchronous `memcpy` plus a 67 KiB overlay verify | ~0.9 GB/s over five lanes, ~2-7 us/job/lane; <2 % of lane time |
| overlay verify | `spu_overlay_resident_verified()` (`runtime/spu/spu_channels.c:2379`) is a `memcmp` of the verified spans; the memo (`:2420`) skips copy+verify when the source is unchanged | removed by `SPU_CODE_DMA_MEMO`, measured flat: the copy is cache-resident and does not touch the host arbitration |
| ready-queue locking / atomics | GETLLAR/PUTLLC ~2.5 per gap with 7 % retries; lock-line spans are 87 ms in 26.4 s of old gap time | 0.3 % of gap time; not the 25 us |
| syscalls / sleeps / yields | worker sleeps on the instance CV when idle; coordinator does a 1 ms timed CV wait when not kicked | inside a burst the CV round trip **is** the host phase |
| cache misses | prior flat profile: c690 job functions 47 %, PM 3 %, rest spread over dispatch/runtime | no single helper dominates |

The previous `SPU_CODE_DMA_MEMO` arm is the control: it removed ~97 % of
overlay re-loads with `ovl_bad=0` but was flat on frame time. The copy is
~0.9 GB/s, cache-resident and overlapped across lanes; it cannot buy the 5 ms
host phase.

## 2. The three cuts

All are in `libs/spurs/cellSpurs.c` and are live control-file keys
(`libs/video/ps3_live_ctl.h:75-77`), default off. `SPURS_PM_FAST_RESCHED=8`
(the existing bound) is unchanged.

### 2.1 `SPURS_PM_FAST_ACTIVE`

The old fast-rerun test refused any rerun when `si->active[wid] >=
maxContention`. That is exactly the steady state when a workload has all of
its lanes (the c690 mesh workloads have `maxContention=5`). A rerun on a lane
that is already counted in `active[wid]` adds no lane, so only a demand the
guest has lowered below the active set should refuse it
(`spurs_pm_fast_static_ok`, `cellSpurs.c:1890`). The ready-level boundary is
relaxed the same way (`spurs_pm_fast_lane_within_level`): the lane is already
in the count, so `active <= ready + idle` is the equivalent admission test.

### 2.2 `SPURS_PM_FAST_ONE_LOCK`

The split path read ready/signal/idle under the lock line, unlocked, then took
it again to consume. Check and consume in one critical section exactly like
the coordinator does (`spurs_pm_fast_try_rerun`, `cellSpurs.c:1996`). This
removes one lock-line acquisition per rerun and the window where the two reads
could disagree.

### 2.3 `SPURS_PM_FAST_GEN`

The coordinator snapshots `pm/sizePm/spurs_ea/data` under
`s_workload_registry_lock` at dispatch. Every add, remove, shutdown and retire
bumps a release/acquire epoch under the same lock. While the epoch matches,
the worker uses its dispatch-time snapshot instead of taking that lock again
(`spurs_pm_fast_static_ok`, `cellSpurs.c:1890`). Guest fields that can change
without an epoch bump (`maxContention`, priorities, ready/signal/idle) are
still read every time.

Exactness: all three mirror the coordinator's bookkeeping byte-for-byte
(consume one ready unit or clear the signal bit, refresh
`wklCurrentContention`, reset idle). The only guest-visible difference is
*when* the lane yields to the coordinator, which section 3 shows matters.

Unit tests (`tests/test_spurs_scheduler.c`, all pass): fast consume takes
exactly one lock-line lock/unlock; every `can_rerun` early return leaves the
line unlocked; an already-active lane at maxContention reruns only with
`FAST_ACTIVE`; one-lock check+consume; the epoch path trusts the snapshot
until a bump and then sees the removal. `SPURS_LANE6` also got an env fallback
and a wid/lane restriction test. `check_spu_context_abi.sh` PASS (no shared
`spu_context` fields changed).

## 3. Live A/B

One launch, meter confirmed ~157 s after start, timeline armed 2 s after the
meter for 95 s (775 MiB, 25.4 M events; deleted after analysis), then 12
alternating segments. A = all three keys 0, B = all three keys 1. No C arm
(section 4). Both arms carried `PS3_LWCOND_STATS=1` and
`SPURS_SCHED_TRACE=1`; the live-ctl/marks self-check passed for all 12
segments, no `[pm-end]`, no resync, no wedge.

### 3.1 Frames (6 segments per arm, 8 s measured each)

| arm | fps ([flip]) | p50 ms | p90 ms | p99 ms | std ms* | mean ms | walker ms | wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A today | **35.30 +/- 1.49** | 34.15 | 38.84 | 42.80 | 37.93 | 29.24 | **16.47** | **11.32** |
| B cuts | 33.29 +/- 1.29 | 34.15 | **38.37** | 43.43 | 64.40 | 31.87 | 17.36 | 12.26 |

\* pooled std is inflated by two late present stalls (segments 10-11, 100.9 and
159.7 ms std); p50/p90/p99 are unaffected. Paired per-segment difference
**A-B = +2.00 +/- 1.52 fps**, 95 % CI **[+0.48, +3.53]**. B loses every pair.

2 ms frame-time histogram (all measured intervals): A has **25.6 % <=16 ms**
vs B **21.4 %**; both have their mode at **34-36 ms (42.2 % / 42.8 %)**; B
shifts the 36-38 ms bin up (8.9 -> 13.5 %). The two-vsync 34.15 ms floor does
not move.

Deadline frames (`PS3_LWCOND_STATS`, sum of the measured windows):
**A 670, B 581** - the only frame-level metric that favours B.

### 3.2 c690 throughput did not change

Whole-run `[c690-stats]` deltas: **A 14,622 jobs/s, B 14,826** (+1.4 %);
charged job wall **2.589 vs 2.591 Ms/s**; mean job **177.0 vs 174.7 us**.
There is no duplicate execution and no extra guest work; the cuts move *when*
the work runs, not how much.

The fast/coordinator split from `SPURS_SCHED_TRACE` (per measured window):

| arm | fast reruns | coordinator dispatches | fast share |
|---|---:|---:|---:|
| A | 266,148 | 505,910 | 34.5 % |
| B | 665,811 | 112,856 | **85.5 %** |

So the bundle does exactly what it was built to do: it removes two-thirds of
the host round trips at maxContention. The lane timeline in section 1 shows
the same: host 23.54 -> 9.19 ms per burst.

### 3.3 Why the burst win is a frame loss

The long meter frame is pinned at the **34.15 ms two-vsync present/hand-off
floor**, not at the burst. A big burst is 34.25 ms in A and 30.99 ms in B, yet
the p50 frame is 34.15 ms in both. Shortening the burst below the floor buys
slack inside the frame, not another frame. The fps difference comes from the
frame *distribution* (B has fewer sub-16 ms frames) and from the hand-off
getting slower: walker wait +0.94 ms, walker engine +0.91 ms, draw +0.34 ms.

The coherent explanation is cross-workload arbitration. The instance has seven
workloads on six lanes. At maxContention the coordinator round trip is where
the kernel decides whether this lane keeps the same workload or hands the slot
to another one. A fast rerun keeps the lane for up to eight quanta. That
compresses the c690 burst (fewer, denser lanes, occupancy 70.3 -> 76.6 %) but
delays the other workloads - DXT, Crowd, synth, MP3, Kinematics - that the
same frame needs before the PPU can finish it. The c690 work finishes sooner;
the frame finishes later.

This is also why the earlier `SPURS_PM_FAST_RESCHED` win and this loss are not
contradictory: the old test fired only when `active < maxContention`, i.e. when
there was no contention to arbitrate. `FAST_ACTIVE` fires at the contended
steady state, which is the case the coordinator exists for.

The next cut, if this is revisited, is a *fairness-aware* rerun: allow it at
maxContention only while no other enabled workload has ready work or a signal
for a lane. That keeps the cheap case and yields at the arbitration point.
It is not implemented or measured here.

## 4. The sixth lane

The limit is the title's configuration, not the runtime:

* `recomp-work/bm_1.log` / this log: `Initialize "" ea=0x01243E00 nSpus=6`.
* `MeshJobs0` and `MeshJobs1` (wids 1 and 2): `maxContention=5`, priority
  table `04 04 04 04 04 00` - **lane 5 has priority 0**.
* Lane 5 is also the lane for `DXTJob` (`maxC=4`), `CrowdJobList` (`maxC=5`)
  and the `maxC=1` workloads (`nextsynthJobLst`, `LibMP3 JobList`,
  `KinematicsJobs`). It is not spare capacity.

Overriding both checks for wids 1/2 is the `SPURS_LANE6` live key (kept,
default off). It is not safe: the guest's mesh job-list/ring protocol assumes
five concurrent mesh instances. The live probe in `c690-profile.md` section
3.2 ran A normally at 28.67 fps, then the flip counter stopped within ~10 s
after the key was enabled and `ab_live.sh` exited rc=7. A sixth concurrent
MeshJobs instance corrupts the protocol; it would also take a lane from DXT
(the boot-hitch workload) and from audio/synth.

There is therefore no guest-transparent sixth lane to measure. C was not run:
B has no winner, and repeating a documented hard wedge would burn the second
launch for a known failure. The key remains as a probe only.

## 5. What was kept

* `libs/spurs/cellSpurs.c` and the live keys (commit `84e5dd06`), default off:
  `SPURS_PM_FAST_ACTIVE`, `SPURS_PM_FAST_ONE_LOCK`, `SPURS_PM_FAST_GEN`.
* Tests: `test_spurs_scheduler` (fast active/one-lock/epoch, lane-6
  restriction), DXT-capture stubs in the API-limits, task-signal and teardown
  fixtures (they compile `spu_channels.c`/`spurs_policy.c` directly).
* **Nothing enabled by default.** Both `play_windowed.sh` copies and
  `ab_live.sh` are unchanged; `SPURS_PM_FAST_RESCHED=8` stays on as before.
* The 775 MiB timeline was deleted after analysis.

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work
rm -f /tmp/dispatch1_tl.bin /tmp/dispatch1_tl.go
( for i in $(seq 1 900); do f=$(ls -t ab_live_dispatch1_*.log 2>/dev/null | head -1);
    [ -n "$f" ] && grep -q "pitch meter confirmed" "$f" && { sleep 2; touch /tmp/dispatch1_tl.go; break; }
    sleep 1; done ) &
TAG=dispatch1 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 VIDEODRIVER=x11 \
  REPORT=1 SETTLE=10 PITCH_THRESH=35 LIMIT=800 \
  ARMS='A=SPURS_PM_FAST_ACTIVE=0;SPURS_PM_FAST_ONE_LOCK=0;SPURS_PM_FAST_GEN=0|B=SPURS_PM_FAST_ACTIVE=1;SPURS_PM_FAST_ONE_LOCK=1;SPURS_PM_FAST_GEN=1' \
  BOOT_ARM='SPURS_PM_FAST_ACTIVE=0;SPURS_PM_FAST_ONE_LOCK=0;SPURS_PM_FAST_GEN=0' \
  PS3RECOMP_TIMELINE=/tmp/dispatch1_tl.bin PS3RECOMP_TIMELINE_TRIGGER=/tmp/dispatch1_tl.go \
  PS3RECOMP_TIMELINE_SECS=95 PS3RECOMP_TIMELINE_RING_MB=64 \
  PS3RECOMP_TIMELINE_FLUSH_MB=16 PS3RECOMP_TIMELINE_HITCH_US=0 \
  PS3_LWCOND_STATS=1 SPURS_SCHED_TRACE=1 ./ab-tools/ab_live.sh
# frames and per-arm rollup
python3 ab-tools/pin_ab.py --log ../ab_live_dispatch1_*.log \
  --marks ../ab_live_dispatch1_*.marks --seg 10 --drop 2
# burst wall and gap phases split by arm (streaming variant of
# analyze_burst.py that only keeps policy/job spans; also hist_arm.py for the
# 2 ms histogram and schedstat.py for the fast/coordinator share)
cd ab_live_dispatch1_20261104_025208
python3 analyze_arms.py /tmp/dispatch1_tl.bin ../ab_live_dispatch1_*.log \
  ../ab_live_dispatch1_*.marks 10 2
python3 hist_arm.py ../ab_live_dispatch1_*.log ../ab_live_dispatch1_*.marks
```

## 7. Caveats

* **One launch, one scene.** The meter is the standard parked scene; the
  multi-scene checks remain the user's windowed runs.
* **The timeline covers segments 0-9** (95 s from meter+2 s), so the burst
  table pools five A and five B segments. The frame table uses all six
  segments per arm from the flip log.
* **Segments 10-11 have large present stalls** (max 2.3 s in the MangoHud
  CSV). They inflate pooled std/mean in both arms; p50/p90/p99 and the
  paired per-segment fps are the robust numbers.
* **The bundle was not split.** The A/B measured all three keys together.
  `ONE_LOCK` and `GEN` do not change when the fast path fires; alone they can
  only shave the fast path's own overhead, and in A that path is 34.5 % of
  dispatches. A follow-up A vs (`ONE_LOCK=1;GEN=1`) is cheap but was not run
  (one launch used, and B had no winner to protect).
* **`SPURS_SCHED_TRACE` and `PS3_LWCOND_STATS` were on in both arms.** The
  trace prints one stderr line per 4096 coordinator scans, so B prints less
  often; the counters are cumulative and the shares above are deltas over the
  same measured windows.
* **No `perf` was taken.** The existing profile plus the timeline phase split
  answer the per-part question without another launch.

## 8. Clean room

This is our own reverse engineering of the title's WWS policy module, the
lifted c690 job, the captured LS/GPR corpus and our runtime's trace stream.
The stage/corpus method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan)
with attribution; no NHL code was copied. No RPCS3 or other GPL emulator
source was read or cited for this work.
