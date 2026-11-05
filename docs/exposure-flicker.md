# Auto-exposure: the chain, the flicker, and what our readback does

Date: 2026-11-05. **Analysis only**: zero game launches, no `ps3recomp` edits,
no `build-play` rebuild. Sources: static Ghidra on `EBOOT.elf`
(`a58eb36f...`), the existing light/WCB/timeline logs under `recomp-work/`,
the MIT reference ports, and the runtime source read from
`ps3recomp-play` (live tree, another mission owns its edits).

User symptom: **over-bright field, and the pitcher/batter/catcher/umpire
lighting flickers between over- and underexposed** (the rainbow faces are
gone; WCB is on). RPCS3 with Write Color Buffers looks right.

## TL;DR

- The chain is now fully named (new Ghidra work):
  `Lighting_InitStadium` creates `ToneMapSample` (32x32, 6 mips);
  `Lighting_RenderExposureRT` renders a temp camera into it and runs
  `ToneMapMipGen`; `Lighting_UpdateAutoExposure` (`0x004f3590`) runs on the
  **PPU**, locks the **last mip (1x1) through the asset library** and reads
  its first two bytes with ordinary loads. It never issues a GCM readback;
  it relies on the RSX having already left the chain in main memory.
- The controller is a high-gain dead-band: the byte just above the per-mode
  threshold drives exposure up, below drives it down, at `62.5/s` with
  `dt` clamped to `[1/60, 1]`, then clamps to a per-mode range (0.3..1.1 in
  the default mode) and writes the exposure/gamma bytes (`powf(...,0.454)`).
  **One sample on the wrong side of the threshold moves the whole LUT**, and
  the next sample can move it back.
- Our WCB=2 does serve the chain fresh in every capture we have
  (`fresh=1`, serial advanced, 1x1 luma steady at 78-80 on the meter), and
  the one user session that was probed during the 11-02 drift shows the
  exposure stable while the look changed. So the existing data does **not**
  support "stale/zero bytes on alternate frames" as the steady-state cause
  of the 11-02 drift.
- The 11-05 alternation has a concrete timing cause that our captures could
  not show: an on-demand copy is made **when the walker services the
  request**, and the walker may have advanced further into the FIFO (or
  not as far) than the PPU read's logical position. The returned frame
  therefore depends on load. The controller turns one wrong sample into a
  visible step. `PS3RECOMP_RSX_WCB_PREFETCH` already changes the contract to
  "the target as of the sync point", which is deterministic; it is default
  off. That is the first thing to test.
