# 30 fps investigation: the title's own frame clock is elapsed-time based, and its 30 fps mode is off in our runtime

Date: 2026-11-04. **Offline only: zero game launches.** Static analysis of
`EBOOT.elf` (Ghidra, md5 `a58eb36f8bc058402021421cb63d18c4`) plus our existing
logs and docs. No build, no runtime change, no RPCS3 or other GPL emulator
source was read or cited.

Follows [`meter-pacing.md`](meter-pacing.md),
[`flip-quantisation.md`](flip-quantisation.md),
[`flip-deadline.md`](flip-deadline.md),
[`frame-pacing-floor.md`](frame-pacing-floor.md),
[`menu-34ms.md`](menu-34ms.md) and [`menu-flip-fix.md`](menu-flip-fix.md).

## TL;DR

* **Verdict (a): no 30 fps cap is active in our runtime, and the simulation is
  time-correct at 60 fps. 60 fps is purely a speed problem.**
* The title has its own **frame clock** (`Engine_FrameClock`, `0x0005F120`,
  formerly `FUN_0005f120`). It measures the 79.8 MHz timebase, sleeps to a
  target period, and publishes a per-frame **delta time in seconds**
  (`g_engine_frame_dt`, `0x01120328`) as
  `max(param_1, floor((residual + elapsed) * 60)) / 60`, carrying the
  sub-1/60 residual. `param_1` is **1 for 60 fps mode and 2 for 30 fps mode**.
* The main loop (`Game_FrameLoop`, `0x002ED360`, formerly `FUN_002ed360`)
  multiplies that dt by a time scale and hands the product to **every**
  gameplay update. At 16.7 ms frames it is `1/60` per frame, i.e. real time,
  not 2x. Animation, physics and the pitch meter all consume this dt.
* **The pitch meter is dt-scaled** (`Pitch_GetFrameDelta`, `0x006323B0`,
  returns `g_game_frame_dt * g_game_time_scale`; `Pitch_ActionUpdate`,
  `0x00BE16B0`, accumulates the delivery value with it). The earlier note in
  [`boot-spu-hitches.md`](boot-spu-hitches.md) §4 that the meter advances by a
  fixed per-update increment is **superseded**: the increment is per update
  but scaled by dt, so its wall-clock rate does not change with fps.
* There **is** a real 30 fps mode in the title, but it is selected by two
  flags our runtime does not set:
  1. `g_engine_fps_setting_index` is 6 (60 fps) when the Gfx display-thread
     flag (`gfx_ctx+0x4E4`) is set and 5 (30 fps) otherwise. Gfx init sets it
     to 1 (`0x000687E4`), so the shipped path selects 60.
  2. Even in 60 fps mode, `Engine_FrameClock` promotes `param_1` to 2 when
     `Pad_SystemInfoReady()` (`0x00062900`) returns 1, which is
     `cellPadGetInfo2().system_info & 1`. Our HLE writes `system_info = 0`
     (`ps3recomp-play/libs/input/cellPad.c:1250`), so no promotion happens. **Do not set that
     bit**; on hardware where it is 1 the title self-caps at 30 fps.
* The only other frame-time wait is the already-known flip deadline
  (`2 * DAT_011203F8`), a timeout, not a cap. `DAT_011203F8` is 16,683 us
  (`2x = 33.366 ms`) in 720p and 20,000 us (`2x = 40 ms`) in the 576-line
  mode; it is written only by the video-mode function `FUN_00064b68`, never
  per scene or per menu/gameplay state.
* **Above 60 fps the sim would run fast**, because dt has a 1/60 s floor; the
  title's own limiter stops that at ~63.2 fps (at most ~5 % fast). At exactly
  60.0 fps the value is 1/60 and the game is real-time correct.
* No patch is needed. The one-run check that would confirm the setting at the
  meter is written in §8 but was **not run** in this mission.

## 1. Time sources in the title

| source | where | used for |
|---|---|---|
| `mftb` (`TBLr`) | inlined everywhere as `(tb - DAT_013545C8) * 5 / 399` = microseconds (79.8 MHz; magic `DAT_00F2D768 = 0x4880522014880523`) | the engine frame clock, most engine timers |
| `sys_time_get_system_time` | `0x0001C730` (init), `0x00064B68` (video mode change), `0x00065760` (per-frame publish stamp), `0x00069138` (buffer-ref callback stamp), `0x00069288` (vblank handler), `Gfx_DisplayFlip`, `0x00EE984C`, `0x00A41C8C` | wall-clock stamps and the flip deadline only |
| `syscall_sys_timer_usleep` | frame clock, waits, fades | sleep |
| `cellGcmGetVBlankCount` | **not imported** (known) | — |
| vblank handler `FUN_00069288` | increments counters and writes a deadline with no reader (known) | dead |

