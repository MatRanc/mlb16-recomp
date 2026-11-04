# Walker per-draw cost: offline memoisation wins, the live meter does not

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Offline benchmark: `replay_rsx_engine` on the existing
one-frame captures, 7 interleaved runs per arm per capture, 240 measured frames,
20 warm-up. Live: one pitch-meter A/B (`ab_live.sh`, N=6 x 10 s, 2 s dropped).
Follows [`c690-serialisation.md`](c690-serialisation.md),
[`walker-cuts-2.md`](walker-cuts-2.md), [`walker-profile.md`](walker-profile.md),
[`long-frames.md`](long-frames.md), [`readback-cost.md`](readback-cost.md),
[`live-ab-and-ceilings.md`](live-ab-and-ceilings.md) and
[`nhl-lessons.md`](nhl-lessons.md).

## TL;DR

* **The offline replay says the per-draw answer memo is worth -14 %.** From the
  shipping play configuration (`PIPE_MEMO=1 REMAP_FAST=1`) the reference-list
  remap memo plus an incremental reference hash cuts `meter_frame` **8.986 ->
  7.742 ms/frame (17.90 -> 15.42 us/draw)** and `walker_frame` **13.146 ->
  11.294 ms/frame (19.68 -> 16.91 us/draw)**, medians of 7 runs each. All
  replay gates are bit-identical. The whole win is `fetch_plan` (plan/remap
  build): meter 1.90 -> 0.47 ms, walker 2.87 -> 0.69 ms.
* **The live meter rejects it.** One launch, A = keys off, B = keys on:
  p50 **34.15 vs 34.17 ms** (unchanged, the frame is c690-bound), walker
  **17.72 vs 19.49 ms**. The arms did not sample the same scene: A averaged
  ~519 backend draws/flip, B ~555 (+7 %). Normalised per draw, the walker is
  **31.5 vs 32.0 us (+~2 %)** - neutral to slightly worse, never better. The
  raw fps drop (34.44 -> 30.96) is mostly the draw-count drift, not the cut.
* **Why it does not transfer:** the capture is one static frame, so its
  reference lists repeat exactly within and across replayed frames and the
  remap memo hits **497 of 502 draws, 0 misses**. Live the title rewrites the
  dynamic index streams, so the same memo misses and every draw pays the key
  hash, probe and save copy instead of skipping the remap build. This is the
  same lesson as `walker-cuts-2.md`, one level lower: a static capture can rank
  a cache but cannot prove its live hit rate.
* **What the offline profile does establish:** with the play defaults the
  largest real per-draw costs are the remap build (18.3 % of sampled cycles
  before the memo), `eng_pipeline_get` (5.2 %), the vertex conversion loop
  (5.1 % + 3.3 % fetch blocks), and the FIFO method loop (4.1 %). Two terms are
  replay-only: texture sparse/content hashing (16.8 %; live uses
  `PS3RECOMP_TEX_DIRTY`) and the null backend's CPU conversion (live offloads
  nearly every draw to the GPU).
* **Decision:** no default changes. `PS3RECOMP_REMAP_CACHE`,
  `PS3RECOMP_REFS_HASH_FAST` and `PS3RECOMP_MEMO_INDEX` stay **off** (the first
  was already off in `play_windowed.sh`). They are exact, tested, and useful
  for offline analysis and for titles whose index streams are static; they do
  not win on this one. The next lever is not another offline cache: it is the
  live-only Vulkan recording and the phase-2 WCB readback waits.

## 1. What was measured

### 1.1 Offline harness

`replay_rsx_engine` replays a `PS3RECOMP_WALKER_CAPTURE` frame through the real
`rsx_draw_engine.c` with the null backend. The walker, dispatcher, vertex
fetch/remap, texture decode/hash, pipeline key and bind traffic are the
shipping code; only Vulkan command recording is absent. `REPLAY_NULL_HASH=0`
turns off the null backend's per-vertex digest for timing (the exactness gate
below runs with it on).

Captures used:

