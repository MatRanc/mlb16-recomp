# What the WCB=2 readback actually costs on the pitch meter

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Meter run: `ab_live.sh`, X once, `METER=1`,
`WCB=2`, `PIN=1`, `PS3RECOMP_RSX_CUBE_RT=1`, x11, `N=2 SEG=10 DROP=2`.
All runs are one game launch each.

This started from the premise that the normal visual configuration
(`PS3RECOMP_RSX_WCB=2`) pays ~7.5 ms/frame of readback and that much of it is
triggered by the **walker's texture binds**, so serving those targets as GPU
textures (the 2-D equivalent of the cube path) would cut the meter's walker
time. The attribution below says the first half is right and the second half
is not: at the meter there are **no texture-bind readbacks at all**. The whole
readback bill is guest-mandated (an NV3089 transfer plus PPU/SPU page faults).

## TL;DR

* **Attribution is implemented** behind `PS3RECOMP_RSX_WCB_ATTR=1` (default
  off): every WCB=2 copy is counted against its surface, the path that
  triggered it (`texbind`, `guest`, `other`) and the copy + guest-wait time,
  printed per 60-frame window.
* **Meter, steady window (35 x 60 frames, 19:27:14-19:27:49):**
  **1.32 copies/frame, 4.60 ms/frame copy, 1.57 ms/frame guest wait** -
  about 6.2 ms/frame, the same order as the 7.5 ms WCB=2-vs-WCB=0 gap.
* **Trigger split at the meter: 0.00 texbind, 0.66 guest, 0.66 other.**
  Texture binds copy nothing. The remaining two thirds are:
  * `other`: a 512x512 RGBA target at `0x0B36BC00` (VRAM), copied **once
    every ~3 frames**, 3.13 ms/frame average. It is read through the NV3089
    blit path (`cellGcmSys.c:1168` -> `rsx_draw_engine_guest_read`), i.e. the
    guest asked for the bytes in its command stream.
  * `guest`: a 32x32 + 2x2 + 1x1 mip chain at `0x019D7900/0x019D9000/
    0x019D9080`, read by a PPU/SPU page fault every ~3 frames, 1.47 ms/frame
    copy plus 1.57 ms/frame wait for 4 KB of data. The time is GPU
    submission/stall, not bandwidth.
* **The bind readback path exists, but not here.** On the menu screen the
  same attribution saw a 2560x720 Z24S8 depth target bound as A8R8G8B8 copied
  every frame (~3.3 ms, 2.5 MB). `eng_zdepth_as_color`
  (`rsx_draw_engine.c:1840`) would take it on the GPU, but Vulkan does not
  implement the `depth_as_color` backend hook; Metal does
  (`rsx_metal_backend.m:3250`). At the meter the game never binds its depth
  that way.
* **Consequence:** `PS3RECOMP_RSX_RT_TEX_GPU` cannot move the meter, because
  there are no texture-bind readbacks to serve. The requested A/B was not run
  (see "Why there is no RT_TEX_GPU A/B").
* The real meter levers are on the guest-mandated copies: a rectangle-sized
  NV3089 resolve instead of a whole 512x512 target, and one GPU submission for
  the four tiny mip resolves instead of four.

## 1. Static: where WCB=2 decides to read back

`eng_wcb_sync_point` (`rsx_draw_engine.c:2789`) arms every dirty non-display
colour and depth target with page protection; nothing is copied there.
`eng_wcb_resolve` (`:2718`) runs when one is touched and decides the trigger:

| reason (`rsx_wcb.h`) | who | attribution |
|---|---|---|
| `RSX_WCB_REASON_REQUEST` | another thread faulted (PPU/SPU load or DMA) | `guest` |
| `RSX_WCB_REASON_FAULT` | the owner thread faulted | `guest` |
| `RSX_WCB_REASON_EXPLICIT` from `sink_bind_textures` (`:4365`, `:4422`) | walker texture bind | `texbind` |
| `RSX_WCB_REASON_EXPLICIT` from `eng_guest_read` (`:1629`) | NV3089 blit the guest issued | `other` |
| `RSX_WCB_REASON_FLUSH` | eviction / shutdown | `other` |

The two engine-initiated copies are only reached when a direct bind cannot
serve the unit:

* **2-D colour already avoids the readback.** `sink_bind_textures`
  (`:4249`) scans `g.surfaces` for the exact `(location, offset)`, and binds
  the target handle or a `surface_view` directly (`:4444`). Only a target the
  same draw is writing needs a `color_snapshot`; no guest copy. A texture
  that names a colour target in a **format the host image cannot present**
  (`eng_format_reinterprets_rgba8`, `:2410`) goes through guest memory and
  then decodes as that format (`:4422`).