- The over-bright field can also be a **zero** 1x1 sample (exactly the
  failure the NHL Legacy port documents: "a 1x1 luminance RTT read as 0 ->
  blown to white"). Our logs show exact-zero runs only at scene transitions
  (92 consecutive 0 reads at 21:44:46-52), so that explains a flash, not a
  steady over-bright field. If the field stays over-bright with a stable
  non-zero 1x1, the cause is downstream (tonemap/sRGB), not the readback.

## 1. The guest chain (Ghidra)

All addresses are EBOOT entry points. Renames made in this mission are
listed in §7.

### 1.1 Creation

`Lighting_InitStadium` (`0x004f0380`), when `no_render == 0`:

```c
uVar12 = FUN_004b8f04("ToneMapSample", 0x20, 0x20, 6, 2, 0x20, 0x14008);
param_1[0x1a] = uVar12;      /* lighting + 0x68 */
...
uVar12 = FUN_004b8f04("QuickAmbientSky", 0x20, 0x20, 1, 2, 0x20, 0x14008);
uVar10 = FUN_004b8f04("HarmonicsSample", 0x20, 0x20, 1, 2, 0x20, 0x10008);
```

So `ToneMapSample` is a 32x32 A8R8G8B8 render target with **6 mip levels**
(32x32, 16x16, 8x8, 4x4, 2x2, 1x1). The asset library places it in main
memory; the guest address is allocator-assigned. Observed chains in the
logs: `0x019D7900/0x019D9000/0x019D9080` (only 32x32/2x2/1x1 read in the
11-02 sessions) and `0x019E1F00/0x019E2F00/0x019E3300/0x019E3500/
0x019E3600/0x019E3680` (all six read in the 11-04 sessions).

### 1.2 Rendering

`Lighting_RenderExposureRT` (`0x004f48c8`) returns unless
`lighting+0x68 != 0` and `lighting+0x1f4 == 0`. It then:

1. sets up the exposure scene (`FUN_009edd08`, `"phmoundcolor"`, `"phdirt"`);
2. builds `"TempCam RenderToneMapExposureRT"` and renders it into the
   `ToneMapSample` handle with `FUN_004c6ac0(..., lighting+0x68, 0, 0)` and
   `FUN_004c6a8c`;
3. sets the `"ToneMapMipGen"` pass and calls `Lighting_ToneMapMipGen`
   (`0x004b21f0`, was `FUN_004b21f0`) with `(cam, 1)`.

The chain is therefore produced **by the GPU**, not by an SPU job. There is
no SPU consumer in the path.

### 1.3 The reader

`Lighting_UpdateTimeOfDay` (`0x004f3ac8`, was `FUN_004f3ac8`) ends with a
call to `Lighting_UpdateAutoExposure` (`0x004f3590`, was `FUN_004f3590`).
The latter is the whole auto-exposure controller:

```c
iVar2 = *(int *)(param_1 + 0x68);            /* ToneMapSample handle        */
if (iVar2 == 0) { param_1[0x1c0] = 0.3f; return; }
dt = g_game_frame_dt * g_game_time_scale;     /* fallback 1/60 when 0        */
level = *(ushort *)(*(int *)(iVar2 + 0x48) + 4) - 1;   /* the last mip: 1x1 */
pb = Asset_LockTextureLevel(iVar2, level, 5, 0);       /* CPU pointer        */
b1    = pb[1];
a     = 1.0f - pb[0] * (1/255);
Asset_UnlockTextureLevel(iVar2);
...
```

`Asset_LockTextureLevel` (`0x004b907c`) tails into
`Asset_LockTextureLevelImpl` (`0x004dd470`). With flags `5` it takes the
read branch and `Asset_TextureLevelReadPtr` (`0x004f6a48`) ->
`Asset_TextureLevelPtrFromTable` (`0x004f6328`) computes a **plain guest
pointer** into the level's data. It is not a GCM call, a transfer or a DMA;
the PPU reads the bytes with ordinary loads. That is why WCB=2 sees a page
fault (`how=guestreq` / `REQUEST`) rather than the NV3089 `other` path.

Constants read from `0x004f3540..0x004f3578`:

| address | value | use |
|---|---:|---|
| `0x3540` | 0.0 | dt floor |
| `0x3544` | 0.016 (1/60) | dt fallback |
| `0x3548` | 1.0 | scale |
| `0x354c` | 0.00392 (1/255) | byte normalise |
| `0x3550` | 20.0 | alpha/coverage gain |
| `0x3554` | 1.57 (pi/2) | smooth-step argument |
| `0x3558` | 0.025 | luminance slope |
| `0x355c` | -1.0 | clamp |
| `0x3560` | **62.5** | adaptation rate / s |
| `0x3564` | 0.1 | output scale |
| `0x3568` | 0.454 | **gamma 1/2.2** |
| `0x356c` | 1.1 | output scale |
| `0x3570` | 255.0 | byte quantise |

Per-mode clamps live at `0x011408d4 + mode*0x18` (init 0.3 both in
`Lighting_InitStadium`) and the threshold at `0x011408c4 + mode*0x18`.
The final LUT bytes are written at `param_1+0xa81/0xa82/0xa83`, via
`FUN_00a4eb44` which is a `powf` (exponentiation by squaring). The
controller has a dead band: above the threshold it steps one way, below it
the other; `FUN_00a4fe90` is the smooth-step (`sin`/trig) shaping term.

**Expected latency.** The reader has no explicit sync and no readback
call. It assumes the RSX has already written the chain into main memory.
The title allows **one render frame in flight** (the `FUN_000655cc` ref
gate; `cube-rt-and-frames-in-flight.md` §A), so the PPU read in frame N
sees the chain as of frame N-1. The adaptation rate (62.5/s, i.e. a ~16 ms
time constant) is consistent with a one-frame update. It is **not** a
slow multi-frame adaptation that would hide a one-frame error.

**Cadence.** On the meter the light probe sees the chain read ~0.32-0.35
times/frame per level (about every 2-3 frames), while the target is
re-rendered every frame. `Lighting_UpdateTimeOfDay` has one xref
(`FUN_002abaa8`), so the read is tied to the scene/weather update, not to
every draw.

## 2. What our readback actually serves

`PS3RECOMP_RSX_WCB=2` (ondemand, the play default) arms every dirty
non-display target at a sync point (`eng_wcb_sync_point`,
`libs/video/rsx_draw_engine.c:4129`) by taking its guest pages away
(`rsx_wcb_protect`, `libs/video/rsx_wcb.c:326`). The PPU's first load faults
and the walker (`eng_wcb_resolve`, `rsx_draw_engine.c:4048`) calls
`eng_wcb_copy_surface` (`:3784`). Without a ticket that path does
`submit_and_wait(FLUSH_GUEST_REFERENCE)` and then a `readback` of the whole
surface; the copy runs on the **walker thread**, not at the faulting load.
Two submits and two fence waits per copy (`wcb-async.md` §1).

`PS3RECOMP_RSX_WCB_PREFETCH` (commit `097e6c46`, default off) changes that
for a target read within the last 8 frames: at the sync point the copy is
queued into the open batch and the batch is submitted without waiting; the
later fault collects the ticket (`eng_wcb_copy_surface:3816`). The bytes
are then **the target as of the sync point**, which is deterministic and
does not depend on where the walker is when the fault is serviced.

### 2.1 Evidence from the existing captures

Light-probe sessions (`PS3RECOMP_LIGHT_PROBE=1`):

| log | mode | chain reads | value | freshness |
|---|---|---|---:|---|
| `play_20261102_214343.log` (user play) | ondemand | 3,375 x 1x1 | steady 78-80; 92 consecutive 0 at 21:44:46-52 | `fresh=1`, `how=guestreq` |
| `ab_live_exp_a_off_20261102_212018.log` | ondemand | 6,189 x 1x1 | steady 78-80, 0 at boot | `how=ticket` 18,499 / `copy` 151, `fresh=1` |
| `ab_live_exp_b_cuts_20261102_212936.log` | ondemand + RECT/BATCH/SKIP | 6,189 x 1x1 | same | `how=batch` 18,643, `fresh=1` |
| `ab_live_wcbpre_20261104_163914.log` | ondemand + PREFETCH | 3,406 x 1x1 | steady 78-80 | `how=ticket` 4,233 / `guestreq` 6,006 |
| `ab_live_wcbstream_20261104_162915.log` | ondemand + STREAM | 3,246 x 1x1 | steady 78-80 | `how=batch` 4,028 / `guestreq` 5,713 |

In every one of these the exposure sample is **fresh at the serial it is
copied at**. The 11-02 user-play session is the one where the user reported
the lighting drift, and at the reported moments (21:45:52 onward) the 1x1
reads are 80.5-82.1, not zero or alternating. The only large swings are at
camera cuts/scene changes (21:45:00-17: 14 -> 150 -> 93; 21:46:10-16). The
zero runs are the transitions (21:44:46-52 and the first seconds of every
run).

So the "our WCB hands the game stale/zero bytes on alternate frames" part of
the premise is **not reproduced by any captured run**. What the captures do
show is that a zero is a legitimate render result during a scene change and
that a single unexpected value is enough to move the exposure a long way.

### 2.2 The timing hole the captures cannot show

`eng_wcb_copy_surface` copies **what the walker has executed at service
time**:

- the walker flushes and waits for the open batch (`submit_and_wait`);
- the open batch may already contain draws recorded after the PPU read's
  logical position in the frame, if the walker ran ahead while the PPU was
  doing other work;
- if the walker is behind, the batch may not yet contain this frame's
  `ToneMapSample` render, so the copy is the previous frame's chain.

The bytes are always "fresh" relative to the surface's serial (the probe's
`fresh=1` only means `guest_valid_serial == color_serial` for the copied
version), but **which frame that is depends on the walker's position**.
On the meter the drain is long and regular, and the 1x1 value is flat, so
the two cases are indistinguishable. In live play with 30-140 ms frames,
camera cuts and replays, the walker position varies; the same load can
therefore read N-1 on one update and N on the next. The auto-exposure
controller is a **dead-band comparator with a saturated step**, so a sample
from the wrong side of the threshold moves the LUT to the other clamp and
the next sample moves it back: over/under alternation at the update cadence.