So the simulation's clock is the same one the rest of the engine uses: a
measured timebase delta, not a vblank count and not a fixed step count.

`sys_time_get_system_time` appears in no gameplay update. The per-buffer
timestamps written by `Gfx_FrameEnd` (`0x1245110`/`0x1245130`),
`FUN_00065760` (`0x1245150`) and `FUN_00069138` (`0x1245170`) have **no
readers** in the EBOOT; they are diagnostics.

## 2. The engine frame clock (`Engine_FrameClock`, `0x0005F120`)

This is the title's own frame limiter and delta-time producer, called once per
main-loop iteration with `g_engine_frames_per_update` (`0x01132F7C`).

Constants at `0x0005F0D0`: `1/60` f32, `1/1200` f32, `1e6` f32, `1e-6` f32,
`60.0` f32, `-1/1200` f32, `1.2` f32; double `2^32` at `0x0005F100`.

Pseudo-code (from the disassembly, not the decompiler's rendering):

```c
now_us = tb_to_us();
if (g_engine_frame_clock_base_us == 0)
    g_engine_frame_clock_base_us = now_us - 0x411A;   /* 16666 us */
target_us = (param_1 / 60.0f - 1/1200.0f - (residual < 0 ? residual : 0)) * 1e6f;
while (now_us - g_engine_frame_clock_base_us < target_us) {
    sys_timer_usleep(target_us - elapsed);
    now_us = tb_to_us();
}
elapsed_s = (now_us - g_engine_frame_clock_base_us) * 1e-6f;
frames    = max(param_1, (int)((residual + elapsed_s) * 60.0f));
dt        = frames / 60.0f;
residual  = residual + elapsed_s - dt;
if (residual < -1/1200.0f) residual = 0.0f;
if (display_height == 0x240) dt *= 1.2f;             /* 576-line / 50 Hz */
g_engine_frame_dt = dt;
g_engine_frame_clock_base_us = now_us;
```

* `param_1 = 1`: target ~15.833 ms (63.2 fps ceiling) and a **1/60 s dt
  floor**. `param_1 = 2`: target ~32.5 ms (30.8 fps) and a **1/30 s dt
  floor**.
* Below 60 fps the residual carry makes the average dt equal the measured
  elapsed time (at 45 fps you get a 1, 1, 2 quanta pattern), so the sim stays
  real-time correct.
* Above 60 fps the floor wins: every frame advances at least 1/60 s. The
  title's own limiter caps that at ~63.2 fps, so the worst case is ~5 % fast
  while the limiter holds. At exactly 60.0 fps dt = 1/60 and there is no
  error.
* `0x11201EC` (a Gfx/replay flag) selects an optional smoothed delta from
  `FUN_0005EEB0`; it is 0 in the normal play path.

`Engine_GetFrameDelta` (`0x0005F0C0`) is the getter for `g_engine_frame_dt`
and is called by the six mode loops, including `Game_PublishFrameDelta` and
the frontend loop.

## 3. How the sim advances: `g_game_frame_dt * g_game_time_scale`

`Game_PublishFrameDelta` (`0x002ED1D8`, formerly `FUN_002ed1d8`):

```c
g_game_frame_dt   = Engine_GetFrameDelta() * DAT_004D4E80;  /* scale = 1.0 */
g_game_time_scale = DAT_00074840;                           /* = 1.0f */
```

`Game_FrameLoop` (`0x002ED360`) then computes

```c
dVar15 = (float)g_game_frame_dt * g_game_time_scale;   /* seconds */
```

and passes `dVar15` to the whole update chain — `FUN_00862bc0`,
`FUN_002d8958`, `FUN_002ed0ec`, `FUN_002aca88`, `FUN_00852680`,
`FUN_00b311fc`, `FUN_007dd534`, `FUN_007d25c0`, `FUN_007d131c`,
`FUN_006f67c0`, `FUN_002d8a80`, `FUN_009a7ae0`, `FUN_0070a360`,
`FUN_00e93614`, `FUN_0070b320` and the virtual calls. At the end of the same
iteration it calls `Engine_FrameClock(g_engine_frames_per_update)`.

Two consequences:

1. every gameplay system above is delta-time driven;
2. **at 16.7 ms frames the delta is 1/60 s, so the game is real time — not 2x.**

### The pitch meter is dt-scaled (supersedes boot-spu-hitches §4)

* `Pitch_ActionUpdate` (`0x00BE16B0`, formerly `FUN_00be16b0`) owns the pitch
  action state machine. It accumulates the delivery value `+0x29BC` from
  `Pitch_GetFrameDelta()` (`0x006323B0`), which returns
  `g_game_frame_dt * g_game_time_scale` (fallback branch) or the same
  dt-scaled value through `FUN_0054BEF4`.
* `FUN_00BE02E0` (the meter display update) maps `+0x29BC` to the meter
  position and `FUN_007041E4` prints/compares it. There is no time argument
  in the mapping, but the input it maps is advanced with dt.
* Therefore the meter's wall-clock fill time does not depend on fps. The
  older "fixed per-update increment" reading came from seeing the fixed
  *mapping*; the accumulator above it is time-scaled.

This is the strongest evidence that gameplay is not a 30 Hz fixed step.

## 4. The title's own 30 fps mode (present, but off in our runtime)

`FUN_002AA4A8` (game-entry setup) contains:

```c
g_engine_fps_setting_index = 6;                       /* 60 */
if (Gfx_IsDisplayThread60Hz() == 0) g_engine_fps_setting_index = 5;  /* 30 */
g_engine_frames_per_update = 60 / g_engine_fps_table[g_engine_fps_setting_index];
DAT_01243A84 = g_engine_fps_table[DAT_012E91B8];
```

with the table at `0x01132F80` = **`{1, 5, 10, 15, 20, 30, 60}`**. Index 6
gives `60/60 = 1`, index 5 gives `60/30 = 2`. `Game_ApplyFrameRateSetting`
(`0x002AA320`) recomputes the same pair when the title's frame-rate event
fires (`FUN_004FDB30`, events `0x14B`/`0xC8`).

`Gfx_IsDisplayThread60Hz` (`0x00062838`, formerly `FUN_00062838`) returns
`gfx_ctx+0x4E4`, where `gfx_ctx = 0x01246180` (the "Ice" renderer; see
`FUN_0006904C` → `FUN_00067734(&DAT_01246180)`). Gfx init stores 1 there
(`0x000687E4`), so **the shipped path selects 60 fps**.

The second gate is inside the frame clock:

```c
if (param_1 < 2 && Pad_SystemInfoReady() != 0) param_1 = 2;   /* 30 fps */
```

`Pad_SystemInfoReady` (`0x00062900`) returns `CellPadInfo2.system_info & 1`
(it calls `cellPadGetInfo2` and reads the struct at `+0x08`). Our HLE writes
`system_info = 0` (`ps3recomp-play/libs/input/cellPad.c:1250`), so the promotion
never happens. **This is the one latent 30 fps switch in the title: if the
HLE ever reports that bit as 1, the game will pace at 30 fps and use a 1/30
dt floor.** The same byte is also stored in `DAT_01246662`, where
`FUN_000655CC` uses it only to suppress a 5555-iteration buffer-ref timeout,
so the only frame-rate effect is the promotion above.

`Caveat`: this analysis cannot prove what the real PS3 sets that bit to. If
it is 1 on hardware, the title is 30 fps *by design* there, and our port is
already running the 60 fps branch because of the HLE's 0. Either way, **our
build is not capped**, which is what the target needs.

## 5. Flip deadline and `DAT_011203F8` (the only other frame-time wait)

Already documented in [`flip-deadline.md`](flip-deadline.md); confirmed again
here:

* `DAT_011203F8` = **16,683 us** (`0x412B`) normally, **20,000 us** when
  `DAT_0124AD24 == 0x240` (576-line mode). It is written only in
  `FUN_00064B68` (video-mode configure) at `0x000651B0`/`0x000651BC`.
* The vblank handler `FUN_00069288` writes `now + DAT_011203F8` into
  `DAT_011203F0`, which has no reader.
* `Gfx_DisplayFlip` (`0x000692DC`) uses it as a **timeout**:
  `while (DAT_011203e0 != 0) { if (now - start < DAT_011203F8 << 1) wait; else
  give up; }` → 33.366 ms in 720p, 40 ms in 576-line mode.
* No menu/gameplay/60 Hz-vs-50 Hz config changes it by scene; only a video
  mode change does. The 50 Hz case is the 576-line mode, and the frame clock
  compensates with the `dt *= 1.2` above (60/50).

So the only "target frame time" constants are 15.833 ms / 32.5 ms in the
engine clock and 33.366 ms / 40 ms in the flip deadline. None of them is a
hard 30 fps cap in the 60 fps mode.

## 6. Is there a 1/30 or 2-vblank constant feeding the sim?

* The frame clock's constants are `1/60`, `1/1200`, `1e6`, `1e-6`, `60.0`,
  `1.2`; no `1/30`, `33333`, or `2 vblank` value.
* `1/30` floats exist elsewhere (`0x2AE12C`, `0x2E50B0`, `0x37B480`,
  `0x4EFD80`, …) as ordinary fractions in unrelated code; none is in the
  clock, the flip path or the main-loop dt hand-off.
* The only "2" that matters is `param_1 = 2` (30 fps mode), and it is off.
* The flip deadline's `<< 1` is a timeout of two display periods, not a
  two-vblank pacing rule.

## 7. Evidence from our own runs (no new launch)

* **Menus**: the flip fix gives 59.44 fps (`menu-flip-fix.md`); the capture's
  scene thumbnails advance no faster than the 42.10 fps arm (every cross-arm
  pair inside the A/A control), so no animation speed-up appeared.
* **Meter**: `NO_DRAW` with an 8 ms tick reached 54.0/60.4/55.8 fps in three
  segments (`tick-and-resync.md` §4b), i.e. well above 30. A 30 fps engine
  cap cannot produce that.
* **Meter dt**: the p50 34.15 ms frames are ~2.05 quanta; the engine clock's
  residual carry gives a 2/60 dt on those frames, i.e. the sim stays in real
  time at ~29-30 updates/s. Nothing in our logs shows a 2x or half-speed
  sim; the only "half rate" note was the pitch-meter inference now superseded
  by §3.
* **The user's ~43 fps moment** is an RPCS3 reading
  ([`overlap-analysis.md`](overlap-analysis.md) §3), not one of our scenes;
  it says nothing about our sim speed.

## 8. Verdict, patch and verification

### Verdict: (a) — no cap active, time-correct at 60, 60 is a pure speed problem

Nothing in the title holds gameplay to 30 fps in our runtime. The simulation
consumes a measured elapsed-time delta quantised to 1/60 s with a carried
residual. The title's own 30 fps mode exists but is not selected (Gfx flag 1,
pad `system_info` 0). The 34.15 ms meter p50 remains the producer/flip-deadline
problem already documented; it is not a speed cap.

