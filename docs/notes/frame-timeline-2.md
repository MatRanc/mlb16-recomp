# Frame timeline 2: the long frame is one c690 producer burst, not the walker drain

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. One windowed meter run
(`recomp-work/ab_live_timeline2_20261103_065615`), standard
`play_windowed.sh` configuration (`TEX_DIRTY=1`, `PIPE_MEMO=1`, the four cheap
wins, `WCB=2`, `CUBE_RT=1`, `PACE=1`, `PRESENT_NEW_ONLY=1`) plus `METER=1`,
`SKIP_CINE=1`, `PIN=1`, x11. `PS3RECOMP_TIMELINE` recorded 45.0 s after the
meter had been up 22 s; `PS3RECOMP_FLIP_LOG=1`, `PS3RECOMP_C690_STATS=1`.
Analysis only: no code, config or build changed.

This is the follow-up to [`frame-timeline.md`](frame-timeline.md). The walker
drain has since fallen **26.4 -> 18.9 ms** and fps has risen **~29 -> 31.4**,
but per-flip p50 is unchanged (**33.85 -> 34.23 ms**). The answer is that the
critical chain moved: the frame now waits on the **c690/mesh producer burst**
(~36 ms wall), not on the walker's own work. The walker spends the saved time
in `rsx.put_wait` and the idle kick wait, both waiting for that burst.

## TL;DR

* **p50 did not move; the tail did.** 300 consecutive meter frames (frames
  200-499): p50 **34.23 ms** (old 33.85), p90 **44.33** (old 51.32), p99
  **49.14** (old 59.60), mean **32.43** (old 33.64), std 10.76 (old 12.13).
  Full 45 s window (1,412 intervals): p50 34.11, p90 43.55, p99 49.96, mean
  31.83.
* **The walker work is gone from the critical chain.** Drain **26.37 -> 18.85
  ms** mean (-7.5), p90 42.4 -> 29.3, p99 53.1 -> 34.3. What replaced it is
  producer wait: walker kick-idle **3.17 -> 6.47 ms** and walker
  `rsx.put_wait` **4.10 -> 7.12 ms**. `drain + idle + put_wait` is exactly the
  frame; there is no unaccounted walker work left.
* **A long frame is one c690 burst plus ~5-6 ms.** 131 frames > 35 ms: L 41.6,
  c690 lane union **36.0**, the enclosing burst **37.0 ms wall**, drain 25.8,
  idle 10.0, put 5.9. The walker drain is *less* than the producer wall, so
  the walker is not setting it: the frame ends when the burst's last PUT
  lands, then the walker drains.
* **Inside a burst the five lanes are not all busy.** Big bursts (wall >= 20
  ms, n=213): wall 37.1 ms, union 35.1 ms, per-lane busy 20.0 / 23.5 / 24.6 /
  24.9 / 24.9 ms, mean **3.19 of 5 lanes busy (64 %)**, all five busy only
  **19.6 %** of the wall. The idle is **139,664 tiny inter-job gaps** (p50
  **60 us**, p90 0.12 ms, 98 % < 0.5 ms), not lane start/end skew (ramp 0.86
  ms, tail 0.39 ms) and not a global scheduler tick.
* **Not two game frames per burst.** `gfx.frame_end`, `ppu.frame_end` and
  `gcm.flip_request` are **1.00 per frame**; the GCM user handler fires
  **0.70 per frame**. One 1 ms-defined burst contains **0.77 flip ends**. A
  burst is ~1.2 frames long, so 81/246 bursts spill their tail into the next
  frame; that pair is one burst plus its tail, not two frames of c690 work.
* **Halving c690 wall would move p50 to the main floor.** From the per-frame
  data (scale each frame's c690 wall by 0.5, clip at the 16.9 ms main-ref
  median): p50 **16.9-18.7**, p90 24.2-25.3, p99 **28.7-30.5**, mean 19.4-20.3.
  Partial reductions: c690 x0.8 -> p50 27.0 / p99 41.6; x0.69 (perfect lane
  packing) -> p50 23.1 / p99 37.5.
* **Ranked lever: c690 lane utilization first, native c690 kernels second.**
  The 37 % inter-job idle on the five lanes is worth ~11 ms of p50; reducing
  c690 work with native kernels is worth the next ~4-7 ms and both together
  reach the 17-22 ms floor. The walker and present are not p50 levers now.

## 1. Run and window