* **Depth-as-colour has a GPU path only on Metal.** A unit naming a depth
  target tries `depth_snapshot` (DEPTH24_D8) and `eng_zdepth_as_color`
  (`:1840`) before `eng_zdepth_to_guest` (`:4365`). The latter stores the
  target into guest memory exactly as RSX would, and the texture is decoded
  from those bytes. Vulkan's backend table (`rsx_vulkan_backend.c:4910`) has
  no `.depth_as_color`, so on this build every Z24S8-as-A8R8G8B8 bind is a
  full readback.

That is the complete WCB=2 copy policy; there is no copy at flip or at a
label, only at a guest touch, an engine transfer, a bind that needs converted
bytes, or eviction.

## 2. Attribution instrumentation

`PS3RECOMP_RSX_WCB_ATTR=1` (default off; window follows
`PS3RECOMP_RSX_WCB_STATS`, else 60 frames). Code:

* `eng_wcb_attr_note` (`:965`) counts one finished copy against its surface
  (colour slot or depth slot) and the current trigger.
* The trigger is set around the two `sink_bind_textures` calls (`:4365`,
  `:4422`) and derived from the resolve reason otherwise (`:2718`).
* `rsx_wcb_attr_set` / `rsx_wcb_attr_take_wait` (`rsx_wcb.c`) collect the
  guest-thread wait per region; only the request path adds to it, so the wait
  is attributed to the guest copy.
* `eng_wcb_attr_flush` (`:2895`) prints one line per copied surface plus a
  total, per frame, then resets. Off, the note path is one flag test.

Example steady line:

```
[rsx wcb attr] loc=1 addr=0x019D7900 fmt=0x03 32x32 pitch=128 color:
  reads 0.33/f [texbind 0.00 guest 0.33 other 0.00]
  copy 1.389 ms/f [texbind 0.000 guest 1.389 other 0.000]
  wait 1.458 ms/f bytes 1 KB/f
```

## 3. Meter run

Launch: `TAG=wcb_attr_retry`, `WCB=2`, `PIN=1`, `METER=1`,
`PS3RECOMP_RSX_CUBE_RT=1`, `PS3RECOMP_RSX_WCB_ATTR=1`,
`PS3RECOMP_RSX_WCB_STATS=60`, `N=2 SEG=10 DROP=2`, x11.
Log: `recomp-work/ab_live_wcb_attr_retry_20261102_192254.log`.

Steady window: 35 consecutive 60-frame attribution windows, 19:27:14-19:27:49
(after the 10 s settle; the first switch was at 19:27:10).

| surface | reads/f (tex/guest/other) | copy ms/f (tex/guest/other) | wait ms/f | bytes/f |
|---|---|---:|---:|---:|
| `loc=0 0x0B36BC00` RGBA8 512x512 pitch 2048 | 0.33 (0.00 / 0.00 / 0.33) | 3.132 (0.000 / 0.000 / 3.132) | 0.000 | 338 KB |
| `loc=1 0x019D7900` RGBA8 32x32 pitch 128 | 0.33 (0.00 / 0.33 / 0.00) | 1.389 (0.000 / 1.389 / 0.000) | 1.458 | 1 KB |
| `loc=1 0x019D9000` RGBA8 2x2 pitch 64 | 0.33 (0.00 / 0.33 / 0.00) | 0.055 (0.000 / 0.055 / 0.000) | 0.057 | <1 KB |
| `loc=1 0x019D9080` RGBA8 1x1 pitch 64 | 0.33 (0.00 / 0.33 / 0.00) | 0.023 (0.000 / 0.023 / 0.000) | 0.057 | <1 KB |
| **total** | **1.32 (0.00 / 0.66 / 0.66)** | **4.60 (0.00 / 1.47 / 3.13)** | **1.57** | **~339 KB** |

The `[rsx wcb]` line over the same windows agrees: `copied 1.2-1.6/f`,
`demand 1.2-1.6`, `sync 0.0`, `copy 3.8-5.5 ms/f`, `faults/waits 0.6-0.8/f`,
`timeouts 0`, `pending 46-47`.

### What the two non-bind rows are

* **512x512 `other`.** `cellGcmSys.c:1168` calls
  `rsx_draw_engine_guest_read` for an NV3089 blit; the engine sees the source
  surface is armed and copies it whole through `eng_wcb_resolve` with
  `EXPLICIT` (`rsx_draw_engine.c:1642`). This is the guest's own command
  stream asking for the bytes, not a texture bind.
* **32x32..1x1 `guest`.** The same cadence (once per ~3 frames) and adjacent
  addresses are the mip levels of a rendered texture. The PPU/SPU touches
  them, the pages fault and the engine copies each tiny target. 4 KB costs
  1.4 ms of copy and 1.4 ms of wait: each resolve does its own
  `submit_and_wait`, so this is stall, not PCIe.

