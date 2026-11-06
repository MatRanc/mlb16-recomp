# Tiled logo wipe: capture, root cause and PS3RECOMP_WIPE_FIX

Date: 2026-11-06. Branch `exp/wipe` (off `exp/crowd`, tip `ab2de01f`) in
`ps3recomp-sync`, build `recomp-work/build-sync`. Offline only: **no game
launch** was made for this mission; the user's walker capture
`recomp-work/captures/wipe2_cap.{fifo,state,mem}` was replayed. No RPCS3 or
other GPL emulator source was read or cited.

## TL;DR

* **Root cause: our own render-target readback writes a linear image into guest
  memory, then the wipe samples that address through a Morton (swizzled)
  texture.** The wipe is one full-screen blended draw (`pass 022`) into
  `0x01110000` sampling a 512x256 A8R8G8B8 texture at local `0x0B836000`
  (`SET_TEXTURE_FORMAT` = `0x00018529`, format byte `0x85`, no LN bit, so the
  RSX layout is Morton). The captured guest bytes at that address decode to a
  clean "NEW YORK METS / KANSAS CITY ROYALS, Kauffman Stadium" card when read
  **linear**, and to the reported checkerboard of scrambled tiles when read
  **Morton**. The game renders the card into a colour target at that address
  (FIFO methods `0x0210/0x0214/0x0218` name `0x0B836000`, clip 512x256) inside
  the tile region `SetTile(index=6, offset=0x0B610000, size=0x800000,
  pitch=8192)`; our `eng_surface_to_guest` copies the host image row by row
  (linear), which is not the layout the texture unit will read.
* **Fix:** `PS3RECOMP_WIPE_FIX` (live key, default **0**). For a 32-bit
  A8R8G8B8/D8R8G8B8 power-of-two 2-D texture whose format says Morton, it
  compares the two candidate decodes on 8-texel-spaced neighbours (Morton
  preserves distance-1 locality, so stride 8 is what separates the layouts)
  and reads the bytes the way they are actually stored. Genuinely Morton
  images keep the hardware layout (tested with a Morton-stored copy of the same
  card).
* **Proof:** `captures/wipe/w2/replay.png` (key off) is the checkerboard;
  `captures/wipe/w2/wipefix_1.png` (key on) is the correct wipe card over the
  stadium. Key off is byte-identical to the original replay. On
  `captures/walker_replay_fixed` (crowd) and `meter_frame` the null-backend
  gate and the Vulkan dump are **byte-identical** with the key on and off.
  Video fixtures 34/34 in the final run (the known `test_gcm_tick_in_drain`
  flake failed once in an earlier run of the same tree).
* **Oddity explained:** the replay prints `draws/frame: 0.0` because that field
  is `rsx_draw_engine_guest_draws()` = `g.last_guest_draws`, which is only
  updated by `eng_present()` on a guest flip. `PS3RECOMP_VK_DUMP` presents
  through `rsx_draw_engine_replay_present_last()`/`present_offset()`, which call
  the backend directly and never `eng_present()`. The walker's own
  `[walker-prof]` line in the same run shows the real count (686 draws,
  `libs/video/rsx_draw_engine.c:9226` resets the counter in `eng_present`).

## 1. Phase 0 (previous mission): the frame sequence

`ab_live.sh` on `BUILD=build-sync`, one pitch-meter route, `SCENE_SHOTS=0`,
`PS3RECOMP_VK_DUMP_EVERY=3`, plus a watcher that copied every distinct
`frame.ppm` to `captures/wipe/wipe1/live/`.

The batter-intro transition, 00:37:42.1-00:37:45.3 (dense frame names):

