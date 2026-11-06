# Walker MT repair: build, black field, C hang, texture drain

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, built and measured in
`recomp-work/build-play` (live) and `recomp-work/build-mt` (offline replay).
Follows [`walker-mt-live.md`](walker-mt-live.md),
[`walker-mt-m4.md`](walker-mt-m4.md),
[`walker-mt-m3.md`](walker-mt-m3.md),
[`walker-mt-m1m2.md`](walker-mt-m1m2.md) and
[`walker-multithread-design.md`](walker-multithread-design.md) sections 3.5,
3.6, 3.7, 5 and 7. **Four game launches** (the hard cap): two pre-fix
A/B/C reproductions, one validation-layer A/B/C, one timing A/B.
No RPCS3 or other GPL emulator source was read or cited.

## TL;DR

* **Build-mt is fixed for the mission targets.** Its `Release` flags lacked
  `-march=native` (so `-mfma` was missing for `c690_native_cull.c`); it is now
  configured with the same `-O2 -DNDEBUG -march=native` as `build-play`.
  `replay_rsx_engine`, `test_draw_queue`, `test_color_readback_guest` and the
  new `test_mt_turns` build. A *full* `ninja -C build-mt` still stops on three
  pre-existing, unrelated link failures (`replay_rsx`, `fiber_switch`,
  `test_vdec_stub`: missing engine/hook sources in those targets); this is not
  an M4 or flag regression and is left for a separate change.
* **The black field was a worker command-buffer bug, and the field is no
  longer black — but B is still visually wrong.** A worker's first-depth
  clear and its scratch-depth setup called `vk_begin`/`vk_barrier_image`/
  `CmdClear*` while the worker's one-shot buffer was current; the barrier and
  clear landed in the walker's shared `s_vk.cmd` and the clear was submitted
  outside the chunk's turn, before earlier chunks. It now records into the
  worker's chunk at the packet position (`vk_t_chunk`/`vk_rec_cmd`), and
  state-thread clears/uploads are submitted before the chunk that uses them
  (`submit_batch`, `eng_mt_submit_state_batch`). The black region is gone
  (dark fraction B 0.137 vs A 0.143), **but A/B thumbnails still differ by
  mean abs 38.8/255 vs an A/A baseline of 3.4**, consistently across six
  alternations. The visual criterion **fails**.
* **The C hang is fixed by removing the third thread from the backend path.**
  It reproduced twice: after the `TEX_RESOLVE=1` switch the walker stopped
  (no `[walker-prof]` lines) while guest `[flip]` lines continued and the
  presented frame froze (C thumbnails byte-identical). The `rsx-tex` resolver
  ran `eng_texture_slot` on its own thread with `vk_t_user == NULL`, so its
  texture create/upload used the walker's global one-shot command buffer,
  fence and staging buffer (`s_vk.cmd`, `s_vk.fence`, `s_eb`, `s_up_stage`)
  concurrently with the state thread; that can wedge in `vkWaitForFences`.
  Plain textures now resolve on the state thread before the packet is queued
  (step 4), so the resolver never touches the backend. The C arm in the
  validation run produced walker output for its whole window and did not hang.
  The turn/epoch protocol is unit-tested (`test_mt_turns`, five scenarios,
  timeout). **No gdb stack dump was captured**: both reproduction watchdogs
  failed before attaching (a non-executable script, then a process-name
  mismatch in `pgrep`), and the last two launches were needed for validation
  and timing.
* **The per-bind texture drain is mostly gone; the timing gate still fails.**
  The plain-bind and surface-view drains are removed for a recording backend,
  and texture-handle releases are deferred to real sync points. Live `tex`
  fell from 14.5 ms (M4) and ~50 ms (first repaired validation build) to
  **7.85 ms/frame**, but MT-off is 0.34 ms. Timing A/B (N=6 x 10 s): fps
  **35.10 -> 33.03**, walker **9.50 -> 12.77 ms**, wait 16.51 -> 17.84;
  `tex` **0.34 -> 7.85**, while the state `draw` phase falls **2.62 -> 1.04**
  and `bind` to zero. The pass criterion "walker ms lower than MT off"
  **fails**.
