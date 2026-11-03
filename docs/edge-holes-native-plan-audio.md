# Mesh holes, the native-Edge plan, and the silent play test

Date: 2026-11-03. **Static analysis only**: no game was launched (another
session had the game). Sources: the EBOOT in Ghidra, the generated c690 module
(`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/`), the step-1 corpus stage
pairs, `recomp-work/play_20261103_163747.log`, and the runtime sources in
`ps3recomp-play`. The c690/Edge facts are our own; NHL Legacy Native (MIT,
(c) 2026 HeckYouMan) is cited only where its method or numbers are used.

## TL;DR

* **A. The size hypothesis does not hold as stated.** The title configures
  1280x720 output and a **2560x720 (2x1 antialiased) scene target**. Our walker
  also renders that 2x1 sample grid, maps NDC over the pixel extent, and uses
  the standard Vulkan pixel centres; the guest viewport scale/translate carry
  the RSX half-pixel. The sizes and sample grid match. What remains is the
  **cull's sub-pixel test versus the 2x1 sample grid**: a triangle the SPU cull
  drops as "no pixel" at 720p can still cover one of the two samples, and the
  hole is exposed when the 2x1 target is resolved/presented. That is the same
  class of artefact the user saw in RPCS3 only when raising the resolution
  scale. The generated module already has a default-off diagnostic switch that
  settles it: `MLB16_CULL_BYPASS=1` (all culls), `=2` (0x2C only), `=3`
  (0x25/0x26 only). If the holes vanish, the cull is the cause; if not, the
  next suspects are the 36-byte vertex layout and the resolve/depth path.
  Native Edge as scoped in step 2 (`0xFDA0`) does **not** include the screen
  cull, so it would not fix A by itself.
* **B. The step-1/2 native cull wedge is a coverage/switch bug, not a design
  dead end.** The kernel is bit-exact on 1,604 captured stage pairs and 12
  job-file pairs, but the live meter class that wedged was not captured: the
  corpus cap filled inside the first ~10-frame burst, and the live switch was
  flipped mid-stage. It is also only **1.30x** (about -1.0 ms/worker), below
  the frame A/B noise. The honest order is: prove the live class with a 10x
  cap + gap and a 60 s shadow, make the switch atomic at job boundaries, then
  either hand-write the kernel to >=5x or go to **unpack/pack** (23.5 %, 5-10x,
  simpler data flow) before spending more on tree/cull. Non-native levers
  (AVX2/FAST_LS, PGO, more lanes) are each ~1-3 ms and are cheaper to try.
* **C. The premise "no `[cellAudio]` lines at all" is wrong.** The log has
  `Init()`, `Mixing thread started`, `PortOpen(8,8)` and `PortStart(port=0)`;
  Ghidra shows the game imports and calls `cellAudioInit`/`cellAudioPortOpen`,
  and the audio path is SNDPS3/libsnd + the LibMP3 SPU workload feeding port 0.
  Our SDL2 backend opens a device (a failure would print; none did) and the mix
  thread queues blocks. The silence is therefore downstream of the mixer:
  routing/sink, or the PCM/read-index phase. The new
  `PS3RECOMP_AUDIO_DIAG=1` probe (committed behind an env switch) settles it in
  one run: SDL driver/spec, backend-open line, and once a second
  blocks/s, mixed peak, queued samples, port-0 run/read index.

## A. Mesh holes ("cheese-grated" character meshes)

### A.1 What the game configures

| item | value | evidence |
|---|---|---|
| video output | **1280x720** | `[cellVideoOut] GetResolution(id=2) -> 1280x720`, log:86; `libs/video/cellVideoOut.c:54-69` |
| display buffers | 1280x720, pitch 5120 | log:202-203 |
| scene target | **2560x720, 2x1 antialiased** | `[rsx engine] surface 0:0x01110000 2560x720 fmt 3`, log:861; the 2x1 AA comment in `libs/video/rsx_draw_engine.c:6065` |
| viewport | guest viewport in **pixels**; default 1280x720 | `libs/video/rsx_commands.c:59-67`; `SET_VIEWPORT_*` methods `:591-609` |
| scissor | guest scissor in pixels; default 4096x4096 | `libs/video/rsx_commands.c:65-67`, `:950-956` |
| cull 0x25/0x26 | `0x13518`/`0x13520` -> `0x132E0` | `docs/c690_function_map.md:39`, generated `spu_recomp.c:30710` |
| cull 0x2C | `0x12DC0` -> `0x12C08` -> `0x82C0` | `docs/c690_function_map.md:42`, generated `spu_recomp.c:29779`, `:8035` |
| cull constants | `0x34000000` = FLT_EPSILON, `0x3F000000` = 0.5 | `spu_recomp.c:8041`, `:17746` |
| viewport literal in job input | **none** | scanned LS `0x0000..0x1FFF` of the stage input for 1280/720/640/360/1920/1080 (u32 and f32): no hit |