| dense frame | time | what it shows |
|---|---|---|
| `f_184` | 42.0 s | normal stadium flyover, no overlay |
| `f_186` | 42.12 s | small checkerboard rectangle enters at the top-left |
| `f_188` | 42.46 s | the rectangle is a left band |
| `f_190` | 42.63 s | large rectangle over sky/stands; scene visible through the tile pattern |
| `f_194` | 43.32 s | tiles cover most of the frame |
| `f_196` | 44.85 s | thin boundary at the right edge, batter close-up |
| `f_198`/`f_200` | 45.0/45.3 s | clean batter close-up |

This is a short animated wipe, not a persistent overlay. It lasts ~2.7 s.

## 2. Phase 1: root cause (offline replay of `wipe2_cap`)

### 2.1 The capture and the replay

`recomp-work/captures/wipe2_cap.{fifo,state,mem}`: 62,037 words, 13,304 read
ranges, 332.9 MB guest image, 0 read misses in replay.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0 \
PS3RECOMP_VK_DUMP=$PWD/captures/wipe/w2/replay.ppm \
  ./build-sync/ps3recomp_sdk/replay_rsx_engine ./captures/wipe2_cap --frames 2 --warmup 1
```

The image is the checkerboard. Pass/draw dumps were taken with
`RSX_ENGINE_PASS_DUMP=$PWD/captures/wipe/w2/pd2 RSX_ENGINE_PASS_DUMP_NOW=1
RSX_ENGINE_PASS_DUMP_DRAWS=1` (the replay's present now flushes the last open
pass, see §5). `pd2/passes.txt` has 23 passes; the wipe is the final one:

```
pass 022: draws 1 clears 0 | A 0x01110000 fmt 3 2560x720 p10240 |
  zeta 0x01990000 ... | pipe 35 dt 0 dw 0 blend 1 302/303 ...
  | t0 0:0x0B836000 f85 512x256 p0 rAAE4 zf0 G
