# Thread placement (`PS3RECOMP_PIN_LAYOUT`) and SPU idle back-off

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Runs use the standard `play_windowed.sh`
configuration (`TEX_DIRTY=1`, `PIPE_MEMO=1`, the four cheap wins, WCB=2,
CUBE_RT=1, PACE=1) plus `METER=1`, `SKIP_CINE=1`, `PIN=1`, x11. Per-flip frame
times come from `PS3RECOMP_FLIP_LOG` `fin`, not the every-10th `[flip]`
decimation.

This file first documented the implementation and a contaminated first A/B.
The retest below used the two-launch budget: launch 1 took the load sample and
rotated `today`/`os`/`lanes`, launch 2 ran the SPU idle back-off A/B at the
meter. **The retest is clean**: layout `lanes` no longer wedges, the loading
sample is valid, and the back-off is measured. No layout or back-off beat the
baseline outside run-to-run noise, so neither `play_windowed.sh` was changed.

## TL;DR

* **`PS3RECOMP_PIN` was silently a no-op in `build-play` until the first
  mission.** `thread_name.h` reaches `ps3_thread_pin_by_name()` through a weak
  reference, and a weak reference does not make the static linker extract
  `runtime/platform/thread_affinity.c.o`. The fix is a non-weak
  `ps3_thread_pin_link_anchor()` in the same object, whose address
  `templates/project/main.cpp` takes. The binary used here has the symbol and
  every run log has the `[pin] PS3RECOMP_PIN=1 topology ...` banner.
* **`lanes` retested with the fixed registry: no wedge, but still a loss.**
  Three 10 s segments (launch 1) completed with no hang. It is 29.17 fps vs
  **30.82 fps** for `today`, and has **three presented-frame gaps > 100 ms
  (699.7 / 899.8 / 707.6 ms)** while `today` and `os` have none. The lane
  mapping is not kept.
* **`today` vs `os` is a lane-affinity comparison, not a pin-on/off test.**
  Both pin walker cpu 0 and ppu main cpu 1; `os` only lets the five SPU lanes
  float over the full online mask. `today` is 30.82 vs 30.53 fps, p99 50.96 vs
  51.14 ms, std 11.12 vs 10.41 ms, walker+wait 31.66 vs 32.58 ms. That is
  inside run-to-run noise (`today` measured 31.33 fps in launch 2), and `os`
  actually has the better lane-0 job rate (2,482 vs 2,371 jobs/s). **No
  evidence that the lane pinning helps; a true `PIN=0` vs `PIN=1` A/B was not
  in the two-launch budget.**
* **Loading is not a spin problem.** 30 s `top -H` samples during the match
  load: **no thread reaches 90 %**. Busiest with back-off off: `spurs pm` mean
  19-35 % (max 78.8), `rsx walker` 34.0 % (max 85.8), `ppu main` 23.7 % (max
  87.8). The on half is later in the same load (total CPU 219 % -> 444 %) so
  its higher SPU numbers are load phase, not back-off.
* **SPU idle back-off at the meter is neutral.** Six 10 s segments per arm on
  `today`: 31.33 fps off vs 31.40 on, p50 34.18 vs 34.14, p99 50.45 vs 49.04,
  walker/wait within 0.13 ms, c690 lane rates within 1 %. One 540.8 ms outlier
  in the on arm is a one-off stall, not a back-off effect. **Default stays off.**
* **No `play_windowed.sh` change.** `PIN` is still not enabled by the play
  script; the layout `today` is only the in-code default when `PIN=1`.

## 1. Thread placement

### 1.1 Topology

Ryzen 5 2600X, 6 cores / 12 SMT threads. `lscpu -e` and sysfs sibling lists:
(0,6) (1,7) (2,8) (3,9) (4,10) (5,11). `thread_affinity.c` reads the same
sysfs lists, so the mapping is derived, not hard-coded.

### 1.2 Layouts

`PS3RECOMP_PIN_LAYOUT` (default `today`; names `a/b/c` and `0/1/2` also work):