The c690 job's entry descriptor is in LS at `0x1330` (`r3 = 0x1330`). It is a
set of LS offsets and context pointers, not a viewport record; the screen-space
transform and the cull context are built by the PPU and arrive through the
context/matrix. The cull's screen space is therefore the game's **720p pixel
viewport**, while the actual colour/depth target is the **2560x720 2x1 sample
grid**. The job module has no 1280/720 literal, so whether the cull also knows
about the 2x1 sample grid is not visible there; the constants it does carry are
FLT_EPSILON and 0.5. That is the important gap to keep in mind, and it has the
shape of the RPCS3 upscaling artefact: a triangle the cull drops as sub-pixel
at 720p can still cover one of the two samples, so the dropped triangle leaves
a visible sample gap.

### A.2 What our Vulkan walker does

| item | value | evidence |
|---|---|---|
| window swapchain | 1280x720, FIFO | log:220 |
| offscreen target | 1280x720, D32 float + stencil (format 130) | log:225; `libs/video/rsx_vulkan_backend.c:1108-1141` |
| AA detection | `antialias 3/4/5` -> `aa_sx=2`; `4/5` -> `aa_sy=2`; clip scaled to samples | `libs/video/rsx_dispatch.c:582-592` |
| fixed-function viewport | NDC mapped over the **pixel extent** `Wv = W/aa_sx`, `Hv = H/aa_sy`; Y flipped; guest `vp.scale`/`vp.translate` used directly | `libs/video/rsx_draw_engine.c:6405-6418` |
| guest vertex-program path | x/y identity, z remapped from GL clip space to [0,1]; the guest VP carries the viewport | `libs/video/rsx_vulkan_backend.c:1818` |
| Vulkan viewport | full target `0..W`, `0..H`; standard pixel centres `(x+0.5, y+0.5)`, top-left fill rule | `libs/video/rsx_vulkan_backend.c:1865`, `:1911` |
| window blit | `vkCmdBlitImage` with `VK_FILTER_LINEAR` (box average for an exact 2:1 downscale) | `libs/video/rsx_vulkan_backend.c:2579-2588` |

So the walker's pixel space and the game's cull pixel space are both 720p, and
the sample grid is the same 2x1 grid the 2560x720 target uses. The half-pixel
offset is not lost: RSX viewport scale/translate include it, and both the
fixed-function transform and the guest VP use those guest values. **There is no
static evidence that we rasterise at a different size or with a different
pixel-centre convention than the game's cull assumes.** The hypothesis in the
task is therefore only half right: the mismatch is not 720p vs another size;
it is the cull's sub-pixel threshold versus the 2x1 sample grid, and how the
result is resolved/presented.

### A.3 Other causes, ruled in or out

* **Depth precision — ruled out as a hole cause.** We use
  `VK_FORMAT_D32_SFLOAT_S8_UINT` (log:225, format 130; candidates in
  `rsx_vulkan_backend.c:1108-1141`). That is more precise than the RSX 24-bit
  depth, so it cannot make triangles disappear; it can change z-fighting, not
  coverage.
* **T-junctions from LOD — not ruled out, but the wrong shape.** T-junctions
  produce long cracks/seams along a LOD boundary; the user's artefact is a
  spray of pinholes in a dense area (around the eyes). Keep it as a fallback
  if the cull test is negative.
* **Vertex quantisation decode — ruled out for the current build.** The c690
  unpack/transform/normalize path is the lifted code, and the offline reference
  is bit-exact on the captured stage pairs (`docs/edge-native-step1.md:281-288`).
  The native kernel's FTZ deviation is default-off and not in this build's live
  path. A decode difference would be a systematic shape change, not pinholes.
