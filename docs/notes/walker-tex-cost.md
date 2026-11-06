# Walker texture cost: per-bind work in the crowded replay, not the rehash

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two game launches (the first was killed by the
system for low memory, the counted A/B is the second), pitch meter only.
Offline: the crowded replay capture `recomp-work/walker_replay`, the user's
40 ms hitch recordings, the two crowded play logs, and the engine fixtures.
Follows [`texture-dirty.md`](texture-dirty.md),
[`walker-per-draw.md`](walker-per-draw.md),
[`readback-cost.md`](readback-cost.md), [`wcb-async.md`](wcb-async.md),
[`walker-mt-repair.md`](walker-mt-repair.md),
[`nhl-lessons.md`](nhl-lessons.md) and
[`targets-and-backlog.md`](../targets-and-backlog.md). No RPCS3 or other GPL
emulator source was read or cited.

## TL;DR

* **The crowded `tex` phase is per-unit binding work, and it is the crowded
  frames' own population.** In the user's replay logs the reports whose
  sampled drain is a real crowded frame put `tex` at **13-31 ms for
  ~2,300-3,400 unit binds, i.e. 6.8-8.3 us/bind**. The same engine on the
  crowded capture `walker_replay` (1,702 draws, null backend) spends
  **2.4-3.0 ms** on `tex` (1.9 us per cache slot). The low-`tex`
  (<2 ms) lines in the same logs are **not** fast crowded frames: the
  frame-sampled profiler counts `draws` every frame but times phases on one
  drain in eight, so an idle drain produces a low phase line next to a real
  draw count (`walker-prof.md`). Filter reports by `flip`/sampled-drain
  consistency, not only by `engine=0`.
* **Texture uploads are the tail, not the median.** The 40 ms recording has
  3 uploads/frame median (64 KiB, 0.03 ms), p90 **6.6 ms/frame**, max
  **290 ms** on a load frame. The expensive cases are whole 8 MB images
  decoded and uploaded (20-33 ms spans); they make `tex` bimodal but do not
  explain the crowded median.
* **Render-target binds are numerous but cached.** A crowded capture frame
  answers 647 depth-target binds from `eng_zdepth_snapshot` and 53 colour
  targets through `vk_eng_surface_view`, against 1,403 plain guest-texture
  slots. The snapshot is per-serial; the surface-view lookup scanned all
  2,048 backend objects before the new memo.