| layout | walker | ppu main | c690 lanes | present / DisplayFlip / spurs kernel | everything else |
|---|---|---|---|---|---|
| `today` (a) | 0 | 1 | 2-5,8-11 (the old `other` mask) | 2-5,8-11 | 2-5,8-11 |
| `lanes` (b) | 0 | 1 | 2,3,4,5,10 | 8 / 9 / 11 | 8,9,11 |
| `os` (c) | 0 | 1 | 0-11 (unpinned) | 2-5,8-11 | 2-5,8-11 |

The lane layout is built from the sibling groups of the CPUs left after
excluding the walker and main core pairs. It assigns the first four lanes to
the four remaining physical cores (2,3,4,5) and the fifth lane to the sibling
of the lane-2 core (10), so the unavoidable SMT pair is lane 2 + lane 4, not
two of the highest-rate lanes. The walker/main siblings 6,7 are left idle.
`present`/`DisplayFlip`/`spurs kernel` take the remaining siblings 8,9,11.
`os` differs from `today` only for the five lane workers; the walker and ppu
main masks are the same in both.

The lane index comes from `ps3_thread_pin_spurs_lane(slot->spu_num)` in
`spurs_pm_worker`; all six workers share the thread name `spurs pm`, so the
name alone cannot identify them.

### 1.3 Live switching

* `ps3_live_ctl` has a string-valued key (`PS3RECOMP_PIN_LAYOUT`) and
  `ps3_live_ctl_get_str()`.
* The walker calls `ps3_thread_pin_poll()` next to `ps3_live_ctl_poll()` at the
  top of a drain pass. It compares the (truncated) layout name with the last
  applied one and, on a change, rebuilds the masks and re-applies them to the
  registered long-lived threads with `pthread_setaffinity_np`.
* Only `walker`, `ppu main`, `spurs pm` lane workers, `present`,
  `ppu DisplayFlip` and `spurs kernel` are registered. `OTHER` threads get
  their mask once at creation and are not tracked; a transient guest thread's
  `pthread_t` can be freed and reused, and re-applying to it is a
  use-after-free (the first mission reproduced that as a segfault).

## 2. SPU idle back-off (`PS3RECOMP_SPU_IDLE_BACKOFF=1`)

The GETLLAR handler in `runtime/spu/spu_channels.c`, when the option is on,
compares the line just polled with the previous poll of the same line:

1. different line/context/data -> reset, no delay;
2. unchanged for the first 32 polls -> `pause`;
3. up to 96 -> `sched_yield`;
4. beyond that -> at most one `nanosleep(100 us)` per ms.

The next GETLLAR happens immediately after the helper, so the guest re-reads
the line either way; a wake-up that lands during the nap is delayed by at most
100 us and cannot be missed. Plain DMA GET polls already have the
identical-transfer watchdog's back-off (`mfc_repeat_watch` in `spu_dma.h`,
yield for the first 2 ms then 1 ms sleeps), so the flag covers the atomic path,
which had none. Default off; env and live key (`PS3_LIVE_SPU_IDLE_BACKOFF`).

Measured at the meter in launch 2: neutral (section 4.3). Left default-off.

## 3. Runs

| run | tag | result |
|---|---|---|
| retest 1 | `ab_live_pin2a_20261103_063735` | load sample + 12 meter segments (`today`/`os` alternating, then three `lanes`); meter at 176 s; all segments completed |
| retest 2 | `ab_live_pin2b_20261103_064509` | `today` with SPU back-off off/on, six 10 s segments each; meter at 162 s; all segments completed |

The first mission's three launches (`ab_live_pinload1`, `ab_live_pin1`,
`ab_live_pin2`) are history only; their layout-b wedge and load sample were
confounded by the registry and control-file bugs described below and are
superseded by these two.

