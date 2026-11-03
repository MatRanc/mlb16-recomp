# Thread placement (`PS3RECOMP_PIN_LAYOUT`) and SPU idle back-off

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Runs use the standard `play_windowed.sh`
configuration (`TEX_DIRTY=1`, `PIPE_MEMO=1`, the four cheap wins, WCB=2,
CUBE_RT=1, PACE=1) plus `METER=1`, `SKIP_CINE=1`, `PIN=1`, x11. Per-flip frame
times come from `PS3RECOMP_FLIP_LOG` `fin`, not the every-10th `[flip]`
decimation.

This mission implemented the layout switch and the SPU idle back-off, and used
three launches (the cap). **It did not produce a clean A/B**: one run was lost
to the orchestration timeout, the layout-B arm was the first live re-apply and
the guest stopped flipping, and the last run's load sampler was contaminated
by a control-file quoting bug in the harness plus a registry bug now fixed in
the tree. The numbers that are valid, and exactly why the rest are not, are
below. No winner was measured, so neither `play_windowed.sh` was changed.

## TL;DR

* **`PS3RECOMP_PIN` was silently a no-op in `build-play` until this mission.**
  `thread_name.h` reaches `ps3_thread_pin_by_name()` through a weak reference,
  and a weak reference does not make the static linker extract
  `runtime/platform/thread_affinity.c.o`; `nm build-play/MyGameRecomp` had no
  `ps3_thread_pin_*` symbol and no run log ever contained a `[pin]` banner. The
  fix is a non-weak `ps3_thread_pin_link_anchor()` in the same object, whose
  address `templates/project/main.cpp` takes. Every `[pin]` number here is from
  a binary that actually pins.
* **Layouts a/b/c are implemented and live-switchable** through the existing
  `PS3RECOMP_CTL` file (`PS3RECOMP_PIN_LAYOUT=today|lanes|os`). The lane
  layout on this topology is exactly the proposed one: walker 0, main 1,
  lanes 2/3/4/5/10, present 8, DisplayFlip 9, spurs kernel 11, light 8,9,11.
* **One valid meter segment (layout a, pinned): 243 frames, 30.38 fps,
  p50 34.25 / p90 44.91 / p99 50.21 ms, std 11.06, walker drain 18.59,
  walker wait 14.19 ms.** c690 was 2,451 jobs/s on worker 0 vs 3,025-3,184 on
  workers 1-4 (max/min 1.30), the same lane-0 deficit the profile saw.
* **Layout b stopped flipping on the first live switch** (14 s, no flip) and
  the run was stopped. That observation is **confounded**: the same run used
  the first version of the registry, which also kept transient guest threads
  by `pthread_t`; re-applying to a recycled/freed `pthread_t` is a
  use-after-free (reproduced offline as a segfault). The registry now keeps
  only long-lived roles (+ lane workers), so b needs a retest; do not read the
  wedge as "lane pinning is illegal" yet.
* **SPU idle back-off is implemented behind `PS3RECOMP_SPU_IDLE_BACKOFF=1`**
  (also a live key) for the GETLLAR poll path, which had none. It was not
  measured: the only load sample was invalid (see below).
* **The loading CPU sample is invalid.** The last run's control file was
  written with a `NAME=value;NAME=value` line instead of two lines, so the
  layout string never matched and the walker re-applied affinities on every
  drain pass: **107,588 `[pin] layout` lines**. `top` showed `rsx walker` at
  81.6 % (10/14 samples >= 90 %), which is the re-apply storm, not guest work.
  Do not conclude anything about what spins during loading from it.

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

The lane index comes from a new `ps3_thread_pin_spurs_lane(slot->spu_num)` call
in `spurs_pm_worker`; all six workers share the thread name `spurs pm`, so the
name alone cannot identify them.

### 1.3 Live switching

* `ps3_live_ctl` now has a string-valued key (`PS3RECOMP_PIN_LAYOUT`) and
  `ps3_live_ctl_get_str()`.
* The walker calls `ps3_thread_pin_poll()` next to `ps3_live_ctl_poll()` at the
  top of a drain pass. It compares the (truncated) layout name with the last
  applied one and, on a change, rebuilds the masks and re-applies them to the
  registered long-lived threads with `pthread_setaffinity_np`.