* **Fixes, all behind live keys, default off:** a target-address bloom that
  lets a plain texture skip both target-table scans, a 4,096-entry texture
  cache with a 64K-entry lookup index (the 1,024-entry cache is full at the
  meter and the crowded scene's working set is larger), a per-entry binding
  memo that survives target/upload churn elsewhere, and an O(1) backend
  surface-view memo. The inFAMOUS 2 MIT port's bloom and 4,096-entry cache
  are the reused ideas (attribution below).
* **Exactness holds.** `meter_frame` (64 frames) and `walker_replay` (10
  frames) report the same last hash and gate with every key off and on;
  `tools/test_video_fixtures.py` **30/30**; `test_tex_memo` covers the local
  memo, the bloom and the raised cap. The TEX_DIRTY tracker is untouched;
  its 44,656-pair/0-stale check from `texture-dirty.md` still applies.
* **The meter A/B is neutral on frames, as expected.** N=6 x 10 s per arm,
  12 segments: fps **36.72 -> 36.53** (paired +0.19, CI [-2.23, +2.61]),
  p50 **34.15 -> 34.15 ms**, walker **9.56 -> 8.84 ms**, `tex`
  **0.346 -> 0.292 ms**. The meter is c690-bound, keeps exactly 1,024
  textures with 0 evictions and 0 uploads, and cannot show the crowded win.
  A/B thumbnails are at the A/A level (mean abs 1.98 vs 1.70).
* **Nothing is default-on.** The user's crowded-replay command is in
  section 6.

## 1. Scenes and method

### 1.1 Crowded live logs

`recomp-work/play_20261104_173326.log` (1,705-draw full-field replay),
`play_20261104_165634.log` (prefetch-off arm) and `play_20261104_165829.log`
(prefetch-on arm) carry the user's crowded reports. A report is a real
crowded sample when it has `draws >= 800`, `engine > 0`, at least 100 unit
binds, and `tex/bind >= 2 us` (an idle drain timed by the sampled profiler
costs ~0.4 us/bind):

| log | reports | draws (med) | binds/frame (med) | `tex` ms (med) | us/bind (med) |
|---|---:|---:|---:|---:|---:|
| `173326` | 51 | 1,240 | 2,486 | **21.3** | **7.8** |
| `165634` | 328 | 1,180 | 2,265 | **17.6** | **6.8** |
| `165829` | 146 | 1,160 | 2,240 | **36.3** | **13.5** |

The third run is a segment with animated/video textures (its recording shows
1.8 MB/frame of uploads against 2 KB/frame in the first), which is why it is
higher; it is not the mission's cited log.

Dropping the idle-sampled reports leaves `tex` scaling with `binds`
(r = 0.84) and, in the same report, with the draw count. The classifier is in
section 8.

### 1.2 Crowded offline replay

`recomp-work/walker_replay` is a one-frame capture of the crowded instant
replay (1,702 submitted draws; the capture images one page of each c690 mesh
ring read, so 1,261 draws replay; see `targets-and-backlog.md`). The null
backend exercises the real engine, cache, remap and texture-slot code, but
not Vulkan recording, TEX_DIRTY (the replay reads through `cap_read`) or
uploads. `PS3RECOMP_WALKER_PROF2=1` gives the phase split and the census:

| per crowded replay frame | value |
|---|---:|
| `tex` phase | 2.7-3.0 ms |
| `tex_slot` (plain cache slots) | 2.4-2.7 ms |
| `tex_bind` (all `sink_bind_textures`) | 2.9 ms |
| `tex_upload` | 0.0 ms |
| `slots` (plain cache probes) | 1,403 |
| binding memo hits / full resolves | 2,618 / 2,103 |
| depth snapshots / surface views / cubes | 647 / 53 / 0 |
| texture entries / colour targets / depth targets | 312 / 31 / 25 |

The whole 2.9 ms is the plain-cache probe plus target-table scans and
sampler decode; the target-address paths are ~0.3 ms here.

### 1.3 Trace recording

The user's 40 ms hitch recordings (`longframes_20261104_142459.pstl`, plus
the two 16:56 replay recordings) carry `rsx.wcb_done` and `tex.upload` spans.
Over the 1,930 hitch frames of `longframes`:

| event | per hitch frame |
|---|---:|
| `tex.upload` count | 3 (p50), p90 4 |
| `tex.upload` bytes | 64 KiB (p50), p90 8 MiB |
| `tex.upload` duration | 0.03 ms (p50), p90 **6.6 ms**, mean 2.2, max 290 |
| `rsx.wcb_done` copies | 10 (p50) |
| WCB 7.2 MB copies | up to **26-30 ms** each |

The upload tail is the animated/video texture case: a whole 8 MB image
decoded and uploaded in one frame. The median crowded frame does not upload.

### 1.4 Meter A/B

`ab_live.sh`, N=6 x 10 s per arm, 2 s dropped, 12 segments
(`ab_live_texcache9ab_20261104_223458`). The live control file was applied at
every switch (the harness self-check passed). Arms:

```
A = PS3RECOMP_ENG_TEXCACHE=0;PS3RECOMP_ENG_TARGET_BLOOM=0;
    PS3RECOMP_ENG_TEXMEMO_LOCAL=0;PS3RECOMP_VK_VIEW_MEMO=0
B = the same four keys = 1
```

## 2. Where the crowded `tex` time goes

The evidence supports this split for a 13-31 ms crowded frame:

| part | crowded size | evidence |
|---|---|---|
| plain guest-texture cache probe (`eng_texture_slot`) | **dominant**: 1,403 probes/frame in the crowded capture (up to 3,400 unit binds); the replay's `tex_slot` is 2.4-2.7 ms at 312 resident entries and the live per-bind cost is 4-8x higher | replay census; live `tex/bind` |
| target-table scans in `sink_bind_textures` / `eng_target_at` | 2 scans (colour, depth) per full bind, up to 64+64 entries each | replay: **3,506 scans/frame**, 2,482 removable by the bloom; live meter is 64+64 targets |
| depth/target binds | 647 `eng_zdepth_snapshot` + 53 `surface_view` per crowded capture frame; cached by serial, but the view lookup scanned 2,048 objects | replay census |
| texture re-upload bursts (decode + layout + ring copy) | 0 in the median, **6.6 ms p90**, 20-33 ms when an 8 MB image changes | trace `tex.upload` |
| TEX_DIRTY revalidation | one page-range check per entry per frame, no per-bind hash; not visible in the replay (dirty off) | `texture-dirty.md`; meter `tex` 0.3 ms |
| WCB copies triggered by texture binds | none at the meter (`dguest=0`, `reint=0`); crowded WCB copies are 10/frame, mostly tiny, with 7 MB/26-30 ms outliers | meter `tex-stats`; trace |
| sampler / view creation per bind | samplers are decoded values, no object; the view object is cached, the lookup was the cost | code; replay census |

Two things follow.

1. The fix must shorten the **common plain bind**, not the rare upload. Under
   write tracking the rehash is already gone; the remaining per-bind work is
   the cache probe (with a linear scan on a memo miss), the two target-table
   scans and the sampler decode.
2. The cache is full. The meter keeps exactly 1,024 texture entries
   (`ntex=1024`) with essentially zero evictions, and a crowded frame binds
   more units than the meter. If the crowded working set crosses 1,024, every
   eviction both re-uploads a texture and bumps the global binding memo
   generation for the rest of the frame.

## 3. The cuts

All are exact and default **off**.

### 3.1 `PS3RECOMP_ENG_TARGET_BLOOM` (live)

A 4,096-bit filter keyed by a hash of `(location, offset)` says whether a
colour or depth target may live at a texture address. A plain texture — most
binds — skips both target-table scans and `eng_target_at`. The filter is
rebuilt on every target create/recreate, so a clear bit is authoritative; a
stale set bit only costs the scan it always did. Adapted from the MIT
inFAMOUS 2 port's target bloom.

Offline (`walker_replay`, 7 interleaved runs, `REPLAY_NULL_HASH=0`): the
non-slot part of `tex_bind` falls **0.326 -> 0.222 ms/frame**; target-table
scans fall **3,506 -> 1,024 per frame** (2,482 bloom skips). At the meter the bloom
skips ~650 scans/frame but they are cheap at 64+64 targets; `tex` falls
0.054 ms.

### 3.2 `PS3RECOMP_ENG_TEXCACHE` (live)

Opens the engine texture cache from 1,024 to 4,096 entries
(`RSX_DRAW_ENGINE_TEXTURE_CACHE_MAX`), raises `VK_ENG_MAX_OBJ` to 8,192 so
the handles exist, and replaces the 4,096-entry direct-mapped key memo with a
64K-entry map mixing every key field (the old hash is dominated by `offset`).
At most 6% occupancy at 4,096 textures, so a lookup rarely falls back to the
linear scan. The cache is always the same cache — same keys, same answers;
only the cap and the index change.

The meter cannot show this: it holds exactly 1,024 entries and evicts 0.01
times/frame in arm A, so arm B has nothing to avoid. The replay capture has
312 entries. The crowded scene is the test.

### 3.3 `PS3RECOMP_ENG_TEXMEMO_LOCAL` (live)