| capture | words | backend draws/frame | vertices | converted |
|---|---:|---:|---:|---:|
| `recomp-work/meter_frame` | 109,318 | 502 | 114,688 | 4.0 MiB |
| `recomp-work/walker_frame` | 120,618 | 668 | 180,163 | 9.0 MiB |
| `recomp-work/captures/logo/logo_cap` | 65 | **1** | 4 | 0.1 KiB |

The team-select capture is a single 4-vertex quad: the logo is never submitted
to the RSX (see `team-logo.md`). It cannot rank per-draw work and is not used.
The crowded-case capture the next mission needs is in section 7.

Arms:

| arm | environment |
|---|---|
| A replay default | no switches |
| B play default | `PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1` |
| C cuts | B + `PS3RECOMP_REMAP_CACHE=1 PS3RECOMP_REFS_HASH_FAST=1 PS3RECOMP_MEMO_INDEX=1` |

`PIPE_MEMO` and `REMAP_FAST` are the shipping play defaults
(`recomp-work/play_windowed.sh:153,157`). `FETCH_MEMO` is deliberately off in
the play scripts; the online remap memo (C) is the alternative that is
independent of the per-frame fetch-plan registers.

### 1.2 Medians of 7 interleaved runs

240 measured frames, 20 warm-up, `REPLAY_NULL_HASH=0`:

| capture | arm | median ms/frame | us/draw | runs (ms) |
|---|---|---:|---:|---|
| meter | A replay default | 10.473 | 20.86 | 10.473 10.499 10.641 10.451 10.829 10.467 10.464 |
| meter | B play default | 8.986 | 17.90 | 8.968 8.986 9.001 9.117 8.954 8.959 8.991 |
| meter | **C cuts** | **7.742** | **15.42** | 7.739 7.738 7.765 7.729 7.742 7.798 7.781 |
| walker | A replay default | 15.286 | 22.88 | 15.277 15.270 15.294 15.304 15.286 15.384 15.278 |
| walker | B play default | 13.146 | 19.68 | 13.135 13.183 13.139 13.145 15.266 13.226 13.146 |
| walker | **C cuts** | **11.294** | **16.91** | 11.273 11.283 11.303 11.254 11.294 11.398 11.315 |

C vs B: **-1.24 ms/frame (-13.8 %) on the meter, -1.85 ms (-14.1 %) on the
walker**. The walker's B set has one 15.27 outlier (thermal/turbo); the median
is stable to +/-0.15 ms in every other run. The 15 us/draw target is met on the
meter and nearly met on the walker; the walker's remaining gap is the two
replay-only terms in section 2.2 (texture hashing and null-backend conversion),
not the case this mission set out to cut.

## 2. Where the 47 us/draw lives, and what the offline harness can see

### 2.1 Flat `perf` profile (meter, play defaults, digest off)

`perf record -F 999` over 500 frames, `PS3RECOMP_PIPE_MEMO=1
PS3RECOMP_REMAP_FAST=1`:

| share | symbol | note |
|---:|---|---|
| 18.3 % | `rsx_vertex_remap_build` | dedup of the reference list, every draw |
| 8.9 % | `eng_texture_content_hash` | **replay-only** (live: `TEX_DIRTY`) |
| 8.6 % | `[vdso]` (clock_gettime) | trace/profiler timestamps |
| 7.9 % | `eng_texture_sparse_hash` | **replay-only** |
| 5.2 % | `eng_pipeline_get` | memo fingerprint over the program bytes |
| 5.1 % | `rsx_loop_f32_all` | vertex conversion (CPU path) |
| 4.1 % | `gcm_rsx_process_fifo_unlocked` | FIFO walk / method loop |
| 3.3 % | `rsx_vertex_fetch_blocks` | per-block attribute conversion |
| 2.9 % | `sink_end_work` | group setup / state decode |
| 2.7 % | `cap_read` | **replay harness** page lookup |
| 2.4 % | `rsx_vertex_decode_element` | scalar fallback inside conversion |
| 2.2 % | `rsx_fp_program_size` | pipeline fingerprint |
| 1.9 % | `rsx_vp_analyze_inputs` | pipeline fingerprint |
| 1.4 % | `rsx_draw_engine_write_topology_indices` | index-buffer rebuild |

