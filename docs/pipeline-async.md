# First-use pipelines off the walker (PS3RECOMP_PIPE_ASYNC)

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two cold pitch-meter launches (the mission cap):
one with the key off, one with the key on. The key is **default off**; the
async frame-dump writer and every other switch from
[`present-stalls.md`](present-stalls.md) stay as they were.

Follows [`present-stalls.md`](present-stalls.md) §2 (the 1-2 s first-use
drains), [`hitch-census.md`](hitch-census.md) §3 (the 5.1 s match load),
[`shader-disk-cache.md`](shader-disk-cache.md) §6 (why translation was left
synchronous then), [`walker-per-draw.md`](walker-per-draw.md) and
[`targets-and-backlog.md`](targets-and-backlog.md).

## TL;DR

* **A cold first-use drain is our HLSL -> SPIR-V translation.** In the cold
  key-off run, 918 stage translations took **5,600.8 ms** (459 VS, 459 FS;
  6.1 ms mean), `vkCreateShaderModule` took **25.3 ms** total, the fp/vp
  decompilers **53.7 ms**, and `vkCreateGraphicsPipelines` **448.8 ms**
  (460 builds, 0.98 ms mean, 6.7 ms max). The biggest single drain was
  `walker=4324.1 engine=4297.9 pipe=3440.9 pipecrt=178`; the walker profile's
  `pipe_decompile` in that drain was 22.1 ms. Translation is ~93 % of the
  first-use work and decompile is noise.
* **The disk cache removes the translation on a warm run; it cannot remove
  the first cold run.** This run started with `shader_cache/` moved aside:
  918 misses, 0 hits, 16 MiB written. [`shader-disk-cache.md`](shader-disk-cache.md)
  measured the warm load's `shader.compile` at 22 ms (vs 3,302 ms cold).
  The async path targets the run that fills the cache, and a cache
  invalidation.
* **Implementation (default off):** `PS3RECOMP_PIPE_ASYNC`
  (live key `PS3_LIVE_PIPE_ASYNC`). `pipeline_create` copies the two HLSL
  programs and returns a slot immediately; one worker thread
  (`libs/video/vk_pipe_queue.c`, 256-job bounded FIFO) translates both stages
  and creates the two `VkShaderModule`s. The `VkPipeline` itself is built on
  the draw's own thread when the modules are ready (measured max 6.7 ms,
  under one frame), because every attempt to build it on the worker returned
  null in the key-on run below.
* **Draw rule:** while a pipeline is pending, a draw waits until the frame's
  shared budget is spent (`PS3RECOMP_PIPE_ASYNC_WAIT_US`, default 2000 us,
  reset at each present) and is then skipped for that frame. Every skipped
  draw is counted and logged. A 2 ms budget bounds the walker's first-use
  cost to under one frame at 30 fps; the worker finishes the pipeline for
  the next frame.
* **Persistent `VkPipelineCache`:** the existing cache
  (`PS3RECOMP_VK_PIPELINE_CACHE`, device/driver/UUID-keyed file) is turned on
  by the async key unless explicitly set to 0. At worker start it also
  prefetches the guest-keyed shader-cache files into the page cache
  (`rsx_hlsl_to_spirv_disk_prefetch`), so the first disk hits do not wait on
  the filesystem.
* **The key-on launch did not complete.** Its worker translated modules
  correctly (36 `[pipe-timing]` lines) but `vkCreateGraphicsPipelines` never
  ran on the worker for any pipeline (0 `[pipe-create]` lines); every draw
  using those pipelines was skipped, the game never reached the pitch, and
  the 900 s `LIMIT` killed it. The code was changed afterwards to create the
  `VkPipeline` on the draw thread (see §4). **The key-on path has not been
  measured live since that change**; the morning user run is its first test.
* **Gates:** key-off replay digests unchanged (meter last frame
  `a6fcc849341eba9b`, gate `ff2a220c28750bc3`; walker last frame
  `416abee2c5bff47d`); `test_vk_pipe_queue` passes under clang TSan; video
  fixtures 34/34 (the pre-existing `test_gcm_tick_in_drain` flakes and passes
  on a rerun).

## 1. Where a first-use drain goes (cold, key off)

Run: `ab_live_pipeasync_off_20261105_061224`, boot -> pitch meter at 217 s,
`N=1 SEG=10`, `SKIP_CINE=1 PIN=0 WCB=2 VIDEODRIVER=x11`, shader and pipeline
caches moved to `recomp-work/captures/pipeasync_20261105/`, key off,
`PS3RECOMP_VK_PIPELINE_CACHE=1`, `PS3RECOMP_WALKER_PROF2=1`,
`PS3RECOMP_PIPE_TIMING=1` (the harness already sets the last one).

Drains before the X press, from the `[walker-prof]` lines:

| walker drain | count |
|---|---:|
| > 50 ms | 427 |
| > 200 ms | 9 |
| > 1 s | 1 (4,324.1 ms, the match load) |

The four largest drains:

| walker ms | `pipe` ms | `pipecrt` (driver) ms | what |
|---:|---:|---:|---|
| 4,324.1 | 3,440.9 | 178 | match load |
| 971.8 | 732.9 | 27 | first pitch/scene pipelines |
| 857.3 | 557.5 | 65 | next scene burst |
| 649.1 | 23.0 | 1 | the game's own work, no new pipelines |

Whole-run stage totals (the `[pipe-timing]` lines, 918 translations):

| step | total | mean |
|---|---:|---:|
| HLSL -> SPIR-V (`rsx_hlsl_to_spirv_keyed`) | **5,600.8 ms** | 6.10 ms |
| `vkCreateShaderModule` | 25.3 ms | 27.6 us |
| fp/vp decompile (walker `pipe_decompile`) | 53.7 ms | — |
| `vkCreateGraphicsPipelines` (`[pipe-create]`) | 448.8 ms (460) | 0.98 ms (max 6.7 ms) |

Source of each translation: 216 ran glslang, 702 were memory-cache hits for
the same HLSL (the title reuses program text across render states). With a
cold guest-keyed disk cache every one was a miss; the run wrote 918 files,
16 MiB. The driver-pipeline step is only 7 % of the walker's first-use cost.

## 2. What was built

### 2.1 Key and defaults

| switch | default | meaning |
|---|---|---|
| `PS3RECOMP_PIPE_ASYNC` (live `PS3_LIVE_PIPE_ASYNC`) | **0** | translate pipelines on the worker |
| `PS3RECOMP_PIPE_ASYNC_WAIT_US` | 2000 | per-frame wait budget for a pending pipeline; 0 = never wait |
| `PS3RECOMP_VK_PIPELINE_CACHE` | async key turns it on | persistent driver pipeline cache (existing) |

No `spu_context` field was added.

### 2.2 Worker and queue

`libs/video/vk_pipe_queue.{c,h}`: one named thread and a bounded FIFO of
`void*` jobs. `push` never blocks and never drops (it returns -1 when full,
so the caller falls back to the synchronous build); `destroy` stops the
worker, joins it, and calls a drop callback for every job still queued
(so the copied HLSL is freed exactly once). The queue has its own mutex and
condition variable; the backend has a second mutex/condition for pipeline
state, and completed work is published with release/acquire atomics.

`vk_eng_pipeline_create` with the key on:

1. allocate the backend slot and copy the render state, layout, stride,
   target format/count and cube mask (the same fields the synchronous path
   sets after compiling);
2. copy both HLSL strings;
3. push a modules job; on success set `async_state = 1` and return the
   handle. The queue being full, or the worker failing to start, falls back
   to the old synchronous build of the same slot.