One launch, 179 s to the pitch, X once, meter confirmed at 06:58:56; timeline
trigger at 06:59:18 (22 s later) for 45.0 s. 7,684,186 events from 41 threads,
**0 dropped**, 246 MB raw. 1,413 `gfx.frame_end` records, 1,412 flip-to-flip
intervals; the 300-frame analysis is frames 200-499 (seconds ~9-19 of the
window), the same indexing as the 2026-11-02 document.

Overhead check, same launch: recorded window vs the 30 s after it closed
(flusher off): guest flip-to-flip p50 34.11 vs 34.08 (+0.03), p90 43.55 vs
42.92 (+0.63), p99 49.96 vs 48.84 (+1.12), mean 31.83 vs 31.16 (+0.67).

## 2. Side by side with 2026-11-02

### 2.1 Frame distribution (300 frames, frames 200-499)

| metric | 2026-11-02 | 2026-11-03 |
|---|---:|---:|
| p50 / p90 / p99 ms | 33.85 / 51.32 / 59.60 | **34.23 / 44.33 / 49.14** |
| mean / std ms | 33.64 / 12.13 | **32.43 / 10.76** |
| <25 / 25-35 / 35-45 / >45 | 83 / 138 / 12 / 67 | **84 / 85 / 106 / 25** |
| full 45 s p50 / p90 / p99 | 33.90 / 50.96 / 58.57 (1,328) | **34.11 / 43.55 / 49.96 (1,412)** |

The two runs have the same number of short frames (~84); the 2026-11-02 tail
(>45 ms) has collapsed into the 35-45 ms band. p50 sits at the centre of that
band, so it did not move.

### 2.2 Per-frame terms (mean / p50 / p90 / p99, ms)

| term | 2026-11-02 | 2026-11-03 |
|---|---:|---:|
| frame | 33.64 / 33.85 / 51.32 / 59.60 | 32.43 / 34.23 / 44.33 / 49.14 |
| walker drain | 26.37 / 25.81 / 42.40 / 53.06 | **18.85 / 21.73 / 29.30 / 34.28** |
| walker idle (kick) | 3.17 / 2.19 / 7.69 / 12.55 | **6.47 / 6.23 / 13.49 / 18.88** |
| walker put wait (outside drain+idle) | 4.10 / 5.00 / 8.57 / 13.23 | **7.12 / 7.33 / 14.13 / 17.44** |
| main blocked | 21.10 / 18.95 / 32.90 / 40.49 | 17.81 / 17.51 / 24.06 / 31.58 |
| main work (frame - blocked) | 12.54 / 17.73 / 22.24 / 25.96 | 14.63 / 18.16 / 24.45 / 28.55 |
| main ref wait 0x1245090 | 20.54 / 18.76 / 32.07 / 39.73 | 17.12 / 16.88 / 23.19 / 30.56 |
| ref wake lag | 0.18 / 0.00 / 0.75 / 1.04 | 0.10 / 0.00 / 0.46 / 1.03 |
| walker idle wake lag | 0.00 | 0.00 |
| c690 lane union | 22.05 / 32.13 / 35.66 / 38.92 | **25.87 / 32.68 / 38.13 / 41.83** |
| present fence wait | 3.30 / 4.67 / 5.58 / 11.86 | **7.18 / 7.83 / 12.98 / 14.02** |

The old "walker other" is now identified: it is `rsx.put_wait` outside drain
and idle. `drain + idle + put` sums to the frame mean
(18.85 + 6.47 + 7.12 = 32.44), so the walker is fully accounted for. Its own
work is down 7.5 ms; its producer waits are up 6.3 ms.

The `main ref wait` is smaller because the walker is no longer the producer:
main's wait is still inside the frame, but on the long frames the chain is
c690 -> walker, not walker -> main. Wake-up lags are unchanged and negligible
(ref 0.10 mean, p99 1.03; walker idle 0.00).

### 2.3 Frame classes, old definition (dominant term > 65 % of the frame)

| class | 2026-11-02 | 2026-11-03 |
|---|---:|---:|
| walker drain | 256 | 90 |
| main ref wait | 37 | 80 |
| walker idle | 0 | 7 |
| mixed | 7 | 123 |

The class boundary is less informative now because drain is 18.9 ms inside a
34 ms frame (55 %, below the 65 % test) while the c690 producer occupies the
rest. The meaningful split is by length and by which producer sets it.

### 2.4 Groups by frame length (same thresholds both runs)