This is the same class the NHL Legacy port hit. Its notes name the exact
failure ("HDR tonemap/auto-exposure (a 1x1 luminance RTT read as 0 ->
blown to white)", `docs/notes/NEXT_STEPS.md`, 2026-10-20) and its fix for
the readback side was #218: "GPU-SAMPLED surfaces are not written back at
sync; marked deferred and written back on demand when an NV3089 blit reads
their memory" (`NHL_WB_ONDEMAND`, default on, removed the 433 ms intro
frames). It is the same "serve the read, not the sync" direction as our
prefetch, and it is where the over-bright-field symptom is written down.

## 3. Explaining the two symptoms

### 3.1 Over/under player lighting (the flicker)

Most likely: the exposure sample is **not the frame the guest's read was
ordered against**, and the controller amplifies it. The 1x1 is produced
every frame but consumed every 2-3; the only thing that makes it stable is
that the meter's walker cadence is stable. That is a timing coincidence,
not a contract.

Second candidate, independent of the exposure chain: the players' lighting
is also driven by the **cube/ambient probes**. The 11-02 user session shows
the reported look changes at 21:45:53-57 and 21:46:18-20 coincide with the
scene-setup 32x32 reads (`0x018C9200`, six reads), not with an exposure
change; one of the six is pure black every time (RGB 0,0,0, alpha 255).
`cube-rt-and-frames-in-flight.md` already fixed the biggest version of this
(the 8x8 cube probes), but the "incomplete cube keeps the last complete
image / only a never-complete probe gets the dummy" rule
(`sink_bind_textures`, `rsx_draw_engine.c:5950` onward) has the same
per-frame completeness sensitivity. Check it in the same run with
`[tex-miss] reason=cube` and `PS3RECOMP_CUBE_RT_LOG=1` before blaming the
exposure read.

