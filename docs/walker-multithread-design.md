# Multi-threaded RSX walker: dependency map, designs and implementation plan

Date: 2026-11-04. **Design only**: no game launch, no build, no code change in
`ps3recomp-play`. The numbers below are re-analysed from the user's own
crowded-replay logs (`recomp-work/play_20261104_165634.log` and
`recomp-work/play_20261104_165829.log`) plus the measurements already written
up in [`walker-profile.md`](walker-profile.md),
[`c690-serialisation.md`](c690-serialisation.md),
[`wcb-async.md`](wcb-async.md), [`targets-and-backlog.md`](targets-and-backlog.md)
and [`nhl-lessons.md`](nhl-lessons.md). Code references are to the
`exp/live-ab` tree (`ps3recomp-play`) at `097e6c46` plus the in-flight
vertex-remap diff; line numbers move with that diff, so re-check them before
coding.

Follows [`c690-serialisation.md`](c690-serialisation.md) (the replay's
80-250 ms frames are walker-bound), [`walker-per-draw.md`](walker-per-draw.md)
(the caches that do not transfer live), [`walker-cuts-2.md`](walker-cuts-2.md)
(the walker is not the meter p50 lever) and [`wcb-async.md`](wcb-async.md)
(the readback waits are guest-driven and the prefetch removes the sync).

## TL;DR

* **The premise holds, but not for the reason "1,500 draws are independent".**
  The draws are ordered through (a) the mutable decoder/caches and (b) the GPU
  submission order. Given an **immutable per-draw packet** and an **ordered
  commit**, all the expensive per-draw work (reference build/remap, vertex
  conversion, texture revalidation/upload, pipeline fingerprint, Vulkan
  command recording) is independent and can run on workers. Nothing in the
  guest contract requires those to happen on the walker thread.
* **What cannot move off the ordered thread:** FIFO parse and method decode,
  surface/RT creation and target-set bookkeeping, clears, WCB arm/sync/fault
  service, `SET_REFERENCE`/semaphore labels, inline `NV3062`/`NV3089`
  transfers, `GET_REPORT`, flip/present and buffer-ref publication. This is
  ~10-12 ms of the crowded walker once the pure work is elsewhere (parse 4,
  state/packet build 3-4, commit/order 2, sync slack 1-2).