* **Index/vertex fetch (the 36-byte c690 output) — still open.** The c690
  assembler emits 36-byte records (`docs/c690_function_map.md:83`). Our engine
  handles generic attribute layouts, with a fast 24-byte "MLB 3D mesh" path and
  a generic descriptor path (`libs/video/rsx_draw_engine.c:6147-6185`). If the
  cull test is negative, log the actual attribute layout/stride for the
  character draw and compare it with the 36-byte producer.
* **Backface culling — ruled out.** Vulkan cull mode/front face are taken from
  the guest state per draw (`rsx_vulkan_backend.c:4154-4157`); a wrong winding
  would remove large regions, not a pinhole spray.

### A.4 The test the user can run, and the fix

The generated c690 module already contains a default-off diagnostic switch
(`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c:12`), and the
current `build-play` binary has it compiled in:

| value | effect |
|---:|---|
| `MLB16_CULL_BYPASS=1` | bypass all three culls (0x25, 0x26, 0x2C) |
| `MLB16_CULL_BYPASS=2` | bypass 0x2C only (`0x12DC0`) |
| `MLB16_CULL_BYPASS=3` | bypass 0x25/0x26 only (`0x13518`/`0x13520`) |
| unset / 0 | normal |

Run the pitch/close-up where the pitcher's face is visible with
`MLB16_CULL_BYPASS=1 ./play_windowed.sh` (the script does not clear the
variable). Then repeat with `=2` and `=3` to identify the stage.

* **If the holes vanish with `=1`:** the cull is the cause. The clean fix is to
  reimplement the sub-pixel test with the 2x1 sample grid (or bypass it for
  character meshes) so it keeps triangles that cover a sample. Bypassing also
  draws backfaces, which the depth test hides on closed meshes but can expose
  on open ones (eyes/hair), so this is a diagnostic first, not necessarily the
  final fix. A native cull is the natural place to do it correctly.
* **If the holes remain with `=1`:** the cull is exonerated. Next probes: (a)
  log the character draw's target, viewport, stride and attribute layout; (b)
  frame-diff the same camera with the cull off; (c) check the 36-byte vertex
  layout against the producer. The 2x1 resolve path is the next suspect after
  the layout.

**Native Edge does not automatically fix A.** The step-2 native kernel replaces
`0xFDA0` (tree/BVH traversal and vertex emit), not `0x132E0`/`0x12C08`/`0x82C0`
(the screen culls). A native cull stage could fix A; the current native stage
cannot.

## B. Native Edge: why it wedged, and an honest plan

### B.1 Why step 1/2 wedged

The facts from `docs/edge-native-step2.md` and the run log
`recomp-work/ab_live_edge2ab3_20261103_055613.log`:

* The native kernel is bit-identical (full 256 KiB LS + all 128 GPRs) on
  **1,604 / 1,604** spool pairs and **12 / 12** job-file pairs.
* The switch was flipped at flip 10989 (`log:34855`). The next 10 s produced
  no flips; at `05:59:45` the FIFO walk hit
  `[FIFOSTEP] io=0136E970 w=2136E970` (`log:34892`), then the jump-park resync
  (`log` tail). The bad word is a FIFO pointer written as command data, i.e.
  the guest's FIFO stream was corrupted, not a host crash.
* The live meter burst fills the corpus cap in ~10 frames; the 401 meter pairs
  span ~0.3 s. The input class that the live arm hit after the burst was **not
  captured**. The step docs also list a resume path that may not enter at
  `0xFDA0`.
* Attempt 1 never ran the kernel (a depth-tracking bug left the outermost flag
  set); attempt 2 ran it and wedged. The switch can flip while another lane is
  mid-stage.

**Verdict: a bug we can fix, not a design dead end.** The kernel is exact on
every input actually captured; the failure is an uncaptured input class (or a
resume path) plus a switch that was not atomic with respect to stage entry. The
fix is coverage and atomicity, not a rewrite of the whole approach:

1. Capture the live class: trigger at the meter with a **10x cap** and a
   `..._STAGE_GAP_MS`, so the spool covers the burst **and** the quiet period
   after it.
2. Run a **60 s shadow** at the meter with `PS3RECOMP_C690_NATIVE_SHADOW=1`;
   accept only 0 mismatches over the whole window, including the post-burst
   class.