```

So the wipe is **one full-screen, blended draw** into the main 3D target,
sampling exactly one guest texture: 512x256 A8R8G8B8 (format byte `0x85`) at
local offset `0x0B836000`. The fragment program is trivial
(`captures/wipe/w2/shaders/pipe35.ps.hlsl`): `texture0(tc0) * vertex colour`,
then the SRGB output curve. `pipe35.vs.hlsl` takes the UVs from attribute 8.
Nothing else is sampled, so the checkerboard must be in those bytes or in the
way they are decoded.

### 2.2 The bytes are linear; the format says Morton

`SET_TEXTURE_FORMAT` for unit 0 is `0x00018529` (raw FIFO word at
`wipe2_cap.fifo` word 60,920): bits 8-15 = `0x85`, bit 5 of that byte (the
`CELL_GCM_TEXTURE_LN` flag) is clear, dimensions 512x256 are powers of two.
`rsx_texture_layout_pitched` therefore sets `tl->swizzled = 1`
(`libs/video/rsx_texture_layout.c:108-121`) and `rsx_texture_decode` walks the
image in Morton order (`libs/video/rsx_texture_layout.c:333-345`).

Decode the captured guest bytes both ways (`captures/wipe/w2/`):

| file | decode | result |
|---|---|---|
| `tex2_0B836000_lin.png` | linear rows, 512x256, pitch 2048 | **correct**: "NEW YORK METS 90-72 / KANSAS CITY ROYALS 95-67, Kauffman Stadium" |
| `texdump/tex_0_0B836000.png` | engine's Morton decode (dumped by `PS3RECOMP_ENG_TEXDUMP`) | **the checkerboard** |

The engine's own dump and an independent Python Morton decode agree, so the
decoder is doing exactly what the format asks; the **bytes are the problem**.

### 2.3 Where the linear bytes came from

The card is a render target, not an uploaded asset. In the same run's raw FIFO
(`captures/wipe/wipe1.bin`, found around byte 264,714,032) the command stream
sets:

```
0x0210 = 0x0B836000    colour target A
0x0214 = 0x0B836000    colour target B
0x0218 = 0x0B836000    colour target C
0x0200 = 0x02000000 / 0x0204 = 0x01000000   clip 512 x 256
```

and the live log shows the matching tile region:

```
[cellGcmSys] SetTile(index=6, loc=0, offset=0xB610000, size=0x800000, pitch=8192)
```

`PS3RECOMP_WIPE_FIX` is not involved in this: the bytes are linear because the
engine's colour-target readback copies the host image row by row
(`eng_surface_to_guest`, `libs/video/rsx_draw_engine.c:3965-3978`, using
`SET_SURFACE_PITCH` rows) while hardware stores a tiled/Morton target in the
tile region. When the target is no longer in the engine's surface table (it is
rendered once, then the table churns; `[rsx engine] surface table full:
evicting the least recently used` appears earlier in the same log), the texture
bind falls through to guest memory — and reads our linear copy as Morton.

This is the same class as the unproven candidate in
[`rsx-sync.md`](rsx-sync.md) ("a tiled surface written between the bind and the
sample"), but it is a **layout** bug, not an ordering bug, which is why
`PS3RECOMP_RSX_SYNC_FIX` does not change it.

## 3. Phase 2: the fix (`PS3RECOMP_WIPE_FIX`)

`PS3RECOMP_WIPE_FIX` (live key `PS3_LIVE_WIPE_FIX`,
`libs/video/ps3_live_ctl.h:109`, env default 0) adds a layout probe in the
texture uploader, before the decode:

* `eng_tex_layout_probe` (`libs/video/rsx_draw_engine.c:3134`) applies only to
  **2-D, uncompressed, 32-bit A8R8G8B8/D8R8G8B8, power-of-two, format says
  Morton, at least 64x64** textures.
* `eng_tex_adjacency` (`:3102`) scores both candidate layouts: the sum of
  neighbouring-texel channel differences at a stride of **8 texels**,
  horizontally and vertically, on a 64x64 sample grid. Distance 1 barely
  differs between the layouts (Morton keeps it local); distance 8 scatters a
  row in the Morton interpretation and scores much worse. Measured on the
  captured card: linear `490,585` vs Morton `991,339`. A synthetic
  Morton-stored copy of the same card scores the other way (`670,344` vs
  `490,585`), so the probe keeps the hardware layout for genuine Morton data.
* If linear is clearly smoother (`lin * 4 < mor * 3`), every mip level's
  `tl->swizzled` is cleared before `rsx_texture_decode` (`:3234`). The span,
  mip offsets and texture-cache key are unchanged (layout does not change
  `face_bytes`).
* When the key is off the probe is a single cached boolean; nothing else
  changes.

The live engine already samples a **still-registered** colour target through
`surface_view` instead of guest memory, so the probe is a no-op there; it fixes
the cases where the target has been evicted/reused and the guest copy is all
that is left (which is what the capture and the live wipe hit).

### 3.1 Why a sampler probe instead of fixing the readback

Writing the readback in Morton order would fix future frames live, but the
capture's `.mem` was taken with the old readback and a replay cannot recreate
the surface (it was rendered in an earlier frame). A provenance flag in the
capture would need a new live capture. The probe corrects the existing bytes,
so the same capture proves the fix offline, and it also covers any other title
whose render target is read back linearly and sampled as Morton.

## 4. Proof

Commands (the "after" arm; the "before" arm is the same with
`PS3RECOMP_WIPE_FIX=0`):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0 \
PS3RECOMP_WIPE_FIX=1 \
PS3RECOMP_VK_DUMP=$PWD/captures/wipe/w2/wipefix_1.ppm \
  ./build-sync/ps3recomp_sdk/replay_rsx_engine ./captures/wipe2_cap --frames 2 --warmup 1
```

| image | what it shows |
|---|---|
| `captures/wipe/w2/replay.png` | before: full-screen checkerboard of scrambled tiles |
| `captures/wipe/w2/wipefix_1.png` | after: the Mets/Royals card over the stadium (the wipe) |
| `captures/wipe/w2/texdump/tex_0_0B836000.png` | the input texture as the engine decoded it before the fix (checkerboard) |
| `captures/wipe/w2/tex2_0B836000_lin.png` | the same input bytes read linear (the intended card) |
| `captures/wipe/w2/pd2/pass022_t0.png` | the pass that produces the wrong present (the wipe draw) |

