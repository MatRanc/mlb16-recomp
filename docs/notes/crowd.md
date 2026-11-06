# Crowd draws: sub-draws replayed with one vertex state (PS3RECOMP_CROWD_FIX)

Date: 2026-11-06. Branch `exp/crowd` (off `exp/spu-v4`, merged `fork/port/stable`
= `fcb81efb`), build `recomp-work/build-sync`, replay
`captures/walker_replay_fixed` (the crowded capture). Offline only: no game
launch. Follows [`crowded-walker-profile.md`](crowded-walker-profile.md),
[`capture-tools-fix.md`](capture-tools-fix.md) and
[`frame-timeline.md`](frame-timeline.md). No RPCS3 or other GPL emulator source
was read or cited; nothing here comes from the user's RPCS3 patch diff.

## TL;DR

* **The capture contains the crowd group.** One `SET_BEGIN_END(QUADS)` group at
  FIFO word 236523 of 385,837 holds **2,170 draw batches** (392
  `DRAW_ARRAYS`, 1,778 `DRAW_INDEX_ARRAY`), **536 in-group
  `SET_VERTEX_DATA_ARRAY_OFFSET` writes** forming **268 distinct vertex-fetch
  states**, and **zero render-state changes** (same surfaces, zeta, textures,
  blend, depth, cull and FP/VP program from the first batch to the last). The
  index array offset/format never changes.
* Our walker has all three defects the RPCS3 history predicts, plus a cap:
  1. **One vertex state per group.** `dc_fetch` builds the reference list at
     `END` from the state current then, so every batch uses the *last* offset
     set (`0x8080A720`); the other 267 sets are lost
     (`libs/video/rsx_draw_engine.c:5832`, `:5836`).
  2. **Mixed types reordered.** Arrays are fetched first, then indexed, and the
     whole group becomes one backend draw. The real RSX draws each batch in
     FIFO order with its own type.
  3. **`ENG_MAX_BATCHES` = 256** (`:98`): of 2,170 batches only the first 256
     arrays and first 256 indexed batches are recorded; 1,654 are silently
     dropped.
* **`PS3RECOMP_CROWD_FIX` (default 0, live key)** splits a group at each change
  of draw type or vertex-fetch state and replays each run with the state its
  first batch saw. 268 runs replace the single 512-batch draw.
* **Offline proof.** Crowded capture, null backend: draws 1,694 -> 1,961,
  recorded source vertices 35,468 -> 106,080. Rendering the crowd group alone
  to the 3D target shows a clump at the origin before and rows of fans after.
  Key off is byte-identical to `build-play`/`build-prof`; the non-crowd
  `meter_frame` capture is byte-identical with the key on or off. Video
  fixtures 34/34 (including `test_rsx_draw_engine`).
* Cost in the crowded replay: +267 backend draws, ~+2.6 ms of null-backend
  walker (37.2 -> 39.8 ms median, one 3-frame run). Nothing is default-on.

## 1. The capture

`captures/walker_replay_fixed.{fifo,mem,state}` is the capture-tools-fix page
set (385,837 words, 113 MB of guest pages, 1,694 backend draws, 0 read misses).
Scanning the FIFO without a backend (`tools/fifo_dump_scan.py` plus a
group/offset scanner) gives:

| property | value |
|---|---|
| BEGIN/END groups | 1,702 |
| groups with QUADS | 57 |
| crowd group | 1 group, **2,170 batches** |
| batch types | 392 `DRAW_ARRAYS`, 1,778 `DRAW_INDEX_ARRAY`, 118 type transitions |
| in-group `VTXBUF_OFFSET` writes | 536 |
| distinct vertex-fetch states | **268** (offset tuples; formats/base/restart constant) |
| index array state | one offset/format for the whole group |
| render-state writes in the group | **0** |
| runs (consecutive same type+state) | 268: 121 array runs, 147 indexed runs |
| colour target | local `0x1110000`, 1280x720, AA modes `aa_sx=2` (2560x720 store), pitch `0x2800` |
| zeta | local `0x1990000`, pitch `0x2800` |

The batch sequence is the four game modes interleaved: runs of forward
`DRAW_ARRAYS` (`first` 0, 256, 512...) followed by runs of descending
`DRAW_INDEX_ARRAY` (`first` 180..1600) for another section, repeating as the
camera tests pick a mode. The 268 offset sets are the per-section batches.

