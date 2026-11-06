# Walker M4: per-worker Vulkan recording and the texture resolver

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, built and
measured in `recomp-work/build-mt`. Follows
[`walker-multithread-design.md`](walker-multithread-design.md) section 5
(M4) and [`walker-mt-m3.md`](walker-mt-m3.md). **No game launch was used**:
the mission stopped at the first failed gate (offline timing), as the stop
rule requires. Everything here is default off (`PS3RECOMP_WALKER_MT=0`).

## TL;DR

* **Per-worker Vulkan recording is implemented** (`rsx_vulkan_backend.c`,
  `rsx_draw_engine.[ch]`). `mt_init` creates one context per record worker:
  its own command pool, primary command buffer, fence, descriptor pool, GPU
  vertex-unpack descriptor set, and slices of the vertex/raw/index/uniform/
  remap rings. Bind state (bound targets plus the whole E2 per-draw state,
  sampler cache, render-pass and framebuffer caches, scratch depth, split
  cube layout) is per context. A worker records its chunk into its own
  primary buffer; the engine submits the segment at the chunk's global turn,
  and `queue_mu` serialises the submits, so command buffers reach
  `vkQueueSubmit` in guest order. Primaries, not secondaries, are used
  because every draw already begins and ends its own render pass (§3.5).
  Pipeline-variant creation (first draw of a topology) is serialised on the
  shared `VkPipelineCache` with a mutex. The null replay backend has no
  recording entries, so it keeps the M3 turn-per-packet path.
* **The M4 `rsx-tex` resolver is implemented** for plain guest textures
  (`PS3RECOMP_WALKER_MT_TEX_RESOLVE=1`, default on with MT). The state thread
  enqueues a texture job instead of resolving it, takes a turn token for the
  packet position, and the packet carries `ENG_TEX_TOKEN | index`; the
  worker waits for the handle at record time. Target, depth, cube and
  feedback units stay on the walker. Each texture group is an ordered turn:
  workers split their recording at it, so the resolver's uploads execute
  between the packets before and after the bind in both the null digest and
  the Vulkan submission order. Guest bytes are snapshotted at enqueue by
  default (`PS3RECOMP_WALKER_MT_TEX_SNAP=1`) so a later FIFO method cannot
  change them; `=0` is the fence-only path.
* **Exactness passes.** `meter_frame` and `walker_frame`, MT off / W=0..4,
  `REPLAY_NULL_HASH` on and `REPLAY_NULL_VSRANGE=1`, 5 and 64 frames: all
  hashes are the historical serial values. `test_video_fixtures.py` 30/30;
  `test_draw_queue` passes; `test_color_readback_guest` 85 checks / 0
  failures (its CMake target now links `rsx_draw_queue.c`, needed since the
  engine has a live reference to the queue).
* **The timing gate FAILS.** 7 interleaved runs, 120 frames + 20 warm-up,
  `REPLAY_NULL_HASH=0`: `walker_frame` MT off **14.756 ms**, W=1 15.731,
  W=2 15.912, **W=3 16.055 (+8.8% slower)**, W=4 17.194. With the resolver
  forced off (`PS3RECOMP_WALKER_MT_TEX_RESOLVE=0`) W=3 is **12.869 ms
  (-12.8%)**, still short of the -25% gate, and the state `tex` term is
  ~6.1 ms, not <=2 ms. Async resolver `tex` is ~6.6 ms. The mission stops
  here; no live A/B, nothing default-on.
* **Why the resolver did not pay off offline.** Almost every `walker_frame`
  draw binds a texture state that has not been seen this frame, so the
  resolver's ordered group falls between nearly every pair of packets. Each
  group is a turn: the recording worker submits its segment, waits for the
  state thread's job to be resolved, and the resolver in turn waits for the
  preceding chunk to be submitted. That is a third thread in the same
  serial chain the state thread already had at each texture op; the extra
  hand-offs cost more than the drain they replaced. The worker recording
  path does not help the null replay (there is no Vulkan recording there),
  so this measurement is the turn/queue machinery only.
* Per-worker recording cannot be judged offline: the replay has no Vulkan
  command buffers. Its validation is the live pitch-meter A/B and the
  user's crowded replay, which the stop rule did not reach.

## 1. What changed

### 1.1 Backend: per-worker recording contexts

`libs/video/rsx_draw_engine.h` adds optional backend entries after
`submit_async`: `mt_init`, `mt_shutdown`, `mt_ctx`, `mt_chunk_begin`,
`mt_chunk_submit`. A backend that leaves them NULL keeps the serialised
turn-per-packet path, which is what the null replay uses.

`libs/video/rsx_vulkan_backend.c`:

* The E2 per-draw state and the bound-target state became
  `vk_e2_state` / `vk_eng_bound_state` selected per thread, so the state
  thread keeps `s_e2_global` and each worker gets its own copy. The sampler
  cache moved into the E2 state for the same reason.
* `struct vk_mt_ctx` owns the command pool, two primary command buffers
  (chunk + one-shot), two fences, descriptor pool, GPU-unpack pool/set, and
  a 16 MB vertex, 8 MB raw, 1 MB remap, 4 MB index and 4 MB uniform ring
  slice. `vk_mt_init` creates `workers` contexts; `vk_mt_chunk_begin` waits
  for the context's previous submission, resets its descriptor pool and ring
  heads and begins a primary buffer; `vk_mt_chunk_submit` ends and submits
  it under `queue_mu`.
* `vk_mt_draw` is the worker form of the batched draw path: it reserves
  from the context's rings, writes constants and descriptors from the
  context's state, records the GPU-unpack dispatch when eligible, and binds
  and draws into the context's command buffer. `bind_*`, `set_*` and the
  draw entries route on a non-NULL `user` (the context pointer).
* Pipeline variants are created under `s_pipe_create_mu` with a re-check.
  One-shot work during preparation (scratch depth creation) uses the
  context's second command buffer.
* Resource destruction waits for all worker fences before releasing
  (`vk_mt_wait_all`), so no submitted chunk can still name a released
  surface, texture or pipeline.

### 1.2 Engine: chunk recording and the texture resolver

`libs/video/rsx_draw_engine.c`:

* The packet gained `tex_epoch` (number of texture groups before it).
  `s_mt` gained `tex_epoch` / `tex_taken`.
* `eng_tex_enqueue` allocates a job (key plus a guest-byte snapshot), takes
  a turn token for the packet position (shared by every unit resolved
  there) and increments the epoch. `eng_tex_group_close` is called when the
  packet is finalised and at every engine sync.
* The worker records a chunk in segments split at texture-group epochs. It
  submits the segment before a group, waits for `tex_taken` to reach the
  next packet's epoch, then begins the next segment. Turns stay in guest
  order; the resolver takes the group's turn after publishing every handle.
* The resolver thread (`eng_tex_resolver_main`) waits for the group to
  close and for its turn, runs the existing `eng_texture_slot` path (one
  writer for the texture cache), publishes each handle, then takes the
  turn. While it runs, its own `eng_mt_sync` is a no-op: it already holds
  the ordered slot and blocks every later packet.
* Synchronous texture paths (target, depth, cube, reinterpreted, vertex
  textures, non-workerable draws) call `eng_tex_sync_path` first, which
  drains the resolver, so those paths never race the cache.
* `eng_tex_frame_reset` runs after the present's `eng_mt_sync` and recycles
  the frame's job slots. `eng_mt_shutdown`/`eng_mt_reset` stop and join the
  resolver before the backend goes away.

Keys: `PS3RECOMP_WALKER_MT`, `PS3RECOMP_WALKER_MT_WORKERS` (as M1-M3),
`PS3RECOMP_WALKER_MT_TEX_RESOLVE` (default 1 with MT),
`PS3RECOMP_WALKER_MT_TEX_SNAP` (default 1). No live-ctl key was added; no
`spu_context` field was added or moved.

## 2. Exactness

`build-mt`, `--frames 5 --warmup 2` and `--frames 64 --warmup 10`,
play defaults (`PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_FAST=1`):

| capture | mode | last hash (5) | 64-frame gate |
|---|---|---|---|
| `meter_frame` | off / W=0..4 | `a6fcc849341eba9b` | `ff2a220c28750bc3` |
| `walker_frame` | off / W=0..4 | `416abee2c5bff47d` | `0bdb6c112e64e643` |
| `meter_frame` | `REPLAY_NULL_VSRANGE=1` | `461ff3a834c62331` | `089e7b1082b4bcc3` |
| `walker_frame` | `REPLAY_NULL_VSRANGE=1` | `9b78ac38a25c5cbf` | `e20a9511ff2547c3` |

Every worker count prints the same value as MT off. The texture resolver is
active for W=1..4 in all of these (the null backend keeps the ordered
turn path, and the resolver uses the same turn tokens).

`python3 tools/test_video_fixtures.py`: **30/30**.
`test_draw_queue`: order, wrap, full and sync/drain OK.
`test_color_readback_guest`: **85 checks, 0 failures**.

The `TEX_DIRTY` paranoid check could not be exercised in the null replay:
that path needs the live page-protection mapping, and the replay's guest
reader is the capture reader. It remains a live-run check, not an offline
gate; it was not reached because the timing gate stopped the mission.

## 3. Timing (the failed gate)

7 interleaved runs, 120 measured frames + 20 warm-up, `REPLAY_NULL_HASH=0`,
play defaults, unpinned, `build-mt`:

| arm | `walker_frame` median | vs MT off |
|---|---:|---:|
| MT off | 14.756 ms | - |
| W=1 (resolver on) | 15.731 ms | +6.6% |
| W=2 (resolver on) | 15.912 ms | +7.8% |
| **W=3 (resolver on)** | **16.055 ms** | **+8.8%** |
| W=4 (resolver on) | 17.194 ms | +16.5% |
| W=3, `TEX_RESOLVE=0` | 12.869 ms | -12.8% |

Gate: W=3 at least 25% better than MT off. Fails. The state-thread `tex`
term at W=3 is ~6.1 ms with the resolver off and ~6.6 ms with it on
(unprofiled runs; `PS3RECOMP_WALKER_PROF=1` is not usable under MT, its
timestamps become the dominant share). The gate also asks for <=2 ms.

`meter_frame` was not re-timed beyond the exactness runs: the meter is not
the walker lever (M3 and `walker-cuts-2.md`), and the mission had already
failed its gate.

The crowded capture (`recomp-work/walker_replay.*`) is still the 17:35
capture, older than the mission's 19:11 recapture condition, so it was not
timed; the user recapture remains an M0 prerequisite.

## 4. Why it fails, and what would be next

1. **The ordered texture group is a third thread in a serial chain.** In
   M3 the state thread did the texture work itself and waited for the
   workers. In M4 it hands the work to `rsx-tex`; the worker must then wait
   for that thread, which waits for the preceding chunk to be submitted.
   When nearly every packet binds a texture, this is a ping-pong per packet.
   The offline replay exposes exactly that shape and none of the live
   recording win.
2. **Per-worker recording only shows live.** The replay null backend has no
   command buffers; its "draw" is a digest. The live pitch meter and the
   crowded replay are where the parallel recording lowers the state thread's
   `draw`/bind time and where the extra submissions can be measured. The
   stop rule kept the mission from launching.
3. **Next experiment** (not run): record the texture upload into the
   worker's own command buffer at the token's position instead of a separate
   resolver submission (ordered upload records), and keep only the cache
   decision on the state thread. That removes the third thread from the
   submission path. It is a larger change to the backend upload path and
   was out of scope once the timing gate failed.

The code is left in the tree as an experiment, default off. The resolver
can be disabled at any time with `PS3RECOMP_WALKER_MT_TEX_RESOLVE=0`, which
is the faster offline arm; the M3 turn-per-packet path remains the null
backend's path.

## 5. Live commands (prepared, not run)

Pitch meter A/B (`ab_live.sh` defaults: `METER=1 PIN=0 WCB=2 N=6 SEG=10
DROP=2 VIDEODRIVER=x11 PITCH_THRESH=35`), one launch, MT off vs W=3 with
WCB prefetch:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=walkermt4 METER=1 PIN=0 WCB=2 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
  PITCH_THRESH=35 LIMIT=900 SETTLE=10 \
  PS3RECOMP_RSX_WCB_STATS=60 \
  ARM_A="PS3RECOMP_WALKER_MT=0" \
  ARM_B="PS3RECOMP_WALKER_MT=1
PS3RECOMP_WALKER_MT_WORKERS=3
PS3RECOMP_RSX_WCB_PREFETCH=1" \
  BOOT_ARM="PS3RECOMP_WALKER_MT=0" \
  ./ab_live.sh
```

User zoomed-out replay (40 ms hitch recording), one launch per arm, same
binary:

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A: play defaults, walker MT off.
PS3RECOMP_TIMELINE=/tmp/mt4_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_WALKER_MT=0 \
./play_windowed.sh

# Arm B: W=3, per-worker recording, texture resolver, WCB prefetch.
PS3RECOMP_TIMELINE=/tmp/mt4_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_WALKER_MT=1 \
PS3RECOMP_WALKER_MT_WORKERS=3 PS3RECOMP_RSX_WCB_PREFETCH=1 \
./play_windowed.sh
```

Live fallback (same launch): start with `PS3RECOMP_CTL=/tmp/mt4_ctl` and
write `PS3RECOMP_WALKER_MT=0` to it; the walker drains and resumes the old
path at the next drain.

Compare `[walker-prof]` `draws=`/`walker=`/`tex=`, the `[walker-mt]`
worker/fallback lines, `hitch_table.py --min-ms 40 --bins --classify`, the
Vulkan validation output, and the user's visual check of the replay.

## 6. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The
ordered-record shape, copied borrowed payloads and sync-call drain are
reused from the MIT inFAMOUS 2 port (`rsx_be_thread.c`,
`bq_direct_begin`); the header and the packet block carry the required
notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

No RPCS3 or other GPL emulator source was read or cited.
