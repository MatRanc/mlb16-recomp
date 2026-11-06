# Hitch census: boot, menus, match load and pregame cinematics

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh` / `ab_live.sh`
configuration (`METER=1 SKIP_CINE=0 PIN=1 WCB=2 VIDEODRIVER=x11`, batch-1
switches on, `PS3RECOMP_VK_PACE=1`, `PS3RECOMP_PREPARE_FLIP_RET=1`).

The A/B harness only measured the parked pitch meter, so the startup,
menu, match-load and cinematic hitches were previously sampled from log
gaps and not attributed. This census records the whole path once with a
compact, hitch-only timeline.

## TL;DR

* **824 guest frames > 50 ms** in the 318 s cold run (10,051 flips):
  **67.4 s** of frame time in those frames, **39.9 s** of it above a
  33.3 ms budget. By scene: boot **18 / 5.92 s**, menus **0**, match load
  **1 / 5.11 s**, pregame cinematics **798 / 55.97 s**, pitch meter
  **7 / 0.37 s**.
* **The 5,112 ms match-load gap is first-use shader translation on the
  RSX walker**, not file I/O. Inside the gap the walker is in a drain
  **3.74 s**, of which **2.95 s is our HLSL -> SPIR-V translation**
  (`vk_eng_translate`, 352 spans = VS+FS of 176 pipelines) and **0.25 s
  is `vkCreateGraphicsPipelines`**. Texture uploads are 0.31 s, surface
  creation 0.04 s, c690 0.24 s. File reads are **20 reads / 514 KiB /
  0.16 ms total**, all on `ppu DataStream Process Thread`; **no read is
  on the frame thread or the main PPU thread**.
* **The 237-654 ms boot gaps are SPU asset loading**, not synchronous
  reads and not pipelines. 18 boot frames of 353-589 ms: the display
  thread waits on its flip lwcond, the main PPU thread (`host thread 2`)
  sleeps/polls (`ppu.usleep`, 1,862-2,300 calls per 0.5 s gap), the
  walker is idle, and the six `spurs pm` lanes run SPU jobs (1.2-1.5 s
  lane-union per 0.5 s window). No pipeline is created and the only
  file read is ~40 KiB in ~0.02 ms. The menus themselves had **zero**
  frames > 50 ms in this run.
* **Pregame cinematics are the c690/mesh producer, as at the meter.**
  790 of 798 cinematic hitches are dominated by `c690.job` (79.1 s
  lane-union over the cinematic phase) with the walker drain as the
  parent. Six cinematic hitches are first-use shader translation (0.83 s).
* **A persistent `VkPipelineCache` is not the fix.** It removes
  **289 ms** of driver pipeline creation across the whole cold run
  (322 -> 33 ms) but **none** of the 3.8 s of our own shader
  translation: the 5.1 s load hitch is **5.11 s cold vs 5.26 s warm**,
  with `pipe.create` 248 -> 19 ms and `shader.compile` 2.95 -> 3.01 s.
  Total hitch time in the first 261 s changes by -1.1 %, inside run
  variation. The switch stays default off.
* **The expensive first-use work is our shader translation, not the
  driver pipeline compile.** A disk cache keyed by the translated
  SPIR-V/module would target up to ~3.8 s (5.6 % of all hitch time);
  the top cheap fix is not NHL #128.

## 1. What was recorded

### 1.1 Hitch-only timeline

The normal `PS3RECOMP_TIMELINE` writes every event continuously; the meter
run already produced 218 MB in 45 s, so a whole boot/load/cinematic run
would exceed the 1 GB budget. The timeline gained a hitch mode
(`include/ps3emu/ps3_trace.h`):

```
PS3RECOMP_TIMELINE=<path>          binary PSTL sink (unchanged)
PS3RECOMP_TIMELINE_HITCH_US=50000  only keep frames longer than 50 ms
PS3RECOMP_TIMELINE_RING_MB=16      per-thread ring (one long frame fits)
PS3RECOMP_TIMELINE_FLUSH_MB=2
```

* `ps3_trace_frame_boundary()` is called at the guest flip request
  (`libs/video/cellGcmSys.c:3904`). In hitch mode it keeps the previous
  frame time and, when the new frame exceeds the threshold, queues the
  window `[previous boundary, now]` and emits a `hitch` instant.
* The flusher (`ps3_trace.h:521`, `:675`) writes only records that
  overlap a queued window, then advances the ring to the first record
  after the window. Between hitches it discards records older than the
  current frame boundary, so memory stays bounded and the next long
  frame keeps its start (`ps3_tr_drain_bin_hitch`, `ps3_tr_drop_before`).
* New attribution events (all at the end of `ps3_tr_ev`):
  `pipe.create` (kind + us), `shader.compile` (stage + source bytes),
  `tex.upload` (KiB + mips), `surface.create` (size + format),
  `fs.read` (fd + bytes), `spurs.add_wl` / `spurs.remove_wl`,
  `spu.overlay_verify`, `gcm.resync`, plus the existing `rsx.drain`,
  `c690.job`, `ppu.lwcond`, `ppu.usleep`, `present.*`, `rsx.wcb_done`.
* Hooks: `rsx_vulkan_backend.c:93`, `:4077`/`:4084`;
  `rsx_draw_engine.c:1661`, `:2746`; `runtime/ppu/ppu_fs.cpp:499`;
  `libs/spurs/cellSpurs.c:1474`, `:1696`; `runtime/spu/spu_channels.c:2371`;
  `cellGcmSys.c:1968`.

The overhead is the ring write plus one clock per flip. The cold run's
229 MB timeline was written while the game ran at its normal rate
(10,051 flips in 318 s); the hitch-only form kept the file below the
1 GB budget. Raw timelines are deleted after analysis; the derived CSV
and PNG stay in the run folder.

### 1.2 Runs

| run | tag | cache | result |
|---|---|---|---|
| cold census | `ab_live_hitchcold2_20261103_155504` | empty file, `PS3RECOMP_VK_PIPELINE_CACHE=1` | boot -> menus -> load -> cinematics -> **pitch meter at 264 s**; 20 s meter; 318 s total |
| warm cache | `ab_live_hitchwarm_20261103_160035` | 3,732,523 B file from the cold run | same path to the load, then stayed in cinematics and did not reach the pitch menu within the 600 s poll; 965 s total |
| first attempt | `ab_live_hitchcold_20261103_154407` | no cache | reached the same cinematics but the pitch window was too short; superseded by the run above |

The first attempt also showed that an over-broad `spu.overlay_verify` span
(one per SPU run) produced 66 M records and a 2.3 GB file; the hook is now
only taken when the overlay is actually pending, and that run is not used.

Markers are `CLOCK_MONOTONIC` nanoseconds written by
`recomp-work/ab-tools/hitch_run.py` while it tails the `ab_live.sh` log:
`launch`, `nav_start`, `load_start`, `pitch_reached`, `x_press`,
`meter_up`, `game_exit`.

## 2. The census (cold run)

### 2.1 Frames > 50 ms

| scene | frames | total ms | excess over 33.3 ms |
|---|---:|---:|---:|
| boot (launch -> nav) | 18 | 5,917.4 | 5,317.4 |
| menus (nav -> match load) | **0** | 0.0 | 0.0 |
| match load | 1 | 5,111.5 | 5,078.2 |
| pregame cinematics (+ pitch menu) | 798 | 55,972.2 | 29,372.4 |
| pitch meter | 7 | 366.3 | 132.9 |
| **total** | **824** | **67,367.4** | **39,901.0** |

The "excess" column subtracts a 33.3 ms budget from each frame; it is
the extra wall time the user waits, as opposed to the frame time itself.

### 2.2 Per-hitch dominant cause

Each hitch is assigned the largest *work* category inside its window,
with the walker leaf taken over its parent drain and the six-lane SPU
union divided by six so it is comparable to wall time. This is a
critical-path label, not a sum.

| scene | dominant cause | hitches |
|---|---|---:|
| boot | `spu.run` (asset jobs) | 17 |
| boot | `shader.compile` | 1 |
| match load | `shader.compile` | 1 |
| cinematics | `c690.job` | 790 |
| cinematics | `shader.compile` | 6 |
| cinematics | `rsx.drain` | 1 |
| cinematics | present | 1 |
| pitch | `c690.job` | 7 |

Raw unions (overlapping: `rsx.drain` contains the shader/tex/surface
work; `spu.run` is a union over six lanes):

| scene | cause | count | union ms |
|---|---|---:|---:|
| boot | `spu.run` | 7,437 | 19,113 |
| boot | `rsx.drain` | 1,851 | 68 |
| boot | `shader.compile` | 8 | 25 |
| boot | `tex.upload` | 29 | 14 |
| load | `spu.run` | 13,476 | 6,334 |
| load | `rsx.drain` | 603 | 3,747 |
| load | `shader.compile` | 352 | **2,949** |
| load | `tex.upload` | 348 | 308 |
| load | `pipe.create` | 176 | **248** |
| load | `c690.job` | 2,243 | 240 |
| load | `surface.create` | 63 | 44 |
| load | `fs.read` | 20 | **0.16** |
| cinematics | `spu.run` | 651,164 | 119,640 |
| cinematics | `c690.job` | 556,046 | **79,129** |
| cinematics | `rsx.drain` | 7,581 | 46,424 |
| cinematics | `tex.upload` | 2,687 | 3,191 |
| cinematics | `present.fence_wait` | 801 | 2,487 |
| cinematics | `surface.create` | 888 | 933 |
| cinematics | `shader.compile` | 250 | 831 |
| cinematics | `pipe.create` | 125 | 72 |
| cinematics | `fs.read` | 501 | 172 |
| pitch | `spu.run` | 5,439 | 1,187 |
| pitch | `c690.job` | 4,772 | 865 |
| pitch | `rsx.drain` | 55 | 271 |

### 2.3 Largest hitches

| t (s) | duration | scene | dominant | detail |
|---:|---:|---|---|---|
| 127.0 | 5,111.5 ms | load | `shader.compile` | 2,949 ms shader, 176 pipelines, 20 fs reads/514 KiB |
| 26.9 | 589.3 ms | boot | `spu.run` | 1.53 s lane-union, main usleep 576 ms |
| 151.0 | 582.8 ms | cinematics | `shader.compile` | 439 ms shader, 36 pipelines |
| 28.6 | 518.2 ms | boot | `spu.run` | 1.22 s lane-union |
| 24.1 | 504.1 ms | boot | `spu.run` | 1.30 s lane-union |
| 24.6 | 504.1 ms | boot | `spu.run` | 1.28 s lane-union |
| 28.1 | 408.2 ms | boot | `spu.run` | 1.40 s lane-union |
| 23.2 | 384.6 ms | boot | `spu.run` | 1.46 s lane-union |
| 21.8 | 383.4 ms | boot | `spu.run` | 1.50 s lane-union |

## 3. The 5.1 s match-load gap

The gap is the single frame from 127.0 s to 132.1 s after launch, 11.1 s
after `load_start` (the options screen was confirmed and the match began
loading).

The frame thread (`ppu DisplayFlip`) spends **5.099 s in its flip
lwcond** (563 waits); the main PPU thread (`host thread 2`) spends
**3.788 s in the ref lwcond + 1.147 s in 5,531 `ppu.usleep` calls**.
The walker is the producer that is late:

| walker work inside the gap | spans | union |
|---|---:|---:|
| `shader.compile` (our HLSL -> SPIR-V) | 352 | **2,949.0 ms** |
| `tex.upload` (decode + upload) | 348 | 307.6 ms |
| `vkCreateGraphicsPipelines` (`pipe.create`) | 176 | 247.5 ms |
| `surface.create` | 63 | 43.8 ms |
| `c690.job` (six lanes) | 2,243 | 239.9 ms |
| `spu.run` (six lanes) | 13,476 | 6,333.5 ms |
| `fs.read` | 20 | **0.16 ms** |

The walker's whole drain accounts for 3.75 s of the 5.11 s; the shader
translation is 79 % of that drain and 58 % of the gap. The 176 pipelines
are VS+FS pairs: 352 translations, 8.4 ms mean, 92.8 ms max.

**File I/O is not the stall.** The 20 reads are 513,876 bytes in total,
max 0.02 ms, all on `ppu DataStream Process Thread` (fd 20, the data
streamer). The frame thread and `host thread 2` issue none. This is not
the `cellFsRead` path blocking the main thread; the real PS3 would do
the same streaming asynchronously, and here it is already asynchronous.

## 4. The 237-654 ms boot gaps

The earlier flip-deadline run reported 16 gaps of 237-654 ms "during the
boot/menu scene". The census reproduces them as **18 boot frames of
353-589 ms**, all before the start menu; the menu phase itself has no
frame > 50 ms.

A representative 589 ms boot frame (26.9 s):

| thread | what it did inside the frame |
|---|---|
| `ppu DisplayFlip` | flip lwcond 577.2 ms (63 waits) |
| `host thread 2` (main PPU) | `ppu.usleep` 575.8 ms over 2,300 calls |
| `rsx walker` | idle wait 569.5 ms; drain 0.9 ms; no pipeline |
| six `spurs pm` lanes | `spu.run` 1.53 s lane-union, `spu.policy` 1.53 s |
| file system | 1 read, 40 KiB, 0.02 ms |

The same shape holds for all 18 (1.2-1.5 s SPU lane-union per 0.5 s gap,
1,862-2,300 usleeps). These are the title's asset-loading loop: the main
thread polls and sleeps while SPU jobs decompress/prepare data. There is
no long blocking read (the only read is 40 KiB/0.02 ms), no pipeline
creation (0-1), and the walker is idle. The boot frames are long because
the guest's SPU work takes that long on our SPU runtime, not because we
serialise file I/O.

## 5. Pregame cinematics and pitch

790 of 798 cinematic hitches are `c690.job`-dominated: 556,046 c690 jobs,
79.1 s lane-union, with the walker drain (46.4 s) as the parent. This is
the same six-lane mesh producer that sets the meter's long frames (see
[`frame-timeline.md`](frame-timeline.md) and
[`frame-timeline-2.md`](frame-timeline-2.md)); a cinematic frame is a
mesh batch plus its drain. Six cinematic hitches (0.83 s of shader
translation) are first-use pipelines, and one 583 ms one is dominated by
shader translation.

At the pitch meter the same census finds 7 frames > 50 ms in 20 s
(366 ms total), all c690-dominated, which is consistent with the steady
meter work in `frame-timeline.md`.

## 6. Persistent `VkPipelineCache` (NHL #128) cold vs warm

Implemented behind `PS3RECOMP_VK_PIPELINE_CACHE` (default **off**):
`libs/video/rsx_vulkan_backend.c:444` loads a cache file, all four
pipeline creation sites use it (`:1302`, `:1645`, `:4241`, `:4580`), and
`vk_pipe_cache_tick` saves it after a creation at most every 2 s (so a
killed run still leaves a file). The filename is keyed by the physical
device's `pipelineCacheUUID` and `driverVersion`:
`recomp-work/vk_pipe_cache_<device>_<driver>_<uuid>.bin` (3.6 MB cold).

Same binary, same path, empty file vs the cold run's file:

| metric | cold | warm |
|---|---:|---:|
| 5.1 s load frame | 5,111.5 ms | 5,262.2 ms |
| `shader.compile` in it | 2,949.0 ms | 3,006.7 ms |
| `pipe.create` in it | **247.5 ms** (176) | **18.9 ms** (176) |
| first 261 s, hitches >50 ms | 815 / 66,885 ms | 781 / 66,161 ms |
| first 261 s, `pipe.create` | 321.7 ms | 32.6 ms |
| first 261 s, `shader.compile` | 3,786 ms | 3,988 ms |

The cache removes **92 % of driver pipeline-creation time** and
**289 ms of the ~67 s** hitch time (0.4 %). It does not remove a single
hitch: the load frame is unchanged within run-to-run noise, because 2.95 s
of it is our own `vk_eng_translate` (HLSL -> SPIR-V), which a
`VkPipelineCache` cannot store. The warm run's total hitch time in the
first 261 s is 1.1 % lower, inside the variation from the extra 25 boot
hitches.

**First-use accounting.** 22 of the 824 hitches contain a pipeline
create; 305 pipelines are built in the cold run, 301 of them in two
frames (176 in the load, 125 in one cinematic frame). Their driver cost
is 322 ms total. The shader translation beside them is 3.8 s total.
A persistent pipeline cache therefore removes ~0.3 s, not the hitches;
an on-disk cache of our translated SPIR-V/modules would target up to
~3.8 s and still leave the c690 cinematic frames.

The switch is not added to the play scripts. A cold cache file is left in
`recomp-work/` from the experiment; it is a normal cache file and may be
deleted.

## 7. What this means for the backlog

* **Match-load hitch:** cache translated shader modules (or the SPIR-V)
  on disk, keyed by the guest program + state. That is the only way to
  remove the 2.95 s. Pipeline cache is already done and is not enough.
* **Boot gaps:** the SPU asset jobs are the cost. Speeding the SPU
  runtime (or the job modules) shortens them; changing file I/O will not.
* **Cinematic/pitch hitches:** the c690/mesh producer and walker drain
  are the same lever as the meter, not a startup-specific bug.
* **Menus:** no >50 ms frames in this run. The "menu lag" report should
  be re-checked in the windowed view; the census does not reproduce it.

## 8. Reproduction

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp

# one full path, hitch-only timeline (cache on; delete the file for cold)
rm -f /home/m/Documents/mlb16-online/recomp-work/vk_pipe_cache_*.bin
cd /home/m/Documents/mlb16-online
TAG=hitchcold2 PITCH_WAIT_S=600 LIMIT=1000 \
  PS3RECOMP_TIMELINE=/tmp/hitch_tl_cold2.bin \
  python3 recomp-work/ab-tools/hitch_run.py

python3 recomp-work/ab-tools/analyse_hitches.py \
  --timeline /tmp/hitch_tl_cold2.bin \
  --log recomp-work/ab_live_hitchcold2_20261103_155504.log \
  --markers /tmp/hitch_markers_cold2.txt \
  --out recomp-work/ab_live_hitchcold2_20261103_155504
```

