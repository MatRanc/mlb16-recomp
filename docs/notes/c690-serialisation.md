# The replay long frames are walker-draw-bound, not serialised Edge work

Date: 2026-11-04. Offline analysis only: **zero game launches, no build**. The
data is the user's own 40 ms hitch recording
`/tmp/longframes_20261104_142459.pstl` (788 MB, ~5 min, 1,930 frames >= 40 ms)
plus the matching `recomp-work/play_20261104_142459.log` and MangoHud CSV.
Follows [`long-frames.md`](long-frames.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`frame-timeline-2.md`](frame-timeline-2.md),
[`c690-native-dispatch.md`](c690-native-dispatch.md) and
[`targets-and-backlog.md`](../targets-and-backlog.md).

## TL;DR

* **The "Edge on one lane of five" is an average over a frame the walker
  stretched, not a serialised Edge batch.** In the 80-250 ms frames the five
  c690 lanes run **3.82 of 5 lanes concurrently** while the Edge work exists
  (4.00 in 40-50, 3.92 in 50-80). They drain the whole batch and are idle.
  The batch ends at **+24 ms** in the representative 86 ms frame; the `rsx
  walker` then drains for another **62 ms**.
* **The Edge work per frame is nearly constant across all three bands.**
  c690 lane-sum (the sum over the five lanes) is **102 / 111 / 109 ms** for
  40-50 / 50-80 / 80-250 ms frames, **20-22 ms per lane**, **575 / 712 / 796
  jobs**. The last c690 job ends **+29.3 / +29.1 / +31.7 ms** after the frame
  start in every band. Edge does not grow with the frame; the frame grows
  around it.
* **The frame is one walker drain plus the put-wait before it.**
  drain + put-wait is **100 %** of the frame in every band (drain 78 / 83 /
  83 %); `c690.job` lane-sum is 229 / 185 / 118 % of the frame because it is
  five parallel lanes. The existing census labels 92 % of these frames
  "c690-dominated" because it compares that summed lane time with the single
  walker thread's drain. On wall time the walker is the chain.
* **Cross-check in the same run's `[walker-prof]`:** the replay frames carry
  **~1,570 draws per frame** (the meter's live runs are ~500-800). Walker drain 45-67 ms,
  engine 36-41 ms, parse 8-25 ms, wait 10-20 ms. Across the session, frame
  time scales linearly with draws: **47.5 us per draw**, correlation
  **0.803**; 240 draws -> 20.2 ms, 547 -> 32.3, 916 -> 47.8, 1451 -> 74.9,
  2131 -> 111.7, 3025 -> 179.6 ms. This is the user's "scales with players on
  screen": more visible players = more draws, not more Edge jobs (796 c690 jobs/frame
  here vs ~680 at the meter).
* **Verdict:** there is no Edge serialisation to fix here. The per-list PPU
  wait (`Mesh_WaitPrevJobList` -> `WwsJobList_Wait` -> `WwsJobList_SpinUntilDone`)
  is real and still runs, but it is hidden under the walker. A per-frame Gantt
  of a typical 86 ms frame is below.
* **Lever:** the walker's own span, not Edge. In the representative frame
  35.9 ms of the 86.1 ms is guest-mandated WCB render-target readback
  (copy + GPU sync: one 13.2 ms copy before the drain, 4.5 and 17.4 ms inside
  it) and ~44 ms is the draw/parse CPU. A 25 % cut of the draw CPU is
  ~-11 ms (86 -> 75 ms, 13.3 fps); the readback stalls are guest-driven and
  the existing byte cuts (RECT/BATCH/SKIP) target bytes, not the sync. The
  existing `PS3RECOMP_WALKER_FAST` is **not** a lever: live its reference
  sets change every frame, it hits ~5 % and costs ~2.4 ms/frame
  ([`walker-quick-wins.md`](walker-quick-wins.md)); it is left off. No code
  change was made and no live key was added.
* **No A/B was run.** The premise ("something serialises the Edge work")
  is contradicted by the per-frame data: an Edge key cannot shorten a frame
  whose Edge batch ends 60 ms before the frame does. The stop rule was used
  rather than spending one of the two allowed launches on it.

![Typical long replay frame: the c690 batch ends at +24 ms, the walker drains
to +86 ms](c690-serialisation-gantt.png)

## 1. Method and why the old attribution points at c690

`tools/hitch_table.py` reads the PSTL hitch windows `[frame_end - dur,
frame_end]` and sums every `PS3_TR_C690_JOB` span (event 33) that overlaps the
frame. Five lanes run in parallel, so a frame with five lanes busy for 22 ms
contributes **110 ms** to `c690.job` while lasting 22 ms of wall time. That is
the number the census ranks against `rsx.drain`, which is a single thread.
Hence `c690.job` "wins" 1,779 of 1,930 frames (92 %), and `rsx.drain` only 148,
even though the walker is the wall chain in nearly every frame.

