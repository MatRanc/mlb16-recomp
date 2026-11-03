# Cube render targets on the GPU, and what the per-frame ref wait really is

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. Windowed meter runs: `ab_live.sh`, X once, WCB=2,
PIN=1, x11, `PS3RECOMP_WALKER_RESYNC_FIX=1`.

This closes the two questions left by
[`textures-dummy-census.md`](textures-dummy-census.md):

* **A.** `Gfx_FrameEnd` allows three *display* flips ahead, but the PPU still
  blocks ~19-21 ms per frame in `FUN_000655cc`. It is waiting for the
  **previous** frame's `SET_REFERENCE` token, not the current one and not
  N-3. That is a one-render-frame-in-flight gate in the guest; we do not force
  it. The three-flip gate is the separate display queue and is not hit.
* **B.** The dark players are the 8x8 cube environment probes. The game renders
  six separate 8x8 colour targets at `base + face * 0x200`; the engine now
  copies them into one six-layer cube image on the GPU (`PS3RECOMP_RSX_CUBE_RT=1`,
  default off) and binds that instead of the dummy. At the meter the cube
  misses go from ~93-195/frame to **0**, with no pink left with
  `VK_DUMMY_BLACK` unset, and **no measurable fps cost** (30.06 vs 30.09 fps).

## TL;DR

| question | answer |
|---|---|
| frame the ref wait targets | **N-1** (the previous `Gfx_FrameEnd`) |
| why ~19-21 ms | PPU builds frame N in 9-15 ms while the walker drains N-1 for 29-37 ms; the remainder is the wait. One render frame in flight by guest design |
| 3 flips ahead | display-queue depth (`submitted - completed <= 3`), a different counter; not the measured block |
| host-side late ref? | no: the token is queued when the walker executes `SET_REFERENCE` (the last sync command of the frame) and published at drain end / by the 5 kHz poll publisher. A dropped token would time out at ~9 s, not 19 ms |
| cube layout | six colour targets, one per face, stride `rsx_texture_cube_face_stride(...) = 0x200`; two adjacent cube maps are `0x059F3000` and `0x059F3C00` (6 faces each) |
| producers | one fragment program `0xbd8ae188ddb5b0b8`, pipelines 48/88/260, one draw per face; serials advance in a burst, not every frame |
| cube RT implementation | `PS3RECOMP_RSX_CUBE_RT=1`, live key; six-face GPU copy, cached by face handles + colour serials; incomplete cubes keep the dummy and count a new `cube` census reason |
| meter misses, cube on | **0** after the switch (A segments: 25,800-29,796 per 10 s) |
| fps | A (off) **30.06 +/- 0.53**, B (on) **30.09 +/- 0.58**, paired **+0.03** (95 % CI -0.19..+0.25), N=6 |
| pixels | cube-on black vs cube-on magenta mean diff 2.2-2.7/255, **0 magenta pixels**; vs the old black-dummy meter 5.0-5.9/255 |

## Part A: which ref `FUN_000655cc` waits for

All addresses are Ghidra entry points; the EBOOT is `a58eb36f...`.

### The array and the token

`FUN_0006309c` (0x0006309c) is the fence writer: it reserves an NV406E
method `0x0050` (`SET_REFERENCE`) with the next global token
`DAT_01246044 + 1`, appends the method/argument to the command buffer, and
returns the token:

```c
iVar3 = DAT_01246044 + 1;
DAT_01246044 = iVar3;
... *p = 0x40050; p[1] = iVar3; ...       /* 0x0050 SET_REFERENCE */
... (&DAT_012451dc)[...] = arg address;   /* debug table */
return iVar3;
```

`Gfx_FrameEnd` (0x00066530) calls it once per frame and stores the token in the
four-entry array indexed by the current buffer id:

```c
uVar15 = FUN_0006309c(&DAT_00f3264c);          /* or ...2644 on the flip branch */
(&DAT_01245058)[*(uint *)(param_1 + 0x4bc) & 3] = uVar15;
```

The token is inserted at the **end** of the frame's command stream, after the
flip preparation in the flip branch (`cellGcmSetPrepareFlip` and the
`0x4e000/0x4eb00` label pair).

`FUN_00065760` (0x00065760) does, in order:

```c
FUN_000655cc();                       /* 0x00065798: the wait */
DAT_011203d4 = DAT_011203cc;          /* 0x00065808: remember THIS frame's id */
... store put into *DAT_01123b00 ...  /* 0x00065af8 */
```