`wipefix_0.ppm` is byte-identical to the original `replay.ppm`
(`md5 e331d4538caf96853005e462fee1b20f`), so the key-off path is unchanged.

### 4.1 Gates

| capture | arm | null draws | last hash | gate |
|---|---|---:|---|---|
| `wipe2_cap` | key 0 | 686 | (VK hash 0) | original image, md5 `e331d453…` |
| `wipe2_cap` | key 1 | 686 | (VK hash 0) | corrected image, md5 `129e9bdc…` |
| `walker_replay_fixed` (crowd) | key 0 | 1,694 | `0e8f7c71ca8be27c` | `f71ee0f438ed900d` |
| `walker_replay_fixed` (crowd) | key 1 | 1,694 | `0e8f7c71ca8be27c` | `f71ee0f438ed900d` |
| `meter_frame` | key 0 | 502 | `a6fcc849341eba9b` | `ff1ae064a377ce16` |
| `meter_frame` | key 1 | 502 | `a6fcc849341eba9b` | `ff1ae064a377ce16` |

The Vulkan dumps are also byte-identical on the two gate captures (crowd
`e7b76dba…`, meter `d0cccbb8…`, key on and off). `python3
tools/test_video_fixtures.py`: **34/34 pass** in the final run; the known
`test_gcm_tick_in_drain` flake failed once in an earlier run of the same tree
(`test_rsx_draw_engine` passes both times).

### 4.2 Cost

The probe runs once per newly uploaded qualifying texture. On the captured
512x256 card it samples 64x64 positions x 2 neighbours x 2 layouts (~16k
channel sums); the replay's frame time is unchanged within noise (the wipe
frame is dominated by the 686 draws). Nothing is default-on.

## 5. Diagnostics added (all default off)

* `PS3RECOMP_ENG_TEXDUMP=<dir>`: each newly uploaded texture's level 0 as
  `<dir>/tex_<location>_<offset>.ppm` (the pixels the sampler gets). This is
  how `texdump/tex_0_0B836000.png` was made.
* `PS3RECOMP_REPLAY_SURFACE_INFO=1` / `PS3RECOMP_REPLAY_SURFACE_DUMP=<file>`:
  list the surface table at replay present time, and write the presented
  surface's own readback (to tell a wrong surface from a wrong present blit).
* `rsx_draw_engine_replay_present_last/_offset` now close the still-open pass
  before presenting, so a capture that ends mid-frame writes its last pass to
  `passes.txt`. (Previously the wipe pass 022 was invisible: pass 021 was the
  last flushed pass, which is why `pass015_t0` looked clean while the presented
  surface did not.)

## 6. Live command for the user

One launch, windowed. `PS3RECOMP_CTL` is an explicit absolute path; no shell
variables. This build is `build-sync`, not the play build.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
rm -f /tmp/wipe.ctl && : > /tmp/wipe.ctl
BUILD=build-sync \
PS3RECOMP_CTL=/tmp/wipe.ctl \
PS3RECOMP_WIPE_FIX=0 \
./play_windowed.sh
```

At the pitch screen press X once (the meter; never twice), let the batter intro
start, then toggle the key in a second terminal:

```sh
printf '%s\n' PS3RECOMP_WIPE_FIX=1 > /tmp/wipe.ctl
```

The log prints `[rsx engine] wipe-fix: 512x256 fmt 85 read linear (...)`. The
wipe should change from the checkerboard of tiles to the smooth card; toggle
back to 0 to compare.

## 7. Clean room

Only this repository's runtime, capture format, replay tool and the game's own
FIFO were used. No RPCS3 source, diff or fork was read or cited.
