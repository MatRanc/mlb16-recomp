# Walker M1/M2: packet ABI and one record worker

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, builds
`recomp-work/build-play` (live) and `recomp-work/build-mt` (offline replay).
Offline: `replay_rsx_engine` on `meter_frame`, `walker_frame` and the user's
`walker_replay` capture, play defaults (`PS3RECOMP_PIPE_MEMO=1
PS3RECOMP_REMAP_FAST=1`), `REPLAY_NULL_HASH=0` for timing. Live: the pitch
meter through `ab_live.sh`, X once, `WCB=2`, `PIN=0`, x11, N=6 x 10 s, 2 s
dropped. Follows [`walker-multithread-design.md`](walker-multithread-design.md)
M1/M2, [`walker-remap.md`](walker-remap.md) and
[`walker-per-draw.md`](walker-per-draw.md).

## TL;DR

* **M1 landed and passed its gate.** `PS3RECOMP_WALKER_MT=1` turns the old
  `sink_end_work` body into an immutable `eng_draw_pkt` prepare plus an
  ordered commit. `WORKERS=0` commits inline on the walker.
  `meter_frame` / `walker_frame` hashes are unchanged, the video fixtures are
  30/30, and the interleaved `walker_frame` median is **+0.16 ms** (gate
  +0.5 ms).
* **M2 landed: one SPSC record worker** (`rsx_draw_queue.c`), the inFAMOUS 2
  shape: packet-owned payload copies, worker replays backend calls in order,
  every state-thread backend call drains the queue first. MT hashes equal the
  serial vector on all three captures.
* **The first live A/B found a real bug, which is why there were two.** The
  ranged VS-constant call reads `rows + first*16` of the block it is handed;
  the worker packet held only `count` rows, so large draws got garbage
  transforms (the whole infield rendered black). The packet now keeps the
  leading rows. `REPLAY_NULL_VSRANGE=1` is a new opt-in replay gate that
  exercises the ranged call in the null backend and would have caught it.
* **Second live A/B (the counted one):** p50 **34.16 (off) vs 34.26 ms (on,
  +0.10 ms, gate +/-0.5)**, walker **10.88 -> 9.65 ms**, Vulkan `draw` phase
  **3.05 -> 2.26 ms**, no validation error, no visual diff beyond A/A noise.
  Fps was 34.48 vs 33.19 (one noisy B segment; the meter is not the crowded
  case).
* **The user's `walker_replay` capture fails the M0 gate**: 1,278 draws and
  23,535 read misses (gate >= 1,300 draws, 0 misses). Its hashes are
  self-consistent in all modes, but its timing is not crowded-case evidence.
* Everything is **default off** (`PS3RECOMP_WALKER_MT=0`); the key is live
  (`PS3_LIVE_WALKER_MT`), and turning it off mid-run drains the queue before
  the old path resumes. Nothing else is default-on. `spu_context` unchanged
  (ABI check passes).

## 1. What M1 changed

`libs/video/rsx_draw_engine.c`, `sink_end_work`:

1. `eng_pkt_prepare` keeps the old order of engine work: topology decision,
   `dc_fetch`, index rebuild, target set, depth target, pipeline, textures,
   constants, `pd_note`, first-bind depth clear bookkeeping. It stores
   everything the backend call sequence needs in `eng_draw_pkt`:
   targets (backend handles and engine slots), depth handle, pipeline,
   viewport, scissor, stencil, depth bounds/bias, the 16 texture + sampler
   descriptors, the vertex textures, the layout, the VP `vpc_lo/n` rows and
   tail, and the FP constant block.
2. `eng_pkt_commit_live` replays exactly the old backend call order:
   depth clear, `bind_targets`, `bind_pipeline`, ranged/full VS constants,
   PS constants, textures, vertex textures, viewport, scissor, stencil,
   bounds, bias, then `draw_fill`/`draw`, then write tracking.
3. `PS3RECOMP_WALKER_MT=0` takes the original code path untouched.
   `=1` with `WORKERS=0` uses the packet path inline. `=1` with `WORKERS=1`
   uses the worker (M2).

The packet is built in a 64 MB arena that never reallocates while a packet is
being built. A draw that does not fit runs inline and increments the fallback
count.

## 2. What M2 changed

`libs/video/rsx_draw_queue.{c,h}` is a small SPSC byte ring:
push copies one record and publishes it; the worker executes records in
order; `sync()` returns only after every published record has executed; a full
ring refuses rather than overwrites. `libs/video/tests/test_draw_queue.c`
checks order, wrap, full/refuse, oversize/refuse and sync/drain. It is a CMake
test (`test_draw_queue`) and is not part of `test_video_fixtures.py`, so that
gate stays 30/30.

With `WORKERS=1` the prepare path additionally snapshots every byte the
worker will read:

* converted vertices (non-deferred draws) or the fetch plan's stable attribute
  spans, its inline stream and the unique reference list (deferred draws);
