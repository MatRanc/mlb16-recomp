# Exposure, the sky samples, and the readback cuts

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. Meter runs: `ab_live.sh`, pitch screen, X once,
`METER=1`, `WCB=2`, `PIN=1`, `PS3RECOMP_RSX_CUBE_RT=1`,
`PS3RECOMP_VK_PRESENT_NEW_ONLY=1`, `PS3RECOMP_VK_PACE=1`, x11. One game launch
per run, 240 s on the meter per run, 2 s thumbnails.

## TL;DR

* **The uncommitted readback cuts are committed and fully gated.** RECT,
  BATCH and SKIP each have a live key and are off by default; the off path is
  the old path. One gating bug was fixed: the new `guest_read` code cleared
  `guest_dirty` even with both cuts off. Two GCM fixtures that were already
  failing on the branch (missing stubs for the walker-profiling/capture hooks)
  now pass. All 29 video fixtures pass; commit `618ec118` plus the probe-label
  fix `952795bb`, pushed to `fork` only.
* **Static: the three lighting renders are 32x32 RGBA8.** `Lighting_InitStadium`
  (`0x004f0380`) creates `ToneMapSample` (32x32, **6 mip levels**, flags
  `0x14008`, handle at lighting+`0x68`), `QuickAmbientSky` (32x32, **1 level**,
  `0x14008`, lighting+`0x64`) and `HarmonicsSample` (32x32, **1 level**,
  `0x10008`, lighting+`0x9a4`). `Lighting_RenderExposureRT` (`0x004f48c8`)
  renders a temp camera to the `ToneMapSample` handle, then runs
  `ToneMapMipGen`. `Lighting_UpdateFrame` (`0x004f2198`) runs every frame and
  writes the per-material lighting block that consumes the result.
* **The game reads the exposure target continuously, not once at start.** The
  probe saw the `ToneMapSample` chain (32x32 + 2x2 + 1x1; 16x16/8x8/4x4 exist
  but are not read) copied by a guest request about **16.5 times/s** on the
  meter, i.e. roughly every second frame, for the whole 4-minute window.
  `QuickAmbientSky`/`HarmonicsSample` were **not** CPU-read at the meter; the
  only other 32x32 reads were short bursts during boot/scene transitions.
* **No stale or partial readback was found, and the cuts do not change the
  lighting.** In both runs every logged copy was at the surface's current
  colour serial (`fresh=1`, `covers=1`, no async-ticket/serial mismatch). The
  1x1 exposure value is **78.7 +/- 0.3** (0-255 luma) in the all-off run and
  **78.7 +/- 0.2** with RECT+BATCH+SKIP on; drift across the window is
  **-0.1** and **0.0**. Thumbnail mean luma is **101.2** in both. The values in
  the two runs are indistinguishable.
* **The reported live-game lighting drift did not reproduce on the meter.**
  Nothing here implicates the uncommitted cuts, WCB=2 on-demand timing or the
  exposure readback. A live-game capture (below) is the next step; no fix was
  made and the third run was not used.

## 1. What was committed

`libs/video` in `exp/live-ab`:

* `PS3RECOMP_RSX_WCB_RECT` (live key): resolve only the page-aligned
  rectangle an NV3089 transfer reads, then re-arm the rest of the target once
  the transfer's read is over (`rsx_draw_engine_guest_read_done`).
* `PS3RECOMP_RSX_WCB_BATCH` (live key): take all pending requests, queue their
  copies into one submission and one wait, then release the waiters.
* `PS3RECOMP_RSX_WCB_SKIP` (live key): skip a copy whose colour serial has
  not moved and whose previous copy covers the requested rectangle; a guest
  write to the target clears the copy.
* `PS3RECOMP_LIGHT_PROBE` (env, default off): see section 3.

The cuts were reviewed against `ecbabe48`. With both cuts off the resolve,
copy and store paths are the old ones; the only unconditional additions are
fields, counters and one flag test. The one real behaviour change found and
fixed was in `rsx_draw_engine_guest_read`: the new code set `guest_dirty` after
a full transfer read even with the cuts off. It is now inside
`if (eng_wcb_rect_on() || eng_wcb_skip_on())`.