| group | run | n | L | drain | idle | put | ref | c690 union |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| short <=25 | old | 83 | 19.0 | 13.1 | 0.6 | 5.2 | 18.7 | 2.6 |
| short <=25 | new | 84 | **16.6** | 5.4 | 1.8 | 9.3 | 16.2 | 3.8 |
| mid 25-35 | old | 138 | 32.8 | 25.8 | 3.4 | 3.6 | 16.5 | 26.3 |
| mid 25-35 | new | 85 | 34.0 | 21.5 | 5.7 | 6.8 | 13.1 | **32.0** |
| long >35 | old | 79 | 50.5 | 41.2 | 5.5 | 3.8 | 29.4 | 35.2 |
| long >35 | new | 131 | **41.6** | 25.8 | 10.0 | 5.9 | 20.3 | **36.0** |

Short frames are unchanged in kind: main's own ref cycle (ref wait 16.2 of
16.6 ms), almost no c690 (3.8 ms) and no walker drain (5.4). Long frames are
now **c690 wall + walker tail**: c690 36.0, drain 25.8 (less than c690), and
wait 15.9 ms (idle 10.0 + put 5.9).

## 3. What sets the long frame: a c690 burst plus a small tail

For the 80 frames in the analysed window in which a whole burst fits inside
the frame: L **42.07 ms**, burst wall **36.06 ms**, c690 union 36.50, drain
26.53, idle 9.98 -> **L - burst wall = 6.0 ms**. The walker is idle/put-waiting
for 10 + 6 ms while the burst finishes and only then drains 26 ms. The
remaining ~6 ms is the walker/present tail (submit, lock, flip), not another
work phase.

The long frames are not all identical: 131 frames >35 ms contain 676 c690 jobs
on average, mid frames 584, short frames only 98. The c690 work is
concentrated in the long/mid frames.

## 4. The c690 burst critical path

Definition: the union of all lane job spans in the window, split where the
distance between merged intervals exceeds 1 ms (the "burst" is the production
period; see caveats for sensitivity).

### 4.1 Whole-window burst statistics

| quantity | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| burst wall (ms) | 33.48 | 36.10 | 41.75 | 45.77 |
| burst union (ms) | 31.71 | 34.78 | 38.67 | 41.55 |
| union / wall | 0.944 | 0.95 | 0.98 | 0.99 |
| sum of 5 lane busy / (5 x wall) | 0.628 | 0.64 | 0.68 | 0.70 |
| wall - union (no-lane-running) | 1.77 | 1.58 | 3.23 | 5.31 |

The union covers 94 % of the burst wall: some lane is almost always running.
But the five lanes are only **63 % occupied**: mean per-burst busy is
20.0 / 23.5 / 24.6 / 24.9 / 24.9 ms against a 37.1 ms wall for the 213 big
bursts (the sixth `spurs pm` thread has no c690 jobs and is idle).

### 4.2 Where the idle is

Big bursts (wall >= 20 ms, n=213):

* lane concurrency: k=0 lanes busy **5.1 %**, k=1 8.1 %, k=2 15.8 %, k=3
  24.1 %, k=4 27.4 %, k=5 **19.6 %**; mean **3.19 / 5** lanes busy.
* ramp-up (burst start until every lane has started): **0.86 ms** mean (p50
  0.04, p90 3.02); drain-down (first lane ends until burst end): **0.39 ms**
  (p50 0.10, p90 0.68). The idle is **inside** the burst, not at the edges.
* per-lane gaps between consecutive jobs: 139,664 gaps, mean **0.10 ms**, p50
  **0.060 ms**, p90 0.12, p99 0.84; **85 % < 0.1 ms**, 98 % < 0.5 ms, 1 %
  > 1 ms. Jobs are small (mean duration 0.18 ms, p50 0.13, p90 0.26, p99 1.03).
* interior occupancy (after all lanes started, before any finished): 3.23 of
  5 lanes over 35.8 ms.

So the burst is a dense chain of ~130 tiny jobs per lane (656 per burst) with
a ~60 us dispatch gap after each. The gaps are staggered across lanes (k=0 is
only 5 %), which is why the union still covers 94 % of the wall. This is
per-job turnover / admission latency (or a short per-job dependency), not a
global 1 ms SPURS kernel tick and not lane start skew.

## 5. Specific answers

### (a) Is the ~38 ms long frame one c690 burst wall + something?

**Yes.** The long frame is one burst: burst wall 37.1 ms (big bursts), long
frame 41.6 ms, and frames with a whole burst inside are 42.1 = 36.1 + 6.0 ms.
The walker drain (25.8 ms) is *smaller* than the burst, so the walker is not
the critical path; the frame ends when the burst's last PUT lands and the
walker drains the tail.

