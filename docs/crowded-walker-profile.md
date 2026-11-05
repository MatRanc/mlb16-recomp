# Crowded walker profile: the real Vulkan replay

Date: 2026-11-05. Branch `exp/crowd-prof` (off `exp/live-ab`) in a new
worktree `ps3recomp-prof`, build `recomp-work/build-prof`. **Zero game
launches**: everything here is the crowded capture replayed offline, plus the
user's own `recomp-work/play_20261105_110451.log` for the live numbers.
Follows [`walker-tex-cost.md`](walker-tex-cost.md),
[`walker-prof.md`](walker-prof.md),
[`capture-tools-fix.md`](capture-tools-fix.md),
[`texture-dirty.md`](texture-dirty.md),
[`walker-mt-live.md`](walker-mt-live.md) and
[`targets-and-backlog.md`](targets-and-backlog.md). No RPCS3 or other GPL
emulator source was read or cited.

## TL;DR

* **The replay tool now drives the real Vulkan backend.**
  `PS3RECOMP_REPLAY_VK=1` on `replay_rsx_engine` runs the same captured frame
  through `libs/video/rsx_vulkan_backend.c` headless on the RTX 3060 Ti,
  with the shipping guest-program path, `VK_DRAW_BATCH=1` and
  `VK_FRAME_PIPELINE=1` (the `play_windowed.sh` settings). The null path is
  untouched and still reproduces its old digest exactly.
* **The puzzle was the backend, not the engine.** The same frame
  (`captures/walker_replay_fixed`, 1,694 backend draws / 1,702 sink groups,
  0 read misses) costs **walker 34.0 ms and `tex` 5.0 ms with the null
  backend**, but **walker 62.2 ms and `tex` 23.8 ms with the real Vulkan
  backend**. The missing ~19 ms of `tex` is Vulkan texture-bind work the null
  backend never does; the missing ~8 ms of `draw` is Vulkan command recording.
  The "2.4-3.0 ms tex" in the premise is a null-backend number from the old
  1,261-draw capture, not a crowded Vulkan frame.
* **With Vulkan, the offline split matches the live crowded split.** Offline
  VK: `tex` 23.8, `draw` 13.8, `fetch` 10.4, `pipe` 5.4, `other` 5.4,
  `parse` 2.8 ms. Live crowded (11:06-11:11 window, 314 reports): `tex`
  28.1, `draw` 14.5, `fetch` 8.7, `pipe` 7.8, `other` 10.8, `parse` 16.9 ms.
  `tex` and `draw` are reproduced; `parse` and `other` are the live-only
  remainder.
* **The single biggest measured lever is the depth-target guest fallback.**
  The capture binds a depth target as a texture 222 times/frame under Vulkan
  (`dguest`; live: 228/frame). Each dirty target goes through
  `eng_zdepth_to_guest` -> `eng_zdepth_store` -> backend readback + guest
  store, and the store also dirties a texture that is re-uploaded next frame.
  A measurement-only switch that skips the guest copy
  (`PS3RECOMP_ENG_DEPTH_NOGUEST=1`) takes **`tex` 24.2 -> 5.5 ms and walker
  64.9 -> 43.7 ms**. Live pays the readback but not the re-upload
  (`uploads=0.0`), so the conservative live value is **~13 ms/frame**, the
  upper bound ~19 ms.
* **Next: per-draw Vulkan recording (~4-8 ms), then the live-only `parse`
  term (up to ~14 ms, not CPU contention).** A 5,725-sample `perf record -g
  --call-graph dwarf` of the steady Vulkan replay puts 15.5% of all samples in
  `libnvidia-glcore` under `vk_eng_draw_from`; the null->Vulkan `draw` delta is
  6.0 -> 13.8 ms. The live `parse` is 16.9 ms against 2.8 ms offline, and a
  memory-burner sweep shows CPU contention does **not** inflate `parse`
  (3.0 ms with 2 x 16 MB burners, 9.6 ms only at 5 x 64 MB), so it is an
  in-drain live stall / FIFO-path cost that needs a live perf run, not a
  renderer cut.