Two of the top five and the harness page lookup do not exist in the live
walker at the meter: with `PS3RECOMP_TEX_DIRTY=1` a texture is validated by a
guest-page write generation, not by reading its bytes, and the live draw path
offloads almost every conversion to the GPU (`vtxgpu` in
`walker-profile.md`). That leaves the remap build, the pipeline fingerprint and
the FIFO loop as the real offline-visible targets.

### 2.2 `WALKER_PROF2` phase split (meter, 120 frames, medians of one run)

| phase | B play default | C cuts | delta |
|---|---:|---:|---:|
| `fetch_plan` (key + remap + plan prepare) | 1.898 | **0.469** | **-1.43** |
| `fetch_refs` (arrays/indices -> refs) | 0.736 | 0.730 | -0.01 |
| `pipe_lookup` (fingerprint + memo) | 0.765 | 0.769 | +0.00 |
| `tex_slot` | 0.988 | 0.988 | 0.00 |
| `convert` | 1.296 | 1.298 | +0.00 |
| `draw_be` (contains convert) | 1.426 | 1.426 | 0.00 |
| `layout` | 0.374 | 0.368 | -0.01 |
| `index` | 0.171 | 0.172 | 0.00 |
| `fetch_hit / remap_hit` | 0 / 0 | **0 / 497** | |

The win is entirely the memoised remap answer. `plan prepare` and the pipeline
fingerprint are unchanged; they are candidates for a later mission, not this
one.

## 3. The cuts

All three are exact (same keys, same answers), default **off**, and gated by
one live key each (the first already existed and is live; the two others were
added to `ps3_live_ctl`). `spu_context` is untouched, so the ABI check passes.

### 3.1 `PS3RECOMP_REMAP_CACHE` (existing key, now recommended for analysis only)

`eng_remap_memo` already stored a whole-list collapse keyed by the source
reference list. It was off in `play_windowed.sh` because `REMAP_FAST` had been
measured first. With the static capture it hits 497/502 draws with zero
misses; the remap build is the single largest function in the profile, so this
is the biggest offline lever.

### 3.2 `PS3RECOMP_REFS_HASH_FAST` (new, `PS3_LIVE_REFS_HASH_FAST`)

`eng_refs_key` hashes the whole reference list a second time after `dc_fetch`
has already walked it. The new path folds the same FNV-1a word into
`dc.refs_hash` while each reference is appended (`dc_push_ref`,
`dc_push_index_run`). The hash is byte-for-byte the old one: `rsx_vertex_ref`
is two u32s, and the old key hashed one native 64-bit word per reference plus
the count. The full-list and incremental forms now live together in
`rsx_vertex_compact.c` (`rsx_vertex_refs_hash`,
`rsx_vertex_refs_hash_fold`), and `test_vertex_remap` checks them against each
other over lists of 0-257 references and a restart-cut case.

This key only has an effect when a ref-list memo is on (`REMAP_CACHE` or
`FETCH_MEMO`); by itself it is inert. That is why C turns on all three.

### 3.3 `PS3RECOMP_MEMO_INDEX` (new, `PS3_LIVE_MEMO_INDEX`)

The fetch, pipeline and remap memos each answered a lookup with a **linear scan
of the whole table** (1024/512/1024 entries) on every draw. A 4096-entry
direct-mapped index now answers the common hit in one probe; a collision falls
back to the exact scan, so an index collision can only lose the shortcut, never
an answer. Reset clears the index; enabling it mid-run clears it too. Offline
this is small (-0.14 ms on the walker, ~0 on the meter); it is included because
it is the structural form of the lookup and costs nothing when off.

### 3.4 Exactness

With the digest on, all arms report the documented gates:

| capture | last-frame hash | 20/64-frame gate |
|---|---|---|
| `meter_frame` | `a6fcc849341eba9b` | `ff2a220c28750bc3` (64) |
| `walker_frame` | `416abee2c5bff47d` | `0f91a96c79a7aadf` (20) / `3e6f6ad4ebb23ed6` (64) |