* **Recommended design: (a) a three-stage pipeline** - ordered state/packet
  thread -> per-resource resolver(s) and N record workers -> ordered commit
  and submit. Contiguous draw chunks per worker, one primary command buffer
  per chunk, submitted in chunk order. No frame-level split: the render target
  is not a safe parallel boundary here (draws sample earlier targets, and the
  backend's per-draw caches are global).
* **Expected crowded-replay walker time: 59.5 -> 20-28 ms (2.1-3.0x)** with
  three workers, packet-owned source bytes and the WCB prefetch on. Without
  moving texture/pipeline resolution (first implementation stage) it is
  **32-36 ms (1.7-1.9x)**; a single record thread alone is **45-50 ms
  (1.2-1.3x)**, which matches the inFAMOUS 2 recording-thread result.
* **Meter p50 will not move** (34.15 ms is the c690 producer); the design is
  for the crowded replay/play frames. It must be A/B'd at both ends and
  default **off** until the crowded test passes; a live key falls back to the
  single thread at any time.
* **No offline crowded capture exists yet.** `recomp-work/` has `meter_frame`
  (502 draws) and `walker_frame` (668 draws) but no 1,300-1,600-draw replay
  frame. The offline gates in this plan therefore use those two captures for
  exactness and fetch/pipeline/texture scaling, and the **first prerequisite**
  is one `PS3RECOMP_WALKER_CAPTURE` frame from the user's zoomed-out replay
  (command in [`walker-per-draw.md`](walker-per-draw.md) section 7.2).

## 1. What was measured, and the premise check

### 1.1 Crowded-frame medians from the user's logs

Both logs are 40 ms-hitch-recorder play sessions in the zoomed-out instant
replay, play defaults (`PS3RECOMP_TEX_DIRTY=1`, `PIPE_MEMO=1`, `REMAP_FAST=1`,
`VK_DRAW_BATCH=1`, `VK_FRAME_PIPELINE=1`, `WCB=2`). Reports with
`draws >= 1500` only (the crowded class), medians:

| term | `_165634` (n=185) | `_165829` (n=138) | notes |
|---|---:|---:|---|
| draws / report | 1,647 | 1,678 | the mission's 1,300-1,570 is the low end |
| walker drain | **59.5** | **76.7** | `walker=` |
| put-wait | 20.1 | 17.4 | outside the drain |
| engine (dispatch) | 53.9 | 55.5 | |
| parse | 4.0 | 18.1 | bimodal; 4 is the steady value |
| fetch | 8.3 | 9.2 | remap build + fetch plan |
| tex | 10.9 | 2.0 | bimodal; see below |
| pipe | 6.1 | 6.1 | fingerprint + memo |
| bind | 0.6 | 0.7 | |
| draw | 14.2 | 13.8 | Vulkan recording + CPU conversion |
| other | 8.1 | 16.3 | state decode / group setup / constants |

p10-p90 for the first log: walker 41-105, parse 3.7-20.9, fetch 6.8-13.3,
tex 1.4-26.8, pipe 5.2-9.8, draw 11.8-20.5, other 6.3-26.8. The second log's
`tex` and `parse` are anti-correlated: when the per-frame texture rehash
cadence lands, `tex` jumps and `parse` is low; the steady per-draw cost is the
first log's median.

At the meter (`walker-profile.md`, 552 draws) the live split is tex 11.3, draw
5.5, fetch 4.3, pipe 2.1, parse 1.6, other 2.6, bind 0.2 ms. The crowded
replay is the same code with ~3x the draws and a heavier texture/RT mix, not a
different bottleneck.

`[rsx wcb]` in `_165634`: 1.7 copies/frame, 6.9-8.3 ms copy+sync/frame, guest
wait 6.3 ms/copy. Those copies land inside the `tex` term when a texture unit
names a target, and inside the walker wall when the guest faults. This is the
guest-mandated part that must not be parallelised naively; the existing
`PS3RECOMP_RSX_WCB_PREFETCH` is the intended overlap.

### 1.2 Premise check

The mission's stated range (1,300-1,570 draws, 52-57 ms walker, 37-41 us/draw)
is the low end of what the logs show; the median crowded report is 59.5 ms
(36 us/draw) and the p90 is 105 ms. The wider range does not change the
design: the same components scale with draws.

There is **no guest-ordering reason the whole draw cannot be parallelised**.
The guest observes:

1. the method stream (state writes, clears, transfers, labels, flips) in FIFO
   order;
2. render-target contents at the sync points it reads back;
3. the fence/label values and the presented image.

It does **not** observe which host thread computed a vertex or recorded a
`vkCmdDraw`. Therefore the ordered thread and the workers can be split at the
point where a draw has been fully decoded into an immutable packet.

The two real constraints are:

* **cache/state mutation order**: surfaces, texture cache, pipeline table,
  WCB page state, write serials and the frame's `g.rsx` register file are
  single-threaded state. A worker that reads them while the walker decodes the
  next methods is a race. Packets must carry **derived copies**, not pointers
  into `g.rsx`.
* **guest data lifetime**: a worker that reads guest vertex/texture/program
  bytes later than the walker did can read a buffer the SPU/PPU has already
  reused. The guest's own fence (the `SET_REFERENCE` the walker publishes) is
  what usually prevents reuse; the current walker publishes it only after the
  commands are executed (and, with the GPU backend, recorded and submitted). A
  packet that defers a guest read must therefore
  either keep the fence unpublished until the read is done, or own a copy of
  the bytes. Both are used below (see section 2.5).

## 2. Dependency map of one draw

### 2.1 Stream and per-draw stages

The walker is `gcm_rsx_process_fifo_unlocked` (`cellGcmSys.c:2871`), called
from `cellGcm_rsx_process_fifo` (`:2600`). The relevant per-draw path:

```
FIFO words
  -> method decode (cellGcmSys.c:2871 loop, :3195 flip, :2745 CALL/JUMP)
  -> 3D method mirror (gcm_3d_method, cellGcmSys.c:2694; NV4097 -> rsx_state)
  -> draw engine method dispatch (rsx_draw_engine_method(s),
     rsx_draw_engine.c:7249/:7264 -> rsx_dispatch_method,
     rsx_dispatch.c:322)
  -> draw group accumulation (rsx_dispatch.c:436-479 ->
     sink_begin :5406, sink_draw_arrays :5444, sink_draw_index :5474,
     sink_inline_array :5457, sink_immediate_vertex :5415, sink_end :6845)
  -> per-draw work (sink_end_work, rsx_draw_engine.c:6500)
       layout          eng_vertex_layout                 :4207
       fetch           dc_fetch                          :5199
       render state    rsx_draw_engine_decode_render_state :1406
       targets         eng_current_target_set            :1820
       surface/zeta    rsx_dsp_get_surface (rsx_dispatch.c:572), eng_zdepth_get :2332
       pipeline        eng_pipeline_get                  :4527
       textures        sink_bind_textures                :5645
                        -> eng_texture_slot               :3239 -> :2994
                        -> eng_texture_upload             :2877 -> :2624
       indices         eng_topology_rebuild :1636, rsx_draw_engine_write_topology_indices :1546
       constants       g.vp_cb / g.fp_cb / g.fp_constants (rsx_draw_engine.c:6663-6685)
       capture/debug   pd_note                           :6147
       backend binds   bind_targets :3153, bind_pipeline :4273,
                       bind_vs_constants(_range) :4304/:4317, bind_ps_constants :4346,
                       bind_textures :4355, set_viewport :4372, set_scissor :4377,
                       set_stencil_ref :4382, set_depth_bounds :4392,
                       set_depth_bias (engine -> backend)
       record draw     draw_fill :5295 / draw :5286 -> vk_eng_draw_from :5050
                        -> vk_begin :819, vk_cmd_after_everything :810,
                           descriptor write :4955, render pass :3900,
                           framebuffer :4399, CmdDrawIndexed :5271, CmdDraw :5273,
                           submit/flush :4887
       write tracking  eng_surfaces_wrote :3446, eng_zdepth_wrote :3434
```

| stage | reads | writes | order requirement | can move? |
|---|---|---|---|---|
| FIFO parse / method decode | guest ring, `s_fifo_getoff` | `s_state`, `g.rsx`, jump/park/get | **guest order** | no |
| surface/target resolve | `g.rsx` surface regs, `g.surfaces[]`, `g.zdepths[]` | `g.surfaces[]`, `g.zdepths[]`, backend targets, `g.write_serial` | guest order (a target written by draw N is sampled by N+1) | state thread only |
| clear | `g.rsx`, targets, zeta | backend clear, surface serials | in stream order, before later draws | ordered commit |
| fetch/remap/convert | `g.rsx` layout/VTXFMT/index regs, guest bytes | `dc.refs`, `dc.ref_remap`, `dc.verts`, memos | none beyond packet's own data | **yes** (packet owns bytes or lifetime) |
| texture resolve/upload | `g.rsx` unit regs, `g.surfaces/zdepths`, guest bytes, texture cache | `g.textures[]`, handles, `s_tex_gen`, uploads | guest order only for target aliasing / WCB | **partly** (plain guest textures; target/depth paths stay ordered) |
| pipeline fingerprint/lookup | `g.rsx`, `g.rsx.vp`, guest FP bytes, `layout`, render state | `g.pipelines[]`, `g.pipeline_index[]`, `s_pmemo`, `g.fp_constants`, `s_vpc`, `s_vs_hlsl/s_ps_hlsl` | none if keyed on a copied packet; creation is ordered/serialised | **yes** with per-worker scratch + a serialised create |
| backend bind + record | packet fields, handles, rings | backend global state, command buffer, descriptor pool, rings | **graphics submission order** | **yes** on workers, commit in order |
| write tracking | target slots, draw state | surface/zdepth serials, `last_writer_*` | **guest order** | ordered commit |

### 2.2 What must stay in guest order (with the code)

1. **FIFO parse and method state.** `gcm_rsx_process_fifo_unlocked`
   (`cellGcmSys.c:2871`) is the only reader of the ring and the only writer of
   `s_fifo_getoff`, `s_state` and the engine's `g.rsx`. `gcm_3d_method`
   (`:2694`) and `rsx_draw_engine_method(s)` (`rsx_draw_engine.c:7249/:7264`)
   apply methods in order; `rsx_dispatch_method` (`rsx_dispatch.c:322`) updates
   the register file and calls the sink.
2. **Surface / render-target switches.** `eng_current_target_set`
   (`rsx_draw_engine.c:1820`) and `eng_surface_get` (`:1738`) create/reuse the
   target behind the offset; `eng_zdepth_get` (`:2332`) does the same for
   zeta. A target written by draw N is sampled by draw N+1; the write serials
   (`eng_surfaces_wrote` `:3446`, `eng_zdepth_wrote` `:3434`) must advance in
   draw order.
3. **Clears.** `sink_clear` (`:6862`) clears every target the set names and
   calls `eng_surfaces_wrote`/`eng_zdepth_wrote`; a clear cannot be moved
   ahead of or behind the draws it separates.
4. **WCB sync points.** `rsx_draw_engine_flush_dirty_to_guest` (`:7002`) is
   called at `SET_REFERENCE` (`cellGcmSys.c:2709-2711`), at semaphore
   `RELEASE` (`:3432-3434`), at the FIFO flip word (`:3221-3223`), at present
   (`eng_present`, `rsx_draw_engine.c:7063`) and from `rsx_draw_engine_flush`
   (`:7286`). In ondemand mode it arms dirty targets
   (`eng_wcb_sync_point`, `:3881`) and, with prefetch, submits hot copies.
   Faults are serviced only on the owner thread (`rsx_draw_engine_wcb_service`
   `:3941`, `rsx_wcb.c:558`; owner set at `rsx_draw_engine.c:3852/:3944`).
5. **Fences, labels and semaphores.** `gcm_ref_push_at` (`cellGcmSys.c:2227`)
   and `gcm_ref_publish_one` (`:2260`) queue/publish `ctrl->ref`;
   `gcm_ref_publish_one(0)` runs at the drain tail (`:3911`) and on the poll
   path (`cellGcm_ref_on_poll`, `:2430`). `NV406E` `OFFSET/ACQUIRE/RELEASE`
   (`:3410-3470`) write labels and can park the pass. All stay on the walker.
6. **Inline transfers.** `gcm_2d_method` (`cellGcmSys.c:1731`) and
   `nv3089_blit` (`:1523`) copy guest memory in stream order and call
   `rsx_draw_engine_guest_read`/`guest_wrote` (`rsx_draw_engine.c:2141/:2275`)
   around the copy; they may force a WCB resolve. They cannot be reordered
   with the draws around them.
7. **Report / zcull.** `NV4097_GET_REPORT` (`0x1A8`) is handled synchronously
   in `gcm_3d_method` (`cellGcmSys.c:2713-2736`) and writes 16 bytes to guest
   memory; it must stay in stream order. Zcull configuration is a guest API
   (`cellGcmSetZcull`, `:4930`), not a per-draw query; no readback is done.
8. **Flip, present and buffer-ref publication.** The `0xFEAD` word
   (`cellGcmSys.c:3195`) stops the drain and calls
   `rsx_draw_engine_flush_dirty_to_guest`, then
   `cellGcmSetFlipCommand`/`gcm_flip_request` (`:4081`). `sink_flip`
   (`rsx_draw_engine.c:7088`) -> `eng_present` (`:7060`) -> backend present
   (`rsx_vulkan_backend.c:3205`). The guest's `Gfx_DisplayFlip` waits for the
   buffer ref published by the walker; the present must be queued after every
   draw of that frame.

### 2.3 Pure per-draw work

Given an immutable packet and packet-owned bytes, these have no guest-visible
order of their own:

* reference-list build, dedup/remap and fetch-plan prepare
  (`dc_fetch`, `rsx_draw_engine.c:5199`; `rsx_vertex_remap_build`,
  `rsx_vertex_compact.c:888`; `rsx_vertex_fetch_plan_prepare`, `:199`);
* vertex conversion (`rsx_vertex_fetch_many(_stream)`,
  `rsx_vertex_compact.c:777/:784`), including the existing GPU unpack desc
  (`eng_check_gpu_vertex_offload`, `rsx_draw_engine.c:6334`);
* topology rebuild and index writing (`eng_topology_rebuild` `:1636`,
  `rsx_draw_engine_write_topology_indices` `:1546`) — output is packet-owned;
* plain guest texture revalidation/decode/upload (`eng_texture_slot_work`
  `:2994`), **excluding** target aliasing, depth snapshots, cube assembly and
  the WCB explicit copy (`sink_bind_textures` `:5645`, `eng_zdepth_snapshot`
  `:2403`, `eng_zdepth_as_color` `:2457`, `eng_cube_collect` `:2760`);
* pipeline fingerprint/key and memo lookup (`eng_pipeline_get` `:4527`),
  provided each worker has its own `s_vs_hlsl`, `s_ps_hlsl`,
  `g.fp_constants`, `s_vpc` scratch (or the packet carries the result);
* Vulkan command recording for the draw (`vk_eng_draw_from`
  `rsx_vulkan_backend.c:5050`) once the backend has per-worker contexts;
* constant-block formatting (`g.vp_cb` / `g.fp_cb`, `rsx_draw_engine.c:6663`).

### 2.4 Shared state and what a worker may touch

| state | owner today | risk if shared | plan |
|---|---|---|---|
| `g.rsx` (register file) | walker | every draw reads it | packet carries derived values only |
| `g.surfaces[]`, `g.zdepths[]` | walker | target handles, serials, WCB flags | state thread only; packet holds handles + a frame ref |
| `g.textures[]`, `s_tex_gen`, `s_tslot_last`, `s_tmemo`, `s_tdec` | walker | cache/LRU/revalidation races | one texture resolver (or per-shard locks); state thread keeps target/WCB paths |
| `g.pipelines[]`, `g.pipeline_index[]`, `s_pmemo` | walker | directory and memo races | state thread or one pipe resolver; per-worker memos are acceptable (hit rate drops, answer same) |
| `s_fmemo`, `s_rmemo`, `s_fmemo_index`, `s_rmemo_index` | walker | table races | per-worker (answer same; hit rate drops) or state thread |
| `s_vs_hlsl`, `s_ps_hlsl`, `g.fp_constants`, `s_vpc` | walker | shared scratch | per-worker copies, or pipeline resolve stays on the state thread |
| `dc.*` | walker | one group's refs/verts | becomes the packet's own buffers, per worker |
| `g.write_serial`, `g.frames`, `g.guest_draws` | walker | order | state thread only (or atomics for counters) |
| `s_wprof*`, `s_wp2*`, `s_wp_*`, `s_fill_ok`, `s_fs_draw`, `s_miss_*` | walker | profiling/statics | TLS per worker; force `WALKER_PROF` off for MT timing |
| backend `s_vk`, `s_e2`, `s_g`, `s_eb`, `s_ebs`, `s_epipe` | walker | one global context | per-worker context/rings/pools; state thread owns creation |
| `s_wcb`, `rsx_wcb` module | owner thread | faults, page tables | owner thread only; barrier before service |

### 2.5 Guest data lifetime (the constraint that shapes the design)

`dc_fetch` builds references and the fetch plan, and the actual bytes are read
later, either in `dc_convert_verts` (`rsx_draw_engine.c:4978`) or inside
`dc_fill_verts` (`:4962`) called by the backend's `draw_fill`
(`rsx_vulkan_backend.c:5295`). Textures are read in `eng_texture_slot_work`
(`:2994`). Programs are read from `g.rsx.vp` and guest VRAM.

Today all of that happens on the walker between the method that issued the
draw and the next, so guest memory cannot move underneath it. Two facts from
the reference ports say not to defer those reads naively:

* inFAMOUS 2's recording thread copies every payload it borrows into its queue
  and holds the queue record back until the backend has consumed it
  (`rsx_be_thread.c`, `OP_DRAW`, `OP_VSC_RANGE`, `rfree`; `docs/NOTES.md`
  "Recording Vulkan on a thread of its own").
* NHL Legacy's deferred-record bugs are exactly this: the fragment program
  hashed at draw-record time but compiled at render time read an
  already-overwritten VRAM ring (their patch #82), and movie textures read
  guest memory after the ring slot was reused (their patch #179). Their fixes
  snapshot the bytes when the draw is recorded.

Therefore this design uses two rules:

1. **Small/derived state is copied into the packet** (layout, render state,
   target slots, surface/viewport, 16 texture descriptors, VP constant rows,
   FP constant block, pipeline key inputs, vp/fp bytes or their hashes).
2. **Bulk guest bytes are either copied into a packet-owned arena or the
   packet's read is protected by the unpubished fence.** For mesh-ring vertices
   (EA 0x11900000-0x122fffff, the five c690 output rings) use the copy: the
   c690 producer is the one writer that can overwrite while the walker runs
   ahead. For plain PPU-built static geometry and textures, the walker can
   keep `get`/refs unpublished until the worker has read them (the normal
   no-reuse-until-ref rule) and skip the copy. Make this a live choice
   (`PS3RECOMP_WALKER_MT_SNAPSHOT=1` conservative, `0` fence-only) so a stale
   scene can be bisected without a rebuild.

The copy costs bandwidth but is parallel to the decode it feeds; the mission's
"whatever it takes" target is CPU wall time, and a memcpy of the source
vertices is cheaper than the expanded conversion it replaces.

## 3. Designs compared

### 3.1 (a) Pipeline split: ordered packets -> N workers -> ordered commit

**Shape.** The walker becomes the *state and packet* thread. For each draw it
emits a fixed-size intent (`eng_draw_packet`) with derived state and resource
keys; it never touches the Vulkan context after that. A contiguous run of
packets (a *chunk*, 8-32 draws or one `BEGIN/END` group boundary) is handed to
a worker. A worker resolves/records its chunk into its own primary command
buffer (`vkCmdBeginRenderPass`/`vkCmdDraw`/`vkCmdEndRenderPass` per draw, as
today) and signals completion. The state thread (or a dedicated commit thread)
submits chunk command buffers **in chunk order**, preserving guest draw order.
At a sync point it closes the chunk, waits for all prior chunks, submits, and
then services WCB/labels/flip.

```
 FIFO --> [state thread] --packet--> [worker 0] --cmd buf 0--\
            |  \--packet--> [worker 1] --cmd buf 1--+--> [ordered commit] --> VkQueue
            |   \-> [texture resolver] -> handles    |        |
            |                                        |        +--> present
            +-- sync points / clears / WCB / refs ---+  (barrier: all prior chunks done)
```

**Why it wins.** The four large terms (fetch 8.3, tex 10.9, pipe 6.1,
draw 14.2) are 39.5 ms of the 59.5 ms median. At three workers that is ~13 ms
of wall; the serial work (parse 4 + state/packet 3-4 + commit 2 + sync 1-2)
is ~10-12 ms, so the frame is ~20-28 ms. The texture resolver (and the optional pipeline
resolver) mutates a global cache in order; each is a single extra thread
because it is a pipeline, not a parallel fan-out.

**Vulkan shape.** Contiguous chunks, one primary command buffer per chunk per
worker, one `VkSubmitInfo` with the chunk buffers in order (or one submit per
chunk-group). No secondary buffers are needed because every draw is already a
complete render pass; this avoids the render-pass-continue restrictions.
`VK_EB_SETS=3` stays the frame pipeline; rings are partitioned per worker
inside each set, not multiplied.

**Risk.** High: backend context must be per worker; descriptor/ring/pool
ownership; packet-owned guest bytes; the WCB/label barriers; pipeline
creation serialisation. This is the only design that reaches 2-3x.

### 3.2 (b) Frame-level split between surface switches

**Shape.** Split the frame into segments between render-target switches (and
clears), replay the state at each segment start, record each segment on a
different thread, commit in order.

**Problem.** The segments are not independent: a later segment samples a
target produced by an earlier one (post-process, shadow, reflection, cube
faces), and the texture/pipeline/surface caches are global. You need a
dependency graph and cache locking, and the state at a segment start is a
subset of `g.rsx` (copying the 200 KB `rsx_dispatch` per segment is not
viable; deriving only what the segment needs is the same packet problem as
(a)). Load balance is target-driven: a frame may have a few large segments and
many tiny ones, and the replay's target mix is not known until the capture
exists. If only recording is parallelised, the win is the draw term alone:
14.2/3 ~ 4.7 ms, walker ~48-52 ms (**1.15-1.25x**). If the engine work is also
split per segment, a best case is ~25-30 ms but with a high deadlock/stale
risk (**1.4-1.8x realistic**).

**Verdict.** Rejected. It adds the hardest part (cross-segment dependencies and
cache locking) for less parallelism than (a).

### 3.3 (c) Narrower: one walker thread, helpers ahead, record on one thread

**Shape.** Keep the walker exactly as today for state and cache decisions.
Move the pure fetch/remap/convert and the plain texture hash/decode/upload to
one or two helper threads that run ahead of the draw; move the backend call
stream to one recording thread with a copied-payload queue (the inFAMOUS 2
`PS3RECOMP_BE_THREAD` design).

**Expected.** This is a real, low-risk win but it is not 2-3x. The walker
still does parse (4), pipeline (6), texture decisions/WCB (5-7), other (8),
binds (0.6): ~24-26 ms serial; the helpers overlap fetch (8.3), the upload part
of tex (4-6) and draw (14.2), ~26-28 ms over two helpers. Best case ~28-32 ms
(**1.9-2.1x**), realistic 35-45 ms (**1.3-1.7x**) because texture creation,
snapshots and readbacks are synchronous backend calls that drain the record
queue (the inFAMOUS port measured ~10% in its heavy view for this reason:
`t_native_latch` and texture creates were the queue drains). It cannot
parallelise the record across workers, and it leaves the biggest term (tex)
on the walker.

**Verdict.** Recommended as the **first two implementation stages** of (a),
not as the final design. It is the fallback (`PS3RECOMP_WALKER_MT=1,
WORKERS=1`) and the quickest way to prove the queue/packet machinery.

### 3.4 Comparison

| | (a) pipeline split | (b) frame segments | (c) helpers + one record thread |
|---|---|---|---|
| crowded walker (median 59.5 ms) | **20-28 ms (2.1-3.0x)** | 48-52 (1.15-1.25x); 25-30 best with engine split | 28-32 best, 35-45 realistic (1.3-2.1x) |
| parallel terms | fetch, tex, pipe, draw | draw (+engine if split) | fetch, tex upload, draw |
| serial floor | parse + state/packet + commit + sync | parse + state + cross-segment graph | parse + tex decisions + pipe + other |
| Vulkan work | per-worker contexts, ordered primary CBs | per-segment state replay | one record thread |
| risk | high | very high | medium |
| effort (agent missions) | 5-6 | 6-8 | 2-3 |
| main failure mode | packet/guest lifetime, backend context races | cross-segment dependency, load imbalance | queue drains (creates/readbacks), no record scaling |
| replay hash | per-packet digest vector + ordered commit | segment order + state replay | ordered record stream (same as today) |

### 3.5 Vulkan rules the implementation must follow

* **Command pools are not thread-safe.** One `VkCommandPool` per worker per
  frame set (or per chunk), with one command buffer and one fence per
  (worker, frame set); reset the pool only after that worker's fence has
  signalled. `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` only if you
  reset individual buffers; whole-pool reset is cheaper.
* **Secondary vs primary.** A primary buffer per chunk is the simplest correct
  shape here: each draw already begins/ends its own render pass, so a
  secondary would need `RENDER_PASS_CONTINUE` and a render pass opened by the
  parent. If the render-pass-merge optimization from
  [`walker-profile.md`](walker-profile.md) is done later, switch to one render
  pass per segment and record draws as secondaries inside it.
* **Submission order.** `VkSubmitInfo::pCommandBuffers` and sequential submits
  on one queue preserve the order the guest saw. Put one full memory barrier at
  the start of each chunk and at the end of the previous (`vk_cmd_after_everything`,
  `rsx_vulkan_backend.c:810`), and keep the per-target image barrier
  (`vk_barrier_image`, `:874`) in the chunk. Validate with
  `VK_LAYER_KHRONOS_validation` because cross-command-buffer render-target
  dependencies are exactly where a missing barrier shows up.
* **Descriptor pools are not thread-safe.** One pool per worker per frame set;
  allocate inside the worker only; reset after the set's fence. The existing
  `s_eb.pool` (`:4697`) is the model. Do not share `s_g.set` or
  `s_e2.split_set`.
* **Pipeline cache.** If `PS3RECOMP_VK_PIPELINE_CACHE` is enabled
  (`rsx_vulkan_backend.c:400-450`), `VkPipelineCache` access is externally
  synchronised: keep all `vkCreateGraphicsPipelines` on the state/compile
  thread, or take one mutex around creation. Workers may look up an existing
  `VkPipeline`, never create one in the first three missions.
* **Queue access.** `vkQueueSubmit`/`vkQueuePresentKHR`/`vkDeviceWaitIdle`
  stay externally synchronised by the existing `queue_mu` (`:781-806`). Only
  the commit thread and the present thread call them; workers never do.
* **Ring memory.** `VK_EB_*` sets are ~212 MB per frame set
  (`:4530-4535`) and there are 3 (`VK_EB_SETS`, `:194`). Do **not** allocate
  one full set per worker. Partition each set's VB/UB/IB/staging ranges by
  worker, or give each worker its own bump allocator over a fixed slice;
  reset at the set's fence as today (`vk_ebs_swap`, `:4768`).
* **Resource lifetime.** Workers reference handles created by the state
  thread; the no-lock guarantee is that the state thread does not release
  anything a pending chunk references. Keep the current frame-set lifetime
  (3 sets) and add a per-chunk refcount for surfaces/views that can be evicted
  by `eng_surface_get` (`rsx_draw_engine.c:1738`).
* **Present.** The present is the last submission of the frame; the commit
  thread calls `vk_eng_present` (`:3205`) after every chunk of the frame is
  submitted. The existing present thread and pacing are unchanged.

### 3.6 WCB, sync points and the replay gate

* **WCB is not parallel work.** `rsx_wcb` resolves only on the owner thread
  (`rsx_wcb.c:558`, owner `rsx_draw_engine.c:3852`); the faulting thread
  blocks in the handler. At a sync point the state thread must (1) close the
  chunk, (2) ensure all prior chunks are recorded and their command buffers
  submitted, (3) then arm/service. With `PS3RECOMP_RSX_WCB_PREFETCH=1` the
  copy is queued at the sync point and submitted without a wait, so this
  barrier is "submitted", not "GPU idle".
* **Labels/refs.** The drain tail publishes refs after the pass
  (`cellGcmSys.c:3911`); with MT, publish after the frame's chunks are
  submitted. `SET_REFERENCE` (0x50) queues a fence (`:2711`) and does not
  need the GPU to finish; submission order is the hardware contract.
* **Flip/present.** The flip word stops the drain (`:3195`) and presents; the
  commit of that frame must be a full barrier (all chunks submitted, present
  last).
* **Replay harness.** `replay_rsx_engine` drives the same walker
  (`tools/replay_rsx_engine_core.c:262`, `cellGcm_rsx_process_fifo` at `:279`)
  with the null backend, whose digest covers every backend call, binding,
  constant block, draw parameter and vertex/index byte
  (`tools/replay_null_draw_backend.c`). Two gates:
  * **exactness**: `PS3RECOMP_WALKER_MT=0` (or the null backend's
    `mt_workers=0`) and the historical hashes — `meter_frame` last
    `a6fcc849341eba9b`, gate `ff2a220c28750bc3`; `walker_frame` last
    `416abee2c5bff47d`, gate `3e6f6ad4ebb23ed6` (20/64);
  * **MT order**: a new `REPLAY_NULL_DIGEST_VEC` mode that stores one digest
    per draw on each worker and compares the vector to the serial path's
    vector in order. This is stronger than a single FNV chain and does not
    require making the null backend itself thread-safe.
  For timing, `REPLAY_NULL_HASH=0` (as today). The null backend has no
  Vulkan recording, so offline timing measures fetch/remap/texture/pipeline
  preparation, not the `draw` term; add a calibrated
  `REPLAY_NULL_DRAW_US=<us>` busy-wait (set from the live `VK_DRAWPROF`
  per-draw record cost, ~8-10 us) to exercise the chunk/commit pipeline
  offline. Say in every result whether the draw term was modelled or absent.
* **No WCB in the replay** (`wcb-async.md` section 5): the WCB barriers are
  live-only and must be validated with the unit tests and the crowded run.

### 3.7 Determinism and debuggability

* **Single-thread mode runs the same code.** `PS3RECOMP_WALKER_MT=1` with
  `WORKERS=1` executes the packet path on the walker inline; `WORKERS=0` is
  the packet path with no worker (the reference). `PS3RECOMP_WALKER_MT=0` is
  the old path. This lets every non-threading bug be caught in serial.
* **Live fallback.** Append live keys in `ps3_live_ctl.h` (before
  `PS3_LIVE_COUNT`): `PS3_LIVE_WALKER_MT`, `PS3_LIVE_WALKER_MT_WORKERS`,
  `PS3_LIVE_WALKER_MT_STAGES`, `PS3_LIVE_WALKER_MT_SNAPSHOT`. The walker polls
  `ps3_live_ctl_poll` once per drain (`cellGcmSys.c:2911`), so
  `PS3RECOMP_CTL` can set `PS3RECOMP_WALKER_MT=0` mid-frame; the state thread
  finishes the current chunk on the serial path and logs `[walker-mt] fallback
  why=key frame=N`.
* **Automatic fallback.** Spawn failure, queue full, per-worker allocation
  failure, a chunk that is too large for the ring, or a failed digest check
  makes that draw/chunk run inline and increments `fallbacks`; never abort the
  frame.
* **Instrumentation.** `[walker-mt]` every N frames: draws, chunks, workers,
  queue depth p50/p90, chunk wait, commit wait, sync-point drains by cause
  (label/release/flip/present/WCB), fallbacks. Add `PS3_TR` events for
  `mt.packet`, `mt.chunk_start`, `mt.chunk_done`, `mt.commit`,
  `mt.sync_barrier` so the existing timeline tools see the pipeline. Do not
  measure with `PS3RECOMP_WALKER_PROF` on (its timestamps are per-draw and
  become a large share under MT); use `VK_DRAWPROF` per worker and the
  `[walker-mt]` critical-path numbers.
* **Chunk boundaries are deterministic**: fixed draw count and sync-point
  boundaries. No timing-dependent assignment: worker `k` gets chunk `k mod N`
  of the current frame's contiguous chunks, or better round-robin over
  *closed* chunks; the commit order is the chunk index, not completion order.

## 4. Recommended design in detail

### 4.1 Threads and ownership

| thread | owns | never touches |
|---|---|---|
| `rsx walker` (state) | FIFO decode, `g.rsx`, `s_state`, surfaces/zdepths, clears, WCB owner, refs/labels/flip, packet build, commit/submit order | worker scratch, worker command buffers |
| `rsx-tex` (one) | plain guest texture cache (`g.textures[]`, `s_tex_gen`), texture create/upload; target/depth/cube paths stay on the walker | Vulkan draw recording |
| `rsx-pipe` (one, optional) | pipeline fingerprint/lookup and memo; pipeline creation under the pipeline-cache mutex | draw recording |
| `rsx-rec0..N-1` | fetch/remap/convert from packet-owned bytes, index build, per-worker VB/UB/IB slices, primary command buffer per chunk | guest-order caches, WCB, submit |
| `present` (existing) | window blit/present | draw recording |

The first implementation keeps `rsx-tex` and `rsx-pipe` **on the walker**
(the packet carries handles and the resolved pipeline); the worker only does
fetch/convert + record. That is M2/M3. M4 adds the resolvers.

### 4.2 Packet shape (first cut)

```
eng_draw_packet {
    u32 draw_index;                 /* commit order key */
    rsx_topology topology;
    int indexed; u32 index_count, vertex_count;
    rsx_vertex_layout_plan layout;  /* copied */
    rsx_be_render_state rs;         /* copied */
    u32 targets[4], n_targets;      /* surface handles + slots */
    u32 depth_handle; u32 zslot;
    u32 pipeline;                   /* handle resolved by the state thread */
    u32 tex[16]; rsx_be_sampler_desc samp[16]; u32 tex_mask; u32 vtex_mask;
    u8  vs_rows[...]; u32 vs_lo, vs_n; u32 vs_total;   /* packet-owned */
    u8  ps_rows[...]; u32 ps_bytes;                    /* packet-owned */
    float vp[4]; u32 sc[4]; u32 stencil; float depth_bounds[2];
    float depth_bias[2]; u32 depth_bias_format;
    u32 refs_off, n_refs;           /* packet arena: source or unique refs */
    u32 verts_off;                  /* packet arena: converted bytes, or bytes to convert */
    u32 idx_off;                    /* packet arena: index buffer */
    u64 digest;                     /* optional per-packet gate */
}
```

The packet is allocated from a per-frame arena (a few MB, reset at the frame
barrier) so the state thread never blocks on malloc in the hot path. The
worker's output (converted vertices, indices) stays in its own ring slice, not
the packet arena; the packet only owns the *input* bytes that the worker reads.

### 4.3 Barriers

| barrier | when | what must have happened |
|---|---|---|
| chunk close | every 8-32 draws or at a group boundary | packets immutable, arena frozen |
| commit | after chunks complete, in order | command buffers recorded; submit under `queue_mu` |
| sync point | `SET_REFERENCE`, semaphore RELEASE, WCB arm, NV3089 transfer, `GET_REPORT`, flip | all prior chunks recorded **and submitted**; then WCB/label/flip |
| frame end | flip/present | all chunks + present submitted; then publish `get`/refs |
| shutdown | engine shutdown | join workers, drain queues, destroy contexts |

### 4.4 Expected result

| stage | crowded walker (median 59.5 ms) | speedup |
|---|---:|---:|
| baseline | 59.5 | 1.0x |
| M2: one record thread | 45-50 | 1.2-1.3x |
| M3: 3 workers, fetch+record | 32-36 | 1.7-1.9x |
| M4: + texture/pipeline resolvers, WCB prefetch | 22-28 | 2.1-2.7x |
| M4 with 4 workers, best case | 20-24 | 2.5-3.0x |

Meter (552 draws): the walker is 27.6 ms and the frame is the c690 producer;
expect the walker to fall by a similar fraction (18-22 ms) but p50 to stay
34.15 +/- 0.5 ms. If a worker count steals CPU from c690 and p50 rises, lower
the worker count (2) or pin the workers away from the c690 lanes; the key
stays off at the meter if it cannot be neutral.

## 5. Implementation missions

Each mission ends with a measurable gate. "Serial gate" means
`PS3RECOMP_WALKER_MT=0` (or the null backend's no-MT mode) with the historical
hashes; "MT gate" means the digest-vector comparison. Never merge a mission
that fails its gate.

### M0 (prerequisite, not code): capture a crowded replay frame

The offline crowded benchmark does not exist. Run the existing capture command
from [`walker-per-draw.md`](walker-per-draw.md) section 7.2 in the zoomed-out
instant replay and keep `walker_replay.{fifo,mem,state}` under
`recomp-work/`. Gate: `replay_rsx_engine walker_replay --frames 5` reports
>= 1,300 backend draws and 0 read misses. If the capture is not crowded, the
later timing numbers cannot be trusted; stop and re-capture. This mission
needs a game launch, so it belongs to the next game-running mission, not to a
design-only one.

**Status (2026-11-04, M1/M2 mission): capture exists but FAILS the gate.**
`recomp-work/walker_replay.*` replayed 5 frames with **1,278 draws/frame and
23,535 read misses** (gate: >= 1,300 draws, 0 misses). Its MT hashes are
self-consistent but the timing is not crowded-case evidence; M3 timing must
wait for a clean recapture.

**Status (2026-11-04, M3 mission): capture code fixed, recapture pending.**
The live frame notes only the reads it performs, so a warm fetch-plan memo or
cached texture leaves out pages a cold replay then asks for and drops the
draws. `rsx_wcap_begin_if_triggered` now bumps `s_fast_epoch` and notes every
cached texture span. An offline replay-as-capture reproduced the failure and
went 1,278 -> 1,703 draws / 23,535 -> 211 misses; the user recapture command
is in [`walker-mt-m3.md`](walker-mt-m3.md) section 2.3. The existing capture
still fails the gate and is not used for timing.

### M1: packet ABI, serial packet path (no threads)

* Add `eng_draw_packet` + `eng_prepare_draw` / `eng_commit_draw` (or a
  `rsx_be_draw_packet` backend entry) behind `PS3RECOMP_WALKER_MT=1`.
  `sink_end_work` (`rsx_draw_engine.c:6500`) splits at the point where the
  pipeline, textures, constants and index buffer are resolved.
* The packet owns the VP `vpc_lo/n` rows, the FP constant block
  (`g.fp_constants`), the 16 texture descriptors and the layout.
* `WORKERS=0` executes prepare+commit inline on the walker in packet order.
* **Gate:** serial hashes unchanged (`meter_frame` `a6fcc849341eba9b` /
  `ff2a220c28750bc3`; `walker_frame` `416abee2c5bff47d` /
  `3e6f6ad4ebb23ed6`), 30/30 `test_video_fixtures.py`, and `walker_frame`
  timing within +0.5 ms/frame of today. **Kill:** any hash diff, >1 ms
  overhead, or >5% regression.

### M2: one record worker (the inFAMOUS 2 shape)

* SPSC queue of packet/backend-call records; worker replays the backend calls
  in order. The state thread still does surfaces, textures, pipeline, WCB.
* Copied payloads for the draw's vertices/indices/constants; sync calls
  (texture create/snapshot/readback) drain the queue exactly as in the
  reference (`rsx_be_thread.c`, `bq_direct_begin`; attribution below).
* **Gate:** live crowded walker -5 to -15% at the same draw count; meter p50
  within +/-0.5 ms; no validation error, no visual diff; MT digest vector
  equal to the serial vector. **Kill:** <5% or any queue-drain stall that
  makes the walker slower than baseline.

### M3: N workers, contiguous chunks, ordered commit

* Move fetch/remap/convert and the record into N workers; per-worker remap,
  fetch plan, scratch and primary command buffer; per-worker ring slices,
  command pool, descriptor pool. Commit chunks in order. Start with N=2, then
  3; never more than 4 on this 6C/12T machine.
* Snapshot mesh-ring source vertices into the packet arena; plain geometry may
  use the fence-only path behind `PS3RECOMP_WALKER_MT_SNAPSHOT`.
* **Gate:** `walker_frame`/`meter_frame` `REPLAY_NULL_HASH=0` timing at
  N=1/2/3/4: N=3 at least 25% better than M2 and N=4 not worse than N=3;
  live crowded at least 20% better than M2; MT digest vector identical; meter
  p50 within +/-0.5 ms. **Kill:** <10% incremental, any validation/TSAN race,
  any mesh/vertex hash diff, or a worker stall that shows up as a frame-time
  tail.

**Status (2026-11-04, M3 mission): implemented, exact, default off; timing
gate FAILS.** N workers, contiguous chunks and a global ordered-commit turn
are in `exp/live-ab` (M1/M2 packet shape): each worker does its own remap,
span resolve, index rebuild, mesh-ring snapshot and conversion, and may touch
the backend only at its packet's turn. Digests are identical for N=0..4 on
`meter_frame` and `walker_frame` (VSRANGE and normal), fixtures 30/30, and the
SPSC ring is TSAN-clean after fixing the producer-side acquire on the consumer
cursor. 7-run medians: `walker_frame` N=1 16.07, N=3 14.14 ms (-12.0%),
N=4 14.52 (worse than N=3); `meter_frame` N=3 10.60, N=4 10.89. N=3 is level
with M2 (14.21 ms), not 25% better. The lost win is the state thread's texture
create/upload/snapshot sync waits (`tex` 1.45 -> 7.47 ms on `walker_frame`)
plus its remaining texture/pipeline/reference work: that is the M4 resolver,
not more workers. No launch was used and nothing is default-on. Full numbers
and the failure analysis: [`walker-mt-m3.md`](walker-mt-m3.md).

**Status (2026-11-04, M4 mission): per-worker recording and the texture
resolver are implemented, exact, default off; the timing gate FAILS and the
mission stopped there.** The Vulkan backend now has per-worker contexts
(private command pool/buffer/fence, descriptor pool, GPU-unpack set and
ring slices, per-thread bind/render-pass/framebuffer/scratch state);
workers record whole segments and the engine submits them at the global
turn, so guest order is preserved. The `rsx-tex` resolver takes plain guest
textures; target/depth/cube/feedback stay on the walker. Digests are
identical for MT off/W=0..4 on `meter_frame` and `walker_frame` (VSRANGE and
normal), fixtures 30/30, `test_draw_queue` and `test_color_readback_guest`
pass. 7 interleaved runs on `walker_frame`: MT off 14.756 ms, W=3 16.055 ms
(+8.8%), W=3 with `TEX_RESOLVE=0` 12.869 ms (-12.8%, still short of -25%);
the state `tex` term is ~6.1-6.6 ms, not <=2 ms. Nearly every packet binds a
new texture, so the ordered texture group becomes a third thread in the
serial chain and costs more than the drain it replaced. Per-worker
recording cannot be judged offline (the null replay has no command
buffers); the live A/B was not run because the stop rule had already fired.
No live key and no `spu_context` change. Details:
[`walker-mt-m4.md`](walker-mt-m4.md).

### M4: texture and pipeline resolvers

* Add the one `rsx-tex` thread (plain guest textures only; target/depth/cube
  and WCB stay on the walker) and move the pipeline fingerprint/lookup to a
  resolver, with pipeline creation serialised on the cache. Packet carries
  texture keys and the resolved handles arrive through a per-packet slot.
* Turn `PS3RECOMP_RSX_WCB_PREFETCH=1` on for the A/B so the guest-mandated
  readbacks overlap the worker pipeline instead of the state thread.
* **Gate:** crowded walker <=30 ms (>=1.9x baseline), tex on the state thread
  <=5 ms/frame; meter p50 within +/-0.5 ms; `TEX_DIRTY` paranoid check 0 stale;
  WCB byte-for-byte test (`test_color_readback_guest`); no visual diff.
  **Kill:** <10% incremental, any stale texture/vertex/WCB byte, or a resolver
  wait >5 ms/frame.

### M5: hardening, live A/B and the user test

* TSAN/ASAN builds, 3-run soak, pinned and unpinned, 2/3/4 workers, fallback
  key exercised mid-run, shutdown/restart.
* Meter A/B (`ab_live.sh`, N=6 x 10 s, 2 s dropped, `PITCH_THRESH=35`):
  p50/p90/p99 within noise; walker us/draw not worse.
* User zoomed-out replay: 40-250 ms band -30 to -50%, `draws=` unchanged, no
  visual artifacts, no backward frames.
* **Gate:** crowded walker 2-3x with identical draws and images; meter p50
  neutral. **Kill:** <1.5x crowded -> leave the key off and keep the code as
  an experiment; any wedge/visual regression -> revert the key.

## 6. What the running vertex-remap mission changes

The in-flight diff on `exp/live-ab` (uncommitted at the time of writing)
does two things the design must account for:

1. **`PS3RECOMP_REMAP_DIRECT` / `PS3_LIVE_REMAP_DIRECT`** adds a direct-slot
   dedup path for dense, one-base index lists inside
   `rsx_vertex_remap_build` (`rsx_vertex_compact.c:888`) and a
   `rsx_vertex_remap_reload_env` test hook. The remap answer must remain
   identical; the MT design calls `rsx_vertex_remap_build` on a per-worker
   remap object, so it inherits the faster path for free. Do **not** reach
   into `rsx_vertex_remap` internals in the packet; copy the occurrence map
   and the unique prefix into packet-owned storage.
2. **`PS3RECOMP_WALKER_PROF_SAMPLE`** (`rsx_wprof_drain_begin`,
   `rsx_wprof_pass`) now times only every Nth drain. With MT, `fetch`/`tex`/
   `pipe`/`draw` include only the state thread's share; worker time must be
   counted separately (`WORKER_PROF`/`[walker-mt]`). An A/B with
   `PS3RECOMP_WALKER_PROF=1` is no longer a complete walker measurement; use
   `WALKER_PROF_SAMPLE=1` for a full-lived profile if needed, and never
   compare sampled and unsampled runs.

When the mission commits, re-read its diff in `rsx_vertex_compact.[ch]`,
`rsx_draw_engine.c` and `ps3_live_ctl.[ch]` before M1; it may add keys before
`PS3_LIVE_COUNT` and may move the exact lines above. Do not wait for it: the
packet ABI is independent of the remap implementation.

## 7. Risks and stop rules

* **CPU oversubscription.** 6C/12T already runs the walker, PPU main, present,
  five c690 lanes, SPURS kernel and audio. Three workers are the maximum worth
  trying; if the c690 burst lengthens or p50 rises, use 2 or pin. The crowded
  replay has the c690 lanes idle for most of the walker drain, which is the
  window the workers use.
* **Guest data reuse** (the main correctness risk). Start with the conservative
  snapshot for mesh-ring vertices; only relax to fence-only after a run with
  the conservative path is clean and the capture shows no mesh diffs.
* **WCB/label ordering.** Any sync point that is serviced before the prior
  chunks are submitted can expose stale target bytes or let the guest reuse a
  buffer early. The barrier table in 4.3 is the contract; a missed barrier
  shows up as a WCB stale check or a backward frame, not as a crash.
* **Replay gate is necessary but not sufficient.** The null backend proves the
  engine's decisions and byte outputs; it does not exercise Vulkan. The live
  crowded A/B and the user's zoomed-out replay are the acceptance tests.
* **Same failure twice -> stop.** If a race/wedge reproduces twice, fall back
  to the serial packet path, capture the `[walker-mt]` stats and the timeline,
  and report rather than guessing.

## 8. Reference ports and attribution

The multi-threaded recording design was checked first against the two MIT
reference ports, as the mission requires.

* **inFAMOUS 2** (`infamous2-pc`, MIT, Copyright (c) 2026 the
  infamous2-recomp contributors), `rpi/vk-record-thread/{REQUEST,PLAN}.md`
  and `docs/NOTES.md` ("Heavy area, round 2", where `rsx-record` is measured),
  plus the
  implementation in `work/patches/ps3recomp.patch` (`libs/video/rsx_be_thread.c`):
  * the engine decides on the RSX thread; every backend call is copied into a
    128 MB SPSC record queue and replayed by one `rsx-record` thread, so the
    backend sees the same call sequence on one thread at a time;
  * value-returning calls either answer from a cache (surface views, stable
    snapshots) or drain the queue (`bq_direct_begin`) and call the backend
    directly; payloads the backend borrows are copied and held (`rfree`)
    until the draw consumes them; deferred readback completions are handed
    back to the engine's thread;
  * measured: heavy view ~48 -> 54.5 fps (phase 1), 47.7 -> 53.6 fps after the
    two review fixes, New Marais 97.7 -> 100.1; later their RSX thread was
    ~92% busy with `rsx-record` idle half the time, and the synchronous
    creates/latches were the queue drains. This is the realistic (c) result
    and the reason this plan does not stop at (c).
  * `rpi/frame-pipelining/PLAN.md` and `rpi/vk-batching/REQUEST.md` are the
    two-frames-in-flight and per-frame command-buffer designs; our
    `VK_FRAME_PIPELINE`/`VK_DRAW_BATCH` already implement the same idea, so
    the MT design builds on those, not on a new frame pipeline.
  Ideas reused here: the ordered single-producer queue, copied borrowed
  payloads, sync-call drain, and the view/snapshot memo. New here: N record
  workers over contiguous chunks, packet-owned mesh-ring snapshots, and the
  digest-vector gate.
* **NHL Legacy native** (`nhl/repo`, MIT, Copyright (c) 2026 HeckYouMan):
  its renderer is single-threaded (D3D12 records a draw list on the render
  thread and executes it in `render_frame`); it has **no** multi-threaded RSX
  design to reuse. Two of its deferred-record bug fixes are directly relevant
  and are cited above: patch #82 (copy the fragment program at record time,
  do not re-read VRAM at build time) and patch #179 (snapshot movie textures
  at record time because the guest reuses the ring slot). Both are the same
  "borrowed guest data" rule the packet design adopts.
* **Clean room.** No RPCS3 or other GPL emulator source was read or cited.
  Both reference ports are MIT; the inFAMOUS 2 credits and NHL licence
  (MIT, HeckYouMan) are noted here. Any code derived from them must keep the
  MIT notice and add "Portions derived from inFAMOUS 2 recomp
  (c) 2026 the infamous2-recomp contributors, MIT" or the NHL equivalent in
  the file header and third-party notices.

## 9. Reproduce the data and the gates

The crowded-frame medians in section 1.1 are reproducible from the two logs
with a regex over `[walker-prof]` lines (`draws >= 1500`). The exactness gates
are the existing commands:

```sh
cd recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
# last a6fcc849341eba9b, gate ff2a220c28750bc3
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_frame --frames 64 --warmup 10
# last 416abee2c5bff47d, gate 3e6f6ad4ebb23ed6 (and the 20-frame variant)
cd ../../ps3recomp-play && python3 tools/test_video_fixtures.py   # 30/30
```

The live crowded A/B is the user's zoomed-out replay command from
[`wcb-async.md`](wcb-async.md) section 7, with the MT keys added:

```sh
# Arm A: play defaults, MT off.
PS3RECOMP_TIMELINE=/tmp/mt_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_WALKER_MT=0 ./play_windowed.sh

# Arm B: MT on, 3 workers, prefetch on.
PS3RECOMP_TIMELINE=/tmp/mt_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 PS3RECOMP_WALKER_MT=2 \
PS3RECOMP_WALKER_MT_WORKERS=3 PS3RECOMP_RSX_WCB_PREFETCH=1 \
./play_windowed.sh
```

Compare `[walker-prof]` `draws=`/`walker=` (draws must be equal), the
`hitch_table.py --min-ms 40 --bins --classify` bands, and the `[walker-mt]`
stats; then the user's zoomed-out replay visual check.

## 10. Caveats

* One machine, one scene family, one build. The crowded medians are from two
  user sessions, not a controlled A/B; the second session's `tex`/`parse`
  bimodality is why the design uses ranges.
* No offline crowded capture and no MT code yet: the expected times are
  arithmetic on the measured components, not a run. They are floors/ceilings
  for the missions, not a promise.
* The meter cannot show a walker win; it is c690-bound. A design that makes
  the crowded replay 2x and the meter p50 0.5 ms worse is not acceptable; the
  kill criteria include that.
* The `tex` term contains guest-mandated WCB copies; the prefetch removes the
  sync, not the bytes. If the crowded capture shows the 4 KB 32x32 exposure
  level dominating as in the user's 40 ms recording, expect the tex term to
  overlap but not disappear.
* Vulkan validation/TSAN are mandatory for M3/M4; the replay gate cannot see
  a command-buffer race.