The worker translates VS then FS into caller-owned SPIR-V/log buffers
(`s_pa.spv`/`s_pa.log`, not the walker's `s_g` scratch) and creates the two
shader modules. The existing shader disk cache and translation cache are
already internally locked, so the worker never shares glslang state.

### 2.3 Draws while a pipeline is pending

`vk_eng_variant` (first draw of a topology) checks the pipeline's state:

* `1` (worker translating): wait until `async_state >= 2` or the frame's
  deadline, whichever comes first;
* still pending at the deadline: skip this draw, log
  `[pipe-async] skip draw pipeline=N topo=T reason=modules waited=X ms total=K`
  and return;
* `3` (translation failed): skip and log `reason=failed`;
* `2`: build the missing `VkPipeline` under the existing
  `s_pipe_create_mu` and record the draw.

The deadline is one budget per **frame**, not per draw: `vk_eng_present`
sets it to `now + PS3RECOMP_PIPE_ASYNC_WAIT_US`, and every draw shares it.
A frame that introduces 200 pipelines therefore pays at most 2 ms of waiting,
not 200 x 2 ms; the rest are skipped and appear when the worker has built
them (usually the next frame). This is the "briefly missing objects" trade
named in the mission.

On shutdown the worker is stopped **before** the engine releases its
pipelines, so no worker can touch a freed slot; the mutex/scratch are freed
after `rsx_draw_engine_shutdown` has joined the record workers.

### 2.4 Persistent VkPipelineCache and warm-up

The driver cache already existed (`vk_pipe_cache_init/save`, keyed by
physical-device `pipelineCacheUUID` and driver version). With
`PS3RECOMP_PIPE_ASYNC` set it defaults on; an explicit
`PS3RECOMP_VK_PIPELINE_CACHE=0` still wins. The worker's first job reads every
`<16hex>_<stage>.spv` in `$PS3_GAMEDATA_ROOT/shader_cache` into the page cache
(bounded to 64 MiB, nothing retained) and logs
`[pipe-async] warm-up: N shader-cache files, M MiB in T ms`. It does not
change the cache contents.

## 3. Gates

| gate | result |
|---|---|
| `replay_rsx_engine meter_frame --frames 64` (key off) | last `a6fcc849341eba9b`, gate `ff2a220c28750bc3` (matches `walker-per-draw.md`) |
| `replay_rsx_engine walker_frame --frames 64` (key off) | last `416abee2c5bff47d` (matches) |
| `test_vk_pipe_queue` (clang `-fsanitize=thread`) | PASS, no reports |
| `tools/test_video_fixtures.py` | 34/34 on the final run (`test_gcm_tick_in_drain` is pre-existing flaky; it failed once and passed on rerun) |
| `check_spu_context_abi.sh` | no `spu_context` fields added |

The walker gate value printed here is `0bdb6c112e64e643`, not the
`3e6f6ad4ebb23ed6` in the older document. The last-frame digest, which is the
scene identity, matches; the 64-frame gate has drifted with the commits
between that note and this branch (the replay path does not link the Vulkan
backend).

## 4. The key-on launch: it failed, and what changed

Run: `ab_live_pipeasync_on_20261105_061713`, same cold-cache setup, key on.
The worker started (`[pipe-async] warm-up: 0 shader-cache files`), translated
the first pipelines (36 `[pipe-timing]` lines, modules 4-28 us), and then
every draw of those pipelines logged `reason=failed`. Over the 900 s
`LIMIT` it logged **459,673 skipped draws**; `grep -c '[pipe-create]'` was
**0**, i.e. `vk_eng_variant_create` returned before
`vkCreateGraphicsPipelines` for every pipeline. The screen never reached the
pitch (`nav3 failed` when the timeout killed the process).

The first version built the `VkPipeline` on the worker as well (it took a
second job kind with the topology). Whatever the early return was (render
pass, split descriptor layout, or pipeline layout created in the worker's
scratch context), it made the whole path unusable, and the 2-launch cap
does not allow another live A/B after changing it. The final code keeps only
the translation/modules on the worker and builds the `VkPipeline` on the
draw's thread, which is exactly the proven synchronous path and measured at
at most 6.7 ms. **The live key-on numbers (drains, skips, thumbnails) are
therefore not available for this note.**

The shader cache and the `VkPipelineCache` were moved to
`recomp-work/captures/pipeasync_20261105/` for the two runs and the original
`shader_cache/` and pipeline-cache file were restored afterwards; nothing was
deleted.

## 5. Morning user run (key on)

The play scripts do not enable the key. Start the game windowed with:

```sh
cd /home/m/Documents/mlb16-online
PS3RECOMP_PIPE_ASYNC=1 \
PS3RECOMP_SHADER_DISK_CACHE=1 \
PS3RECOMP_VK_PIPELINE_CACHE=1 \
tools/play_windowed.sh        # or the same variables in the normal launcher
```

Then start a match and let the pregame sequence play, throw a few pitches,
and let one batter cinematic run.

What to look for:

* **Match start / first camera cuts:** the 1-2 s freeze should become a few
  frames of missing objects (players, crowd, HUD pieces appear one or two
  frames later), not a single long stall. The terminal prints
  `[pipe-async] skip draw ...` for each skipped draw and a summary at exit.
* **A few pitches / cinematic:** fewer multi-hundred-ms frames; if something
  is briefly absent, that is the skip side effect, not corruption.
* **If the screen stays incomplete for more than ~1 s**, or `reason=failed`
  appears, stop and report it: the worker's translation failed and the
  pipeline will never appear. The log line names the handle.

## 6. MIT reference ports and clean room

Credit, with attribution:

* **inFAMOUS 2 PC port (MIT), `docs/NOTES.md`:** a new pipeline no longer
  drains the recording queue; the backend translates shaders and fills a
  pipeline slot on the engine's thread beside the worker
  (`pipeline_create_any_thread`), glslang is initialised once thread-safely,
  and the port keeps its own `VkPipelineCache` at
  `~/.cache/ps3recomp/vkpipe-<vendor>-<device>-<uuid>.bin` because Mesa's
  shared cache evicts entries. This mission takes the same shape: one
  worker, a slot returned before the pipeline is ready, translation off the
  frame thread, and a private persistent driver cache.
* **NHL Legacy port (MIT), `docs/notes/NEXT_STEPS.md`:** the persistent
  PSO library (#128, the `VkPipelineCache` analogue) and the async-compile
  plan (#225/#227: "skip a draw until its PSO is ready; keep sync for
  full-screen post/composite passes"). The data here agrees with their
  conclusion that faster compiles alone do not remove a burst of hundreds of
  new programs per match or arena, and that a skipped draw is the visible
  cost.

No RPCS3 or other GPL emulator source was read or cited. Everything here is
this tree's own code, logs, captures and EBOOT.