`python3 tools/test_video_fixtures.py`: **30/30 pass**, including the new
incremental-hash checks.

## 4. Live A/B at the pitch meter

One launch (`ab_live_walkdraw_20261104_160816`), N=6 segments per arm x 10 s,
2 s dropped, X once, `METER=1 SKIP_CINE=1`, `WCB=2`, `PIN=0`, x11,
`PITCH_THRESH=35`, play defaults (including `TEX_DIRTY=1`, `PIPE_MEMO=1`,
`REMAP_FAST=1`). The control file rotated A/B:

```
A = PS3RECOMP_REMAP_CACHE=0;PS3RECOMP_REFS_HASH_FAST=0;PS3RECOMP_MEMO_INDEX=0
B = PS3RECOMP_REMAP_CACHE=1;PS3RECOMP_REFS_HASH_FAST=1;PS3RECOMP_MEMO_INDEX=1
```

Per-segment (`pin_ab.py --seg 10 --drop 2`):

| seg | arm | frames | fps | p50 | p90 | p99 | walker ms | wait ms |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | A | 283 | 35.38 | 34.15 | 38.38 | 40.96 | 17.26 | 11.16 |
| 1 | B | 244 | 30.50 | 35.82 | 40.49 | 43.92 | 20.80 | 12.20 |
| 2 | A | 272 | 34.00 | 34.16 | 38.95 | 41.91 | 18.45 | 11.22 |
| 3 | B | 271 | 33.88 | 34.15 | 39.78 | 42.95 | 18.61 | 11.16 |
| 4 | A | 286 | 35.75 | 34.15 | 38.31 | 42.10 | 17.19 | 11.14 |
| 5 | B | 259 | 32.38 | 34.21 | 40.30 | 44.35 | 19.84 | 11.51 |
| 6 | A | 281 | 35.12 | 34.15 | 38.77 | 43.32 | 17.69 | 11.12 |
| 7 | B | 263 | 32.88 | 34.16 | 39.63 | 43.68 | 19.11 | 11.77 |
| 8 | A | 252 | 31.50 | 34.16 | 38.60 | 40.89 | 18.31 | 11.26 |
| 9 | B | 194 | 24.25 | 34.16 | 40.76 | 235.56 | 18.44 | 11.74 |
| 10 | A | 279 | 34.88 | 34.15 | 38.75 | 41.88 | 17.43 | 11.33 |
| 11 | B | 255 | 31.88 | 34.35 | 39.92 | 44.27 | 20.15 | 11.79 |

Pooled: A fps **34.44**, p50 **34.15**, p90 38.63, p99 42.03, walker **17.72**;
B fps **30.96**, p50 **34.17**, p90 40.17, p99 44.18 (inflated by one 236 ms
stall in segment 9), walker **19.49**. c690 lane jobs/s are identical
(A 3,138-3,211, B 3,073-3,118), and p50 does not move: as `frame-pacing-floor.md`
and `meter-p50-pin.md` say, the meter frame is the c690 burst.

**The raw table is not a clean A/B.** The arms sampled different draw counts:
the `[walker-prof]` reports in the dropped windows give **A 519 draws/flip, B
555 draws/flip (+7 %)**. Normalised, the walker is **31.5 us/draw in A and
32.0 us/draw in B (+~2 %)**; the paired per-segment differences are +0.33,
+1.03, +0.81, -0.18, +0.15, +0.82 us/draw (mean +0.49). So the cut is neutral
to slightly *worse* live, while it is 14 % better offline. The B-side fps loss
is mostly the extra draws (more players/geometry in those windows), not the
switch; the p50 identity is the honest frame-level statement.

## 5. Why the offline win does not transfer

The capture is one static frame. Its index arrays never change, so the
reference list is identical from one replayed frame to the next and the remap
memo hits every draw after the first (497/502; the five misses are immediate /
single-reference groups). Live, the title rebuilds its dynamic index streams:
the reference lists differ, the memo misses, and the draw now pays