3. Make the switch **atomic at the outermost stage entry** (read once per
   outermost `0xFDA0`, store the decision in the stage context) and never flip
   a stage that is already in flight.
4. Keep the layout guard and the fallback counter; require fallbacks <=10 % of
   cull jobs before enabling live.
5. Re-run the A/B only after 1-4; the same failure twice means stop and keep
   the switch off.

### B.2 Incremental path and expected ms

The numbers below are per worker per frame at the step-1 14.4 ms c690 wall
shape; union is the five busy lanes. "Measured" is what step 2 actually
achieved; the rest are targets, not results.

| # | stage | share | ms/worker | measured / target | union | risk | exactness gate |
|---:|---|---:|---:|---|---:|---|---|
| 1 | tree/cull `0xFDA0`+`0xFA58`+`0xFCB8`+`0xF520` | 31.8 % | 4.58 | **1.30x measured**; 5x target | 35 -> ~26 at 5x | XL; compute-bound in `fi`/`frest`/`frsqest` and `shufb`; live class unproven | full LS+GPR diff; shadow; fallback <=10 % |
| 2 | unpack/pack `0x6D68`,`0x8160`,`0x7C18`,`0x7998`,`0x6860`,`0x7540` | 23.5 % | 3.38 | 5-10x target | 26 -> ~23 | M/L; simple data flow, but many layout variants | snorm byte equality; per-job hash |
| 3 | skin/transform `0xD4A8`,`0xD2B8`,`0xB268`,`0xC9D0`,`0xA798`,`0xA8A0` | 13.6 % | 1.96 | 5-8x target | 23 -> ~20 | L; matrix palette variants | positions <=1e-5; packed bytes <=1 LSB |
| 4 | normalize `0xAF98`,`0xB238` | 8.2 % | 1.19 | 5x target | 20 -> ~19 | M; `frsqest`+`fi` bit-exact | bit-exact; per-job hash |
| 5 | format/parser `0xE750`,`0x10700`,`0x10758`,`0x5BC0` | 5.5 % | 0.79 | 3-5x target | 19 -> ~18.5 | M; descriptor/opcode capture | per-job hash |
| 6 | codegen tail dispatch/shufb/LS/float | tail | 0.5-1.5 | 1.2-1.5x | 18.5 -> ~18 | M; AVX2 exact | SPU float/LS suites |
| 7 | hand-off/overlap once c690 < 19.5 ms main cycle | - | - | - | floor ~20-21 ms | M | frame p50/p99 A/B |

**Honest read of stage 1.** The flattened C kernel is 1.30x, not the 10x the
step-1 plan assumed. At 31.8 % of c690 that is about **-1.0 ms per worker per
frame**, inside the noise of the frame A/B (the measured elasticity is 0.55,
so even a 10x stage would only be ~3-4 ms of p50, not the full lane-union
saving). The kernel is compute-bound in the SPU reciprocal/square-root LUT and
`shufb`; a hand-written AVX2 version might reach 2-3x, and 5x only if the
reciprocal path is allowed to be approximate. That is a different exactness
decision, not a free rewrite.

**Recommended order.** Do not reorder on 1.30x:

1. Finish the live proof for tree/cull (cheap: the spool exists; one shadow run
   with a 10x cap and a gap).
2. If the live class is proven, either invest in a hand-written vector kernel
   to reach >=5x or **stop at tree/cull** and go to unpack/pack.
3. **Unpack/pack next** regardless: 23.5 %, simpler data flow, 5-10x target.
4. Then skin/transform, normalize, format/parser, codegen tail.
5. Hand-off/overlap only once c690 is below the 19.5 ms main-ref cycle.

The NHL comparison is useful for the method, not the millisecond claim: their
whole-job native path reached 22x because it replaced the whole job, including
the cull, with a layout-specific kernel and accepted 7.6e-6 positions, 1-LSB
normals and a handful of borderline cull differences. Our c690 is a different
layout, and the step-2 split leaves index decode, ring allocation and the
submit epilogue lifted. A 5x stage is a realistic ceiling for the current
split, not 22x.

### B.3 Non-native alternatives for the c690 cost

