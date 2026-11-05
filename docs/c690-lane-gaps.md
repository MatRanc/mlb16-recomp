# c690 lane gaps: 82 % is the PM's own head/tail, not runtime dispatch

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play` (native c690 dispatch `SPURS_PM_NATIVE_DISPATCH=6`,
`SPURS_PM_FAST_RESCHED=64`). Two pitch-meter launches (the mission cap), X
once, no second press:

* `ab_live_lanegapsA_20261105_080615` - `SPURS_READY_WAKE=0`, 4 x 12 s
  measured (2 s dropped), 24.2 s timeline (`A_tl.bin`).
* `ab_live_lanegapsB_20261105_081004` - `SPURS_READY_WAKE=1`, same
  configuration (`B_tl.bin`).

Both used `METER=1 SKIP_CINE=1 PIN=0 WCB=2 VIDEODRIVER=x11
PITCH_THRESH=35 SETTLE=15`, `PS3RECOMP_C690_STATS=1`, governor
`performance`. The new switch is an environment variable, default **0**
(off); nothing is default-on.

This follows [`c690-extra-lanes.md`](c690-extra-lanes.md) (the lanes are
busy ~4.0 of the 5 the title allows; the missing ~1 lane is intra-lane gaps),
[`c690-job-gaps.md`](c690-job-gaps.md) (the gap phases and the PM exit
subtree), [`c690-native-dispatch.md`](c690-native-dispatch.md) (the native
rerun), [`c690-serialisation.md`](c690-serialisation.md) (the replay is
walker-bound, not Edge-bound), [`meter-p50-pin.md`](meter-p50-pin.md),
[`frame-pacing-floor.md`](frame-pacing-floor.md),
[`spu-codegen-audit.md`](spu-codegen-audit.md) and
[`targets-and-backlog.md`](targets-and-backlog.md).

## TL;DR

* **The ~20 % intra-lane idle is real but it is not mostly our runtime.**
  Inside the burst a lane is idle **5.56 ms of a 29.16 ms burst (19.1 %)**.
  That gap splits per lane per frame as:

  | class | lane-ms/frame | share of gap | what it is |
  |---|---:|---:|---|
  | job tail (job end -> mesh PM end) | **2.519** | 46.2 % | c690 submit epilogue, output PUTs, PM post-job bookkeeping, exit |
  | job head (mesh PM entry -> job entry) | **2.075** | 38.1 % | PM ticket claim, descriptor DMA, code re-DMA, job context (lifted guest PM code) |
  | host dispatch (mesh PM end -> next mesh PM) | **0.599** | 11.0 % | worker -> coordinator/native rerun -> worker |
  | other workload on the same lane | 0.258 | 4.7 % | not idle: a DXT/crowd/synth run between two c690 jobs |
  | **total measured gap** | **5.451** | | |

  **Tail + head = 4.59 lane-ms/frame = 84 % of the gap.** That is the guest
  WWS policy/job code executed by our lift, not a host wait. The runtime
  (host dispatch + DMA copy + lock line) is **~1.0 lane-ms/frame**.
* **Host dispatch is small and distinct from the tail/head.** Its sub-classes
  (per lane per frame): native/fast rerun check **0.311**, coordinator wait
  **0.215**, OS wake after the kick **0.043**, both-kick **0.029**.
  - Coordinator wait p50 **20.1 us**, mean 106 us, p99 1,420 us: the p50 is
    already the coordinator's dispatch cost; the mean/tail is the PPU not
    having published the next list yet (guest dependency / list append), not
    a poll timeout.
  - Signal -> run wake latency **p50 10.1 us, p90 22.5 us, 0.043 lane-ms/frame**.
* **The missing PPU ready-store wake was the only plausible local runtime
  class, so it was fixed and measured.** `SPURS_READY_WAKE=1` makes
  `cellSpursReadyCountStore`/`Swap`/`Cas` and `cellSpursWakeUp` kick the
  coordinator at once (today only an SPU PUTLLC to the instance line does).
  The A/B is **neutral**: fps 36.50 -> 35.88, p50 34.15 both, p90 37.62 ->
  38.18, p99 40.42 -> 41.04; lanes busy 4.05 -> 4.06; coordinator wait
  0.215 -> 0.201 lane-ms/frame (mean 106 -> 101.5 us, p50 20.1 -> 20.3 us).
  The switch stays **off**.
* **The "close 6 ms of runtime" premise is false.** Even deleting every
  runtime piece (host 0.60 + DMA ~0.34 + lock 0.03 lane-ms/frame) could
  shorten a lane's burst by **at most ~1 ms**, not 6 ms; the other 4.6 ms is
  the PM's own per-job head/tail. The p50 stays at 34.15 ms either way (the
  two-vsync producer floor). The remaining lever is per-job lifted code
  (`spu-codegen-audit.md`, 1.2-1.4x) or fewer jobs - not dispatch latency.

## 1. Method

The timeline (`PSTL`, 32-byte records) has the events this needs:

* `spurs pm` threads 2-6 are the five c690 lanes (`spurs kernel` is a
  separate thread; its tid differs between runs, so the analyser finds it by
  name);
* `c690.job` (event 33) is the job span (LS `0x135B8..0x12AB8`);
* `spu.policy` (event 4) is one PM quantum; its `b` field is the workload id,
  so mesh policies (wids 1 and 2) are separated from other workloads;
* `spurs.kick` (event 31) is the instant a workload is admitted to a physical
  lane; `a` is the workload id, `b` is lane | ready<<8. A kick on the lane's
  own `spurs pm` thread is a native/fast rerun; a kick on `spurs kernel` is a
  coordinator dispatch;
* `spu.lockline_retry` (event 11) is the failed-PUTLLC-to-success span; the
  MFC/DMA is synchronous, so there is no DMA wait span (its copy is inside
  the calling PM phase).

A **burst** is one frame's c690 work: for each `gfx.frame_end` interval
(event 41) the union of that frame's job spans. A **gap** is the interval
between two consecutive `c690.job` spans on the same lane; gaps are clipped
to the burst. For each gap:

* **tail** = job end -> the end of the mesh PM span containing it;
* **head** = the start of the next mesh PM span -> the next job entry;
* **host** = mesh PM end -> next mesh PM start, minus any non-mesh workload
  policy spans on that lane;
* **other** = the non-mesh policy spans inside the gap (the lane is busy, not
  idle).

The two runs are independent launches (the switch is an environment variable,
see Deviations). The old `lanes1` capture (`build-lanes`, extra lanes 0) is a
replication of the shape: its A window split 5.14 lane-ms/frame as tail 2.19
/ head 1.76 / host 1.19 raw, i.e. the same tail+head dominance.

## 2. The burst and the lane idle

A run (24.19 s window, 895 frame ends, 621 frames with c690 work):

| metric | value |
|---|---:|
| mean burst (first job -> last job) | **29.16 ms** |
| c690 lane-sum per burst | 118.01 ms (5 lanes) |
| lanes busy inside the burst | **4.05 of 5** |
| per-lane c690 busy | 23.60 ms |
| per-lane c690 idle | **5.56 ms (19.1 %)** |
| frame p50 / mean | 34.15 / 26.80 ms |
| gaps (both endpoints in a burst) | 370,758 |

The 20 % figure in the mission reproduces exactly. The burst mean is 29 ms
rather than 32 ms because the window includes the fast frames as separate
intervals; the p50 frame is still 34.15 ms.

## 3. Where the gap goes (baseline A)

Per lane per frame, and lane-sum per frame (x5). Percentiles are per gap.

| class | lane-ms/frame | lane-sum/frame | mean us | p50 us | p90 us | p99 us |
|---|---:|---:|---:|---:|---:|---:|
| **tail** (job end -> PM end) | 2.519 | 12.60 | 21.1 | 18.8 | 26.6 | 63.0 |
| **head** (PM start -> job) | 2.075 | 10.38 | 17.4 | 15.7 | 21.3 | 53.9 |
| **host** (PM end -> next PM) | 0.599 | 3.00 | 5.0 | 1.1 | 5.4 | 42.9 |
| other workload in gap | 0.258 | 1.29 | 2.2 | 0 | 0 | 0 |
| **gap total** | **5.451** | **27.25** | **45.7** | **36.5** | **53.2** | **168.5** |
| lock line retry | 0.035 | 0.18 | | | | |

Host sub-classes (cleaned of other-work spans; they sum to 0.599):

| host class | lane-ms/frame | mean us | p50 us | p90 us | p99 us |
|---|---:|---:|---:|---:|---:|
| native/fast rerun check (pre-kick) | 0.273 | 2.3 | 1.0 | 2.0 | 25.1 |
| coordinator wait (pre-kick) | 0.215 | 106.0 | 20.1 | 129.7 | 1,419.7 |
| coordinator wake (post-kick) | 0.043 | 21.2 | 10.1 | 22.5 | 263.4 |
| native post-kick | 0.038 | | | | |
| both-kick pre/post | 0.029 | | | | |

Reading the table:

* **Tail and head are the gap.** They are the lifted PM's own code around the
  job: the c690 submit epilogue and its output PUTs on the tail; the WWS
  ticket claim, descriptor/code DMA and job context on the head. The earlier
  exclusive-time profile in [`c690-job-gaps.md`](c690-job-gaps.md) §3.2 put
  66 % of the gap in the PM exit subtree (`0x2D10 -> 0x3630`), which is the
  same finding. With native dispatch the host phase has shrunk from 27.5 us
  p50 (then) to **1.1 us p50**; the head/tail did not move.
* **The coordinator wait is mostly guest dependency.** Its p50 is 20 us.
  The mean is dragged by a 1-3 ms tail: those are the moments the PPU has not
  yet appended the next list. Only ~2 % of gaps take this path (6,304 of
  370,758); native dispatch handles the rest.
* **Wake latency is not a lever.** Signal -> run is 10 us p50 / 22.5 us p90.
* **Lock contention is not a lever.** `spu.lockline_retry` is 0.035
  lane-ms/frame (41,077 retry spans over 24 s); no other lock span is emitted
  on the lanes.
* **DMA is synchronous, so it appears inside the head/tail, not as a wait.**
  From the existing counts (33 GET + 5.9 PUT per gap, 85 + 15.5 KiB) and the
  measured copy rate on this host (70 KiB / 1.18 us, 60 GB/s), the copies are
  ~0.34 lane-ms/frame in total (~0.14 for the once-per-job 67 KiB code re-DMA
  and ~0.2 for the rest). The `SPU_CODE_DMA_MEMO` A/B already showed the code
  copy is not a frame lever; this explains why.

## 4. The fix that was tried: `SPURS_READY_WAKE`

`cellSpursReadyCountStore`, `ReadyCountSwap`/`CompareAndSwap` and
`cellSpursWakeUp` change the instance's request line, but the only path that
kicked the coordinator was an SPU PUTLLC to that line. A PPU publish left the
coordinator to find it on its 1 ms timed pass. The new switch calls
`spurs_line_committed()` on the store (after releasing the API lock). It is a
kick only: the coordinator rescans and dispatches exactly as it does for an
SPU commit, so guest state and ordering are unchanged.

Offline gates:

| gate | result |
|---|---|
| `tools/test_spurs_coordinator_kick.py` | **PASS** (kick off/on/off, plus the old SPU-commit wake) |
| `tools/test_spurs_scheduler.py` | **PASS** |
| clang TSan, coordinator-kick fixture | **0 reports** |
| clang TSan, scheduler fixture | 4 reports, all `finish` in the fixture writing manufactured guest state while the coordinator reads it lock-free (the documented fixture model; none in the new path) |
| `check_spu_context_abi.sh` | **PASS** (no `spu_context` change) |
| `ninja -C recomp-work/build-play MyGameRecomp` | builds |

Live A/B (two launches, same build and scene):

| metric | A (`=0`) | B (`=1`) |
|---|---:|---:|
| frames (4 x 10 s measured) | 1,460 | 1,435 |
| fps | **36.50** | 35.88 |
| p50 ms | **34.15** | **34.15** |
| p90 ms | 37.62 | 38.18 |
| p99 ms | 40.42 | 41.04 |
| mean ms | 27.41 | 27.86 |
| walker / wait ms | 28.75 / 13.56 | 30.33 / 13.76 |
| burst mean ms | 29.16 | 28.69 |
| lanes busy | 4.05 | 4.06 |
| gap lane-ms/frame | 5.451 | 5.259 |
| tail / head lane-ms/frame | 2.519 / 2.075 | 2.454 / 2.008 |
| host lane-ms/frame | 0.599 | 0.563 |
| coordinator wait lane-ms/frame | 0.215 | 0.201 |
| coordinator wait p50 / mean us | 20.1 / 106.0 | 20.3 / 101.5 |
| wake lane-ms/frame | 0.043 | 0.041 |
| lock-line lane-ms/frame | 0.035 | 0.030 |

Nothing moves outside the ~0.4 fps cross-launch drift. The targeted
coordinator wait falls 6.5 % in the mean (within noise) and is unchanged at
p50, which says the coordinator was already being woken promptly; the
residual is the guest not having published work. The switch stays **off**.

## 5. Why the 6 ms runtime premise fails

The mission's arithmetic was: lane idle ~20 % of a 32 ms burst ~6 ms; if that
is runtime, close it. The measurement answers the conditional in the
negative:

* The idle is **5.56 ms per lane per burst** (19.1 %), so the arithmetic is
  right.
* But **4.59 ms of it (82 %) is the mesh PM's own tail + head**, i.e. lifted
  guest policy/job code. `spu-codegen-audit.md` measures that code at
  ~1.03 ns per SPU instruction vs 0.31 ns on hardware; a 1.2-1.4x codegen
  win would cut this part, not a dispatcher change.
* The local runtime pieces total **~1.0 lane-ms/frame** (host 0.60, DMA
  copies ~0.34, lock line 0.03), and even zeroing all of them cannot move
  6 ms. It also cannot move the p50: the frame is pinned by the c690
  producer ending near the two-vsync deadline
  ([`meter-p50-pin.md`](meter-p50-pin.md), [`frame-pacing-floor.md`](frame-pacing-floor.md)).
* The one host class worth attacking (coordinator dispatch) is already 20 us
  at p50; the long tail is guest dependency, not a poll. The ready-store fix
  confirms it.

**Conclusion:** no further runtime dispatch change is warranted for the lane
gaps. If the meter burst is the target, the remaining lever is per-job lifted
code (`spu-codegen-audit.md`) or reducing the job count, not the scheduler.
The p90/p99 tail on real play is already the walker
([`c690-serialisation.md`](c690-serialisation.md)).

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work
# one launch per arm (the switch is an environment variable)
W=$PWD
cat > /tmp/lanegaps_watch.sh <<'EOF'
W=/home/m/Documents/mlb16-online/recomp-work
for i in $(seq 1 900); do
  f=$(ls -t $W/ab_live_lanegapsA_*.log 2>/dev/null | head -1)
  [ -n "$f" ] && grep -q "pitch meter confirmed" "$f" && { sleep 18; touch /tmp/lanegaps_A.go; break; }
  sleep 1
done
EOF
rm -f /tmp/lanegaps_A.go
bash /tmp/lanegaps_watch.sh &
TAG=lanegapsA BUILD=build-play N=4 SEG=12 DROP=2 METER=1 SKIP_CINE=1 WCB=2 \
  PIN=0 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=15 PITCH_THRESH=35 \
  PS3RECOMP_C690_STATS=1 SPURS_READY_WAKE=0 \
  PS3RECOMP_TIMELINE=$W/captures/lanegaps/A_tl.bin \
  PS3RECOMP_TIMELINE_HITCH_US=0 PS3RECOMP_TIMELINE_TRIGGER=/tmp/lanegaps_A.go \
  PS3RECOMP_TIMELINE_SECS=24 PS3RECOMP_TIMELINE_RING_MB=64 \
  PS3RECOMP_TIMELINE_FLUSH_MB=16 ARMS='A=' BOOT_ARM='' \
  ./ab-tools/ab_live.sh
# repeat with TAG=lanegapsB, SPURS_READY_WAKE=1, B_tl.bin, /tmp/lanegaps_B.go

python3 ab-tools/pin_ab.py --log ../ab_live_lanegapsA_*.log \
  --marks ../ab_live_lanegapsA_*.marks --seg 12 --drop 2
# gap classification: pair c690.job spans per spurs pm thread, clip to the
# per-frame union, split at the mesh (wid 1/2) spu.policy spans, filter
# spurs.kick by workload id and by thread (spurs kernel = coordinator).
```