The per-frame analysis below instead computes, for every hitch window:

* each c690 job span clipped to the window (lane = `spurs pm` host thread
  tid 3-7; tid 8 runs the policy module and no c690 jobs);
* the per-lane busy time, the job count, the union of all lane spans, and the
  concurrency (number of lanes running) inside the union;
* the last c690 job end relative to the frame end;
* `rsx.drain` (event 13), `rsx.put_wait` (15), `rsx.idle_wait` (43), clipped
  to the same window. These are the walker's own spans; the frame boundary is
  the `PS3_TR_HITCH` record on the `ppu DisplayFlip` thread.

The 1,930 windows are the whole recording (40-50: 570, 50-80: 1,059,
80-250: 298, >250: 3). The log is the same session's `[walker-prof]`
(9,291 reports) and `[c690-stats]` (9,500 jobs/s at the replay).

Cross-checks: the c690 job rate (9,500 jobs/s at ~12 fps) gives ~790
jobs/frame, matching the trace's 796 in the 80-250 band; and the
`[walker-prof]` report at `at=258201ms` (1,570 draws, walker 66.8 ms, wait
18.7 ms) is the same frame as the trace's 86.1 ms window at 259.0 s.

## 2. The three bands

Means per frame; medians in parentheses where they differ materially.
`lanes avg` = c690 lane-sum / frame (this is the mission's "lanes busy");
`lanes in burst` = c690 lane-sum / union.

| band (ms) | n | frame | c690 lane-sum | lanes avg | lanes in burst | c690 union | jobs | last c690 job at | lane busy (each) | drain | put-wait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 40-50 | 570 | 44.7 | 102.2 | 2.28 (2.67) | **4.00** | 25.6 | 575 (673) | **+29.3** | 20.4 | 34.8 (78 %) | 9.8 (22 %) |
| 50-80 | 1,059 | 60.0 | 111.0 | 1.91 (1.61) | **3.92** | 28.3 | 712 (727) | **+29.1** | 22.2 | 49.7 (83 %) | 10.2 (17 %) |
| 80-250 | 298 | 92.7 | 109.5 | 1.19 (1.00) | **3.82** | 28.7 | 796 (797) | **+31.7** | 21.9 | 77.2 (83 %) | 15.6 (17 %) |

Reading down the table:

* **The Edge batch is the same size and finishes at the same time in every
  band.** ~575-800 jobs, ~102-111 ms summed lane time (~22 ms per lane, the
  five lanes within 1 %), union 26-29 ms, last job +29 to +32 ms.
* **The frame is longer because the walker drain is longer:** 35 -> 50 -> 77
  ms. drain + put-wait is 100 % of the frame in every band.
* **`lanes avg` falls only because the frame gets longer.** It is
  lane-sum / frame. Nothing becomes less parallel: the concurrency *inside*
  the Edge burst is 3.8-4.0 of 5 in every band.
* The c690 job count rises a little (575 -> 796) while the per-job duration
  falls (178 -> 138 us) and lane busy stays ~22 ms. The larger frame is not
  more Edge content.

## 3. One typical 80-150 ms frame, lane by lane

Frame 1187 in the recording: **86.10 ms**, 797 c690 jobs, c690 lane-sum
85.8 ms, union 23.9 ms. The Gantt at the top of this document is this frame.

| row | what happens |
|---|---|
| `rsx walker` | put-wait 0-20.1 ms, then **drain 20.1-86.1 ms (66.0 ms)**, ending on the frame boundary |
| WCB readback | 10 copies; b-duration sum **35.9 ms**: 6.1-19.2 (13.2 ms, inside the put-wait span), 26.3-30.8 (4.5), 59.7-77.1 (17.4), plus seven 0.04-0.35 ms copies |
| five c690 lanes | all busy 0.1-24.2 ms; 146 / 162 / 139 / 163 / 187 jobs; 16.7-17.7 ms busy each; **last job +24.2 ms** |
| `ppu main` (tid 9) | `gcm.finish` spin 0-22.1 ms, then 6 x ~9.1 ms ref-lwcond waits to the end; blocked 58.8 ms, not producing |
| `ppu DisplayFlip` | flip-done lwcond 0-17.1 ms + 2.2 ms, then the same ref wait; the flip lands at +86.1 ms |
| `present` | blit fence 6.1-18.9 ms, `vkQueuePresentKHR` 18.9-19.7 ms, shown 19.7 ms |

The frame is exactly: **20.1 ms "put-wait" then 66.0 ms of walker drain**.
The Edge lanes finish 62 ms before the frame does. A zero-cost Edge would not
shorten this frame at all; a zero-cost walker would bring it to the ~20 ms
producer side.

### 3.1 Inside the walker span: guest readbacks, then draw CPU

The `rsx.put_wait` span is not all sleep. `cellGcm_rsx_process_fifo` runs
`rsx_draw_engine_wcb_service()` *before* it starts the drain span, so the
13.2 ms copy at +6.1-19.2 is inside the "put-wait" number. The other two big
copies are inside the drain. The ten copies in this frame sum to **35.9 ms of
b-duration (copy + GPU sync)**, i.e. 42 % of the frame:

| part of the 86.1 ms frame | ms | share |
|---|---:|---:|
| WCB copy + sync (10 copies) | 35.9 | 42 % |
| draw/parse CPU (`engine` + `parse` minus the in-drain copies) | 44.1 | 51 % |
| producer wait (put-wait minus the pre-drain copy) | 6.1 | 7 % |

The same `[rsx wcb]` line in the log reports 3.2-4.7 copies/frame and
5.8-11.3 ms/frame copy in the replay seconds, so the long frames carry a
disproportionate share of the readbacks. The bytes are tiny (0-4 KiB): this is
**sync latency**, not bandwidth. It is guest-mandated (the title reads its own
render targets); the existing RECT/BATCH/SKIP cuts reduce bytes or merge
pending requests, not a single synchronous read, which is why they were
neutral at the meter. It is the next thing to profile inside the drain, but it
does not change the Edge conclusion.

The same split holds in the bands: the last c690 job ends 15.4 ms (40-50),
30.9 ms (50-80) and 61.0 ms (80-250) before the frame end, while the walker
drain is still running.

## 4. Candidate-by-candidate

| candidate | evidence | verdict |
|---|---|---|
| Game issues one job list at a time and waits (`WwsJobList_SpinUntilDone`) | The PPU path is real: `Mesh_WaitPrevJobList` (0x6ED24) polls `FUN_009FAD08` every 200 us, then `WwsJobList_Wait` (0x9FBD70) -> `WwsJobList_SpinUntilDone` (0x9FB898) spins on the five per-lane counters and the list counter. But all five lanes run 3.8-4.0 concurrent inside the batch, the batch is ~700-800 jobs, and it finishes at +30 ms. The wait is hidden under the walker, not the frame length. | not the top cause |
| Job-list barriers / dependencies make jobs serial | Lanes finish within 1 % of each other; inter-job gaps p50 **10 us** (3.4k-9.8k gaps per band); no long serial tail; concurrency 3.8-4.0. | rejected |
| Ready count/signal published one job at a time | `spurs.kick` is ~1 per c690 job and native dispatch (`SPURS_PM_NATIVE_DISPATCH=6`) is on in this build; the lanes are not short of work while the batch lasts, they run out of work when the batch is done. | rejected as top cause |
| Walker parks on a command hole and the PPU will not produce until it drains | The walker does put-wait 9.8-15.6 ms at the *start* of the frame, then drains 35-77 ms; it is not starved for the drain. `rsx.idle_wait` is 3.9-4.5 ms/frame. | rejected |
| PPU main thread is the bottleneck | In the representative frame main is blocked 58.8 ms in the ref lwcond and 22.1 ms spinning on `gcm.finish`; it is waiting for the walker/flip, not producing Edge work. | rejected |
| Native dispatch fairness/yield misfiring | Native dispatch is on; the batches still reach 3.8-4.0 lanes and 9.5k jobs/s. A fairness yield costs a coordinator round trip, not a 66 ms drain. | no evidence |
| A lock serialises the lanes | `spu.lockline_retry` is 164k spans in 5 min; the gap analysis in `pin-and-burst-path.md` puts lock-line retries at 0.3 % of gap time. | rejected |
| Guest render-target readback (WCB) | 10 copies / 35.9 ms b-duration in the representative frame (42 %); inside the walker span, not Edge. Guest-mandated bytes; the cost is GPU sync, not bandwidth. | **second, inside the walker** |
| **Walker / draw path** | drain + put-wait = 100 % of the frame; draw count 1,570/frame; frame = 47.5 us x draws (r = 0.803); 44 ms draw/parse CPU in the representative frame. | **top cause** |

## 5. Why the replay is walker-bound while the meter is c690-bound

Both statements are in the data:

* At the pitch meter ([`frame-timeline-2.md`](frame-timeline-2.md)) a long
  frame is a ~36 ms c690 burst plus ~6 ms of walker tail; the walker drain
  (25.8 ms) is *smaller* than the burst, so c690 sets the frame. The meter
  live draw count is ~500-800/frame.
* In the replay the same ~700-800 c690 jobs finish in ~30 ms, but the frame
  submits **~1,570 draws** and the walker takes 45-77 ms. The c690 batch is
  smaller than the draw work, so the walker sets the frame.

The "lanes busy" statistic cannot tell these apart because it divides the
same ~105 ms of lane time by a frame that is 45 or 93 ms long. The separating
measurement is the **last c690 job versus the frame end**: +30 ms vs 45/60/93
ms. In the meter's c690-bound frame the last job is at the frame end; here it
is not.

Consequence for the work plan: an Edge/native-dispatch change moves the
meter's p50/p90 but cannot remove the replay's 80-139 ms frames; a walker
per-draw change moves the replay and only the tail of the meter. The replay
gap is not a scheduling bug, it is draw throughput.

## 6. Levers, with estimates

Basis: 1,570 draws/frame, walker drain 66.8 ms, frame 86.1 ms in the
representative frame; 47.5 us per draw across the session. The frame
breakdown above (35.9 ms WCB + 44.1 ms CPU + 6.1 ms wait) is the chosen
frame, not the session mean.

| lever | estimate | evidence / cost |
|---|---:|---|
| 25 % cut of the draw/parse CPU (44 ms) | **-11 ms/frame** (86 -> 75 ms, 13.3 fps) | linear draw scaling, r = 0.803 |
| 50 % cut of the draw/parse CPU | **-22 ms/frame** (86 -> 64 ms, 15.6 fps) | same |
| remove the WCB sync stalls (35.9 ms) | up to **-30 ms/frame**, but the guest waits for the bytes; only an overlap/async design can take it | guest-mandated; not covered by RECT/BATCH/SKIP |
| Parse/record split (the queued XL item) | could remove a large part of `parse` (8-25 ms/frame) | `walker-cuts-2.md` §4; 4-8 weeks, no key |
| Vulkan recording (keep render passes open etc.) | ~5.5 ms at the meter, scales with draws | live-only, not offline-measurable |
| `PS3RECOMP_WALKER_FAST` / fetch memo | **negative**: ~5 % hit live, ~+2.4 ms/frame | `walker-quick-wins.md`; deliberately off |
| Edge native dispatch / more Edge lanes | **0 ms on these frames** | Edge ends 60 ms before the frame end; the lanes are already 3.8-4.0 concurrent while the batch runs |

If the goal is specifically the user's replay complaint, the next mission
should profile the walker draw path on a replay capture (the `[walker-prof2]`
split at a replay frame), not add Edge work. A cheap first check is whether
the ~1,570 draws include draws the game would have culled; that is a capture /
draw-count question, not a scheduler one.

## 7. Measurement status

No game launch and no A/B. The recording already contains per-frame evidence
for 1,930 frames, and the premise being tested (Edge serialisation) is
contradicted by it. The stop rule in the mission ("if the premise is wrong,
stop and report with evidence") applies. The meter A/Bs that do exist
(`native2`, `walk2b`) remain the meter's own answer and are unchanged.

To reproduce the anatomy (this is the same command that produced the
recording; it is also the user's replay test command):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_TIMELINE=/tmp/longframes_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
./play_windowed.sh
```

Boot to a match, enter the instant replay, zoom out until the whole field is
on screen, and record 2-3 minutes. Then:

```sh
python3 mlb16-recomp/tools/hitch_table.py /tmp/longframes_*.pstl \
  --min-ms 40 --bins --classify --baseline-jobs 680 --top 40
grep -E '\[walker-prof\].*draws=[1-9]' \
  recomp-work/play_*/play_*.log | tail -40   # draw count vs walker ms
```

If a future walker key is A/B'd, the same recording is the acceptance test:
look at the 80-250 band's frame and at `draws=`; the Edge lanes should be
unchanged (same job count, same +30 ms batch end) and only the walker drain
should move.

## 8. Caveats

* One recording, one scene (instant replay, zoomed out), one build. The bands
  are real play; the meter is a different scene and is cited from its own
  documents.
* The hitch recorder keeps only frames >40 ms, so there is no fast-frame
  baseline in the same file. The `[walker-prof]` lines and the `[flip]`
  cadence provide the cross-check.
* `c690.job` has a constant descriptor EA in this build (4912), so jobs
  cannot be split by type; lane and timing are the available dimensions.
* The walker's `parse` term is `drain - dispatch`; it includes FIFO decode,
  vertex-program runs and WCB service, not one named phase. The `[walker-prof]`
  split (`engine`, `parse`, `fetch`, `tex`, `pipe`, `draw`) is the
  decomposition used here.
* `rsx.drain` + `rsx.put_wait` can exceed the frame by the idle overlap
  (4 %); the shares use drain/(drain+put-wait).
* Frame 1187 is chosen to be near the 80-250 band median in duration, job
  count and lane-sum (86.1 ms / 797 jobs / 85.8 ms); it is representative,
  not special.

## 9. Clean room

This is our own analysis of our runtime's trace stream, the title's own log,
and our own EBOOT (Ghidra). No RPCS3 or other GPL emulator source was read or
cited.