| lever | expected at the meter | effort | risk / kill |
|---|---|---|---|
| `-mavx2 -mfma -ffp-contract=off` for the SPU TUs (currently only `-msse4.1`), enabling the exact AVX2 float/int blocks in `spu_helpers.h` | 1-2 ms | S/M | c690 wall -<10 % or any exactness test failure |
| `PS3RECOMP_SPU_FAST_LS=ON` (+ `-mssse3`) for the SPU TUs | 0.5-1.5 ms | S | LS-fidelity test failure |
| `SPU_MFC_LOOKUP_CACHE=1` (exists, default off) | 0-2 ms | XS | c690 wall -<5 % |
| `PS3RECOMP_SPU_FAST_FLOAT=ON` (approximate RNE) | +1-3 ms beyond exact AVX2 | S, risky | any visible/physics divergence |
| PGO (instrumented build + 2-3 scripted runs) | 1-3 ms | M | <3 % frame change, or hot helpers outlined as cold |
| more host threads / a 6th SPU lane | 0-1 ms | M | CPUs are at 25-60 %, but the five lanes are 58.8 % busy with 0.1 % queueing: the limit is the per-frame burst and the walker/main cadence, not a missing thread. The title requests 6 SPUs and one is idle; using it is a scheduler change with contention risk. Test lane occupancy and p50 before believing it. |
| NOP `0x0100` run skip, `GCM_REF_IMMEDIATE` | 0.5-2 ms (walker), 0-1 ms (ref) | S | replay diff / ref-wait unchanged; these are walker cuts, not c690 |

The first two are one build each and are the highest-value cheap changes; if
they do not move the c690 wall by >=10 %, stop the codegen line there and move
to PGO and the walker.

### B.4 Where native Edge would also fix A

Only if the native work includes the screen culls `0x132E0` (0x25/0x26) and
`0x12C08`/`0x82C0` (0x2C), not just `0xFDA0`. A native cull can use the 2x1
sample grid and a threshold in sample space, which is the likely fix for the
holes. The step-2 kernel does not touch those functions, so it will not change
the holes.

## C. Audio: why the play test was silent

### C.1 The log premise is wrong

`recomp-work/play_20261103_163747.log` **does** contain `[cellAudio]` lines:

| log line | event |
|---|---|
| 460 | `GetPortConfig(port=0) status=READY bufEA=0x58000000 ridxEA=0x58100000` |
| 738 | `Init()` |
| 739 | `Mixing thread started` |
| 749 | `PortOpen(nChannel=8, nBlock=8)` |
| 751 | `PortStart(port=0)` |
| 61329-61330 | `PortStop(port=0)` / `PortClose(port=0)` |
| 61340 | `port 0 HAS DATA but never started -- game withheld PortStart` |

The last line is **not** evidence that the game never started the port: it is
the mix thread's "port has data but `running == 0`" warning, and it fires after
`PortStop` (which clears `running`) and before `PortClose` (which clears
`in_use`). It is a shutdown-order artefact.

### C.2 Does the game call cellAudio? (Ghidra)

Yes. The EBOOT imports `cellAudioInit` (`00f29754`), `cellAudioPortOpen`
(`00f29814`), `cellAudioPortStart`, `cellAudioGetPortConfig`, etc.
`cellAudioInit` is called from `FUN_0001d434`; `cellAudioPortOpen` is called
from `FUN_009fd94c` and `FUN_00edf4a0` (the main audio-device setup). The log's
`PortOpen(8,8)` matches the 8-channel path. The title also loads the SNDPS3
sound banks (`SNDPS3/SOUND/*.SKX`) and registers the `Wws_Job/LibMP3 JobList`
SPU workload (log:754-755). The audio path is therefore
**SNDPS3/libsnd + LibMP3 SPU decode -> cellAudio port 0**, and it does reach
cellAudio. There is no second host audio path that bypasses cellAudio; the SPU
mixer's output is the PCM ring that cellAudio consumes.

### C.3 Does our SDL2 backend open a device and queue samples?

