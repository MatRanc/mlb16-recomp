# Walker per-draw: the profiler was a real cost, the remap cache was not

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Offline: `replay_rsx_engine` on the existing
`recomp-work/meter_frame` capture, interleaved runs, `REPLAY_NULL_HASH=0`,
pinned with `taskset -c 2`. Live: two pitch-meter A/Bs (`ab_live.sh`, X once,
WCB=2, PIN=1, x11, N=6 x 10 s, 2 s dropped). Follows
[`wcb-async.md`](wcb-async.md), [`walker-per-draw.md`](walker-per-draw.md),
[`c690-serialisation.md`](c690-serialisation.md) and
[`nhl-lessons.md`](nhl-lessons.md) (#62).

## TL;DR

* **The walker profiler was on in the play defaults and cost ~1.6 us/draw.**
  `play_windowed.sh:101` and `ab_live.sh:255` export
  `PS3RECOMP_WALKER_PROF=1`. On the replay, enabling it moves the meter frame
  **8.12 -> 8.92 ms (0.80 ms/frame, +1.6 us/draw at 502 draws)**, matching the
  live profile's ~2.2 us/draw (the replay has no Vulkan recording to time).
  Most of it is the FIFO loop's per-method `clock_gettime` pair in
  `cellGcmSys.c:3532,3682`, not the per-draw phases, so sampling *draws* did
  nothing.
* **`PS3RECOMP_WALKER_PROF_SAMPLE` now samples whole frames, default 8.**
  Every drain of a selected frame is timed; unselected frames only count
  draws. `[walker-prof]` lines are kept (one per 8 frames), `draws=` is the
  exact per-frame count and `walker=`/`wait=` are the same averages as before
  (phase sums are scaled by frames-since-report / sampled frames). Replay:
  **8.92 -> 8.33 ms/frame with the lines**, leaving ~0.2 ms/frame of
  instrumentation (8.12 with the profiler off). `PS3RECOMP_WALKER_PROF_SAMPLE=1`
  restores the old every-frame behaviour.
* **`rsx_vertex_remap_build` is unique-vertex dedup for the GPU path.** The
  engine converts each unique reference once and draws an index buffer that
  maps occurrences to uniques (NHL #62). A draw that is already ascending and
  single-base skips it; a dynamic index stream does not. `PS3RECOMP_REMAP_DIRECT`
  (live key `PS3_LIVE_REMAP_DIRECT`, **default off**) dedups a dense
  single-base list through a direct slot `(vertex_id - min) & mask` instead of
  the hash and probe. It is exact, but **neutral live**: two A/Bs disagree in
  sign (+0.72 then -0.86 segment-mean fps, both CIs cover 0, p50 34.16-34.20
  both arms). Offline it is also inside noise (-0.2 to +0.1 ms/frame across
  batches).
* **The memmove 6.5 % is vertex/index/ring staging, not one trivial copy.**
  The largest is the GPU path's raw-span copy
  (`rsx_vulkan_backend.c:5142`, `span * stride` per non-contiguous draw),
  then the rebuilt u32 index buffer (`:5151`) and the remap array (`:5144`).
  No safe cut was made; trimming the raw copy to the referenced
  `[min,max]` element range is exact and is the follow-up candidate.
* **Nothing is default-on except the profiler sampling** (pure
  instrumentation, measured cheaper). The remap key stays off.

## 1. The profiler overhead

### 1.1 It was on, and where it goes

Both play scripts set it:

| script | line | setting |
|---|---|---|
| `recomp-work/play_windowed.sh` | 101 | `export PS3RECOMP_WALKER_PROF=1` |
| `recomp-work/ab-tools/ab_live.sh` | 255 | `export PS3RECOMP_PUMP_MS=8 PS3RECOMP_WALKER_PROF=1` |

There are two timestamp sites:

1. **Per method, in the FIFO loop.** `cellGcmSys.c:3532` and `:3682` take
   `rsx_wprof_now()` before and after every `rsx_draw_engine_method(s)` call
   when the profiler is on. A draw is many methods, so this is several clock
   pairs per draw.
2. **Per draw, in the engine.** `sink_end_work` (`rsx_draw_engine.c:6503`)
   times the fetch, pipeline, texture, bind and draw phases; `sink_end`
   (`:6848`) times the whole group and the main/mesh split.

The live `perf` profile put the clock path at ~6.7 % of walker samples
(~2.2 us/draw, [`wcb-async.md`](wcb-async.md) §6). The replay isolates the
same calls without Vulkan recording:

| replay arm (200 frames, 20 warm-up) | median ms/frame | us/draw @502 |
|---|---:|---:|
| profiler off | **8.12** | 16.18 |
| profiler on, every frame (`SAMPLE=1`) | 8.92 | 17.77 |
| profiler on, 1 frame in 8 (`SAMPLE=8`, new default) | 8.33 | 16.59 |
| profiler on, 1 frame in 16 | 8.16 | 16.25 |

Medians of 5 interleaved runs; the run-to-run spread is +/-0.05 ms.
Sampling *draws* (the first implementation tried) was neutral because the
per-method FIFO timestamps still ran for every method of every draw.

### 1.2 The change

`rsx_wprof_drain_begin()` (`rsx_draw_engine.c:687`) decides once per drain
whether the **frame** is profiled:
`(frame_sequence % PS3RECOMP_WALKER_PROF_SAMPLE) == 0`, where the sequence is
advanced by `rsx_wprof_note_flip()`. `cellGcmSys.c:2906` uses the return value
to gate every per-method timestamp; `sink_end_work` gates every per-draw
timestamp on the same pass. `rsx_wprof_drain_end(profiled)` (`:746`) only
reports on a profiled pass, and scales the phase/drain sums by
(frames since the last report / sampled frames) before dividing by flips.

The report keeps its shape:

* `draws=`, `src_main=`, `src_mesh=` are counted on **every** frame and are
  exact per-frame averages;
* `walker=`/`wait=` are the same per-frame averages (drain and wall time);
* `fetch/tex/pipe/bind/draw/other` are measured on the sampled frame and
  scaled to the interval, so they still sum to `engine` in expectation;
* `n/fetch`, `n/tex`, ... are scaled the same way.

The line rate is ~4/s at 30 fps (1 per 8 frames). The analysis tools read
`draws=`, `walker=`, `wait=` and the phases, all of which keep their
meaning. Set `PS3RECOMP_WALKER_PROF_SAMPLE=1` for the old exact-every-frame
profile (it costs the old ~1.6 us/draw on the replay).

### 1.3 What the first live A/B exposed

The first implementation sampled **drains** and kept one global "this drain is
profiled" flag. The live log then alternated full reports with
`engine=0.000ms` reports that still had draws: the FIFO and the flip are
handled by different drain passes/threads, so one thread's decision leaked
into the other's report and the walker column was a mix of full and empty
lines. The replay (one thread) did not show it. The frame-sequence design
above removes the mix: every drain of a frame agrees, and a non-profiled pass
never reports. The fix is verified on the replay (11 lines for 80 frames,
zero `engine=0.000ms` lines with draws); the two-launch budget was already
spent on the remap key, so it is not live-verified.

## 2. `rsx_vertex_remap_build`: what it does and what was tried

### 2.1 Why it runs every draw

`dc_fetch` (`rsx_draw_engine.c:5155`) turns each index/array run into a list of
`rsx_vertex_ref { vertex_id, base_index }`, then calls
`rsx_vertex_remap_build` (`:5351`, implementation at
`rsx_vertex_compact.c:888`) when the draw is rebuilt through an index buffer
and `PS3RECOMP_ENG_VTX_REMAP` is on (the default). The build:

* allocates a generation-tagged open-addressing table sized to the draw;
* walks the reference list once, hashing `(base_index, vertex_id)`;
* rewrites `refs` in place with the first-use unique prefix and fills
  `occurrence_to_unique[occurrence]`.

The result is what the rest of the path needs: the CPU (or the GPU compute
unpack) converts **each unique vertex once**, and the rebuilt index buffer
(`rsx_draw_engine_write_topology_indices`, called at `:6584`) maps every
occurrence to its unique slot. This is the NHL #62 shape; without it a shared
vertex would be converted once per triangle.

The cheap cases are already there:

* a `draw_arrays` or an ascending single-base index run returns immediately
  (every reference is unique, no table is touched);
* `PS3RECOMP_REMAP_FAST` (play default) answers a nearby repeat from a
  1024-entry direct-mapped memo before probing the big table.

The live cost is the remaining dynamic streams: the title rebuilds its index
streams every frame, so the whole-list memo from
[`walker-per-draw.md`](walker-per-draw.md) misses, and the hash/probe runs.

### 2.2 The alternative: direct slots (`PS3RECOMP_REMAP_DIRECT`)

When every reference in the draw has the same `base_index` and the vertex ids
span at most 256K values, `(vertex_id - min) & mask` is one-to-one inside the
span. The build then writes/reads one slot per occurrence and needs no key
comparison: a live slot with the current generation can only be the same
reference. The table is sized to `max(2 * count, span)` in that mode; a list
that fails either test (mixed base or a longer span) keeps the hash path. The
code is `rsx_vertex_compact.c:918-990`, gated by
`PS3_LIVE_REMAP_DIRECT` / `PS3RECOMP_REMAP_DIRECT`, default **0**.

Exactness: the direct answer is the same first-use collapse, so the unique
prefix and the occurrence map are identical. The fixture
`libs/video/tests/test_vertex_remap.c` builds a 600-reference dense
single-base list and a sparse list that must fall back, with the key off and
on, and compares the compacted references and every occurrence index. The
replay gate is identical with the key on:

| replay | last hash | 64-frame gate |
|---|---|---|
| key off | `a6fcc849341eba9b` | `ff2a220c28750bc3` |
| key on | `a6fcc849341eba9b` | `ff2a220c28750bc3` |

`PS3RECOMP_ENG_VTX_REMAP=0` (draw the duplicates, no dedup) was rejected
before implementing this: it changes the backend draw stream on the replay
(`d4b9abb3c954fd3c` / `08a362c41b285b83`), even though the rasterised image is
the same. The mission gate requires identical replay hashes.

### 2.3 Offline result

Interleaved 200-frame runs, meter frame, play defaults
(`PIPE_MEMO=1 REMAP_FAST=1`), profiler off:

| batch | key off (ms/frame) | key on (ms/frame) | delta |
|---|---:|---:|---:|
| 1 (7 runs) | 8.18 | 8.05 | **-0.13 (-1.6 %)** |
| 2 (7 runs) | 8.34 | 8.61 | +0.27 (mixed micro+direct) |
| 3 (5 runs, direct only) | 8.34 | 8.41 | +0.07 |

The sign is not stable across builds/batches; the best case is ~-0.25 us/draw,
the worst ~+0.5 us/draw. The live profile's 5.3 us/draw is the function's
share of the walker, so even a 5 % cut would be ~0.25 us/draw: at the edge of
what this measurement can resolve.

## 3. Live pitch-meter A/B

Two launches, same command, X once, meter held before the swing. The key is an
environment variable, so the two arms are the same binary:

```
A = PS3RECOMP_REMAP_DIRECT=0
B = PS3RECOMP_REMAP_DIRECT=1
```

### 3.1 Run 2 (`ab_live_remapdirect2_20261104_172314`)

`pin_ab.py --seg 10 --drop 2`:

| metric | A off | B direct |
|---|---:|---:|
| fps, pooled frames | 32.31 | 32.25 |
| fps, segment mean (ab_live) | 32.27 +/- 0.97 | 33.13 +/- 0.67 |
| paired segment difference | **-0.86 fps, CI [-2.16, +0.45]** | |
| p50 / p90 / p99 ms | 34.20 / 40.54 / 46.22 | 34.16 / 39.89 / 44.64 |
| walker / wait ms (line mean) | 23.53 / 22.64 | 21.64 / 22.00 |
| frames | 1,551 | 1,548 |
| c690 lane jobs/s (lanes 0-4) | 3104-3209 | 3023-3099 |
| draws/frame, profiled-line median | 599.7 | 552.5 |
| walker us/draw, profiled-line median | 39.6 | 45.3 |

Segment 11 (B) contains an unrelated 98 ms-std present stall; the segment-mean
fps is inflated by the 32-34 fps segments in B, while the pooled-frame fps is
dragged by that stall. p50 is flat (34.20 vs 34.16), as the meter is c690-bound.
The arms sampled different draw counts (600 vs 553 per profiled line), so the
raw walker comparison is not trustworthy either way; the draw-normalised
medians say B is *worse* (45.3 vs 39.6 us/draw), the opposite of the raw line
mean. That disagreement is the result: no effect this measurement can resolve.

### 3.2 Run 1 (`ab_live_remapdirect_20261104_171207`)

Same command, taken with the first (drain-sampled, globally flagged) profiler,
so its `[walker-prof]` lines mix full and empty reports. The frame-level
numbers are still from `[flip]`:

| metric | A off | B direct |
|---|---:|---:|
| fps, segment mean | 32.95 +/- 0.76 | 32.23 +/- 1.15 |
| paired segment difference | **+0.72 fps, CI [-0.11, +1.56]** | |
| p50 / p90 / p99 ms | 34.16 / 40.15 / 46.11 | 34.16 / 39.89 / 45.59 |
| walker us/draw, profiled-line median | 44.3 | 45.7 |

Run 1 says B is 0.72 fps slower, run 2 says 0.86 fps faster; both CIs cover 0,
p50 is identical in both. **Verdict: neutral. Keep `PS3RECOMP_REMAP_DIRECT=0`.**

## 4. The memmove 6.5 %: which copies

From the live per-draw path (`vk_eng_draw_from`,
`rsx_vulkan_backend.c:5016-5270`), per batched draw:

| copy | code | bytes | note |
|---|---|---|---|
| raw vertex span -> ring | `:5142` | `span * src_stride` | biggest; `span` is the attribute array's declared range, not the unique count |
| remap indices -> ring | `:5144` | `unique * 4` | GPU unpack's element map |
| rebuilt index buffer -> ring | `:5151` | `occurrence * 4` | occurrence -> unique |
| PS constants | `:5127` | `(nslots+1)*16` | small |
| VS constants | `:5126` -> `:4340` | used rows + tail | ranged; the 8 KB block is not copied |
| non-batched fallbacks | `:5149,5218,5221` | draw size | live uses the batched ring |

The FIFO method words are read, not copied; the texture upload rows and WCB
readback rows are separate paths. The raw-span copy is the one worth cutting,
but the title's vertex ranges are the mesh's own range, so the win is only
where a draw uses a subrange of a larger array. The exact cut is to shift
`raw_data` and the remap indices by the referenced `min` element and size the
copy `(max - min + 1) * stride`; it is not in this build because the null
replay does not exercise the GPU path, so it would ship without a gate. It is
the first follow-up on this file.

## 5. User command: zoomed-out replay test

Boot to a match, enter instant replay, zoom out until the whole field is on
screen, and record 2-3 minutes per arm. The remap key is an environment
variable; `PS3RECOMP_RSX_WCB_PREFETCH=1` can be added to either arm (it is a
different key and composes).

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A: play defaults, remap key off. 40 ms hitch recording on.
PS3RECOMP_TIMELINE=/tmp/remap_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_REMAP_DIRECT=0 \
./play_windowed.sh

# Arm B: same shot, remap key on (add PS3RECOMP_RSX_WCB_PREFETCH=1 to combine).
PS3RECOMP_TIMELINE=/tmp/remap_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_REMAP_DIRECT=1 \
./play_windowed.sh
```

`play_windowed.sh` already sets `PS3RECOMP_RSX_WCB=2`,
`PS3RECOMP_RSX_WCB_STATS=60` and the profiler. Compare:

```sh
cd /home/m/Documents/mlb16-online
for f in /tmp/remap_[AB]_*.pstl; do
  echo "== $f"
  python3 mlb16-recomp/tools/hitch_table.py "$f" \
    --min-ms 40 --bins --classify --baseline-jobs 680 --top 20
done
grep -aE '\[walker-prof\].*draws=[1-9]' \
  recomp-work/play_*/play_*.log | tail -80
```

What to look for: `draws=` unchanged between arms, `walker=` lower in B only
if the crowded replay's index streams are denser than the meter's, and the
80-250 ms band of `hitch_table.py` shrinking. The meter says not to expect a
frame win; the crowded replay has ~1,570 draws/frame, so a 0.2-0.5 us/draw
change would be 0.3-0.8 ms/frame there.

## 6. Reproduce

```sh
# unit fixtures (remap direct + all video fixtures)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_video_fixtures.py
clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
  -o /tmp/test_vertex_remap libs/video/tests/test_vertex_remap.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c && /tmp/test_vertex_remap

# replay gate (must print a6fcc849341eba9b / ff2a220c28750bc3 in both arms)
cd /home/m/Documents/mlb16-online/recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
PS3RECOMP_REMAP_DIRECT=1 ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame \
  --frames 64 --warmup 10

# profiler cost / sampling (median of 5, interleaved, pinned)
for arm in "PS3RECOMP_WALKER_PROF=0" \
           "PS3RECOMP_WALKER_PROF=1 PS3RECOMP_WALKER_PROF_SAMPLE=1" \
           "PS3RECOMP_WALKER_PROF=1" \
           "PS3RECOMP_WALKER_PROF=1 PS3RECOMP_WALKER_PROF_SAMPLE=16"; do
  env $arm PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1 REPLAY_NULL_HASH=0 \
    taskset -c 2 ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame \
    --frames 200 --warmup 20 | sed -n 's/^median frame:/'"$arm"' /p'
done

# live A/B (already run; one launch, X once)
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=remapdirect2 METER=1 PIN=1 WCB=2 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
  PITCH_THRESH=35 LIMIT=900 SETTLE=10 \
  ARM_A="PS3RECOMP_REMAP_DIRECT=0" ARM_B="PS3RECOMP_REMAP_DIRECT=1" \
  BOOT_ARM="PS3RECOMP_REMAP_DIRECT=0" ./ab_live.sh
python3 pin_ab.py --log ../ab_live_remapdirect2_<ts>.log \
  --marks ../ab_live_remapdirect2_<ts>.marks --seg 10 --drop 2
```

## 7. Caveats

* The replay has no Vulkan recording and its texture cost is a floor, so the
  profiler and remap shares are smaller there than live. The profiler change
  removes clock calls, so the replay delta is a lower bound on the live saving;
  the live A/B could not be re-run for it (launch budget).
* The frame-sampled profiler was verified on the replay and by construction
  (whole-frame selection) but not in a live run. If a live `[walker-prof]`
  line ever shows `engine=0.000ms` with `draws>0`, that is the old bug, not
  this build.
* The two remap A/Bs disagree in sign; with p50 flat in both and both CIs
  covering 0, the honest reading is "no measurable effect at the meter". The
  crowded replay is the place where a per-draw change can accumulate; the
  command above is that test.
* `rsx_vertex_remap_build` is 16.3 % of the *walker* only. The meter frame is
  the c690 producer (34.16 ms p50), so a per-draw win cannot move its p50.

## 8. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The
GPU-indexed-draw shape is credited to the MIT NHL Legacy port notes
(#62, `nhl-lessons.md`); the ranged-constant copy already in the backend is
credited to the MIT inFAMOUS 2 port. No RPCS3 or other GPL emulator source was
read or cited.