### 3.2 Over-bright field

Two ways, and they need different fixes:

1. **Same chain, zero sample.** `Lighting_UpdateAutoExposure` sees byte 0
   and byte 1 of the 1x1. A zeroed level drives `a = 1.0` and the gain
   term to an extreme; the next update can undo it. This is the NHL-
   documented "1x1 luminance RTT read as 0 -> blown to white". Our logs
   show exact-zero levels during scene changes, including the **first
   read of a new chain** (`stored=0`, e.g. frame 3310 at 21:44:46). If the
   first sample after a scene setup is zero, the field flashes over-bright;
   if the chain stays zero (a render-pass problem, not a readback problem),
   the field stays over-bright.
2. **Separate gamma/format issue.** If the 1x1 is stable and non-zero but
   the field is still over-bright, the readback is exonerated. The scene
   target/tonemap output path is then the suspect (framebuffer sRGB,
   `rsx-framebuffer-srgb.md`; texture gamma, `rsx-texture-gamma.md`; the
   `[RSX fp] opcode 25` TXD/gamma gap recorded in
   `visuals-wcb-gamma.md`). That is a separate mission; the exposure probe
   below tells them apart in one run.

## 4. Proposed fix and cost

### 4.1 Serve the sync-point frame (correctness, likely free)

Make the exposure chain read deterministic: for the `ToneMapSample` levels
(32x32 and every level <= 4x4) always serve the copy queued at the **last
sync point the guest passed**, never "whatever the walker has executed".
This is what `PS3RECOMP_RSX_WCB_PREFETCH=1` already does for hot targets,
so the smallest change is:

- turn prefetch on for the exposure chain (either globally for the play
  build, or a targeted rule: a 32x32 or <=4x4 colour target read in the
  last N frames is always snapshotted at the sync point);
- keep the first read after a new surface synchronous (no history yet), so
  a freshly allocated chain cannot be served from a stale ticket;
- leave NV3089 transfers in stream order (already the case).

Cost from the 11-04 A/B (`wcb-async.md` §4): the 32x32 copy
**0.955 -> 0.117 ms/frame**, guest wait per copy **2.50 -> 0.93 ms**, walker
**-4.2 %**, readback engine cost **-86 %**. Fps/p50 at the meter are flat
because the p50 is the c690 producer (34.16 vs 34.17 ms). This fix should
therefore **not cost fps** and can gain a little walker headroom; it is the
one to test first.

### 4.2 One submission for the six levels (optional, small gain)

OND/async copies each level with its own submit+wait: the 11-03 attribution
shows the 32x32 at **1.29-1.39 ms copy + 1.31-1.46 ms wait/frame** and the
1x1/2x2 at 0.02-0.07 ms; the four tiny mip resolves are the `readback-cost.md`
lever #2. Grouping all levels of the chain into one `readback_async` batch
(the existing `PS3RECOMP_RSX_WCB_BATCH` machinery) removes four
submissions per read. Expected **~0.2-0.5 ms/frame** at the meter, more in
crowded frames. Not needed for correctness; do it only after 4.1 is
confirmed.

### 4.3 If the zero is a render result, not a read

Do not "fix" the readback. The only safe lever is to make the first sample
after a scene change non-zero (e.g. skip/clamp a sample whose level was just
created, which is a game-side behaviour we cannot change) or to find why
the temp-camera exposure pass renders black for six seconds (that would be
a render-pass bug, not WCB). The probe below separates the two.

### 4.4 fps verdict

- **4.1 is a correctness fix that should be neutral-to-positive**: it
  replaces the current per-copy wait with a queued copy and removes the
  walker-position dependence. Measured prefetch numbers above.
- **A forced synchronous whole-chain read** (the conservative alternative)
  costs the existing **~1.3 ms copy + ~1.4 ms wait/frame** at the meter
  (11-03) and does **not** remove the walker-position ambiguity by itself.
- **4.2** can save a few tenths of a millisecond of walker time; the meter
  p50 is c690, so this is a headroom/gain for crowded frames, not a meter
  fps win.

## 5. The one-launch experiment (do not run here)

Pitch meter, one game process, `PS3RECOMP_RSX_WCB=2`, CUBE_RT on, x11, and:

```sh
PS3RECOMP_LIGHT_PROBE=1 PS3RECOMP_RSX_WCB_STATS=60 \
PS3RECOMP_RSX_WCB_ATTR=1 PS3RECOMP_TEX_MISS_LOG=1 \
PS3RECOMP_RSX_WCB_PREFETCH=0   # arm A
# then a second segment with PS3RECOMP_RSX_WCB_PREFETCH=1 (live-ctl A/B,
# same launch; ab_live.sh rotates the key per 10 s)
```

What to read, in this order:

1. `[light probe] ... addr=<the 6-level chain> ... luma=... how=... fresh=...`.
   Arm A (prefetch off) should show `how=guestreq`; arm B `how=ticket`.
   The symptom to look for is **adjacent 1x1/32x32 values on opposite sides
   of the threshold** (the threshold is per `param+0x650`; on the meter the
   stable value is ~79, and the transition values seen were 14-29 / 103 /
   150-167). A/B with the same screen and camera: if the flicker tracks
   `how=guestreq` and disappears with `how=ticket`, §4.1 is confirmed.
2. `[rsx wcb] ... copied/waits/copy ms` and `[rsx wcb attr]` for the six
   chain addresses. In B the 32x32 copy should fall to ~0.1 ms/f and the
   async issued/fetched ratio should be ~1.
3. `[tex-miss]`/`PS3RECOMP_CUBE_RT_LOG=1`: `reason=cube` misses in the
   steady window mean the player-lighting half is the cube path, not
   exposure.

If the pitch meter does **not** flicker (it has been stable in every
capture), the experiment cannot confirm the live-play alternation; the same
two keys then need the user's short live-play session, not a longer
automated run.

## 6. Clean room and attribution

- No RPCS3 or other GPL emulator source was read or cited. The user's
  "RPCS3 with WCB looks right" is their observation, not a source.
- MIT references, used as design evidence only:
  - **NHL Legacy** (`/tmp/.../scratchpad/nhl/repo`, MIT): the white-scene
    entry names the 1x1 luminance RTT read-as-0 failure
    (`docs/notes/NEXT_STEPS.md`, 2026-10-20, play69/play81) and #218's
    on-demand writeback for GPU-sampled surfaces fixed the same class of
    stall (`NHL_WB_ONDEMAND`, `LOCAL_PATCHES.md`/`NEXT_STEPS.md`
    play321/play322: 433-500 ms lone frames gone). The "do not touch the
    framebuffer sRGB conversion; tonemap and post-effects depend on it"
    note (same file, end-of-project polish) is the reason §3.2 separates a
    gamma fix from the readback.
  - **inFAMOUS 2** (MIT, ThiagoFraga761 and contributors): the small-target
    writeback recorded into the frame's own batch and the "bloom of target
    addresses" bind memo, as already attributed in `wcb-async.md` §10 and
    `walker-tex-cost.md`. No code was copied here.

## 7. Ghidra renames made in this mission

| address | old | new |
|---|---|---|
| `0x004f3590` | `FUN_004f3590` | `Lighting_UpdateAutoExposure` |
| `0x004f3ac8` | `FUN_004f3ac8` | `Lighting_UpdateTimeOfDay` |
| `0x004b907c` | `FUN_004b907c` | `Asset_LockTextureLevel` |
| `0x004dd470` | `FUN_004dd470` | `Asset_LockTextureLevelImpl` |
| `0x004f6a48` | `FUN_004f6a48` | `Asset_TextureLevelReadPtr` |
| `0x004f6328` | `FUN_004f6328` | `Asset_TextureLevelPtrFromTable` |
| `0x004b90a0` | `FUN_004b90a0` | `Asset_UnlockTextureLevel` |
| `0x004b21f0` | `.opd.FUN_004b21f0` | `Lighting_ToneMapMipGen` |

Pre-existing renames used: `Lighting_InitStadium`, `Lighting_RenderExposureRT`,
`Lighting_UpdateFrame`, `Lighting_SunTimeOfDay` (earlier missions).

## 8. What would falsify this

- A probe run where the 1x1 reads are **stable and on the same side of the
  threshold** while the players still flicker: the exposure chain is not the
  cause; go to the cube/ambient path and the per-material lighting.
- A probe run where the field is over-bright while the 1x1 is a stable
  non-zero value: the exposure read is exonerated; the cause is the
  tonemap/sRGB/format chain.
- A run where prefetch on/off changes nothing but the flicker also does not
  reproduce: the pitch meter is not the scene; the live-play session is
  needed before any code change.