* the rebuilt index buffer;
* the GPU-unpack descriptor's raw span and remap indices;
* the VP rows/tail and FP constants.

`eng_pkt_commit_snapshot` rebuilds a worker-local `rsx_vertex_fetch_plan`
with those spans and calls the same `draw_fill` / `draw` calls the walker
would have made. The fill callback is the same
`rsx_vertex_fetch_many_stream`, so the null backend's vertex digest is
identical.

Every backend call the state thread makes directly (surface/depth create and
release, texture create/upload/release, pipeline create, surface views,
snapshots, cube assembly, readbacks, WCB service/prefetch, clears, present,
flush, guest read/write) calls `eng_mt_sync()` first. So the backend always
sees one thread at a time, and records submitted to the ring cannot be
overtaken by a synchronous call.

## 3. Gates

### 3.1 M1: hashes, fixtures, timing

`build-mt`, `REPLAY_NULL_HASH` on, `taskset -c 2`:

| capture | arm | last hash | 64-frame gate |
|---|---|---|---|
| `meter_frame` | MT off | `a6fcc849341eba9b` | `ff2a220c28750bc3` |
| `meter_frame` | MT on, W=0 | `a6fcc849341eba9b` | `ff2a220c28750bc3` |
| `walker_frame` | MT off | `416abee2c5bff47d` | `0bdb6c112e64e643` |
| `walker_frame` | MT on, W=0 | `416abee2c5bff47d` | `0bdb6c112e64e643` |

The design document lists `3e6f6ad4ebb23ed6` for `walker_frame` 64; the
current tree (and `build-play` before this mission) prints
`0bdb6c112e64e643`. The 20-frame gate is `0f91a96c79a7aadf`. Treat the
documented 64-frame value as stale.

`python3 tools/test_video_fixtures.py`: **30/30 pass**.

Timing, play defaults, `REPLAY_NULL_HASH=0`, 9 interleaved runs, 120 measured
frames, 20 warm-up:

| capture | MT off median | MT on W=0 median | delta |
|---|---:|---:|---:|
| `walker_frame` | 14.479 ms | 14.643 ms | **+0.164 ms** |

Gate is +0.5 ms; pass.

### 3.2 M2: serial vs worker digest vector

The null backend digests every backend call, binding, constant block, draw
and vertex/index byte. With the worker the call sequence is the same, only a
different thread executes it.

| capture | MT off | MT on W=0 | MT on W=1 |
|---|---|---|---|
| `meter_frame` last / gate | `a6fcc849341eba9b` / `ff2a220c28750bc3` | same | same |
| `walker_frame` last / gate | `416abee2c5bff47d` / `0bdb6c112e64e643` | same | same |
| `walker_replay` last / 5-frame gate | `eb13350f14787cb3` / `e2d16ee2c20ce82c` | same | same |

`REPLAY_NULL_VSRANGE=1` is an opt-in extra gate: it makes the null backend
accept `bind_vs_constants_range` (the call the Vulkan backend uses), so the
worker's row pointer/offset is digested. All three modes agree:

| capture | MT off | MT on W=0 | MT on W=1 |
|---|---|---|---|
| `meter_frame` | `461ff3a834c62331` | same | same |
| `walker_frame` | `9b78ac38a25c5cbf` | same | same |

Off by default, so the historical hashes above are unchanged.

### 3.3 Offline timing (M2)

Play defaults, `REPLAY_NULL_HASH=0`, 7 interleaved runs, 120 measured frames,
20 warm-up, unpinned (the worker needs a second core; pinning both to one
core measures context switches, not the design):

| capture | MT off | MT on W=1 | delta |
|---|---:|---:|---:|
| `meter_frame` | 9.294 ms | 10.117 ms | +0.82 ms (+8.9%) |
| `walker_frame` | 14.389 ms | 14.213 ms | **-0.18 ms (-1.2%)** |
| `walker_replay` (bad capture) | 39.912 ms | 44.803 ms | +4.89 ms |

The replay has no Vulkan recording, so the draw term the worker would absorb
is small there; what it does absorb is the null backend's vertex conversion
and digest. The meter capture re-uploads textures on the replay path, and
each upload drains the queue, which is why that capture is slower. Live uses
`PS3RECOMP_TEX_DIRTY=1` and does not pay that. This is the same caveat as
[`walker-multithread-design.md`](walker-multithread-design.md) section 3.6:
offline timing is not the live crowded number.

### 3.4 Live pitch-meter A/B (the counted run)

`ab_live.sh` with `BUILD=build-play`, `TAG=walkermt2`, `METER=1 PIN=0 WCB=2
N=6 SEG=10 DROP=2 VIDEODRIVER=x11 PITCH_THRESH=35`; boot arm MT off. A =
`PS3RECOMP_WALKER_MT=0`, B = `PS3RECOMP_WALKER_MT=1
PS3RECOMP_WALKER_MT_WORKERS=1`. Run
`ab_live_walkermt2_20261104_181555`.