The burst's critical path is **not one lane's busy time and not the union**:
per-lane busy is 20-25 ms, the union (any lane) is 35.1 ms, and the wall is
37.1 ms. Lanes are idle 37 % of the burst in ~60-100 us slots. If the five
lanes were perfectly packed, the same 117.9 ms of lane work would fit in
23.6 ms (x0.67 of the wall); removing half the inter-job gaps only gives about
x0.82. Either way, lane utilization is the headroom, and it is per-lane
turnover, not a single dependency stall.

### (b) Why does a long frame follow a short one — two game frames per burst?

**No, not two game frames per burst.** The flip events say one guest frame =
one flip: `gfx.frame_end` 1.00/frame, `ppu.frame_end` 1.00/frame,
`gcm.flip_request` 1.00/frame, GCM user handler 0.70/frame. A 1 ms-defined
burst contains **0.77** `gfx.frame_end` (1.27 at a 2 ms split, 2.73 at 5 ms).
Frames overlapping a burst: **135 bursts touch one frame, 81 touch two, 29
touch none, 1 touches three**. The two-frame bursts have the length pattern
**[33.97, 16.94]**: the burst starts in the long frame and its tail spills
into the following short frame.

The alternation is the producer's duty cycle: a ~36 ms burst (long frame;
walker waits for the last PUT) followed by a ~15 ms gap (short frame; main's
16.6 ms ref cycle, c690 3.8 ms). Because a burst is ~1.2 frames long, the
phase beats and the dominant cycle is long -> mid -> short -> long (long->long
67/300, short->long 63, mid->short 84); short->long transitions are a new
burst after a gap, not a second half of the previous one.

### (c) Frame distribution if c690 wall were halved

From the per-frame data: scale the measured c690 wall of every frame (the
larger of its burst wall and its lane union) by a factor, clip at the 16.9 ms
median main-ref frame, and recompute. No run was made.

| c690 wall factor | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| 1.00 (measured) | 32.43 | 34.23 | 44.33 | 49.14 |
| 0.90 | 29.86 | 30.61 | 40.21 | 45.38 |
| 0.80 | 27.10 | 27.01 | 36.27 | 41.63 |
| 0.69 (perfect lane packing) | 24.12 | 23.09 | 31.77 | 37.49 |
| 0.50 | 19.41 | **16.91** | 24.18 | **28.73** |

Halving c690 wall puts p50 on the main-ref floor (84 short frames are already
there) and takes p99 under 31 ms. The alternative union-in-frame-only scaling
is slightly more conservative: p50 18.69, p90 25.28, p99 30.49. The 6.9 ms of
non-c690 tail stays.

## 6. Ranked next steps (expected change to p50 / p99)

Numbers are the per-frame counterfactuals in section 5(c); they assume the
rest of the chain does not re-pipeline.