The 512x512 copy and the mip reads happen on the same frames; the total is
about 6.2 ms/frame, which is the measured WCB=2 penalty
(`meter-scene-baseline.md` puts the WCB=2-vs-WCB=0 gap at ~7.5 ms including
the walker's follow-on tex/main-ring work).

## 4. The bind path that does exist (menus, not the meter)

The first launch (`ab_live_wcb_attr_20261102_191439`) stopped on nav
(`pitch not reached`) but captured the menu/loading phase. There the
attribution logged the depth bind readback every frame:

```
[rsx wcb attr] texbind-depth loc=0 addr=0x01990000 zfmt=2 pitch=10240
  used=2560x720 <- tex fmt=0xA5 base=0x85 2560x720 pitch=10240 mips=1 dim=2
  cube=0 linear=1 remap=0xAAE4 current=0
```

`fmt=0xA5` is A8R8G8B8 + LN; the source is a 2560x720 Z24S8 target
(`zfmt=2`), read as A8R8G8B8. `eng_zdepth_as_color` is the path that would
convert it on the GPU, but Vulkan's `depth_as_color` is NULL, so
`eng_zdepth_to_guest` stores all 2.5 MB into guest memory (about 3.3 ms) for
the bind to decode. This is exactly the 2-D equivalent of the cube path, and
it is worth doing for menus; it does not occur at the meter.

## 5. Why there is no RT_TEX_GPU A/B

`PS3RECOMP_RSX_RT_TEX_GPU` would serve **texture-bind** readbacks from the GPU
target. The meter has **0.00 texbind copies/frame**, so the feature cannot
change the meter's 6.2 ms readback or its fps, and the requested A/B (and the
frame-dump comparison) would measure a no-op. Per the operating rules, the
experiment stopped here rather than spend the last game run on a premise the
attribution contradicts.

If the feature is still wanted for menus/other titles, the concrete piece is
a Vulkan `depth_as_color` (port of `rsx_metal_backend.m:3250`, or an
image->buffer->RGBA8/R16 pass), gated behind the live key. The engine side
already prefers it (`rsx_draw_engine.c:1840`); no engine policy change is
needed.

## 6. What would move the meter

All remaining copies are guest-mandated, so the levers are making them
cheaper, not avoiding them:

1. **NV3089 partial resolve (3.13 ms/frame).** A 512x512 target is copied
   whole whenever the guest blits a rectangle out of it. Resolve only the
   rows/columns the transfer covers (page-aligned), or copy the whole target
   asynchronously and let the transfer consume the queue. A 1/3-frame 1 MB
   copy plus a full GPU stall is the biggest single term.
2. **Batch the tiny mip resolves (1.47 ms copy + 1.57 ms wait/frame).** The
   32x32/2x2/1x1 targets each take their own `submit_and_wait`; one
   submission for all regions a service pass resolves would remove most of
   the 3 ms, since they are 4 KB.
3. **Check the cadence.** The same target/mip set is read once every ~3
   frames; if the guest is polling an unchanged texture, the copies could be
   skipped when the target's colour serial did not move (an ondemand "already
   copied this serial" check, like the async ticket).

Only after those is a bind-side GPU path relevant, and then for menus.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=wcb_attr_retry METER=1 PIN=1 WCB=2 N=2 SEG=10 DROP=2 VIDEODRIVER=x11 \
  ARM_A="" ARM_B="" PITCH_THRESH=35 \
  PS3RECOMP_RSX_CUBE_RT=1 PS3RECOMP_WALKER_RESYNC_FIX=1 \
  PS3RECOMP_RSX_WCB_ATTR=1 PS3RECOMP_RSX_WCB_STATS=60 \
  ./ab_live.sh
```

`PITCH_THRESH=35` was needed because the meter's `pitch_wait` luma diff is
~30/255 with the cube render targets on; the 25.0 default was calibrated
without them. The first attempt with the default stopped on
`pitch not reached` (the game was on the pitch but the verifier did not
accept it). This is a nav/verification failure, not a game failure; the retry
reached the meter on the first try.

## 8. Images

* `recomp-work/visuals/rt_tex/meter_wcb_attr.png` - the confirmed meter frame
  of the attribution run (WCB=2, CUBE_RT=1).
* `recomp-work/visuals/rt_tex/meter_wcb_attr_players_2x.png` - 2x crop of the
  batter/catcher/umpire from the same frame.
* `recomp-work/ab_live_wcb_attr_retry_20261102_192254/meter_1793665618700_1.png`
  - the harness's own meter confirmation frame.

## Caveats

* One meter launch for the table (the task allowed one attribution run); the
  steady window is 35 x 60 frames and the per-window spread is small except
  for the 512x512 copy, which lands on one frame in three.
* The 512x512/mip rows are identified by address, format and trigger; the
  guest consumer is not named (no trace window in this run).
* `other` is the default trigger for engine-initiated copies. In this run it
  matched the NV3089 path by construction (`eng_guest_read` is its only
  caller); a `transfer` sub-trigger would make that explicit in the log.
* The Vulkan backend has no `depth_as_color`; the 3.3 ms menu depth copy is a
  backend gap, not an engine policy choice.