`DAT_011203cc` is the frame id copied from `param_1 + 0x4bc` at the end of the
same `Gfx_FrameEnd`, before that field is incremented. So when frame N+1
enters `FUN_00065760`, `DAT_011203d4` is still **N**.

`FUN_000655cc` (0x000655cc) therefore reads:

```c
piVar1 = &DAT_01245058 + (DAT_011203d4 & 3);      /* frame N's token */
iVar2  = *piVar1;
while (*(int *)(DAT_01123b00 + 8) != iVar2)       /* control->ref == N */
    Cond_Wait(&DAT_01245068, 9000);
```

`DAT_01123b00 + 8` is the GCM control register's `ref` word; our runtime writes
it at `GCM_CONTROL_GUEST_ADDR + 8` (`cellGcmSys.c:1908`). The wait target is
**the previous frame's token (N-1)**, written at the end of frame N's stream.
It is not the current frame (not written yet), and not N-3: the array is four
deep for `FUN_00063b98`'s arbitrary-frame wait, but the per-frame gate always
uses the immediately previous id.

### Why that is one render frame in flight

`Gfx_FrameEnd`'s other gate is the flip queue:

```c
uVar21 = DAT_01120410 + 1;                 /* DAT_01120410 = 2 at init */
if (uVar21 < DAT_011203d8 - DAT_011203dc)  /* submitted - completed */
    while (... ) Cond_Wait(&DAT_012450d8, 17000);
```

`DAT_011203d8` is bumped by `Gfx_FrameEnd` on each queued flip and
`DAT_011203dc` by `Gfx_FlipHandler` (0x000691b0) when a flip completes. That
gate allows **three uncompleted display flips**. `Gfx_DisplayFlip`
(0x000692dc) waits for the next queued flip's ref (`DAT_01245020[...]`), calls
`cellGcmSetFlipImmediate` and waits for the flip to complete; it is the display
pacer.

The two gates are independent:

* the flip gate is a **display queue depth** (`submitted - completed`), and the
  traces show the main does **not** wait on its lwcond `0x1245100` at the pitch
  (`ppu-critical-path.md` §2, sites 3 and 8);
* the ref gate is a **render submission depth**: frame N's put cannot be
  written until the RSX has reached frame N-1's final `SET_REFERENCE`.

The arithmetic closes:

| trace | frame | ppu main run | ref wait (0x1245090) |
|---|---:|---:|---:|
| pitch, WCB=0 (`ppu-critical-path.md`) | 29.01 ms | 9.31 ms | 19.69 ms |
| meter, WCB=2 (`meter-pacing.md`) | 36.69 ms | 14.94 ms | 21.15 ms |

One frame in flight means the CPU builds N (9-15 ms) while the RSX drains
N-1 (29-37 ms); the CPU then waits the remainder, ~19-21 ms. If the guest
allowed two render frames, the wait would be ~0 in steady state at these
times. That is exactly what the measurement shows.

### Is our side publishing the token late?

No, in steady state:

* `gcm_3d_method` (`cellGcmSys.c:2322`) calls `gcm_ref_push_at` exactly when the
  walker executes method `0x0050`; `rsx_process_method` has already stored the
  value in `g_rsx_last_reference` (`rsx_commands.c:965`). The token is queued at
  the point the frame ends, which is what the guest compares.
* `gcm_ref_publish_one` writes it to `GCM_CONTROL_GUEST_ADDR + 8` at the end of
  the drain (`cellGcmSys.c:3396`), and `cellGcm_ref_on_poll` publishes while the
  guest spins, paced at ~5 kHz (`cellGcmSys.c:2047`). The guest's own ref read
  drives publication (`runtime/ppu/ppu_loader.cpp:1760`).
* If the walker skipped or dropped the token, `FUN_000655cc` would spin out its
  `0x15b3` iterations with a 9 s `Cond_Wait` each, not return after 19 ms.

So the wait is genuine back-pressure from the walker being the serial resource
(17-27 ms drain), not a label/semaphore written at the wrong point and not a
flip counter completed late. **Conclusion (a): the game really allows one
render frame in flight.** The three-flip allowance belongs to the display queue.

### What would change it (not implemented)

The guest code chooses the N-1 token, so the host cannot relax the gate without
corrupting the ring. The only levers are the ones already in
`ppu-critical-path.md`: cut the walker drain (lever 1) or add buffers and an
explicit ownership handoff (lever 2, needs guest work). No fix for A is
implemented.

## Part B: assembling the cube render target on the GPU

### What the census rows are

The census rows (old format byte double-shifted in the first runs; the real
byte is `0xA5`, A8R8G8B8 + LN) are:

```
surface loc=0 addr=0x059f3000 fmt=0xA5 8x8 pitch=64 mips=1 cube=1
surface loc=0 addr=0x059f3c00 fmt=0xA5 8x8 pitch=64 mips=1 cube=1
```

`rsx_texture_cube_face_stride(0xA5, 8, 8, 1, 64)` is `0x200` bytes (pitch 64 x
8 rows, one mip, 128-byte aligned). The two cubes are therefore
`0x059F3000..0x059F3BFF` and `0x059F3C00..0x059F47FF`: each is six faces, and
the next cube starts exactly one cube later. The meter run's log (final build)
confirms this for every cube address it sees:

```
[cube-rt] frame=21800 cube addr=0x0DE42800 8x8 fmt=0xA5 stride=0x200 found=0x3F complete=0x3F
[cube-rt]   face 0 ready handle=276 8x8 at 0,0 serial=2716418 ...
[cube-rt]   face 1 ready handle=546 8x8 at 0,0 serial=2716419 ...
...
```

Six **separate** colour targets with distinct handles and consecutive colour
serials, one per face. It is not one taller target in this title (the
implementation also handles a stacked 8x48 / 48x8 target, and the unit test
covers it, but no meter frame uses it).

The producer is one fragment program: every face logs
`writer_fp=0xbd8ae188ddb5b0b8` with pipeline 48, 88 or 260 depending on the cube
(a pipeline variant). One draw writes each 8x8 face; the six serials advance
together in one burst (e.g. 631518..631523) and then stay unchanged for
several frames. The probe is not re-rendered every frame; it is refreshed when
the scene changes, which is why the cube copy is cheap.

The consumers are the fragment programs the census already named, e.g.
`0x18e4be5ec62d4144`/pipe 327 and `0x580a2535064e2ee1`/pipe 253 on the pitch,
and `0xfe39042e0022ca48`/pipe 225 and `0x2e371c042c41e091`/pipe 51 on the meter
in the earlier run. One program binds the cube ~195-217 times per frame.

### Implementation