Launch 1 order (each arm names both live keys, so a switch never inherits a
stale value): `os, today, os, today, os, today, lanes, os, lanes, today,
lanes, today`. The first switch therefore differs from the `today` boot arm,
and every switch after it differs from the previous layout, so the log has
exactly one `[pin] layout ->` line per switch (12 lines for 12 switches; the
first mission's malformed control file produced 107,588).

## 4. Numbers

`pin_ab.py` uses the `fin` timestamp of every `[fliplog]` record (including
the delayed `skipped` ones), 10 s windows with the first 2 s dropped. The
`show_gap` column is the independent interval between *presented* frames.

### 4.1 Layout A/B/C at the meter (launch 1)

| arm | layout | frames | fps | p50 | p90 | p99 | std | walker | wait | present gaps >100 ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| A | `today` | 1,233 | **30.82** | 34.03 | 44.27 | **50.96** | 11.12 | 18.12 | 13.54 | 0 (max 82.4) |
| B | `os` | 977 | 30.53 | 34.06 | 44.05 | 51.14 | 10.41 | 20.34 | 12.24 | 0 (max 88.5) |
| C | `lanes` | 700 | 29.17 | 34.07 | 42.41 | 51.14 | 43.23* | 18.06 | 13.68 | **3 (max 899.8)** |

\* The `lanes` fin std is inflated by three multi-hundred-ms gaps in the
`[fliplog]` stream (1.29 s at the switch into the first `lanes` segment,
then 0.91 and 0.71 s). Those gaps' `skipped` records are flushed ~25-30 s
late; the run ended before they appeared, so the tail is missing. The
presented-frame evidence is independent and confirms the regression: the same
segments have `show_gap` 699.7, 899.8 and 707.6 ms and 20 skipped flips, while
`today`/`os` never exceed 96 ms. The lane layout does not hang with the fixed
registry, but it makes the title drop presented frames.

c690 per-worker job rate in the same windows (worker = per-host-thread index;
`spu=` is 0 for every c690 job context):

| arm | w0 | w1 | w2 | w3 | w4 | max/min |
|---|---:|---:|---:|---:|---:|---:|
| A `today` | **2,371** | 2,940 | 2,830 | 2,930 | 2,955 | 1.25 |
| B `os` | 2,482 | 3,027 | 2,924 | 3,013 | 3,045 | 1.23 |
| C `lanes` | 2,540 | 3,198 | 3,038 | 2,516 | 2,608 | 1.27 |

The lane-0 deficit survives all three layouts; `os` reduces it slightly and
`lanes` merely moves the low rates to workers 3 and 4. This is a per-host-thread
worker index, not the physical lane slot.

**Run-to-run noise:** `today` measured 30.82 fps here and **31.33 fps** in
launch 2 (same binary, same layout, back-off off). The 0.95 % `today`-vs-`os`
difference is inside that 1.7 % spread, so no layout is a measured winner.
`today` is at least not worse, and it keeps the lanes off the walker/main
cores, so the in-code default stays `today`.

### 4.2 Loading CPU (launch 1, valid this time)

Two 30 s `top -H -b -d 1 -n 30` captures on the game pid, first two samples
discarded. Off = back-off 0 (`06:39:37-06:40:06`); on = back-off 1
(`06:40:07-06:40:36`). The match load started at 06:39:37; the pitch was
reached at 06:40:28, so the last ~9 s of the on capture are the pitch/meter,
not the loader.

| thread | off mean % (max) | on mean % (max) |
|---|---:|---:|
| `spurs pm` x5 | 34.8 (75.8), 23.2 (74.8), 22.9 (78.8), 22.3 (71.8), 18.8 (37.9) | 66.0 (77.7), 59.7 (72.9), 59.5 (72.8), 59.5 (73.7), 59.4 (73.7) |
| `rsx walker` | 34.0 (85.8) | 48.4 (53.8) |
| `ppu main` | 23.7 (87.8) | 24.3 (32.9) |
| `spurs kernel` | 5.8 (11.0) | 14.0 (17.0) |
| `present` | 1.2 (3.0) | 1.3 (3.0) |

**No thread reaches 90 %** in either half; the nearest are `ppu main` 87.8 and
`rsx walker` 85.8 in the off half, both for one 1 s sample during the early
install/rollover. The two halves are sequential, not simultaneous: total CPU
rises from 219 % to 444 % as the load stage changes, and back-off can only
remove work, never add it. The on half's higher SPU numbers are therefore the
later load phase, not the back-off. The loader is not a spin-bound workload
and there is no evidence for enabling the back-off from this sample.

### 4.3 SPU idle back-off at the meter (launch 2)

`today` layout, six 10 s segments per arm, first 2 s dropped:

| arm | back-off | frames | fps | p50 | p90 | p99 | std | walker | wait |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 0 | 1,504 | 31.33 | 34.18 | 43.12 | 50.45 | 10.85 | 18.09 | 13.76 |
| B | 1 | 1,507 | **31.40** | **34.14** | **42.51** | **49.04** | 16.83* | 17.97 | 13.90 |

\* One 540.8 ms frame in the on arm (segment 5, `06:48:58`, n=8114) dominates
the std; every other segment's maximum is 96.6 ms or less and the off arm has
no such event. It is a one-off stall, not reproducible within the run, so the
p50/p90/p99 are the meaningful comparison.

c690 in the same windows: off w0 2,465 with w1-w4 3,081/2,985/3,115/3,155
(max/min 1.28); on w0 2,448 with 3,020/2,969/3,080/3,117 (1.27). The
differences are ~1 % or less. Back-off changes neither frame time nor lane
throughput at the meter; keep it off.

## 5. Failures, fixes and the dry run

The first mission's bugs are fixed in the tree and were revalidated offline
before these launches:

| failure | cause | fix |
|---|---|---|
| `PS3RECOMP_PIN` never active | weak-only reference to `ps3_thread_pin_by_name` did not extract the archive member | non-weak `ps3_thread_pin_link_anchor()`; `main.cpp` takes its address |
| layout name never matches -> re-apply storm | raw `strcmp` against a 32-byte truncated copy | compare the truncated copy with a truncated local |
| segfault after repeated re-apply | `pthread_setaffinity_np` on a freed/reused `pthread_t` from a transient `OTHER` thread | keep only long-lived roles in the registry; `OTHER` applied once, not tracked |
| harness wrote `KEY=v;KEY=v` as one line | `write_arm` did not translate `;` | `write_arm` `tr ';' '\n'`; arms may use either separator |
| run killed at 120 s | the harness stayed in the orchestration tool's process group | run it in the foreground with a >= 15 min tool timeout |

Before launch 1, the exact arm strings were dry-run through the harness and
the real poll code:

* `DRY_RUN=1 ab_live.sh` parses the arms, writes the boot file and every
  scheduled switch file (`NARM * N` of them) and stops before the game. The
  launch-1 output is 12 correctly split two-line files.
* A small offline host test compiles the real
  `runtime/platform/thread_affinity.c` against a stub string getter and calls
  `ps3_thread_pin_poll()` through the same layout sequence. It logs exactly one
  `[pin] layout ->` per effective switch, none for a same-name repeat, and one
  for a malformed `today;PS3RECOMP_SPU_IDLE_BACKOFF=0` value (the old code
  would have looped forever). The run logs match: launch 1 has 12 lines for 12
  switches; launch 2 changes only the back-off key and has 0.
* Synthetic log/marks/top files produced from the dry-run control files were
  fed to `pin_ab.py` and `top_threads.py` to check their parsers before the
  launch.

The harness lives in `recomp-work/ab-tools/ab_live.sh` (not in this repo):
`ARMS`/`N`, `LOAD_SAMPLE`/`LOAD_TOGGLE_ARM`, `find_game_pid`, and `DRY_RUN`.
Analysis: `pin_ab.py` (per-flip `fin` frame times, c690 worker rates) and
`top_threads.py` (per-thread `top -H` means, max and count >= 90 %).

## 6. Decision

1. **Keep the layout default `today`.** `lanes` is rejected (fps and
   presentation hitches). `os` is within noise and does not fix the lane-0
   deficit. Nothing changes in `play_windowed.sh`.
2. **Do not enable `PS3RECOMP_PIN` in the play script from this evidence.**
   The controlled comparison was `today` vs `os`, which only moves the five
   SPU lanes; both pin the walker and ppu main. A future two-arm run should
   compare `PIN=0` with `PIN=1` + `today` (N=6 each) before making pinning a
   default.
3. **Keep `PS3RECOMP_SPU_IDLE_BACKOFF` default-off.** It is neutral at the
   meter and the loader has no 90 % spin thread. If revisited, make the load
   off/on windows cover the same load phase instead of sequential halves.
4. If `lanes` is revisited, the likely problem is not the lane mask itself but
   the `present`/`DisplayFlip` placement on 8/9/11; the 20 skipped flips and
   ~0.7-0.9 s presented gaps point at the present path.

## 7. Reproduce (after a clean build)

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools

# Launch 1: today/os alternating + three lanes, with the load sample.
TAG=pin2a N=1 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
VIDEODRIVER=x11 REPORT=0 SETTLE=10 LIMIT=600 \
ARMS='B=PS3RECOMP_PIN_LAYOUT=os;PS3RECOMP_SPU_IDLE_BACKOFF=0|A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0|B=PS3RECOMP_PIN_LAYOUT=os;PS3RECOMP_SPU_IDLE_BACKOFF=0|A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0|B=PS3RECOMP_PIN_LAYOUT=os;PS3RECOMP_SPU_IDLE_BACKOFF=0|A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0|C=PS3RECOMP_PIN_LAYOUT=lanes;PS3RECOMP_SPU_IDLE_BACKOFF=0|B=PS3RECOMP_PIN_LAYOUT=os;PS3RECOMP_SPU_IDLE_BACKOFF=0|C=PS3RECOMP_PIN_LAYOUT=lanes;PS3RECOMP_SPU_IDLE_BACKOFF=0|A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0|C=PS3RECOMP_PIN_LAYOUT=lanes;PS3RECOMP_SPU_IDLE_BACKOFF=0|A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0' \
BOOT_ARM='PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0' \
LOAD_SAMPLE=1 LOAD_SAMPLE_SECS=30 \
LOAD_TOGGLE_ARM='PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=1' \
./ab_live.sh

# Launch 2: today, SPU back-off off vs on.
TAG=pin2b N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
VIDEODRIVER=x11 REPORT=0 SETTLE=10 LIMIT=600 \
ARMS='A=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0|B=PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=1' \
BOOT_ARM='PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0' \
./ab_live.sh

python3 pin_ab.py --log /home/m/Documents/mlb16-online/recomp-work/ab_live_pin2a_*.log \
    --marks /home/m/Documents/mlb16-online/recomp-work/ab_live_pin2a_*.marks --seg 10 --drop 2
python3 pin_ab.py --log /home/m/Documents/mlb16-online/recomp-work/ab_live_pin2b_*.log \
    --marks /home/m/Documents/mlb16-online/recomp-work/ab_live_pin2b_*.marks --seg 10 --drop 2
python3 top_threads.py /home/m/Documents/mlb16-online/recomp-work/ab_live_pin2a_*/load_*.top
```

The pin banner must be present and must name the layout; if it is absent, the
link anchor has regressed and `PS3RECOMP_PIN` is inert again. A `[pin] layout`
line should appear once per actual layout change, never once per drain pass.

## Caveats

* The `today`-vs-`os` comparison does not isolate pin-on vs pin-off: both pin
  the walker and ppu main. No `PIN=0` arm was run.
* The loading off/on halves are sequential and the later half overlaps the
  pitch; the SPU increase is the load stage, not the back-off.
* `lanes` ran three segments and never wedged, but its last segment ended
  ~10 s before cleanup, so some delayed `skipped` `[fliplog]` records for it
  were never flushed. The presented-frame gaps and skipped-flip counts are the
  reliable part of the regression.
* `c690-stats` reports `spu=0` for every worker, so "worker" here means the
  per-host-thread index, the same identity the earlier profile used.