| # | what to attack | evidence | expected p50 / p99 |
|---:|---|---|---|
| 1 | **c690 lane utilization / per-job dispatch** (bigger jobs, batched job submission, or more independent jobs per lane) | 3.19/5 lanes busy, 37 % idle in 60-100 us inter-job gaps; 656 jobs/burst | c690 x0.65-0.70 -> **p50 22-23, p99 36-38** (-11 / -12) |
| 2 | **native c690 kernels for the 5-10x stages**: unpack/pack 23.5 %, skin/transform 13.6 %, normalize 8.2 % (cull's 1.3x is only -1 ms/worker) | step-2 plan; c690 is 100 % of the long frame | c690 x0.80-0.90 -> p50 27-31, p99 42-45; with #1 -> **p50 19-22, p99 32-36** |
| 3 | **PPU main work / ref hand-off** only once c690 is below ~20 ms | short frames are main's 16.6 ms cycle (main work 14.6, ref 16.2) | c690 x0.50 -> p50 16.9, p99 28.7; this becomes the floor |
| 4 | present fence (7.18 ms, up from 3.30) | shown 0.99/frame and submit 0.99/frame; parallel, off the guest chain | 0 / 0 now; watch if presents start dropping |
| 5 | further walker drain work | drain already 18.9 and *below* the c690 burst | 0 / 0; do not spend on the walker for p50 |

The old plan (cut walker drain) is exhausted for p50: the 7.5 ms it saved went
straight into producer waits. The same amount of work now has to come out of
the c690 burst.

## 7. Images

* `recomp-work/ab_live_timeline2_20261103_065615/frame_time_timeline.png` —
  all 1,412 intervals of the 45 s window, p50 34.1 / p90 43.6 / p99 50.0, with
  16.7/33.3 ms guides and the 300-frame box. It is still a dense sawtooth, but
  the band is tighter than 2026-11-02: the lower envelope is ~13-17 ms (the
  main-ref short frames) and the upper envelope is mostly 38-47 ms with a few
  60-70 ms spikes. There are no long flat stretches; the blue box is
  representative of the whole window.
* `recomp-work/ab_live_timeline2_20261103_065615/timeline_5frames.png` — five
  consecutive frames F296-F301 (34.2, **14.8**, 34.3, **14.8**, **47.0** ms),
  threads as rows, ms from F296. On the long F300 all five `spurs pm` lanes
  are solid purple for essentially the whole frame and `ppu main` is red (ref
  wait) from ~4 to 47 ms; `rsx walker` is blue drain with yellow idle gaps.
  On the short F297/F299 the c690 rows are nearly empty (a few short purple
  blocks), the walker drain is a thin blue block, and main's green work plus
  red ref wait fills the 14.8 ms. F296/F298 are intermediate. `present` shows
  a fence wait (orange, ~7-10 ms) then a shown dot each frame; `ppu
  DisplayFlip` is blocked red across the strip; `spurs kernel` kicks are
  dense on every frame; the GCM user-handler dots appear about every other
  frame. The picture is section 5(b) directly.

## 8. Reproduce

```sh
# one meter run with a 45 s binary window armed 20 s after the meter
export PS3RECOMP_TIMELINE=/tmp/tl2.bin
export PS3RECOMP_TIMELINE_TRIGGER=/tmp/tl2.go
export PS3RECOMP_TIMELINE_SECS=45
export PS3RECOMP_TIMELINE_RING_MB=32 PS3RECOMP_TIMELINE_FLUSH_MB=8
export PS3RECOMP_C690_STATS=1 PS3RECOMP_FLIP_LOG=1 PS3RECOMP_WALKER_PROF=1
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=timeline2 METER=1 SKIP_CINE=1 WCB=2 PIN=1 VIDEODRIVER=x11 \
  N=1 SEG=90 DROP=0 SETTLE=10 ARM_A="" ARM_B="" ./ab_live.sh &
# when the log says "pitch meter confirmed", wait 20 s, then:
#   date +%s%3N > /tmp/tl2.origin; touch /tmp/tl2.go

# analysis (run folder keeps the scripts and the per-frame CSV)
cd ../../recomp-work/ab_live_timeline2_20261103_065615
python3 analysis_timeline2.py /tmp/tl2.bin timeline_per_frame.csv --frames 200 500
python3 make_timeline_plots2.py       # both PNGs
python3 batch_sens.py                 # burst-definition sensitivity
python3 lane_stats.py                 # ramp/tail, per-lane gaps, job durations
python3 compare_runs.py               # side by side with 2026-11-02
```

The raw `/tmp/tl2.bin` is 246 MB and was deleted after the analysis. The run
folder keeps `timeline_per_frame.csv`, `analysis_out.txt`, `compare_out.txt`,
`analysis_timeline2.py`, `make_timeline_plots2.py`, `batch_sens.py`,
`lane_stats.py`, `compare_runs.py` and both PNGs.

## Caveats

* One launch, one window. The 300 frames are frames 200-499 of 1,412; the
  full-window percentiles differ by <0.2 ms.
* "Burst" is not a protocol object. It is the union of the five lane job spans
  split at a 1 ms gap, chosen because it matches the long-frame duration. The
  split is sensitive: in the analysed window 0.25/0.5/1/2/5/10 ms gives
  496/371/246/165/93/66 bursts and 0.38/0.51/0.77/1.27/2.73/4.11 flip ends
  per burst. The long-frame result holds over 1-2 ms (burst wall 33-50 ms,
  union 35-40 ms); the lane-occupancy result is computed on wall >= 20 ms and
  is not sensitive to the split above that.
* There are five active c690 lanes, not six: one `spurs pm` thread runs the
  policy module and no c690 jobs. Per-lane numbers are per host thread
  (`spurs pm`), not physical lane slots.
* `ppu main` is `host thread 3` in this run (the boot thread is named after
  its host tid; the old run had `host thread 7`). It is identified
  structurally as the thread with `ppu.frame_begin` records and the 0x1245090
  ref cond, excluding `ppu DisplayFlip`.
* The counterfactuals scale the measured c690 wall per frame and clip at the
  main-ref median; they do not model re-pipelining, changed frame pairing, or
  a different present cadence. They are floors/ceilings for the frame-time
  change, not a prediction of a specific implementation.
* `present fence` is GPU blit execution time, not a host wake-up; it is on the
  parallel present thread.
* The 2026-11-02 per-frame values come from its `timeline_per_frame.csv`; its
  full-window values are from `frame-timeline.md`.