`hitch_run.py` and `analyse_hitches.py` are in `tools/` in this repo; the
run copies used here are under `recomp-work/ab-tools/`. The second launch
uses the same command with `TAG=hitchwarm` and the cache file present.

## 9. Artifacts

* `recomp-work/ab_live_hitchcold2_20261103_155504/hitch_timeline.png` —
  whole cold run, frame time (log scale) with scene bands and the largest
  hitches annotated by cause.
* `recomp-work/ab_live_hitchcold2_20261103_155504/load_phase_causes_cold_warm.png`
  — 5.1 s load phase work, cold vs warm pipeline cache.
* `recomp-work/ab_live_hitchcold2_20261103_155504/hitches.csv` — one row
  per hitch (time, duration, scene, dominant cause, fs/pipeline counters).
* `recomp-work/ab_live_hitchcold2_20261103_155504/causes.csv` — by scene
  and cause, count and clipped union.
* `recomp-work/ab_live_hitchwarm_20261103_160035/` — the warm-cache run.
* Raw `PSTL` timelines (`/tmp/hitch_tl_cold2.bin`, 229 MB;
  `/tmp/hitch_tl_warm.bin`, 804 MB) are analysis inputs and were deleted
  after the derived files were produced.

## Caveats

* One cold census launch and one warm launch; the cold run is the
  census. The warm run did not reach the pitch menu (it stayed in
  cinematics for ~800 s), so the cache comparison uses the first 261 s,
  which both runs followed identically. This is a real deviation, not a
  cache behaviour claim; the cold run reached the meter at 264 s.
* Unions in the cause table overlap and are not additive: `rsx.drain`
  contains shader/tex/surface work, and `spu.run` is a union over six
  lanes. The per-hitch dominant cause in §2.2 is the comparable metric.
* Scene boundaries come from log markers with a 0.2 s poll; the pitch
  menu is grouped with cinematics, and the 5.1 s load frame defines the
  load/cinematic split.
* File names are not in the timeline; `fs.read` carries fd and bytes. The
  log's `[fs] open ... -> fd N` lines map the fds; in the load gap fd 20
  is the data streamer's read of the WAD data.
* The timeline's own overhead (ring writes plus one clock per flip) is
  included in the measured frames. It is small; the load gap is identical
  in both runs and the frame rate matches the non-timeline runs.