* Nothing is default-on. The only code changes are measurement switches on
  `exp/crowd-prof`; they are pushed to `fork` only.

## 1. What was added

### 1.1 The replay switch

`tools/replay_rsx_engine_core.c` used to hard-code the null backend. It now
reads `PS3RECOMP_REPLAY_VK` (`core.c:248`):

* `=0` (default): the old path, bit for bit (`replay_null_draw_backend.c`);
* `=1`: `rsx_vulkan_backend_init(w, h, "replay")`, which registers the
  shipping `rsx_draw_backend`, then the captured state is restored on top.
  `PS3RECOMP_VK_GUEST_PROGRAMS` is defaulted to `1` for this tool only so the
  register-file engine is the active path; the run fails loudly if glslang is
  missing.

`CMakeLists.txt` adds the backend, `vk_pipe_queue.c`, `vk_dump_queue.c`,
`rsx_shader_spirv.cpp` and the SDL2/glslang/Vulkan link only when
`PS3RECOMP_RSX_VULKAN` is on. The frame loop is unchanged: the capture's
`0xFEADxxxx` flip word reaches `eng_present` -> `vk_eng_present`, which
records the present copy and submits it (`vk_eng_present` does not wait; the
readback lives in `rsx_vulkan_backend_present_finish`, which a live host runs
on its presentation thread). The replay therefore measures the walker's
recording + submit cost, exactly the live `walker`/`draw` semantics.

Null exactness is unchanged by the switch:

| capture, 5 frames + 2 warmup | output hash | gate |
|---|---|---|
| `captures/walker_replay_fixed` (build-play) | `0e8f7c71ca8be27c` | `a9a80aa0392953cd` |
| the same (build-prof, null) | `0e8f7c71ca8be27c` | `a9a80aa0392953cd` |

Vulkan mode reports `draws/frame 0` in the tool's summary because
`rsx_vulkan_backend_guest_draws()` is read after `eng_present` and the capture
has no display buffer, so `eng_present` returns before setting
`last_guest_draws` (rsx_draw_engine.c:8794). The profiler's `draws=` and the
per-frame draw count in this doc come from `[walker-prof]`, which is exact.

### 1.2 The depth-guest measurement switch

`PS3RECOMP_ENG_DEPTH_NOGUEST=1` makes `eng_zdepth_to_guest`
(rsx_draw_engine.c:3735) return 0, so a depth target sampled as a texture
takes the "guest copy unavailable" branch. It changes the image (the unit gets
the dummy), so it is a measurement switch only; it is how the cost of the
readback + store is bounded.

## 2. Method and the capture