* Only `walker`, `ppu main`, `spurs pm` lane workers, `present`,
  `ppu DisplayFlip` and `spurs kernel` are registered. `OTHER` threads get
  their mask once at creation and are not tracked; a transient guest thread's
  `pthread_t` can be freed and reused, and re-applying to it is a
  use-after-free. This was not theoretical: the malformed-name run ended in a
  segfault.

## 2. SPU idle back-off (`PS3RECOMP_SPU_IDLE_BACKOFF=1`)

The GETLLAR handler in `runtime/spu/spu_channels.c` now, when the option is on,
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

**Not measured.** The task was to first name the threads at 100 % during the
matchup load and only then decide; the only sample is invalid (section 4.3),
and the run crashed before the meter arms. The switch is left default-off.

## 3. Runs

| run | tag | result |
|---|---|---|
| 1 | `ab_live_pinload1_20261103_061420` | killed at 120 s by the orchestration tool (the backgrounded script stayed in the tool's process group); no meter, no data |
| 2 | `ab_live_pin1_20261103_061639` | complete to the meter; layout a one segment; switch to layout b at flip 10550 -> no flips for 14 s -> `rc=7` |
| 3 | `ab_live_pin2_20261103_062206` | control file malformed (`;`); re-apply storm; load samples invalid; game segfaulted at 06:24:39; pitch not reached |

Launch 2 reached the meter 159 s after launch (nav 06:16:59-06:18:39,
cinematics skipped 06:19:10, X 06:19:17, meter 06:19:18).

## 4. Numbers

### 4.1 Layout a at the meter (launch 2, the only valid segment)

10 s segment, first 2 s dropped (`pin_ab.py`, 243 per-flip intervals):

| metric | value |
|---|---:|
| frames | 243 |
| fps | **30.38** |
| p50 / p90 / p99 | **34.25 / 44.91 / 50.21 ms** |
| mean / std | 33.02 / 11.06 ms |
| walker drain (`walker=`) | 18.59 ms |
| walker wait (`wait=`) | 14.19 ms |

This is the first valid pinned meter measurement in the tree (the pin switch
was inert before the link-anchor fix), but it is **one segment of one launch**
and cannot be compared with the older unpinned runs.

c690 per worker in the same window (worker = per-host-thread index; the `spu=`
field is 0 for every c690 job context, so the worker index is the stable
identity):

| worker | jobs/s |
|---:|---:|
| 0 | **2,451** |
| 1 | 3,184 |
| 2 | 3,025 |
| 3 | 3,167 |
| 4 | 3,162 |

max/min = 1.30. The profile's repeated lane-0 deficit survives the `today`
pin (all lanes share the same mask there).

### 4.2 Layout b (launch 2)

`[live-ctl] flip=10550 PS3RECOMP_PIN_LAYOUT=lanes`, then the guest produced no
flip, no walker report and no wait line for 14 s; `ab_live.sh` stopped with
`rc=7`. The log ends at the switch. Because the re-apply in that binary also
walked transient `OTHER` threads, a recycled `pthread_t` could have been
pinned to a single lane CPU; that is the leading hypothesis and it is now
fixed by not tracking `OTHER`. **Retest b before rejecting it.**

### 4.3 Loading CPU (launch 3): invalid

The harness `write_arm` wrote the arm body as one line, so the control file
contained `PS3RECOMP_PIN_LAYOUT=today;PS3RECOMP_SPU_IDLE_BACKOFF=0` and the
layout string never matched the stored 32-byte copy. The walker re-applied
affinities every drain pass (107,588 `[pin] layout` lines). The `top -H -b -d 1
-n 15` samples from that window:

| thread | load_a mean %CPU | load_b mean %CPU |
|---|---:|---:|
| `rsx walker` | 81.6 (10/14 samples >= 90) | 12.8 |
| `spurs pm` (each) | 0.1-2.5 | 0.0 |
| `ppu main` | 3.8 | 0.0 |
| `present` | 0.2 | 0.0 |

The walker number is the re-apply storm (and the `fprintf` flood it caused),
not guest work. `load_a` and `load_b` are the same configuration (the semicolon
made the `SPU_IDLE_BACKOFF` assignment part of the layout value), and the game
segfaulted immediately after `load_b`. **No conclusion about loading CPU or
about which threads are SPU poll loops can be drawn from this mission.**

## 5. Failures and fixes (all offline, no extra launches)

| failure | cause | fix |
|---|---|---|
| `PS3RECOMP_PIN` never active | weak-only reference to `ps3_thread_pin_by_name` did not extract the archive member | non-weak `ps3_thread_pin_link_anchor()`; `main.cpp` takes its address |
| layout name never matches -> re-apply storm | raw `strcmp` against a 32-byte truncated copy | compare the truncated copy with a truncated local |
| segfault after repeated re-apply | `pthread_setaffinity_np` on a freed/reused `pthread_t` from a transient `OTHER` thread | keep only long-lived roles in the registry; `OTHER` applied once, not tracked |
| harness wrote `KEY=v;KEY=v` as one line | `ab_live.sh`'s `write_arm` did not translate `;` | `write_arm` now `tr ';' '\n'`; arms may use either separator |
| load sampler found no pid | the runtime renames the boot thread, so `/proc/PID/comm` is `ppu main`, not `MyGameRecomp` | match `/proc/PID/cmdline` argv[0] against the exe path (mangohud `exec`s the game with that argv), comm as fallback |
| run 1 killed at 120 s | the harness was left in the orchestration tool's process group | run it in the foreground with a >= 10 min tool timeout (this environment forbids `nohup`/`setsid`/`&`) |

The updated harness lives in `recomp-work/ab-tools/ab_live.sh` (not in this
repo): `ARMS` multi-arm rotation, `LOAD_SAMPLE`/`LOAD_TOGGLE_ARM`, and
`find_game_pid`. Analysis: `recomp-work/ab-tools/pin_ab.py` (per-flip frame
times, c690 worker rates) and `top_threads.py`.

## 6. Next (needs a fresh launch budget)

1. **Retest layout b** with the fixed registry. If it wedges again, try
   giving `OTHER` threads the walker/main siblings (6,7) as well; the current
   b puts every non-lane thread on three logical CPUs.
2. **Retest the layout A/C A/B** with `ARMS` and newline (or now
   semicolon-)separated bodies; use `pin_ab.py` for p50/p90/p99 and the c690
   worker rates.
3. **Loading CPU**: run the fixed `LOAD_SAMPLE=1` (the pid match now works) and
   only then decide about `PS3RECOMP_SPU_IDLE_BACKOFF`. If the 100 % threads
   are `spurs pm`, enable the back-off and compare a 15 s baseline with a 15 s
   on window via `LOAD_TOGGLE_ARM`; then A/B it at the meter.
4. Only a measured layout that does not raise p99/stddev should go into
   `play_windowed.sh`; nothing was added.

## 7. Reproduce (after a clean build)

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools

# layouts a/b/c in one live run, plus a two-half load sample
TAG=pin \
ARMS='A=PS3RECOMP_PIN_LAYOUT=today|B=PS3RECOMP_PIN_LAYOUT=lanes|C=PS3RECOMP_PIN_LAYOUT=os' \
BOOT_ARM='PS3RECOMP_PIN_LAYOUT=today' \
LOAD_SAMPLE=1 LOAD_SAMPLE_SECS=15 \
LOAD_TOGGLE_ARM='PS3RECOMP_SPU_IDLE_BACKOFF=1' \
METER=1 SKIP_CINE=1 WCB=2 PIN=1 VIDEODRIVER=x11 \
N=4 SEG=10 DROP=2 SETTLE=10 REPORT=0 ./ab_live.sh

python3 pin_ab.py --log /home/m/Documents/mlb16-online/recomp-work/ab_live_pin_*.log \
    --marks /home/m/Documents/mlb16-online/recomp-work/ab_live_pin_*.marks \
    --seg 10 --drop 2
python3 top_threads.py /home/m/Documents/mlb16-online/recomp-work/ab_live_pin_*/load_*.top
```

The pin banner in the log now exists and names the layout; if it is absent,
the link anchor has regressed and `PS3RECOMP_PIN` is inert again.

## Caveats

* One valid meter segment, one launch. The p50/p99 are 243-frame estimates
  (`pin_ab.py` uses `fin`, so they are true per-flip values).
* Layout b is a single failed observation and is confounded by the
  `pthread_t` registry bug; it is not proven wrong.
* The loading sample is invalid; `SPU_IDLE_BACKOFF` is implemented but
  unmeasured and default-off.
* `c690-stats` reports `spu=0` for every worker, so "lane" here means the
  per-host-thread worker index, the same identity the earlier profile used.