`test_rsx_draw_engine` gained `test_wcb_transfer_rect` (page-aligned rectangle
math, clamping, empty reads) and `test_wcb_skip_invalidation` (same serial is
reused, a new draw and a guest write both invalidate). `test_gcm_fifo_user_command`
and `test_gcm_flip_wrap` gained no-op stubs for the profiling/capture/wake
hooks that earlier branch commits already made `cellGcmSys.c` call; they were
failing to link before this change. `python3 tools/test_video_fixtures.py`
reports **29 passed**.

## 2. Static: the lighting targets

`Lighting_InitStadium(lighting, stadium, no_render, mode)` at `0x004f0380`,
only when `no_render == 0`, calls `FUN_004b8f04`/`FUN_004b8ae4` (`0x004b8f04`)
with `(name, width, height, levels, format_arg, align, flags)`:

| handle | name | size | levels | flags | where |
|---|---|---|---|---|---|
| lighting+`0x68` | `ToneMapSample` | 32x32 | 6 | `0x14008` | created at `0x4f05d0` |
| lighting+`0x64` | `QuickAmbientSky` | 32x32 | 1 | `0x14008` | created at `0x4f09b4` |
| lighting+`0x9a4` | `HarmonicsSample` | 32x32 | 1 | `0x10008` | created at `0x4f09e8`, then `FUN_004b9454(h,0)` |

The engine creates these as `RSX_BE_FMT_R8G8B8A8` surfaces with pitch 128; the
probe logs `fmt=0x03 32x32 pitch=128`, i.e. RGBA8. The six `ToneMapSample`
levels are exactly the observed 32x32,16x16,8x8,4x4,2x2,1x1 chain (4096, 1024,
512, 256, 128, 64 bytes with the 64-byte row alignment).

`Lighting_RenderExposureRT(lighting, camera)` at `0x004f48c8` is the consumer
of `ToneMapSample`: it returns immediately unless `lighting+0x68 != 0` and
`*(int*)(lighting+0x1f4) == 0`. It renders the current camera through
`TempCam RenderToneMapExposureRT` into the handle at `lighting+0x68`, then sets
the pass `ToneMapMipGen` and calls `FUN_004b21f0(cam, 1)`, which generates the
mip chain. That is the chain the CPU later reads.

`Lighting_UpdateFrame` at `0x004f2198` is called from the scene/system update
paths (15 call sites) and writes the lighting constant block that every
material receives: exposure/ambient scalars at `scene+0x548`, `+0x560`,
`+0x564`, `+0x568`, `+0x57c`, `+0x534`, `+0x538`, light vectors at
`+0x1cc..0x1d8`, and the material light slots (`iVar7+0x180`, `+0x238`,
`+0x360`, `+0x250`, ...). It does not itself read the targets; the scene's
lighting step reads the target's guest address with ordinary CPU/SPU loads,
which is why WCB=2 sees a page fault rather than a GCM read call.

**Read cadence.** The guest reads the rendered samples directly from guest
memory. On the meter the probe saw the `ToneMapSample` 32x32, 2x2 and 1x1
copied by a `REQUEST` resolve 4,198 times each in 255 s (16.5/s, about every
1.8 frames at ~29 fps). The 16x16/8x8/4x4 levels are allocated (their
addresses are contiguous) but never read. The read is therefore **continuous
per frame-ish, not once per scene**. `QuickAmbientSky` and `HarmonicsSample`
are 1-level targets and were not read in the meter window; the other 32x32
reads in the log are short bursts at boot (`21:20:40-50`), on a scene
transition (`21:22:25-27`) and in the menu (`21:23:09-24`), several of them
levels of larger 64/128-texel chains. Their per-run guest addresses are
allocator-assigned and were not isolated; the "captured once at start/once per
scene" recollection matches these burst reads, not the exposure target.

## 3. The probe

`PS3RECOMP_LIGHT_PROBE=1` (default off) logs every readback of a 32x32 or
<=2x2 surface, from all the copy paths:

* `eng_wcb_copy_surface` (`copy`, `guestreq`, `ticket` or `rect`),
* `eng_wcb_job_store` / `eng_wcb_job_queue` (`batch`, `batchskip`, `nodirty`),
* `rsx_draw_engine_guest_read` (`direct`, `readskip`, `armedskip`).

It does not touch guest memory (which may still be protected): the mean/luma/
first pixel are decoded from the `rows` buffer the copy just produced. The line
also carries the surface's current colour serial, the serial guest memory held
before the copy (`stored`), the async-ticket serial (if any), and whether the
previous copy covered the rectangle (`covers`, `fresh`). Example:

```
[light probe] frame=4198 slot=11 loc=1 addr=0x019D7900 fmt=0x03 32x32 pitch=128
  how=guestreq rect=0,0 32x32 mean=255.00,71.72,22.73 luma=107.15 p0=15FF4F15
  serial=1846 stored=1774 ticket=0 covers=0 fresh=1 dirty=0
```

Decoding uses the engine's readback order (R,G,B,A bytes), so `luma` is
`0.2126 R + 0.7152 G + 0.0722 B` on the 0-255 bytes. For the 1x1 level this is
the exposure average the game reads; the 32x32 and 1x1 means agree to <0.3,
which also confirms the mip chain is assembled consistently.

## 4. Runs

Both runs are one launch, the same meter, 240 s of measurement after X, with
the probe on and 2 s thumbnails. (a) cuts unset; (b) RECT+BATCH+SKIP set for
the whole launch through the control file.

| run | cuts | meter window | 1x1 exposure mean | std | drift | thumbnail luma |
|---|---|---|---|---|---|---|
| A `ab_live_exp_a_off_20261102_212018` | off | 250-505 s | 78.7 | 0.32 | -0.1 | 101.21 (100.09-101.86) |
| B `ab_live_exp_b_cuts_20261102_212936` | RECT+BATCH+SKIP | 245-499 s | 78.7 | 0.24 | 0.0 | 101.17 (100.52-102.38) |

The 32x32 and 2x2 levels track the 1x1 (same numbers, <0.3 apart). In both runs
every copy is `fresh=1` and `covers=1`, with no `stored`/`serial` mismatch at
the copy and no ticket/serial mismatch; there are **0** skip or rect copies on
the exposure chain (the target is redrawn every frame, so SKIP never fires and
the 32x32 page is read whole, so RECT never truncates it). The probe found no
stale or partial read to fix.

Fps on the meter is the same order in both launches: run A's two (identical,
all-off) arms 28.75 and 29.03 fps; run B's two (identical, cuts-on) arms 28.61
and 29.13. These are separate runs, not a within-run A/B, so they say only that
the cuts did not cost anything here.

PNGs under `recomp-work/visuals/exposure/`:

| file | content |
|---|---|
| `AB_exposure_compare.png` | `ToneMapSample` 1x1 luma, A vs B, meter window |
| `runA_meter_values.png` | run A: every target's luma and thumbnail luma, meter |
| `runB_meter_values.png` | run B: same |
| `runA_values.png`, `runB_values.png` | same over the whole launch (boot/menus included) |
| `runA_thumbs.png`, `runB_thumbs.png` | 2 s meter thumbnails with their full-frame luma |

## 5. Conclusion and next step

At the meter the exposure path is healthy: the sample chain is read
continuously, each read is served from a copy at the current colour serial, and
the decoded value is flat (78.7 +/- 0.3) for four minutes in both the all-off
and the cuts-on build. The uncommitted RECT/BATCH/SKIP code does not change it,
and WCB=2 on-demand readback is not handing the game stale or partial exposure
bytes here.

The live-game drift the user saw (warm/saturated field, dark players, normal in
replay) is not reproduced by the meter scene. Because automated runs must stay
on the meter, the next step is a user-driven live-game run with
`PS3RECOMP_LIGHT_PROBE=1` (and, if possible, a second run with the cuts off)
while the drift happens; the probe will log whether the exposure 1x1 and the
32x32 ambient/sky samples the game reads at that moment are fresh. If they are
fresh there too, the drift is downstream of the readback (time-of-day/weather
state, the per-frame 8x8 lighting texture, or the replay camera's lighting
path), not the readback cuts.