`captures/walker_replay_fixed` is the capture-tools fix's rebuilt page set
(`[`capture-tools-fix.md`](capture-tools-fix.md) §3.2): 385,837 words,
113 MB of guest pages, **1,694 backend draws / 1,702 sink groups, 0 read
misses**. That is the live draw count; the old `walker_replay` (1,261 draws,
4,547 misses) is not used.

Device and path, from the run log:

```
[RSX vulkan] using device 0: NVIDIA GeForce RTX 3060 Ti
[RSX vulkan] guest programs: on (HLSL -> SPIR-V through glslang)
[RSX vulkan] draw batching: on (up to 512 draws per submission)
[RSX vulkan] frame pipelining: on (3 batch sets)
```

Arms (12 measured frames, 4 warm-up, `REPLAY_NULL_HASH=0` for timing,
repeated 3x; medians below are per-report medians after report 0). Numbers in
ms/frame; `walker` is the whole drain, `parse = walker - engine`:

| phase | null | Vulkan | Vulkan + NOGUEST |
|---|---:|---:|---:|
| walker | **34.00** | **62.21** | **43.68** |
| engine | 31.30 | 59.39 | 40.9 |
| parse | 2.67 | 2.82 | 2.81 |
| fetch | 9.87 | 10.40 | 10.74 |
| tex | **4.96** | **23.82** | **5.46** |
| pipe | 5.14 | 5.41 | 5.31 |
| bind | 0.18 | 0.55 | 0.5 |
| draw | **6.01** | **13.76** | 13.33 |
| other | 5.15 | 5.39 | 5.50 |
| us/draw (walker) | 20.0 | 36.5 | 25.7 |
| us/draw (tex) | 2.9 | 14.0 | 3.2 |

The tool's own frame-time medians agree: null **30.0-31.0 ms**, Vulkan
**60.6-61.3 ms**, Vulkan+NOGUEST **40.3-41.7 ms** over the repeated runs.

Enabling the play defaults (`PIPE_MEMO=1 REMAP_FAST=1 RSX_TEX_PIN=1` and the
four `ENG_TEXCACHE`/`TARGET_BLOOM`/`TEXMEMO_LOCAL`/`VK_VIEW_MEMO` keys) did
not change the offline result: walker 66.7 ms, `tex` 24.2, `draw` 16.1
(run-to-run load, not a regression).

### 2.1 The live reference

`recomp-work/play_20261105_110451.log`, 11:06:00-11:11:00 (`at` 69-369 s),
full-crowd reports (`draws >= 800`, `engine > 0`, `flips <= 40`; 314
reports). The full 1,702-draw subset (176 reports) is quoted separately
because it is the same draw count as the capture.

| phase | live crowded (314) | live ~1702 draws (176) | offline Vulkan |
|---|---:|---:|---:|
| walker | **92.3** | **170.9** | **62.2** |
| parse | **16.9** | **36.1** | **2.8** |
| tex | 28.1 | 52.1 | 23.8 |
| fetch | 8.7 | 15.1 | 10.4 |
| pipe | 7.8 | 13.9 | 5.4 |
| bind | 0.8 | 1.4 | 0.55 |
| draw | 14.5 | 25.8 | 13.8 |
| other | 10.8 | 20.4 | 5.4 |
| us/draw (walker) | 54 | 100 | 37 |

Caveat on the live columns: `[walker-prof]` divides every phase by the
report's `flips`, so reports with few flips (the slow frames) produce large
per-frame values and all phases move together. They are the game's own
measurement and are used here as a split, not as an absolute budget. The
absolute on-screen rate is 9-11 fps (90-110 ms/frame).

## 3. Where the Vulkan time goes

### 3.1 The texture bind

`PROF2` on the same frame (one run, medians after warm-up):

| counter | null | Vulkan |
|---|---:|---:|
| `tex_slot` (plain cache slot) | 2.9 ms | 8.5-10.3 ms |
| `tex_upload` | 0.0 | **5.7 ms** |
| `tex_bind` (all `sink_bind_textures`) | 3.3 ms | **23.2-24.8 ms** |
| tex binds/frame | 1,669 | 1,669-1,891 |
| `tex_loads` (uploads) | 0 | **1.0/frame** |
| `dguest` (depth->guest binds) | 0 | **222/frame** |
| `dsnap` (depth snapshot binds) | 702 | 480/frame |
| surface views `sv` | 68 | 68 (290 with NOGUEST) |
| `uploads` stat | 0.0 | 0.0-1.0 |

The null backend answers `depth_snapshot` and `depth_as_color` with fake
handles for every depth bind. The Vulkan backend can snapshot only the
device's D32 format; the other depth targets take
`eng_zdepth_to_guest` (rsx_draw_engine.c:3735, `dguest` at :6222). When the
target is dirty, `eng_zdepth_store` (:3672) does
`submit_and_wait` -> `depth_readback` -> guest store. That wait and copy is
invisible to `perf` (the thread is asleep), which is why the CPU profile
understates `tex`: 222 guest-path binds/frame cost **18.8 ms**
(`tex 24.2 -> 5.5`), and the guest store also dirties a texture that costs
the 5.7 ms re-upload.

Live `[tex-stats]` from the same window. The counter moves in exact
multiples of one draw-carrying frame (228 `dguest`), so a 5-frame report reads:

```
11:06:30 [tex-stats] frames=5 slots=1975.0 memo=20275 full=13160 new=0
evict=0 sv=590 dsnap=2425 dguest=1140 reint=0 cube=270 fb=0 ntex=1024
cap=1024 nsurf=64 nz=64 ... uploads=0.0
```

That is **228 dguest/frame, 485 dsnap/frame, 118 surface views/frame** --
the same path and nearly the same counts as offline (222 / 480 / 68) -- with
`uploads=0.0`, so live pays the readback but not the re-upload. The
conservative live saving from removing the guest path is therefore
**~13 ms/frame** (18.8 minus the 5.7 upload); the upper bound is 19 ms.

The four texture keys from the earlier A/B do not touch this path: they
shorten the plain-cache probe and the target-table scans, not the depth
readback. That is why the live A/B was neutral.

### 3.2 The draw

`perf record -D 6000 --call-graph dwarf` over 100 steady Vulkan frames
(5,725 samples, 90 MB):

| sample owner | share | where |
|---|---:|---|
| `replay_rsx_engine` | 59.5% | engine, fetch, hash, texture decode |
| `libnvidia-glcore` | **15.6%** | **13.4% under `vk_eng_draw_from`** |
| `libc` (memmove/malloc) | 13.2% | vertex, upload, depth readback |
| `[vdso]` | 5.4% | mostly the profiler's own `clock_gettime` and backend threads |
| `[kernel]` | 4.0% | |
| `[nvidia]` | 1.9% | `osDevReadReg032` |

Call-tree shares of the whole run (100% = ~61 ms/frame; 1% ~= 0.61 ms):

| block | share | ms/frame |
|---|---:|---:|
| `sink_end_work` (engine, all draws) | 47.1% | 28.7 |
| - `sink_bind_textures` | 16.9% | 10.3 |
| - - `eng_texture_slot` | 13.2% | 8.1 |
| - - - `eng_texture_upload`/decode | 7.9% | 4.8 |
| - - - content + sparse hash | 5.0% | 3.0 |
| - - `eng_zdepth_store` | 2.8% | 1.7 (CPU only) |
| - `rsx_vertex_remap_build` (fetch) | 9.5% | 5.8 |
| - `eng_pipeline_get` (pipe) | 8.4% | 5.1 |
| - `rsx_vertex_fetch_plan_prepare` | 4.2% | 2.5 (2.2 of it capture `cap_read`) |
| - `vk_eng_draw_from` (exe self) | 1.1% | 0.7 |
| - `rsx_vp_analyze_inputs` | 1.0% | 0.6 |
| - `rsx_draw_engine_write_topology_indices` | 0.8% | 0.5 |
| `vk_eng_draw_from` -> vertex fill (`rsx_vertex_fetch_blocks`) | 7.0% | 4.3 |
| `vk_eng_draw_from` -> driver (the 13.4% above) | 13.4% | **8.2** |

The driver block is `vkCmdBindPipeline`/`vkCmdBindDescriptorSets`/
`vkCmdDraw`/barriers inside `vk_eng_draw_from` (rsx_vulkan_backend.c:5639;
the explicit `vk_eng_write_descriptors_to` part is only ~1% of samples).
No pipeline compilation appears in the steady run (`libnvidia-gpucomp` is
absent after the 6 s delay); the 4-5 s first frame is cold pipeline creation
and is excluded.

Without `VK_DRAW_BATCH=1` the same replay is **walker 162 ms, draw 117 ms**:
per-draw submission is already batched away, and the remaining 13.8 ms is
per-draw recording/binding, not submission.

### 3.3 The live-only remainder

`parse` is the largest live-vs-offline gap (16.9 ms median, 36.1 ms on the
full-draw subset, vs 2.8 ms offline). It is the drain time outside
`sink_end`, i.e. FIFO decode/bookkeeping. It is **not** ordinary CPU
contention: pinning the replay to one CPU and running memory burners beside
it leaves `parse` at 3.0 ms for 2 x 16 MB and only 9.6 ms at 5 x 64 MB
(while `draw` goes 14 -> 30 -> 129 ms and `tex` 24 -> 35 -> 79 ms).
The live drain reads a ring the guest producers are actively writing and
waits on the reference/semaphore path; the capture is a private preloaded
stream with no producer. A live `perf record` of the walker is the only way
to split wait from work.

## 4. What differs, and which were tested offline

| difference | status |
|---|---|
| **Vulkan backend** (recording, descriptor sets, image views, barriers, depth snapshots/readbacks) | **tested**: `PS3RECOMP_REPLAY_VK=1`; +28.2 ms walker, `tex` +18.9, `draw` +7.8 |
| **Depth-target guest fallback** (`dguest`) | **tested**: NOGUEST switch; -18.8 ms tex, -21.2 ms walker |
| **TEX_DIRTY page protection + faults** | not testable in replay: `eng_texdirty_sync` explicitly disables it when `s_guest_reader` is set (rsx_draw_engine.c:3182). Live A/B already measured it: `tex` 10.34 -> 5.89 ms (`texture-dirty.md`) |
| **WCB=2 copies** | not armed offline. The WCB's texture-bind cousin, the depth guest copy, *is* exercised and is the top lever; the colour WCB path is unchanged since `texture-dirty.md` |
| **SPU/PPU core contention** | **tested** with a memory-burner sweep (1, 2, 3 x 16 MB and 5 x 64 MB): walker 63 -> 77 -> 93 -> 99 -> 304 ms. It inflates `draw`/`tex`/`fetch`, not `parse` |
| **Real texture uploads** | live `uploads=0.0` in this window. Offline Vulkan has one induced 5.7 ms upload/frame from the depth guest store; NOGUEST removes it |
| **Descriptor/pipeline churn** | steady run has **no** pipeline compile (perf delay 6 s); descriptor writes are ~1% of samples, driver draw calls ~13.4% |
| **Validation layers** | not installed on this host (`/usr/share/vulkan/explicit_layer.d` has no Khronos validation layer); no layer run, as in `walker-tex-cost.md` |

## 5. Top levers

### 1. Remove the depth-target guest fallback from the texture bind (~13 ms, upper 19)

Evidence: live `dguest=228/frame` (11:06-11:11 `[tex-stats]`), offline
`dguest=222/frame`; `tex` 24.2 -> 5.5 ms and walker 64.9 -> 43.7 ms when the
path is skipped. The cost is a `submit_and_wait` + `depth_readback` + guest
store per dirty target, plus the re-upload it induces offline. Live pays the
readback, not the re-upload. The cut is a real depth-as-colour / sampled-depth
view for the formats that miss `vk_eng_depth_snapshot` (VK_ENG_DEPTH has no
`depth_as_color`; see section 5 of `walker-tex-cost.md`), or at minimum
coalescing the per-target copy to once per frame and keeping the backend image
for the texture. Expected **~13 ms/frame** in the crowded replay (the same
path is present at the meter but with `dguest=0` there).

### 2. Cut per-draw Vulkan recording (~4-8 ms)

Evidence: null -> Vulkan `draw` **6.0 -> 13.8 ms**, and 13.4% of all perf
samples in `libnvidia-glcore` under `vk_eng_draw_from`. Batching already
removed the 100 ms per-draw submit; what remains is per-draw
`vkCmdBindPipeline`/`vkCmdBindDescriptorSets`/`vkCmdDraw`/barrier recording.
Caching the last bound pipeline and descriptor set and skipping unchanged
writes (the backend already tracks `s_e2`) is the direct cut. Expect
**4-8 ms/frame** if the driver calls are halved; the null backend's 6 ms is
the floor for the engine side.

### 3. The live `parse` / in-drain stall (up to ~14 ms; needs a live profile)

Evidence: live `parse` 16.9 ms (314 crowded reports; 36.1 ms on the
1,702-draw subset) against 2.8 ms offline on the same 1,702 draws. The load
sweep rules out CPU/memory bandwidth as the cause of `parse` (3.0 ms at
2 x 16 MB). The likely shape is the walker waiting inside the drain for the
guest FIFO/reference/semaphore path the capture does not have. This is the
largest unexplained live term; a `perf record` on the live walker (or a
drain-scoped timer around the reference/put waits) is the next measurement,
not a blind renderer change. Upper bound **~14 ms/frame**.

Honourable mention: fetch/remap is 10.4 ms offline (`rsx_vertex_remap_build`
5.8 ms + `rsx_vertex_fetch_blocks` 4.3) and 8.7 ms live; the existing
`FETCH_MEMO` hits ~5% and was rejected, so a better reuse key is worth
~2-3 ms, not more.

## 6. Caveats

* The Vulkan replay measures the walker thread only. It does not call
  `rsx_vulkan_backend_present_finish`, so the presentation thread's readback
  is excluded (as in live, where the host presents on its own thread).
* The capture is one crowded instant replayed; it has no guest producers, no
  TEX_DIRTY and no colour WCB. Its `dguest`/`dsnap` counts match live
  closely, but live also has the 1,024-entry texture working set (capture:
  360) and the `TEX_DIRTY` generation scan. The 62 ms is a floor for the
  renderer work, not the live frame.
* The live phase split is scaled by the report's flip count (section 2.1);
  do not add the phases from different reports.
* `PS3RECOMP_ENG_DEPTH_NOGUEST` changes the image. It is a measurement switch,
  not a fix, and is not default-on anywhere.
* The perf data is under `recomp-work/captures/` (never `/tmp`, never git).
  The steady dwarf file is 91 MB; the others are under 1 MB.

## 7. Reproduce

```sh
# null baseline (digest unchanged)
cd /home/m/Documents/mlb16-online/recomp-work
./build-prof/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
  --frames 12 --warmup 4

# real Vulkan, the play settings
PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 \
REPLAY_NULL_HASH=0 PS3RECOMP_WALKER_PROF=1 \
  ./build-prof/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
  --frames 12 --warmup 4

# the depth-guest lever (measurement only: changes the image)
PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 \
REPLAY_NULL_HASH=0 PS3RECOMP_WALKER_PROF=1 \
PS3RECOMP_ENG_DEPTH_NOGUEST=1 \
  ./build-prof/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
  --frames 12 --warmup 4

# steady-state perf, skipping the 4-5 s cold pipeline build
PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0 \
perf record -D 6000 --call-graph dwarf,16384 -F 997 \
  -o captures/crowdprof_vk_steady_dwarf.perf.data -- \
  ./build-prof/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
  --frames 100 --warmup 30
perf report -i captures/crowdprof_vk_steady_dwarf.perf.data --stdio \
  --no-children --no-demangle --sort symbol --percent-limit 0.3
```

Live counters quoted above:

```sh
python3 - <<'EOF'
import re
d=open('recomp-work/play_20261105_110451.log','rb').read().decode('latin1')
for line in d.splitlines():
    if '[tex-stats]' in line: print(line[:230])
EOF
```

Build: `cmake -S ps3recomp-play/templates/project -B recomp-work/build-prof
-DPS3RECOMP_DIR=$PWD/ps3recomp-prof`, copy of build-play's cache/options,
`ninja -C recomp-work/build-prof -j8 replay_rsx_engine`.

## 8. Clean room

This is our own runtime, replay capture, trace/log stream and EBOOT. The
capture format, replay tool and draw engine are ours; the Vulkan backend is
the shipping `libs/video/rsx_vulkan_backend.c`. No RPCS3 or other GPL
emulator source was read or cited.