## 2. What our walker does with it (root cause)

`rsx_dispatch` delivers `begin`, then each batch to `sink_draw_arrays` /
`sink_draw_index`, then `end`. The engine accumulates batch ranges and does
all the work at `END`:

* `sink_draw_arrays` / `sink_draw_index` store only `(first, count)`
  (`rsx_draw_engine.c:6094`, `:6125`), capped at 256 each.
* `dc_fetch` (`:5811`) walks **all array batches first**, then **all indexed
  batches**, using the vertex declaration/offsets and index array read from
  `g.rsx` **as they are at END** (`:5832`, `:5836`). In this group that is the
  last offset set, `0x8080A720`, applied to all 512 surviving batches.
* `sink_end_work` then issues **one** backend draw for the merged list
  (`:8690` and the worker packet at `:8450`), so mixed arrays/indexed become one
  draw type and the batch order is lost.

Consequences against the capture, all measurable:

1. **Wrong bytes for every batch but the last.** 268 offset sets collapse to
   one. The vertex shader's colour attribute is the sprite ID that indexes the
   constants table, so wrong offsets put most fans at the origin, stretch some
   into pillars and leave seats empty.
2. **Reordered/mixed draws.** 392 arrays and 1,778 indexed batches become one
   list with arrays first. The indexed batches read the *last* index state but
   the array batches are drawn as arrays; whichever type is not first is
   replayed with the wrong geometry.
3. **1,654 of 2,170 batches dropped** by the fixed 256-entry arrays. This is the
   "empty seats" term and it is independent of the two state bugs.

No culling/clipping/depth rejection was involved at this stage: the crowd group
binds cull off, depth on, blending off, and the isolated render (section 4)
shows the geometry.

## 3. The fix

`PS3RECOMP_CROWD_FIX` (live key `PS3_LIVE_CROWD_FIX`,
`libs/video/ps3_live_ctl.h:108`, env default 0) changes only the draw-accumulator
path:

* `eng_vtx_state` snapshots the 16 `VTXBUF_OFFSET` and 16 `VTXFMT` words,
  `VERTEX_DATA_BASE`, `VB_ELEMENT_BASE`, `IDXBUF_OFFSET`, `IDXBUF_FORMAT` and
  the two restart-index words (`:552`, capture/apply at `:563`).
* `dc_batch_split` (`:6043`) runs on every batch while the key is on. It
  flushes the pending run when the batch's draw type or snapshot differs from
  the run's, or when the run reaches `ENG_MAX_BATCHES`.
* `dc_flush_run` (`:6032`) saves the live state, applies the run's snapshot,
  calls the existing `sink_end_work`, then restores the live state. `sink_end`
  (`:8866`) does the same for the final run, because the guest may write
  another offset between the last draw and `END`.
* `dc_reset` (`:5446`) clears the run marker at `BEGIN`; `dc.group_n` keeps the
  group's batch count across runs for the measurement switch below.
* The offset-change flush also removes the 256 cap for this group: each run is
  at most a few batches, so no batch is dropped.

The real RSX executes methods in FIFO order. When the key is off, nothing
changes. When it is on, each run is fetched with the vertex state current at
its batches instead of the state at `END`.

Limitation: render state other than the vertex fetch (surface, pipeline,
textures, blend/depth) is still read at flush time, not at each batch. That is
correct for this group (zero in-group render-state writes) and for the
single-state groups that are not split at all; a title that changes render
state between sub-draws of one `BEGIN/END` would need the same snapshot
treatment for those registers.

### 3.1 Measurement-only switches

* `PS3RECOMP_CROWD_ISOLATE=<n>` skips every group with fewer than `n` batches
  (`rsx_draw_engine.c:8382`). Used with `1000` it leaves only the crowd group,
  so its target can be dumped without the rest of the frame.
* `PS3RECOMP_REPLAY_DUMP_OFFSET=<offset>` makes the replay tool present the
  colour surface at a guest offset instead of the last-written one
  (`tools/replay_rsx_engine_core.c:318`,
  `rsx_draw_engine_replay_present_offset`, `:9143`). Together with
  `PS3RECOMP_VK_DUMP` this writes the 3D target to a PPM.