| metric | A (MT off) | B (MT on) |
|---|---:|---:|
| fps pooled | 34.48 | 33.19 |
| p50 / p90 / p99 ms | **34.16** / 38.80 / 41.99 | **34.26** / 38.85 / 42.00 |
| walker ms | 10.88 | **9.65** |
| engine ms | 9.823 | 8.711 |
| fetch / tex / pipe / draw ms | 2.48 / 0.39 / 1.04 / **3.05** | 2.35 / 0.61 / 0.96 / **2.26** |
| draws per profiled line (median) | ~548 | ~547 |
| Vulkan validation errors | 0 | 0 |
| scene thumbnails vs A | - | mean abs diff 2.32 (A/A baseline 1.96) |

p50 moved **+0.10 ms**, inside the +/-0.5 ms design gate. The worker removes
~2 ms of walker time, mostly from the draw phase. The fps difference is one
noisy B segment (p99 55.8 ms, std 42); frame p50, p90 and p99 are otherwise
flat, which is the honest frame-level statement. Draw counts match.

**Run 1 (`walkermt_20261104_180852`) is not counted as a pass.** It found the
ranged-constant bug: the whole infield was black in every B thumbnail because
the worker's `rows` pointer did not preserve the `first` leading rows. p50
was still 34.16 vs 34.16 and walker 11.28 vs 9.16; the visual check is what
failed. The fix and the new `REPLAY_NULL_VSRANGE` gate are in this branch.
The second run is the visual re-check after the fix.

## 4. M0 crowded capture

`recomp-work/walker_replay.{fifo,mem,state}` exists (the user's capture). On
`build-play` and `build-mt`:

```
replay_rsx_engine ../../recomp-work/walker_replay --frames 5 --warmup 2
draws/frame:  1278.0
guest reads:  1201170 hit, 23535 miss
```

Gate: >= 1,300 draws and 0 read misses. **Fail** (1,278 draws, 23,535 misses),
so the capture is not used as crowded-case evidence and M2 was not accepted
on it. The 20-frame MT timing above is recorded only to show it is
self-consistent; it is not a gate. A clean recapture needs the same user
command as
[`walker-per-draw.md`](walker-per-draw.md) section 7.2, in the zoomed-out
instant replay, with `PS3RECOMP_WALKER_CAPTURE=$PWD/walker_replay`.

## 5. User command: zoomed-out replay test (MT off / on, 40 ms hitches)

Boot to a match, enter instant replay, zoom out until the whole field is on
screen, and record 2-3 minutes per arm in the same shot. The keys are
environment variables, so the two arms use the same binary.

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A: play defaults, walker MT off. 40 ms hitch recording on.
PS3RECOMP_TIMELINE=/tmp/mt_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_WALKER_MT=0 \
./play_windowed.sh

# Arm B: same shot, MT on with one record worker.
PS3RECOMP_TIMELINE=/tmp/mt_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_WALKER_MT=1 \
PS3RECOMP_WALKER_MT_WORKERS=1 \
./play_windowed.sh
```

Compare:

```sh
cd /home/m/Documents/mlb16-online
for f in /tmp/mt_[AB]_*.pstl; do
  echo "== $f"
  python3 mlb16-recomp/tools/hitch_table.py "$f" \
    --min-ms 40 --bins --classify --baseline-jobs 680 --top 20
done
grep -aE '\[walker-prof\].*draws=[0-9]' recomp-work/play_*/play_*.log | tail -80
grep -a '\[walker-mt\]' recomp-work/play_*/play_*.log | tail
```

What to look for: `draws=` unchanged between arms, `walker=` and the `draw`
phase lower in B, the 40-250 ms hitch band smaller, no visual artifacts (the
replay is the acceptance test the meter cannot be), and no
`[walker-mt] ... fallback` lines. If anything looks wrong, set
`PS3RECOMP_WALKER_MT=0` in a `PS3RECOMP_CTL` file mid-run; the state thread
drains the queue and resumes the old path.

## 6. What is not done

* M3 (N workers, per-worker caches/rings, contiguous chunk commit) is not in
  this mission. The packet and queue are the shape it needs, but the worker
  now runs the whole draw commit on one thread; there is no chunk split.
* M4 (texture/pipeline resolvers) and M5 (hardening/TSAN) are not done.
* The offline crowded benchmark does not exist in a usable form (M0 above).
* Offline `meter_frame` timing is not neutral with one worker under the replay
  texture path. That is a replay-harness property, not a meter p50 result:
  the live meter p50 is within +0.10 ms.

## 7. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The SPSC
record queue, copied borrowed payloads and the sync-call drain are reused
from the MIT inFAMOUS 2 port (`rsx_be_thread.c`, `bq_direct_begin`); the
header and the packet block carry the required notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

No RPCS3 or other GPL emulator source was read or cited.