On Linux the backend is SDL2 (`libs/audio/cellAudio.c:27-40`). `cellAudioInit`
calls `audio_backend_init` (`:820-845`), which calls
`SDL_InitSubSystem(SDL_INIT_AUDIO)` and `SDL_OpenAudioDevice` (`:184-216`).
Failure paths print `[cellAudio] SDL audio init failed` or
`[cellAudio] SDL_OpenAudioDevice failed`; the log has neither, so the device
opened. The mix thread starts (`:698`) and each block is submitted with
`SDL_QueueAudio` (`:225-230`). With `PS3RECOMP_AUDIO_PACE=1` (on in
`play_windowed.sh`) the loop paces to the device/5333 us block period. So
statically: **device opened, thread running, blocks queued**. Any of those
failures would have printed.

### C.4 What is left, and the cheapest probe

The static evidence cannot distinguish these:

1. **Sink routing.** The host default sink at the time of this analysis is
   `alsa_output.usb-Sony_Interactive_Entertainment_Wireless_Controller-...`
   (`pactl info`), not necessarily the headphones. SDL opens the default
   device; if the headphones are on another output, the game is audible but
   not where the user is listening.
2. **The mixed PCM is silent or misread.** The port is 8-channel big-endian
   float; our mixer byte-swaps and downmixes (`cellAudio.c:458-535`). If the
   guest writes a different format or the read index is out of phase, the mix
   is zero/garbage.
3. **The device queue is not draining** (suspended sink, wrong SDL driver), or
   the port is not running.

The cheapest probe is the new env switch added in this mission:

```sh
PS3RECOMP_AUDIO_DIAG=1 ./play_windowed.sh
```

It prints, once at init, the SDL driver and the obtained spec
(`[cellAudio] diag: SDL driver=... freq=... format=... channels=... samples=...`),
a `host audio backend open` line, and once a second:

```
[cellAudio] diag: blocks/s=... peak=... queued=... port0 run=... ridx=...
```

Interpretation:

| observation | conclusion |
|---|---|
| `peak=0.0000` every second | the guest never writes nonzero PCM into port 0, or our read offset/format is wrong (game/decoder path) |
| `peak>0`, `queued` grows without bound | the device/sink is not consuming (routing or SDL driver) |
| `queued` stays small, `peak>0`, still no sound | the samples reach SDL; the sink/headphones routing is wrong |
| `port0 run=0` after `PortStart` | a port-state bug, not the mixer |
| `blocks/s` far from ~187.5 | pacing/device clock problem |

The existing `AUDIO_RATE=1` also works on Linux and reports blocks/s and the
silent percentage, but it does not print the device or the peak; the new diag
adds both.

### C.5 Code change

Only `libs/audio/cellAudio.c` was changed: the `PS3RECOMP_AUDIO_DIAG` helper,
the SDL init line, the backend-open line, and the once-a-second Linux mix line.
It is default-off and was compile-checked with the exact flags of the
`build-play` runtime TU. Committed on `exp/live-ab` in `ps3recomp-play` and
pushed to `fork`.

## Numbers and method

* No game launches. Static analysis of the EBOOT (Ghidra), the generated c690
  module, the step-1 corpus stage pairs, the play log, and the runtime sources.
* Corpus stage pairs used for the cull/descriptor scan:
  `recomp-work/corpus_edge1_20261103_012604/stage/` (4 pairs, 264,192 bytes
  each: 256 KiB LS + 2 KiB GPRs).
* Log evidence: `play_20261103_163747.log` (6.5 MB, 16:37:47-16:43:45) and
  `ab_live_edge2ab3_20261103_055613.log` (the native-cull wedge).
* The `MLB16_CULL_BYPASS` switch is in the **generated** c690 tree
  (`recomp-work/spu_lifted/.../spu_recomp.c:12`), not in the committed runtime;
  `build-play` has it compiled in. If the tree is regenerated it disappears.

## Deviations from the plan

* The task said the play log had no `[cellAudio]` lines; it has five (plus the
  shutdown warning). This changed the audio conclusion from "the game may not
  use cellAudio" to "the game uses it and the backend opened; the silence is
  downstream of the mixer".
* The cull test uses an existing switch in generated game data rather than a
  new committed switch, because adding one would mean editing the generated
  tree or the lifter and the mission asked for static analysis plus only the
  audio probe.
* No game run and no live A/B; the cull and audio probes are prepared for the
  user's next play test.

## Blockers

* The cull bypass test and the audio diag need one game run by the user; the
  static evidence cannot settle which of the remaining candidates is live.

## Processes left running

None.