### Minimal patch: none

* Do not change `cellPadGetInfo2` `system_info` from 0: setting bit 0 would
  make `Engine_FrameClock` use `param_1 = 2` (30 fps pacing, 1/30 dt floor).
* Do not let the port exceed ~60 fps once it is fast enough: the clock's dt has
  a 1/60 floor, so above 60 fps the sim runs fast (the title's own limiter
  stops it at ~63.2 fps, ≤ ~5 %). If we ever need a hard ceiling, cap the
  display tick/present at 60, not the guest.
* If we ever want to *reproduce* the 30 fps mode for a test, the one-line
  experiment is to make `system_info` bit 0 read 1 (or patch
  `g_engine_frames_per_update` at `0x01132F7C` from 1 to 2 live). Both should
  be avoided in the play build.

### One-run confirmation (written, NOT run)

`PPU_WWATCH` logs stores, so it catches the single store to
`g_engine_frames_per_update` at game entry:

```sh
cd /home/m/Documents/mlb16-online
PPU_WWATCH=0x01132F7C PPU_WWATCH_MAX=4 PPU_WWATCH_LEN=4 \
  ./mlb16-recomp/tools/play_windowed.sh 2>&1 | tee /tmp/fps30_setting.log
# at the pitch meter (X once), confirm the log's store value is 1 (60 fps).
# If it is 2, the 30 fps branch was selected and the HLE/pad bit is the cause.
```

A second, optional watch of `0x01120328` (`g_engine_frame_dt`) would show the
per-frame 1/60 value at 60 fps and 1/30 at 30 fps, but it is not needed for
the verdict.

**Five-minute user check (no patch):** time the pitch-meter delivery from the
pitch selection to the top of the meter at the current ~30 fps and again after
the next fps gain. If the delta-time path is being used (as the code says),
the wall-clock duration is unchanged; if it ever halves when fps doubles, the
per-update hypothesis would be back — but the static chain in §3 says it will
not.

## 9. Clean room

All of this is our own static analysis of the EBOOT (Ghidra) and our own docs
and logs. No RPCS3 or other GPL emulator source was read or cited.