## 7. Caveats and deviations

* **The switch is an environment variable, not a PS3RECOMP_CTL live key.**
  This mission was asked to touch only `runtime/spu` + `libs/spurs` because
  another mission is editing `ps3recomp-play`; the live-key table lives in
  `libs/video/ps3_live_ctl.c`, so the switch is seeded from the environment.
  The A/B therefore used the allowed two-launch fallback. Default is 0.
* **Two launches, not a paired in-run A/B.** The ~0.4 fps cross-launch drift
  is larger than the measured delta; the gap classes are the robust numbers
  (they move <5 % and in the same direction across the two independent
  timelines).
* The timeline window is 24 s per launch, triggered 18 s after the meter is
  confirmed, so it covers the first ~21 s of the 4 x 12 s measured segments.
  It is not the whole measured window.
* The window's fast frames have no c690 work and are not bursts; the 621
  burst frames are the analysis set. The mean burst is 29.16 ms because the
  window mixes hard and tail frames; the frame p50 is still 34.15 ms.
* The DMA estimate uses the older per-gap transfer counts
  (`c690-job-gaps.md` §3.3) plus a local memcpy benchmark; it is not a second
  timed trace. It is small enough that the conclusion does not depend on it.
* The `lanes1` capture is `build-lanes` (the extra-lane branch), A window
  only; it is used as a shape replication, not as an A/B arm.
* TSan on this host needs clang: gcc's `libtsan.so.2.0.0` symlink is missing.
  The scheduler fixture's four reports are the pre-existing manufactured
  guest stores, none in the new path.

## 8. Clean room

This is our own runtime trace stream, our own logs, our own EBOOT/job-module
static analysis (Ghidra), and our own lifted SPU code. No RPCS3 or other GPL
emulator source was read or cited.