* the reference hash (cheap, incremental, but still a pass over the refs),
* a memo probe, and
* `eng_remap_memo_save` copying the unique refs and the occurrence map into the
  memo table on every miss.

With a near-zero hit rate those copies are pure overhead. That is the +2 %
per-draw. The same caveat applies to `FETCH_MEMO` (its key includes the fetch
plan registers, so it is at least as miss-prone) and to any future cache keyed
on the expanded reference list. **A one-frame capture can rank a cache but
cannot prove its live hit rate.** The existing `walker-profile.md` caveat about
texture hashing says the same thing; this mission found the reference-list
version of it.

## 6. Recommendation

* **Do not enable these on the strength of this measurement.** The cuts meet
  the offline target (15.4 us/draw on the meter) but not the live test
  (neutral/slightly worse, p50 unchanged). Per the mission rule, the default
  stays as it is: `REMAP_CACHE=0`, `REFS_HASH_FAST=0`, `MEMO_INDEX=0`.
* **Keep the code** (default off, exact, unit-tested). It is the right answer
  for a title with static index streams, and it makes the offline profile
  readable.
* **The live per-draw cost is elsewhere.** At the meter the live walker is
  ~31-35 us/draw while the same code offline is 15.4; the difference is the
  live texture churn (already generation-based) and Vulkan recording
  (`walker-profile.md`: `tex` 11.3 ms, `draw` 5.5 ms live at 552 draws). The
  next renderer mission should measure those live, not on a capture. The
  crowded replay's readback waits (phase 2) are the other half.
* **If a future mission retries the ref-list cache**, add a two-strike save
  (record a key once, save only when it is seen again) so a miss costs one
  hash and one probe instead of a copy, and A/B it live before believing an
  offline number.

## 7. Commands

### 7.1 The user's crowded-replay test (keys off / keys on)

