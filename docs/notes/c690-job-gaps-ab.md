# c690 job-gap switches: live A/B (fast reschedule wins the tail, not p50)

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play` rebuilt at `204bcd49`. Two launches, both complete and
free of crashes/wedges:

* `ab_live_jobgap4_20261103_133034` - main A/B, one launch, N=6 alternating
  10 s segments, 2 s dropped. A = both switches off, B =
  `SPURS_PM_FAST_RESCHED=8` + `SPU_CODE_DMA_MEMO=1`.
* `ab_live_jobgap5_20261103_133749` - single-switch launch, N=4 each,
  10 s / 2 s dropped. A = both off, F = fast reschedule only, M = code memo
  only.

Standard configuration otherwise: `PIN=1 WCB=2 VIDEODRIVER=x11 METER=1
SKIP_CINE=1`, X pressed once for the pitch meter, ~10 s settle, no timeline
dump, no `perf`. Both arms were switched through `PS3RECOMP_CTL`; the
live-ctl/marks self-check passed for all segments in run 1.

## TL;DR

* **`SPURS_PM_FAST_RESCHED=8` is the winner and it carries the whole effect.**
  In the toggle launch F beats A on every segment: guest fps **28.84 -> 31.41**
  (+2.57), per-flip `fin` p90 **49.75 -> 43.14 ms**, p99 **57.22 -> 48.58 ms**,
  mean **34.68 -> 31.86 ms**, walker wait **15.19 -> 13.05 ms**.
* **The code memo alone is flat.** M vs A: fps 28.88 vs 28.84, p50 34.12 vs
  34.14, p90 49.86 vs 49.75, p99 57.33 vs 57.22, wait 15.24 vs 15.19 ms. It
  does work live - `ovl_load` per 8 s window falls from ~90 k to ~2.7 k with
  `ovl_bad=0` while `code_get` bytes are unchanged - but it does not move the
  frame metrics. It is left **off**.
* **p50 did not move: 34.0-34.2 ms in every arm and every segment.** The
  predicted p50 of 22-23 ms from removing the turnover idle was not observed.
  The win is in the **tail**: fast reschedule removes 1-2 ms of per-frame
  turnover, which shows up as p90/p99/mean and in-burst gaps, while the median
  stays pinned at the ~34 ms (two-vsync) present/hand-off floor of this meter
  scene.
* **In-burst turnover improved.** Window lines: job gap p50 **87 -> 71-72 us**,
  p90 **227 -> 116-140 us**, lane occupancy **0.486 -> 0.531-0.537**. c690 job
  body time per lane did not fall (10.2 -> 10.5 ms/frame): the switches target
  the gap, not the job.
* **No wedge, no crash, no resync, no `[pm-end]`** in either launch. The
  double-unlock fix from `07e90d16` holds: `test_spurs_scheduler.py` passes and
  24 measured segments ran with zero fatal lines.
* Fast reschedule is now on in both `play_windowed.sh` copies
  (`SPURS_PM_FAST_RESCHED=8`); the memo stays off.

## 1. Offline checks

Run before the launches, on the same tree that built `build-play`:

| check | result |
|---|---|
| `runtime/spu/tests/run_code_dma_memo_test.sh` | **PASS** (0 failures; identical/full/partial, wrong offset, outside span, changed source/local store, generation establish/fallback, `PROT_NONE` source proves the O(1) path reads no source bytes) |
| `libs/video/tests/run_texdirty_range_test.sh` | **PASS** (0 failures; tracked write, direct PPU store via page fault, unrelated ranges, disable/enable epochs) |
| `python3 tools/test_spurs_scheduler.py` | **PASS** (includes "fast consume takes exactly one lock-line lock/unlock" and every `spurs_pm_fast_can_rerun` early return leaving the lock line unlocked) |
| `recomp-work/ab-tools/check_spu_context_abi.sh` (with the build's `CMakeCache.txt`) | **PASS** - shared `spu_context` field offsets agree (runtime vs lifted header) |
| `DRY_RUN=1` arm strings through `ab_live.sh` | **PASS** - boot A and the 12 alternating B/A control files are written with both keys named in both arms; no "does not name key" warning |

The `SPU_CODE_DMA_MEMO` state remains appended at the end of `spu_context`
(`resident_code_src_seq[4]`), so the lift/ABI rule in
[`freeze-20261103.md`](freeze-20261103.md) still holds.

## 2. Main A/B (run 1, `jobgap4`)

One launch, pitch meter confirmed 160 s after start. Per-flip frame times are
from the `PS3RECOMP_FLIP_LOG` `fin` timestamps inside each measured window
(`pin_ab.py`); fps is from the guest `[flip]` counter (`ab_live_report.py`).

### 2.1 Per-flip `fin` frame times, pooled over the six measured segments

| arm | frames | fps | p50 ms | p90 ms | p99 ms | std ms | mean ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| A both off | 1359 | 28.27 +/- 1.62 | 34.07 | 51.43 | 65.17 | 13.22 | 35.31 |
| B fast+memo | 1429 | **30.11 +/- 0.99** | **34.04** | **46.06** | **54.75** | 19.75* | **33.61** |

`*` B's pooled std is inflated by one 642 ms present hitch in segment 11; its
other five segments are 10.48-12.39 ms (A's six are 12.21-14.42). Excluding
that hitch, B's per-segment std is lower than A's on every segment.
Paired A-B guest fps: **+1.84 +/- 1.36**, 95 % CI **[+0.48, +3.20]** (the report
prints the A-B convention as -1.84; B is faster).

### 2.2 c690 window lines and walker (mean over the measured windows)

| metric | A both off | B fast+memo |
|---|---:|---:|
| jobs/frame | 275.2 | 276.3 |
| c690 job wall/frame (all lanes, ms) | 50.80 | 52.58 |
| c690 job wall/frame per lane (ms) | 10.16 | 10.52 |
| job gap p50 (us) | 87 | **71** |
| job gap p90 (us) | 227 | **140** |
| lane occupancy, job/(job+gap) | 0.486 | **0.531** |
| walker (ms) | 19.40 | 19.39 |
| walker wait (ms) | 16.05 | **13.70** |

The job body did not get cheaper; the turnover did. Lane occupancy rises 4.5
points and the p90 gap drops 38 %. The walker wait follows it down 2.35 ms,
which is the frame-time gain.

## 3. Which switch carries it (run 2, `jobgap5`)

N=4 per arm, rotating A/F/M/A..., pitch meter confirmed 159 s after start.
`pin_ab.py` output, pooled over the four measured segments:

| arm | frames | fps | p50 | p90 | p99 | std | mean | walker | wait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A both off | 923 | 28.84 | 34.14 | 49.75 | 57.22 | 12.60 | 34.68 | 19.48 | 15.19 |
| F fast only | 1005 | **31.41** | **33.98** | **43.14** | **48.58** | **10.41** | **31.86** | **18.66** | **13.05** |
| M memo only | 924 | 28.88 | 34.12 | 49.86 | 57.33 | 12.38 | 34.66 | 19.37 | 15.24 |

c690 window lines:

| arm | jobs/frame | wall/frame ms | per-lane ms | gap p50 us | gap p90 us | lane occ |
|---|---:|---:|---:|---:|---:|---:|
| A | 281.8 | 51.43 | 10.29 | 83 | 180 | 0.504 |
| F | 277.5 | 50.67 | 10.13 | 72 | **116** | **0.537** |
| M | 279.7 | 52.15 | 10.43 | 83 | 178 | 0.512 |

The memo-only arm is within the A/A spread on p50/p99 and occupancy; the fast
arm moves all of them. **Fast reschedule is the winner; the memo is neutral on
frame time.**

### 3.1 The memo is live (runtime counters)

Per 8 s measured window, delta of the `[c690-gap] runtime` counters:

| arm | `code_get` MiB | `ovl_load` | `ovl_verify` | `ovl_bad` | PUTLLC fail |
|---|---:|---:|---:|---:|---:|
| A | ~5700-5860 | ~92-95 k | ~92-95 k | 0 | ~16.5-17.3 k |
| M | ~5680-5780 | **~2.7 k** | ~91-94 k | **0** | ~17.0-17.3 k |
| F | ~5840-5970 | ~97-101 k | ~97-101 k | 0 | **~11.0-11.4 k** |

The memo removes ~97 % of overlay re-loads while `code_get` bytes stay the same
(the engine still issues the GET; the host memcpy/verify is what is skipped).
Fast reschedule cuts PUTLLC failures by ~35 %, consistent with fewer
coordinator/worker round trips around the ticket claims. Neither changes
`code_get`.

## 4. What this means

* The two switches were aimed at the per-job host phase and the code re-DMA.
  Fast reschedule removes most of the host round trip and does exactly that:
  gap p50 -15 us, p90 -111 us, occupancy +3.3 points (F vs A). That is worth
  **~2.8 ms of mean frame time and ~2.6 fps**, visible on every segment.
* The per-frame model's **p50 22-23 ms assumed the whole idle could be
  removed**. Fast reschedule removes only the host portion (the p50 gap falls
  from 87 to 71 us, not to zero); the PM head/tail, job ticket, descriptor and
  the ~34 ms two-vsync present/hand-off floor remain. The median therefore
  stays at 34.0-34.2 ms. Any further p50 work has to attack the head
  (`code_get` copy, PM staging) or the present/hand-off path, not the
  coordinator round trip.
* The code memo is exact (unit tests + `ovl_bad=0`) and cheap, but its copy is
  not on the critical path at this scene; keep it available as a switch, not
  as a default.

## 5. Changes kept

Both `play_windowed.sh` copies now export

```sh
export SPURS_PM_FAST_RESCHED="${SPURS_PM_FAST_RESCHED:-8}"
```

`SPU_CODE_DMA_MEMO` is not enabled anywhere by default. Both keys remain live
control-file keys, so either can still be A/B'd without a restart.

## 6. Deviations and caveats

* **`PS3RECOMP_C690_GAP=1` was on in both launches.** The mission asked for
  "job gap p50/p90 (2 s window lines; C690_GAP instrumentation off)", but the
  `[c690-gap-window]` lines are gated behind `g_c690_gap`
  (`runtime/spu/c690_stats.c:777`); without it there is no 2 s gap/occupancy
  line at all. The plan in `c690-job-gaps.md` (§7) says to run the A/B with
  `C690_GAP` off because the exclusive-interval hooks cost ~9 % of lane
  samples. The trade was resolved in favour of getting the requested gap and
  occupancy numbers: both arms carry the same instrumentation, so the A/B
  comparison is valid, but absolute fps/frame times here are a few percent
  below a clean `C690_GAP=0` run. No timeline dump and no `perf` were taken.
* **"In-burst lane occupancy" here is the 2 s window's job/(job+gap)**, which
  includes the inter-burst idle between frames. The 64 % figure in
  `frame-timeline-2.md` is the occupancy *inside* a merged burst from a
  `PS3RECOMP_TIMELINE` dump; that measurement is not available from the
  counter lines. The relative change (A -> F) is what the A/B supports.
* Likewise the "c690 wall/frame" column is the sum of per-lane job wall divided
  by the flips in the window (the `[c690-stats]` `(sum=)` delta), not the
  timeline burst-union wall.
* Run 1's B segment 11 contains one 642 ms present hitch (guest fps still
  30.74 there); it inflates the pooled std/max but not the p50/p90/p99
  conclusions. The toggle launch has no such hitch (max frame 97.3 ms in A,
  73.8 ms in F).
* The pitch-meter scene is not stationary in general; both runs used the
  standard "X once" meter state and the standard `SKIP_CINE=1` route. The
  measured windows are 10 s each with the first 2 s dropped.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd ../ab-tools
export PS3RECOMP_C690_GAP=1        # needed for the 2 s gap/occupancy lines
TAG=jobgap4 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11 \
  REPORT=1 SETTLE=10 LIMIT=800 \
  ARMS='A=SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0|B=SPURS_PM_FAST_RESCHED=8;SPU_CODE_DMA_MEMO=1' \
  BOOT_ARM='SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0' ./ab_live.sh

TAG=jobgap5 N=4 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11 \
  REPORT=1 SETTLE=10 LIMIT=800 \
  ARMS='A=SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0|F=SPURS_PM_FAST_RESCHED=8;SPU_CODE_DMA_MEMO=0|M=SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=1' \
  BOOT_ARM='SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0' ./ab_live.sh

# per-flip p50/p90/p99/std and per-arm rollup
python3 pin_ab.py --log ../ab_live_jobgap4_<ts>.log \
  --marks ../ab_live_jobgap4_<ts>.marks --seg 10 --drop 2
```