Both default off and change the image, so they are diagnostics only.

## 4. Offline proof

### 4.1 Draw counts and digests (null backend, 3 frames, 1 warm-up)

| capture | arm | draws | hash | gate |
|---|---|---:|---|---|
| `walker_replay_fixed` | old `build-play` | 1,694 | `0e8f7c71ca8be27c` | `f71ee0f438ed900d` |
| `walker_replay_fixed` | `exp/crowd`, key 0 | 1,694 | `0e8f7c71ca8be27c` | `f71ee0f438ed900d` |
| `walker_replay_fixed` | `exp/crowd`, key 1 | 1,961 | `93812067be0c8bb4` | `04711b432a5e96a5` |
| `meter_frame` (non-crowd) | old `build-play` | 502 | `a6fcc849341eba9b` | `ff1ae064a377ce16` |
| `meter_frame` | key 0 | 502 | `a6fcc849341eba9b` | `ff1ae064a377ce16` |
| `meter_frame` | key 1 | 502 | `a6fcc849341eba9b` | `ff1ae064a377ce16` |

Key off is bit-identical to the pre-change build; the non-crowd capture is
bit-identical with the key on. The crowded capture gains 267 draws (the 268
runs minus the one merged group) and its recorded source vertices go from
35,468 (first 256 of each type) to 106,080 (all 2,170 batches).

### 4.2 Crowd image

The full crowded frame in replay does **not** change: the last draw samples the
3D target through a texture unit, and the offline texture cache never
revalidates guest bytes (`TEX_DIRTY` is disabled when the replay guest reader is
installed), so the composite keeps the captured guest image. Presenting the 3D
target directly and drawing the crowd group alone avoids that:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
for k in 0 1; do
  PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0 \
  PS3RECOMP_CROWD_ISOLATE=1000 PS3RECOMP_CROWD_FIX=$k \
  PS3RECOMP_REPLAY_DUMP_OFFSET=0x1110000 \
  PS3RECOMP_VK_DUMP=$PWD/captures/crowd_analysis/iso_$k.ppm \
    ./build-sync/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
    --frames 2 --warmup 1
done
```

`iso_0.ppm` (key off) is one small clump of fans at the origin;
`iso_1.ppm` (key on) shows the rows of the section across the same area.
The bounding box of the difference is (479,188)-(722,283) in the 1280x720 dump.
This is the renderer-side counterpart of "clumps at the origin" vs "rows".

### 4.3 Cost

One 3-frame run each on an idle machine, cold pipelines excluded by the
warm-up: null backend walker 37.2 -> 39.8 ms median (+2.6 ms) for +267 draws;
the Vulkan replay was 80.6 -> 82.7 ms median in the same one-off pair (the
Vulkan frame also pays the extra descriptor/bind recording). Both numbers are
single runs on the crowded replay, not a benchmark.

### 4.4 Tests

`python3 tools/test_video_fixtures.py`: **34/34 pass**, including
`test_rsx_draw_engine` (and `test_gcm_tick_in_drain`, which happened to pass in
this run).

## 5. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work
# key off must match the old builds
PS3RECOMP_CROWD_FIX=0 ./build-sync/ps3recomp_sdk/replay_rsx_engine \
  ./captures/walker_replay_fixed --frames 3 --warmup 1 --csv
# key on
PS3RECOMP_CROWD_FIX=1 ./build-sync/ps3recomp_sdk/replay_rsx_engine \
  ./captures/walker_replay_fixed --frames 3 --warmup 1 --csv
# non-crowd capture must not move
PS3RECOMP_CROWD_FIX=1 ./build-sync/ps3recomp_sdk/replay_rsx_engine \
  ./meter_frame --frames 3 --warmup 1 --csv
```

The FIFO scanners used for section 1 are under
`recomp-work/captures/crowd_analysis/` (`scan_groups.py`,
`scan_group_detail.py`, `scan_targets.py`, `scan_tex.py`); the PPM/PNG dumps are
in the same directory and are not in git.

## 6. Clean room

This work used only this repository's replay tool, capture format, draw engine
and FIFO, plus the user's own game-side notes on the draw pattern. No RPCS3
source, diff or fork was read.