* Engine (`libs/video/rsx_draw_engine.{c,h}`):
  * new `rsx_be_cube_face {handle, x, y, w, h}` and optional
    `cube_target_view(faces, serials, fmt, w, h, remap, rsx_fmt)` backend hook
    (`rsx_draw_engine.h:188-200`);
  * `eng_cube_collect` (`rsx_draw_engine.c:2091`) accepts the six separate
    targets (trying the texture face stride, `pitch * height`, and the base
    target's own pitch), then a stacked target in rows or columns; a face must
    have a handle, a non-zero colour serial and at least the cube's dimensions;
  * `sink_bind_textures` (`rsx_draw_engine.c:4171`) takes the cube path before
    the 2-D surface scan whenever the base address is a tracked target. Fewer
    than six usable faces binds the dummy and notes `RSX_TEX_MISS_CUBE`;
  * `eng_cube_rt_on` (`:2058`) is the live key `PS3RECOMP_RSX_CUBE_RT`
    (`PS3_LIVE_CUBE_RT`, seeded from the environment);
    `PS3RECOMP_CUBE_RT_LOG=1` prints the face layout and the producer hashes.
  * the last writer of every colour target is recorded (`last_writer_fp/pipe`,
    set in the draw path at `:5196`) so the log can name the face producer.
* Vulkan (`libs/video/rsx_vulkan_backend.c`):
  * `vk_cube_rt` cache (32 entries, keyed by the six face windows, their colour
    serials and the crossbar), `vk_cube_rt_copy` (`:3168`) barriers the six
    targets, records six `vkCmdCopyImage` into layers 0..5 of one
    `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` image and waits;
  * `vk_eng_cube_target_view` (`:3194`) creates the six-layer image through the
    existing texture path and refreshes only when a serial moves; a pending
    draw batch is flushed before an eviction releases an image;
  * registered as `.cube_target_view` (`:4921`). Every draw in a frame reuses
    the same image; no CPU readback.
* Census (`libs/video/rsx_tex_miss.{c,h}`): new reason `cube` (incomplete
  six-face set) alongside `surface`, so a fallback is not hidden.
* Tests:
  * `test_rsx_draw_engine` (`libs/video/tests/test_rsx_draw_engine.c:594`) -
    a cube over six colour targets reaches the backend with the six handles in
    face order, each an 8x8 window at 0,0; one rewritten face changes a serial;
    five faces bind the dummy and count a `cube` miss; a stacked 8x48 target
    yields six rows.
  * `test_rsx_tex_miss` updated for the new reason and summary field.
  * both are in `tools/test_video_fixtures.py`; `test_rsx_draw_engine` is now
    listed there as well.

### Runs

Two meter runs, one game launch each; both use the final binary. The
`ab_live.sh` A/B is live via `PS3RECOMP_CTL`, so the fps comparison needs no
extra launch.

#### Run a: black dummy, CUBE_RT on at the meter, A/B off vs on

Command (from `recomp-work/ab-tools`):

```sh
TAG=cube_rt_a2 DUMMY_BLACK=1 IGNORE_MAGENTA=0 \
BOOT_ARM=$'PS3RECOMP_RSX_CUBE_RT=1' \
ARM_A=$'PS3RECOMP_RSX_CUBE_RT=0' ARM_B=$'PS3RECOMP_RSX_CUBE_RT=1' \
N=6 SEG=10 DROP=2 WCB=2 PIN=1 METER=1 VIDEODRIVER=x11 \
CAPTURE_PREFIX=$W/visuals/cube_rt/run_a/frame \
FINAL_SHOT=$W/visuals/cube_rt/run_a/meter_cube_on.png \
PS3RECOMP_RSX_CUBE_RT=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh
```

Log: `recomp-work/ab_live_cube_rt_a2_20261102_185722.log`.

| arm | fps | walker ms | wait ms |
|---|---:|---:|---:|
| A `CUBE_RT=0` | **30.06 +/- 0.53** | 25.01 | 8.25 |
| B `CUBE_RT=1` | **30.09 +/- 0.58** | 24.83 | 8.27 |

Paired A-B **+0.03 fps** (95 % CI -0.19..+0.25, N=6); the A/A self-check
passes. The cube path is free within noise. Segment fps 29.30-30.83.

Census per segment (from the `.marks` wall times): the A segments have
**25,800-29,796 misses per 10 s** (~86-99/frame), all the old `surface`
reason. In each B segment the only misses are the frames in the switch second
(0-2,284) before the walker applies the control file; the last B segment has
**0** after 19:03:33. The `[cube-rt]` log at 19:03:45 shows every meter cube
`stride=0x200 found=0x3F complete=0x3F`.

Frame: `recomp-work/visuals/cube_rt/run_a/meter_cube_on.png`.

#### Run b: magenta dummy, CUBE_RT on from boot

```sh
TAG=cube_rt_b_magenta DUMMY_BLACK=0 IGNORE_MAGENTA=1 \
BOOT_ARM=$'PS3RECOMP_RSX_CUBE_RT=1' \
ARM_A=$'PS3RECOMP_RSX_CUBE_RT=1' ARM_B=$'PS3RECOMP_RSX_CUBE_RT=1' \
N=1 SEG=10 DROP=2 WCB=2 PIN=1 METER=1 VIDEODRIVER=x11 \
CAPTURE_PREFIX=$W/visuals/cube_rt/run_b/frame \
FINAL_SHOT=$W/visuals/cube_rt/run_b/meter_magenta_cube_on.png \
PS3RECOMP_RSX_CUBE_RT=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh
```

Log: `recomp-work/ab_live_cube_rt_b_magenta_20261102_184923.log`. The harness
now works without black dummies: `nav3.py`, `poll_pitch.py`, `same.py` and
`ab_live.sh` take `IGNORE_MAGENTA=1` / `NAV_IGNORE_MAGENTA=1` /
`PITCH_IGNORE_MAGENTA=1`, which masks magenta pixels out of both the live frame
and the black-dummy reference before the luma diff. The pitch meter was
confirmed on the first attempt (18:53:31) and the run completed.

Census: **0 `[tex-miss]` lines after the X press** (34 s to the final shot).
Before the meter (lineup/pitch screens) there are 6,481 `cube` misses: those
probes are sampled before all six faces have been rendered, so the incomplete
path correctly keeps the dummy and says `cube`. No `surface` miss is left.
The saved meter frame has **0 magenta-classified pixels** (R>110, B>110,
G+40 < min(R,B)) out of 230,400.

#### Visuals

PNGs under `recomp-work/visuals/cube_rt/`:

| file | what |
|---|---|
| `run_a/meter_cube_on.png` | meter, cube on, black dummy |
| `run_b/meter_magenta_cube_on.png` | meter, cube on, magenta dummy (no black) |
| `meter_baseline_cube_black_cube_magenta.png` | old `meter_black.png` \| run a \| run b |
| `meter_baseline_vs_cube_diff_x4.png` | old baseline vs cube on, 4x difference |
| `meter_blackdummy_vs_magenta_diff_x4.png` | run a vs run b, 4x difference |
| `crops/players_baseline_vs_cube.png` | batter / catcher / umpire, 2x, three columns |
| `crops/{batter,catcher,umpire}_{baseline,cube_black,cube_magenta}_2x.png` | individual crops |

Read against `recomp-work/visuals/meter_black.png`:

* **batter** (left, blue jersey): baseline is near-black navy with no fold
  shading; cube-on is a readable blue jersey with lighter sleeve/shoulder
  highlights and visible skin on the forearms. Helmet stays black with a blue
  rim highlight.
* **catcher** (centre, red chest protector): baseline red is dark and flat,
  the mask/face is brown-black; cube-on lifts the red, shows the chest
  protector's panel edges, and the face/mask is beige with the cage visible.
* **umpire** (grey figure behind the catcher): baseline is almost a silhouette;
  cube-on shows the grey trouser shading, the belt/harness and the mask's
  outline.
* **field/crowd**: the field is the same green but with slightly more
  environment shading in the grass; the crowd behind the plate gains
  blue/white shirt colours and skin tones. The "Majestic" and "royals.com"
  boards are unchanged.
* **magenta arm**: run b is visually indistinguishable from run a (mean diff
  2.2-2.7/255, mostly the 1-2 frame pose shift); **no pink region remains**.
  The old baseline differs from cube-on by 5.0-5.9/255, concentrated on the
  players and the lower field/crowd.

## Harness notes

* `ab_live.sh` gained `DUMMY_BLACK=0`, `IGNORE_MAGENTA=1`,
  `CAPTURE_PREFIX=<prefix>` (arm a walker capture at the meter) and
  `FINAL_SHOT=<png>` (save the final meter frame after the last segment).
* `same.py`, `nav3.py` and `poll_pitch.py` accept the magenta mask; the mask
  is built from the live RGB frame and applied to both luma images before the
  diff, so the black-dummy reference screens remain valid. Offline check: a
  reference with the bottom 40 % painted pure magenta diffs 25-42/255 without
  the mask and 0.0 with it.

## Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
# run a: black dummy, A/B, capture and final meter frame
TAG=cube_rt_a2 DUMMY_BLACK=1 IGNORE_MAGENTA=0 \
BOOT_ARM=$'PS3RECOMP_RSX_CUBE_RT=1' \
ARM_A=$'PS3RECOMP_RSX_CUBE_RT=0' ARM_B=$'PS3RECOMP_RSX_CUBE_RT=1' \
N=6 SEG=10 DROP=2 WCB=2 PIN=1 METER=1 VIDEODRIVER=x11 \
CAPTURE_PREFIX=/home/m/Documents/mlb16-online/recomp-work/visuals/cube_rt/run_a/frame \
FINAL_SHOT=/home/m/Documents/mlb16-online/recomp-work/visuals/cube_rt/run_a/meter_cube_on.png \
PS3RECOMP_RSX_CUBE_RT=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh

# run b: magenta dummy (no black), cube on from boot
TAG=cube_rt_b_magenta DUMMY_BLACK=0 IGNORE_MAGENTA=1 \
BOOT_ARM=$'PS3RECOMP_RSX_CUBE_RT=1' \
ARM_A=$'PS3RECOMP_RSX_CUBE_RT=1' ARM_B=$'PS3RECOMP_RSX_CUBE_RT=1' \
N=1 SEG=10 DROP=2 WCB=2 PIN=1 METER=1 VIDEODRIVER=x11 \
FINAL_SHOT=/home/m/Documents/mlb16-online/recomp-work/visuals/cube_rt/run_b/meter_magenta_cube_on.png \
PS3RECOMP_RSX_CUBE_RT=1 PS3RECOMP_TEX_MISS_LOG=1 PS3RECOMP_CUBE_RT_LOG=1 \
./ab_live.sh

cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_video_fixtures.py     # test_rsx_draw_engine, test_rsx_tex_miss
```

## Caveats

* One run per arm pair/tag. The A/B has six 10 s segments per arm and a passing
  A/A check, but the meter's own run-to-run drift is ~0.5 fps.
* The `[cube-rt]` producer hash is the engine's fragment-program hash; it names
  the program, not its guest address.
* In run b the pre-meter `cube` misses are expected: the game samples a probe
  before all six faces exist. They keep the dummy (black or magenta), so the
  pitch/lineup screens can still show a pink probe with `VK_DUMMY_BLACK` unset;
  the meter, where all six faces are complete, has none.
* The cube copy is synchronous (one flush of the pending draw batch per serial
  change). It costs no measurable fps here because the probe changes only in
  bursts, but a title that rewrote six faces every frame would pay six copies
  and a flush per frame.