* **Nothing is default-on.** `PS3RECOMP_WALKER_MT` stays off; with MT on, the
  async resolver is disabled (the live key still parses but selects nothing).

## 1. Step 1: build-mt

### 1.1 Cause

`build-mt` had been configured with `CMAKE_BUILD_TYPE=Release` but without
build-play's `-march=native`:

```
build-mt:   FLAGS = -O2 -g -std=gnu17 ... -O2 -msse4.1
build-play: FLAGS = -O2 -DNDEBUG -march=native -std=gnu17 ... -O2 -msse4.1
```

`runtime/spu/spu_helpers.h:803` errors when `SPU_FAST_FLOAT` is enabled without
FMA, so any incremental build that recompiled `c690_native_cull.c` or
`c690_native_screen.c` stopped. This was a stale configuration, not an M4
regression (M4 never changed those files).

### 1.2 Fix

Reconfigured `build-mt` with

```sh
cmake -S .../ps3recomp-play -B build-mt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=native" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=native"
```

and rebuilt. The runtime and the mission targets build. A full build still
stops on:

* `replay_rsx` (`tools/replay_rsx_core.c` needs
  `rsx_draw_engine_set_memo_index` / `_set_refs_hash_fast`, which are in
  `rsx_draw_engine.c`, not in that target's source list);
* `fiber_switch` and `test_vdec_stub` (undefined
  `g_ps3_guest_write_hook` / `g_ps3_guest_access_hook`; those test targets do
  not link the translation unit that defines the weak hooks).

Those are pre-existing target-definition gaps (the symbols were added before
M4); adding sources would change unrelated tools and was out of scope for this
repair. The offline gates below use the targets that do build.

### 1.3 Exactness after the repair

`build-mt`, 5 frames + 2 warm-up, `REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1`:

| capture | MT off | MT on W=3 |
|---|---|---|
| `meter_frame` | `461ff3a834c62331` | `461ff3a834c62331` |
| `walker_frame` | `9b78ac38a25c5cbf` | `9b78ac38a25c5cbf` |

`test_mt_turns`: all five scenarios; `test_draw_queue`: order/wrap/full/drain;
`test_color_readback_guest`: 85 checks, 0 failures; 30/30 video fixtures.

## 2. Step 2: the black field

### 2.1 The bug

With per-worker recording (`s_mt.rec`), a worker records a whole chunk into its
own primary command buffer. Two backend calls on the worker thread still went
through the walker's one-shot path:

* `eng_pkt_binds` (`rsx_draw_engine.c:7470`) called
  `clear_depth_stencil(g.be->user, ...)` for the first bind of a depth target,
  with `g.be->user` (NULL) rather than the worker context;
* `vk_mt_draw` (`rsx_vulkan_backend.c:5622`) calls `vk_eng_scratch_depth` for a
  draw with no zeta.

Inside them, `vk_begin()` sees the worker TLS user and starts the worker's
`os_cmd`, but `vk_barrier_image()` and the `CmdClear*` calls used `s_vk.cmd`
(the state thread's shared command buffer). The state thread could be recording
a batch there, and the clear's one-shot was submitted *during recording*,
before the worker had waited for the chunk's turn. So a depth clear could
execute before the chunks it was supposed to follow, or corrupt the walker's
batch. The live symptom was exactly the M4 one: the infield/lower frame black
for the first frames of the B window (the previous depth/frame contents not
cleared at the right point).

### 2.2 The fix

* `vk_t_chunk` / `vk_rec_cmd()`: while a worker chunk is recording, the
  generic command helpers target the chunk. `vk_mt_chunk_begin` sets it,
  `vk_mt_chunk_submit` clears it. `vk_barrier_image`, the colour/depth clear
  paths and `vk_eng_scratch_depth` record into the chunk at the packet's
  position; no separate submission can overtake earlier chunks.
* `eng_pkt_binds(p, user)` passes the worker context through.
* The state thread's batch can also hold work that must precede a later worker
  chunk (a clear, an inline packet draw, a texture upload). The new optional
  backend entry `submit_batch` (Vulkan: `vk_eng_submit_batch`, asynchronous
  with frame pipelining, otherwise a waiting flush) is called:
  * after every state-thread/inline packet before its turn
    (`eng_mt_submit_state_batch`),
  * after the colour and depth clears in `sink_clear`,
  * before a workerable packet is added to a chunk, so a texture upload
    recorded during prepare is on the queue before the chunk samples it.
* With MT on, plain texture changes now create a new handle instead of
  rewriting the bound image in place (`texture_upload_ordered` is suppressed
  while workers are active), so a queued chunk keeps immutable bytes. The old
  handle is retired and released at the next real sync point.
* The `rsx-tex` resolver's cross-thread backend use is removed for live MT
  (section 3).

### 2.3 Result: black gone, image still different

Thumbnails in the measured windows (640x360, 0-255):

| run | A/A | B/B | A/B | B dark | A dark |
|---|---:|---:|---:|---:|---:|
| pre-fix (launch 1) | 1.2 | 32.4 | 32.1 | 0.303 | 0.139 |
| repaired timing (launch 4) | 3.35 | 3.99 | **38.84** | 0.137 | 0.143 |

The dark field is gone, and B is stable across six alternations, but B is a
*consistently different image* from A (mean abs ~39 vs an A/A baseline of
~3.4; the field is darker and the upper frame brighter). That is a visual
regression, not A/A noise, so **step 2's proof fails**. The remaining
difference is consistent with the interleaving of a light/shadow pass or a
clear whose position in the stream still differs from the serial batch; the
next debug step is a paired full-frame A/B/A dump with a single MT frame
change (e.g. disable the new clear submission) rather than a timing run.

## 3. Step 3: the C hang

### 3.1 Reproduction

Two runs (launches 1 and 2) switched A (MT off) -> B (MT on W=3,
`TEX_RESOLVE=0`) -> C (MT on W=3, `TEX_RESOLVE=1`) on the pitch meter. C
reproduced both times:

```
last [walker-prof]     ... at=256035ms
[live-ctl] TEX_RESOLVE=1 (was 0)
[ab_live] switch C at flip-ms 256150
[flip] 8420 at 256150 ...        <- guest flip log continues
[flip] ... no [walker-prof] after the switch; frame.ppm frozen
```

The C thumbnails are byte-identical to each other (within-arm diff 0.0) while
the guest flip counter keeps advancing. So the walker wedged, but the PPU flip
log did not; the earlier "0 flips" reading was measuring the host counter, not
the guest log.

**No stack dump.** The launch-1 watchdog was not executable and died at once;
the launch-2/3 watchdogs used `pgrep -x MyGameRecomp`, which does not match the
game's actual `comm`, so they never attached. Both mistakes were found only
after the runs; the remaining launches were reserved for validation and timing.

### 3.2 Root cause (code)

`eng_tex_resolver_main` runs `eng_texture_slot` -> `eng_texture_upload` on the
`rsx-tex` thread. That thread never calls `vk_bind_user`, so `vk_t_user` is
NULL and every upload uses the walker's global one-shot objects:

* `vk_begin()`/`vk_submit_and_wait()` reset, submit and wait on `s_vk.cmd` and
  `s_vk.fence`, which the state thread also uses for clears, snapshots,
  readbacks and present;
* `vk_barrier_image()` records into `s_vk.cmd`;
* `vk_stage_get(&s_up_stage, ...)` reuses one staging buffer.

Two threads resetting/waiting the same `VkFence` and recording the same
`VkCommandBuffer` can consume each other's signal and wedge in
`vkWaitForFences`; that matches a walker stop with no Vulkan API error. The
turn/epoch protocol itself is deadlock-free in the model
(`test_mt_turns`), so the live doc's turn-wait suspect was not the cause.

### 3.3 Fix

The async resolver is disabled for live MT (`eng_tex_resolver_on` returns 0;
the thread is still created but idles). Plain textures are resolved on the
state thread during packet preparation, before the packet is queued, and the
packet carries the immutable handle. This is also step 4's architecture: the
third thread is out of the submission path. The start-up line is now
`[walker-mt] plain textures resolve on the state thread (async resolver
disabled)`; the live key is accepted but inert.

### 3.4 Unit test

`libs/video/tests/test_mt_turns.c` (CMake target `test_mt_turns`) models the
protocol from `rsx_draw_engine.c`: state-thread seq/group allocation, the
worker segment split at the group epoch, `tex_taken`, and the resolver's
ordered group turn. Five scenarios (a group before every packet, every third
packet, two groups before one packet, a group at a chunk edge, no groups) must
finish with every packet committed and every group taken; a 10 s
`pthread_timedjoin_np` watchdog fails the test on a wedge. All pass.

## 4. Step 4: the texture drain

### 4.1 What was removed

* `eng_texture_slot_or_async` no longer calls `eng_tex_sync_path()` for a plain
  bind on a recording backend. It still drains for a non-recording backend
  (the null replay), because the null backend digests the texture call and
  needs it in stream order (this keeps the historical hash).
* The `surface_view` bind no longer drains on a recording backend: a surface
  view is a host object and the queue order already puts the sampling after the
  draws that wrote the target. The null replay keeps the old drain.
* A texture replaced by create-and-swap is retired and released at the next
  real sync point (`eng_mt_retire` / `eng_mt_release_retired`) instead of
  draining the workers at the bind.
* Texture uploads recorded while a packet is prepared are submitted before the
  packet's chunk is queued (`eng_mt_submit_state_batch`).

### 4.2 What still drains

Real sync points still do: `SET_REFERENCE`/semaphore/flip/present, WCB, guest
readbacks, depth snapshots, reinterpreted surface reads, colour-feedback
snapshots, texture-create/release through a non-recording backend, and the
remaining target/depth/cube binds that read back a target. The live `tex`
term says those still cost on the meter:

| term | A (MT off) | B (W=3) | A us/draw | B us/draw |
|---|---:|---:|---:|---:|
| walker | 9.50 | 12.77 | 20.7 | 29.0 |
| engine | 8.64 | 11.92 | 18.9 | 27.1 |
| parse | 0.83 | 0.88 | 1.8 | 2.0 |
| fetch | 2.15 | 0.46 | 4.7 | 1.1 |
| **tex** | **0.34** | **7.85** | **0.75** | **17.8** |
| pipe | 0.96 | 0.84 | 2.1 | 1.9 |
| bind | 0.14 | 0.00 | 0.3 | 0.0 |
| **draw** | **2.62** | **1.04** | **5.7** | **2.4** |
| other | 2.43 | 1.73 | 5.3 | 3.9 |

Per draw (draws/frame median 468.9 A, 433.5 B; the meter is not perfectly
static). The state thread's draw/bind win is real (5.7 -> 2.4 us/draw and bind
to zero), and `fetch` also moved off, but the residual texture cost is still
~17 us per texture slot. The likely sources are the forced create-and-swap for
a changed texture and the target/depth/reinterpreted binds that still read a
target back; the next experiment is **ordered upload records in the worker
command buffer** (the M4 doc's own next step), not another blanket drain.

## 5. Step 5: validation and timing

### 5.1 Validation run (launch 3)

`ab_live_walkermt7valfinal_20261104_210633`, `VK_INSTANCE_LAYERS=
VK_LAYER_KHRONOS_validation`, arms A (MT off) / B (W=3, `TEX_RESOLVE=0`) /
C (W=3, `TEX_RESOLVE=1`), N=1 x 10 s, 2 s dropped.

* **No VUID and no validation error during A, B or C.** The only output is 10
  `UNASSIGNED-Threading-MultipleThreads-Write` swapchain errors at
  21:07:32-21:08:26, i.e. during boot/menus and before the meter; the same
  class exists with MT off and is a pre-existing present/acquire thread-safety
  issue, not an MT regression.
* **C did not hang.** Walker-prof lines continue through the whole C window;
  no frozen frame. The start-up line says the resolver is disabled.
* No `[walker-mt] fallback`, `chunk begin failed` or ring-full line.

The final binary (after the last deferred-release/surface-view changes) was
built after this run and was not re-run under the layer: the hard cap was
already 4 launches. Its timing log has no Vulkan error.

### 5.2 Timing A/B (launch 4, the counted run)

`ab_live_walkermt8timing_20261104_211810`, `TAG=walkermt8timing`, `METER=1
PIN=0 WCB=2 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 PITCH_THRESH=35 LIMIT=1400`,
A = `PS3RECOMP_WALKER_MT=0`, B = `PS3RECOMP_WALKER_MT=1
PS3RECOMP_WALKER_MT_WORKERS=3 PS3RECOMP_WALKER_MT_TEX_RESOLVE=0`. 12 segments,
all labels match the applied control state.

| metric | A (MT off) | B (W=3) |
|---|---:|---:|
| fps (mean +/- 95% CI) | **35.10 +/- 1.30** | **33.03 +/- 1.84** |
| p50 / p90 / p99 ms (CSV, pooled) | 33.82 / 44.66 / 55.21 | 28.32 / 47.92 / 57.66 |
| walker ms | **9.50 +/- 1.27** | **12.77 +/- 2.02** |
| wait ms | 16.51 | 17.84 |
| draws/frame (median) | 468.9 | 433.5 |
| thumbnails vs A (mean abs) | A/A 3.35 | **38.84** |
| validation errors | 0 in the arms | 0 in the arms |

The pooled p50 is noisy (the meter's present-time distribution is not the guest
frame time; per-segment p50 flips sign), but fps and the walker are lower in B
and the p90/p99 tail is worse. The visual comparison is the decisive one:
B is a different image. **Pass fails on both "walker ms lower" and "no visual
diff".**

## 6. What is not done

* **No gdb stack dump of the C wedge** (watchdog mistakes, then the launch cap).
  The fix removes the offending thread and the unit test covers the turn
  protocol, but the C stack was not captured.
* **A full `ninja -C build-mt` still fails** on `replay_rsx`, `fiber_switch`
  and `test_vdec_stub` (pre-existing missing sources, section 1.2). The mission
  targets build.
* **The final binary was not re-run under the validation layer** (launch cap);
  the validation run is one commit behind the final texture-release changes.
* **The B image difference is not explained.** The black field is gone, but the
  consistent A/B difference remains. A paired full-frame A/B/A capture with a
  single ordering change (e.g. the new clear submission) is the next step.
* **The texture drain is not eliminated.** The residual `tex` is ~7.5 ms/frame
  above MT off; ordered upload records are the next experiment.
* **No user zoomed-out replay command is issued.** B does not win, has a visual
  difference and is slower, so the user test would only confirm the failure.

## 7. Reproduce

Offline gates:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_WALKER_MT=0 REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 \
  ./build-mt/replay_rsx_engine ./meter_frame --frames 5 --warmup 2
PS3RECOMP_WALKER_MT=1 PS3RECOMP_WALKER_MT_WORKERS=3 \
  REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 \
  ./build-mt/replay_rsx_engine ./meter_frame --frames 5 --warmup 2
# both: output hash 461ff3a834c62331
./build-mt/test_mt_turns
cd ../../ps3recomp-play && python3 tools/test_video_fixtures.py   # 30/30
```

Validation run:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
TAG=walkermt7valfinal METER=1 PIN=0 WCB=2 N=1 SEG=10 DROP=2 \
  VIDEODRIVER=x11 PITCH_THRESH=35 LIMIT=900 SETTLE=10 REPORT=0 \
  ARMS='A=PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0|B=PS3RECOMP_WALKER_MT=1;PS3RECOMP_WALKER_MT_WORKERS=3;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0|C=PS3RECOMP_WALKER_MT=1;PS3RECOMP_WALKER_MT_WORKERS=3;PS3RECOMP_WALKER_MT_TEX_RESOLVE=1' \
  BOOT_ARM='PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0' \
  ./ab_live.sh
```

Timing run:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=walkermt8timing METER=1 PIN=0 WCB=2 N=6 SEG=10 DROP=2 \
  VIDEODRIVER=x11 PITCH_THRESH=35 LIMIT=1400 SETTLE=10 REPORT=1 \
  ARM_A='PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0' \
  ARM_B='PS3RECOMP_WALKER_MT=1;PS3RECOMP_WALKER_MT_WORKERS=3;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0' \
  BOOT_ARM='PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0' \
  ./ab_live.sh
```

## 8. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The turn /
ordered-record shape is reused from the MIT inFAMOUS 2 port
(`rsx_be_thread.c`, `bq_direct_begin`); the test file and the packet block
carry the required notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

No RPCS3 or other GPL emulator source was read or cited.