The per-unit binding memo (the inFAMOUS 2 port's idea, already in the engine)
used to be dropped by one global `s_tex_gen`. The local mode validates the
answered cache entry itself (same entry, handle, key, not rewritten), and for
a target appearing at the address uses `eng_target_at`. A target created
*anywhere else* no longer drops every unit's answer; a target created *at the
address* still does. The bloom makes the address test one bit in the common
case. Unit-tested: an unrelated upload generation bump keeps the answer; an
unrelated target creation keeps it; a target at the address drops it; a
rewritten entry is revalidated.

### 3.4 `PS3RECOMP_VK_VIEW_MEMO` (live)

`vk_eng_surface_view` scans all 2,048 object slots for a `(surface, crossbar,
format)` view. A 512-entry direct-mapped memo now answers it in O(1),
validated against the object table (kind, surface, remap, format), so a
released or reused handle misses. A crowded capture asks for 53 views/frame;
the old scan is 53 x 2,048 object visits. The scan now stops at the highest
handle handed out (`s_eobj_hi`), so raising the table for the 4,096 cache does
not slow the memo-off path.

## 4. Exactness and tests

Every arm was replayed with the digest on
(`REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1`):

| capture, frames | keys off | keys on |
|---|---|---|
| `meter_frame`, 64 | `461ff3a834c62331` / gate `089e7b1082b4bcc3` | same |
| `walker_frame`, 20 | `9b78ac38a25c5cbf` / gate `c010259828aece3f` | same |
| `walker_replay`, 10 (crowded) | `9f3fea19a38d627b` / gate `ccc4f3e83115e261` | same |

* `test_tex_memo`: all 17 checks pass, including the new local-memo,
  target-bloom and 1,024+ cache-cap cases.
* `tools/test_video_fixtures.py`: **30/30** (one run flaked on the
  pre-existing timing-sensitive `test_gcm_tick_in_drain`, which passes
  standalone and in the final run; it does not touch these files).
* `check_spu_context_abi.sh` is run by `ab_live.sh`; no `spu_context` field
  was added or moved.
* TEX_DIRTY is unchanged. The prior live paranoid run checked **44,656
  texture/frame pairs, 0 stale** (`texture-dirty.md`); these cuts only change
  how a bind is answered, never how a texture is validated or uploaded.

## 5. Live pitch-meter A/B

`ab_live_texcache9ab_20261104_223458`, 12 segments, all labels matched the
applied control state:

| metric | A (off) | B (on) |
|---|---:|---:|
| fps (segment mean +/- 95% CI) | **36.72 +/- 1.12** | **36.53 +/- 1.88** |
| paired difference | | **+0.19 +/- 2.42**, CI [-2.23, +2.61] |
| p50 / p90 / p99 ms (per flip) | 34.15 / 37.65 / 40.99 | 34.15 / 37.32 / 40.79 |
| stddev ms | 11.28 | 11.28 |
| walker ms | 9.56 +/- 2.39 | **8.84 +/- 1.61** |
| wait ms | 14.99 | 16.58 |
| engine / parse / fetch ms | 8.610 / 0.850 / 2.249 | 8.041 / 0.803 / 2.117 |
| **`tex` ms** | **0.346** | **0.292** |
| pipe / bind / draw / other ms | 0.918 / 0.128 / 2.698 / 2.271 | 0.888 / 0.121 / 2.573 / 2.049 |
| draws/frame (mean) | 496.1 | 500.2 |
| c690 lane jobs/s | 3,146-3,210 | 3,137-3,190 |
| thumbnails vs A (mean abs) | A/A 1.70 | **A/B 1.98** (B/B 1.52) |

`p50` is pinned at 34.15 ms by the c690 producer (`meter-p50-pin.md`), so a
per-bind renderer cut cannot move it. The mechanism moves the right way
(walker -0.72 ms, `tex` -0.054 ms) but the meter has no crowded binds, no
uploads and no cache pressure; this A/B is a "no regression" gate, not the
win. One caveat: the A/B predates the object-table high-water bound
(section 3.4), so arm A scanned the enlarged 8,192-slot table for its ~31
views/frame while B answered from the memo; the measured `tex` delta is
therefore an upper bound on the key's meter value and is expected to be ~0. The A/B thumbnails are animated-scene differences at the A/A level
(the old MT arms were ~39); no missing, stale or flickering texture.

## 6. User command: crowded replay A/B

Run the zoomed-out instant replay, 2-3 minutes per arm, with the 40 ms hitch
recorder. Write recordings under `recomp-work/captures/` (this machine's
`/tmp` is RAM-backed). `play_windowed.sh` already enables WCB=2, the walker
profiler and `PS3RECOMP_RSX_TEX_PIN=1`.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
TS=$(date +%Y%m%d_%H%M%S)

# Arm A: all four keys off.
PS3RECOMP_TIMELINE=$PWD/captures/crowdtex_A_$TS.pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_TEX_STATS=1 \
PS3RECOMP_ENG_TEXCACHE=0 PS3RECOMP_ENG_TARGET_BLOOM=0 \
PS3RECOMP_ENG_TEXMEMO_LOCAL=0 PS3RECOMP_VK_VIEW_MEMO=0 \
./play_windowed.sh

# Arm B: same shot, all four keys on.
PS3RECOMP_TIMELINE=$PWD/captures/crowdtex_B_$TS.pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_TEX_STATS=1 \
PS3RECOMP_ENG_TEXCACHE=1 PS3RECOMP_ENG_TARGET_BLOOM=1 \
PS3RECOMP_ENG_TEXMEMO_LOCAL=1 PS3RECOMP_VK_VIEW_MEMO=1 \
./play_windowed.sh
```

Live-ctl fallback (one launch, both arms): boot with the A control file and
switch it in the replay instead of relaunching.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
printf '%s\n' PS3RECOMP_ENG_TEXCACHE=0 PS3RECOMP_ENG_TARGET_BLOOM=0 \
  PS3RECOMP_ENG_TEXMEMO_LOCAL=0 PS3RECOMP_VK_VIEW_MEMO=0 > crowdtex.ctl
PS3RECOMP_CTL=$PWD/crowdtex.ctl PS3RECOMP_TEX_STATS=1 \
PS3RECOMP_TIMELINE=$PWD/captures/crowdtex_ctl_$TS.pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 ./play_windowed.sh
# in the zoomed-out replay, after a minute:
printf '%s\n' PS3RECOMP_ENG_TEXCACHE=1 PS3RECOMP_ENG_TARGET_BLOOM=1 \
  PS3RECOMP_ENG_TEXMEMO_LOCAL=1 PS3RECOMP_VK_VIEW_MEMO=1 > crowdtex.ctl
# the log prints [live-ctl] flip=... for each key; switch back and forth.
```

What to look for (the `[tex-stats]` window is per profiler report; `cap=`
names the arm):

* **`cap`, `ntex`, `evict`, `new`.** If B shows `ntex` above 1,024 and A is
  evicting, the cache cap is doing its job. If A already has `evict=0`, the
  crowded working set is under 1,024 and this key is neutral.
* **`memo` vs `full`.** The fraction answered by the per-unit memo. B should
  keep more answers when targets/evictions churn; if the fraction does not
  move, the memo was not the crowded cost.
* **`smiss` / `ssteps`.** Cache-index misses and the entry comparisons they
  cost. B should have fewer misses at the same `ntex`; `ssteps` is the honest
  measure of the linear scan.
* **`scan` / `bskip`.** Target-table scans avoided by the bloom. B should
  show `bskip` near the plain-bind count and the same answers.
* **`uploads` and the trace `tex.upload` spans.** A large count or a large
  KiB/frame means the frame is in the upload tail, where these cuts do not
  help.
* **`[walker-prof]`**: `draws=` unchanged, `tex=` and `walker=` lower in B.
  Use only reports whose sampled drain is crowded (the low-`tex` reports are
  idle samples); compare the 80-250 ms band of `hitch_table.py`.
* **Visual:** no wrong, stale or flickering textures, no missing field. The
  keys only change where a bind is answered from, never what is uploaded.
* Optional stale check: one short run with
  `PS3RECOMP_TEX_DIRTY_PARANOID=30`; expect
  `[texdirty] paranoid ... checked=N stale=0` (it inflates `tex`, so do not
  use it for the A/B numbers).

```sh
cd /home/m/Documents/mlb16-online
for f in recomp-work/captures/crowdtex_*.pstl; do
  echo "== $f"
  python3 mlb16-recomp/tools/hitch_table.py "$f" \
    --min-ms 40 --bins --classify --baseline-jobs 680 --top 20
done
grep -aE '\[tex-stats\]|\[walker-prof\].*draws=[0-9]{4}' \
  recomp-work/play_*/play_*.log | tail -80
```

## 7. What is not done / caveats

* **No live crowded split.** Automated runs are pitch-meter only, so the
  census (`ntex`, `evict`, `smiss`, `memo/full`, `scan/bskip`, `uploads`)
  comes from the crowded capture and the meter; the crowded live numbers are
  the user's logs. Section 6 is the run that closes that gap; the counters
  are compiled into the build.
* **The meter A/B ran before one post-run refinement.** After the A/B the
  local memo's target validation was changed from "any target creation
  invalidates" to "a target at this address invalidates"; it is unit-tested
  and the replay digests are identical, but the switch itself was not
  re-measured live (launch cap). It can only raise the hit rate.
* **The cache cap is unproven live.** The meter keeps 1,024 entries with
  ~0 evictions, so arm B had nothing to avoid. The crowded test decides it.
* **Uploads are not cut.** A frame whose `tex` is an 8 MB animated texture
  upload will stay expensive. The next step there is a faster swizzled decode
  or uploading only the changed mip levels, not another cache.
* **Vulkan still has no `depth_as_color`.** A depth target sampled in a
  format the snapshot cannot serve falls back to a guest-memory copy
  (`readback-cost.md`); at the meter and in this replay there are no such
  binds (`dguest=0`).
* **No validation-layer run.** The launch cap was spent on the A/B; the
  previous renderer mission ran the layer on this code path with no VUIDs.
  The cuts only change engine-side lookup decisions and add no Vulkan calls
  (the view memo removes a scan, it does not create objects).
* **One fixture flake.** `test_gcm_tick_in_drain` is timing-sensitive under
  the parallel fixture runner; it passes standalone and in the final 30/30
  run. It does not compile `rsx_draw_engine.c` or these keys.

## 8. Reproduce

```sh
# exactness (all keys off and on must print the same hash/gate)
cd /home/m/Documents/mlb16-online/recomp-work
for k in "0 0 0 0" "1 1 1 1"; do set -- $k
  REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 \
  PS3RECOMP_ENG_TEXCACHE=$1 PS3RECOMP_ENG_TARGET_BLOOM=$2 \
  PS3RECOMP_ENG_TEXMEMO_LOCAL=$3 PS3RECOMP_VK_VIEW_MEMO=$4 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 64 --warmup 10
done
REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./walker_replay --frames 10 --warmup 3

# crowded census and phase split (null backend, no Vulkan/TEX_DIRTY)
PS3RECOMP_WALKER_PROF2=1 PS3RECOMP_TEX_STATS=1 REPLAY_NULL_HASH=0 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./walker_replay --frames 40 --warmup 10

# fixtures and the new unit checks
cd /home/m/Documents/mlb16-online/ps3recomp-play && python3 tools/test_video_fixtures.py
```

The crowded-report filter used in section 1.1:

```python
if '[walker-prof]' in line and 'draws=' in line:
    head, _, tail = line.partition('|')
    draws = float(re.search(r'draws=([0-9.]+)', head).group(1))
    engine = float(re.search(r'engine=([0-9.]+)', head).group(1))
    tex = float(re.search(r'\\btex=([0-9.]+)', head).group(1))
    binds = float(re.search(r'\\btex=([0-9.]+)', tail.rsplit('|', 1)[-1]).group(1))
    if draws >= 800 and engine > 0 and binds >= 100 and tex * 1000 / binds >= 2:
        crowded.append((tex, binds))
```

## 9. Clean room and attribution

This is our own runtime, replay captures, trace stream and EBOOT. The
target-address bloom and the "cache too small at 1,024" finding are reused
from the MIT inFAMOUS 2 port (`docs/NOTES.md`: "a bloom of target addresses
spares `sink_bind_textures` its scans of the target tables"; "the texture
cache was too small ... the engine keeps 4096 now"). The binding memo was
already credited there. The code carries the notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

The NHL Legacy port's `#136` (never evict a texture bound this frame) is
already the play default (`PS3RECOMP_RSX_TEX_PIN=1`). No RPCS3 or other GPL
emulator source was read or cited.