Boot to a match, enter the instant replay, zoom out until the whole field is
on screen, and test the two arms in the same session. This is
`play_windowed.sh` with the 40-50 ms hitch recording on; the keys are
environment variables, so no rebuild is needed.

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A (play defaults; the only difference from a normal play run is the
# 40 ms hitch recorder). Record 2-3 minutes in the zoomed-out replay.
PS3RECOMP_TIMELINE=/tmp/walkdraw_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
./play_windowed.sh

# Arm B (same run, keys on). Relaunch and repeat the same replay shot.
PS3RECOMP_TIMELINE=/tmp/walkdraw_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_REMAP_CACHE=1 PS3RECOMP_REFS_HASH_FAST=1 PS3RECOMP_MEMO_INDEX=1 \
./play_windowed.sh
```

Then compare the walks/frames and the hitch census:

```sh
cd /home/m/Documents/mlb16-online
for f in /tmp/walkdraw_[AB]_*.pstl; do
  echo "== $f"
  python3 mlb16-recomp/tools/hitch_table.py "$f" \
    --min-ms 40 --bins --classify --baseline-jobs 680 --top 20
done
grep -E '\[walker-prof\].*draws=[1-9]' recomp-work/play_*/play_*.log | tail -40
```

Expect: draws unchanged, `walker=` lower only if the replay's index streams are
static; the meter result says not to expect a frame win.

### 7.2 Capture one crowded replay frame for the offline benchmark

`PS3RECOMP_WALKER_CAPTURE` images one frame at a flip boundary, triggered by a
`<prefix>.trigger` file (same mechanism as `team-logo.md`/`walker-prof.md`).
The capture is one frame and the game data stays in `recomp-work/`.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
rm -f walker_replay.trigger
PS3RECOMP_WALKER_CAPTURE=$PWD/walker_replay \
PS3RECOMP_WALKER_PROF=1 \
./play_windowed.sh
# In the window: boot to a match, enter the instant replay, zoom out until the
# whole field and both teams are on screen, let it run one frame, then:
touch walker_replay.trigger
# The next flip arms and the next drain writes:
#   walker_replay.fifo  walker_replay.mem  walker_replay.state  walker_replay.missing
```

Replay it with the same arms used above:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1 REPLAY_NULL_HASH=0 \
  ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_replay \
  --frames 240 --warmup 20
PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1 \
PS3RECOMP_REMAP_CACHE=1 PS3RECOMP_REFS_HASH_FAST=1 PS3RECOMP_MEMO_INDEX=1 \
REPLAY_NULL_HASH=0 \
  ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_replay \
  --frames 240 --warmup 20
# exactness (digest on), both arms must print the same last hash and gate:
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_replay \
  --frames 64 --warmup 10
```

## 8. Reproduce (offline benchmark and unit tests)

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
# medians of 7 interleaved runs, both captures (see /tmp/walker_bench.py)
for i in 1 2 3 4 5 6 7; do for arm in A B C; do
  case $arm in
    A) E="";;
    B) E="PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1";;
    C) E="PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1 \
          PS3RECOMP_REMAP_CACHE=1 PS3RECOMP_REFS_HASH_FAST=1 \
          PS3RECOMP_MEMO_INDEX=1";;
  esac
  for cap in meter_frame walker_frame; do
    env $E REPLAY_NULL_HASH=0 ./ps3recomp_sdk/replay_rsx_engine \
      ../../recomp-work/$cap --frames 240 --warmup 20 |
      sed -n "s/^median frame:/$arm $cap/p"
  done
done

# phase split (B vs C)
PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1 PS3RECOMP_WALKER_PROF2=1 \
REPLAY_NULL_HASH=0 ./ps3recomp_sdk/replay_rsx_engine \
  ../../recomp-work/meter_frame --frames 120 --warmup 20 2>&1 | grep walker-prof2

# unit fixtures (includes the incremental-hash check)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_video_fixtures.py
```

Live A/B (already run; one launch, X once):

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=walkdraw PITCH_THRESH=35 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  ARMS='A=PS3RECOMP_REMAP_CACHE=0;PS3RECOMP_REFS_HASH_FAST=0;PS3RECOMP_MEMO_INDEX=0|B=PS3RECOMP_REMAP_CACHE=1;PS3RECOMP_REFS_HASH_FAST=1;PS3RECOMP_MEMO_INDEX=1' \
  BOOT_ARM='PS3RECOMP_REMAP_CACHE=0;PS3RECOMP_REFS_HASH_FAST=0;PS3RECOMP_MEMO_INDEX=0' \
  ./ab_live.sh
python3 pin_ab.py --log ../ab_live_walkdraw_<ts>.log \
  --marks ../ab_live_walkdraw_<ts>.marks --seg 10 --drop 2
```

## 9. Caveats

* The offline captures are single static frames: their texture cost is a floor
  and their reference lists are artificially stable. That is exactly why the
  live A/B exists, and why the offline hit rate must not be read as a live one.
* The live A/B is one launch. The two arms sampled different draw counts
  (A 519, B 555 per flip); the draw-normalised walker (+2 %) is the honest
  comparison, and p50 does not move in either arm. A future A/B should gate on
  equal draws or report us/draw directly.
* One B segment contains an unrelated 236 ms present/pump stall; it inflates
  B's std and p99, not the walker mean. It is reported rather than hidden.
* `MEMO_INDEX` alone measured neutral offline (9.397 vs 9.439 ms on the meter,
  inside noise). It is kept because it is the correct data structure for the
  memos, not because it bought a frame.
* `PS3RECOMP_WALKER_PROF2` adds 11-20 `clock_gettime` calls per draw (~0.25
  ms/frame at 502 draws); the phase table is for attribution, not the A/B
  numbers.
* The crowded replay itself was not run in this mission (the launch budget was
  the pitch meter). Section 7.1 is the user's test; section 7.2 is the capture
  that lets the next mission benchmark the real draw mix offline.

## 10. Clean room

This is our own analysis of our runtime's replay captures, our own trace/log
stream and our own EBOOT. The only external method attribution is the inFAMOUS
2 port's per-unit texture binding memo, already credited in
`rsx_draw_engine.c` (`sink_bind_textures`); no RPCS3 or other GPL emulator
source was read or cited.
