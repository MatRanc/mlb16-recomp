# Missing jerseys / dark players: the dummy-texture census, and the cube surface it found

Date: 2026-11-02 (evening). Branch `exp/live-ab` in the `ps3recomp-play`
worktree, built as `recomp-work/build-play`. Windowed runs with
`PS3RECOMP_VK_WINDOW=1`, `VK_DUMMY_BLACK=1`, `PS3RECOMP_VK_PRESENT_MODE=fifo`,
`VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`, `PS3RECOMP_PIN=1`,
`PS3RECOMP_RSX_WCB=2`, `SDL_VIDEODRIVER=x11`, and
`PS3RECOMP_WALKER_RESYNC_FIX=1`.

## TL;DR

* **No jersey/skin texture hits the dummy through a format, address or size
  failure.** With `PS3RECOMP_TEX_MISS_LOG=1`, the engine resolved every sampled
  texture on the pitch meter: `format=0 address=0 size=0` for the whole run.
* **The only dummy fallbacks are cube units bound to a 2-D colour surface.**
  One fragment program binds an 8x8, 1-mip, `cubemap=1` texture whose guest
  address is also a tracked colour target; the engine hands the 2-D surface
  (or a 2-D `surface_view`) to a `TextureCube` slot, the Vulkan backend rejects
  the kind/cube mismatch, and the unit reads the dummy. At the meter that is
  **~195 misses/frame (max 582)**; ~217 of them come from one fragment program.
* **The team-select 3-D logo is not a dummy.** The team frames with black and
  with magenta dummies are pixel-identical (mean diff 0.0). `RESYNC_FIX=1` does
  not bring the logo back; the census sees no texture miss on that frame.
* **The magenta arm could not reach the meter.** With `VK_DUMMY_BLACK` unset
  the frame dump is full of pink, and the scripted nav/pitch verifiers (whose
  references were recorded with black dummies) lost the path twice. The saved
  pitch frame is b-roll, not the meter. The pink comparison below is therefore
  a b-roll frame, and the meter comparison is the black arm only.
* **`PS3RECOMP_VK_PRESENT_NEW_ONLY=1` gives MangoHud = `[flip]`.** Start-screen
  confirm run: `[flip]` **42.66 fps**, MangoHud **42.57 rows/s**, window
  `shown` **426 / 10 s**, 0 drops, pump gap 6-7 ms. The first attempt gated on
  flip *completions* and under-presented (23/s); the gate is now the flip
  *request* edge.
* The cube-surface fix is **not contained** (it needs a six-face host cube),
  so it is written down as a design, not implemented. The census and
  `PRESENT_NEW_ONLY` are implemented behind default-off env switches.

## 1. Static: every dummy substitution in `libs/video`

Two paths, and they are the only two `dummy` writes in the library.

### 1a. Standalone vtable backend (guest programs off)

`rsx_vulkan_backend.c`:

* `s_vk.dummy` is created in `vk_init_all` as a 1x1 magenta texel, or black
  with `VK_DUMMY_BLACK=1` (`:1093-1094`). `vk_write_texture_descriptor`
  (`:767`) binds it whenever `tex_ready[0]` is false.
* `vk_cb_bind_texture` (`:911`) is the whole resolution: it turns a unit off
  (`vk_tex_off`, `:894`) when, in order:
  * the unit is disabled or has offset 0 (an intentionally off unit, not a
    miss);
  * width/height is 0 or above 4096 (**size**);
  * `cellGcmResolveLocated` fails or there is no guest memory (**address**);
  * the mip-chain layout fails or the format is block-compressed (**format**;
    this path has no BC upload);
  * the decode/upload allocation or the Vulkan upload fails (**other**).
  Every failing branch after the "disabled" test now calls
  `vk_tex_miss_vtable` before `vk_tex_off`.

### 1b. Register-file draw engine (the default with guest programs on)

`rsx_draw_engine.c`:

* `sink_bind_textures` (`:3950`) resolves every unit the pixel program samples
  (`tex_used`). `eng_texture_slot` (`:2038`) returns 0 for:
  * width/height outside `ENG_MAX_TEX_DIM` (**size**);
  * `eng_texture_span*` returns 0 (**format/pitch**);
  * `eng_texture_upload` (`:1950`) cannot read the guest span (**address**),
    cannot allocate staging or create the host texture (**other**).
* A unit that names a tracked colour surface is bound to that surface's
  handle, or to a `surface_view` of it (`:4020-4101`). The code assumes the
  unit is 2-D; that assumption is where the cube case falls out.
* `vk_eng_bind_textures` (`rsx_vulkan_backend.c:3908`) hands the handles to
  the descriptor writer. `vk_eng_write_descriptors_to` (`:4464`) asks
  `vk_eng_sample_view` (`:3998`) for a 2-D or cube view. It returns NULL for
  any handle whose kind is not TEXTURE/COLOR/VIEW, or whose `cube` flag does
  not match the unit, and the descriptor gets `s_vk.dummy.view` or the 1x1
  cube dummy (`vk_eng_dummy_cube_view`, `:4009`).

`grep -n dummy libs/video/rsx_vulkan_backend.c` leaves only those sites; no
other file in `libs/video` substitutes a dummy.

## 2. The census (`PS3RECOMP_TEX_MISS_LOG=1`, default off)

`libs/video/rsx_tex_miss.{c,h}`, compiled into `rsx_draw_engine.c` (like
`rsx_wcb.c`) so every engine fixture gets it, and compiled standalone by its
unit test. The env is read once; the note path is a cached flag test.

Reasons: `format`, `surface` (render target / tiled surface), `address`,
`size`, `other`. Each note carries location, offset (guest address), format
byte, width/height, pitch, mips, cube, the fragment-program hash and the host
pipeline handle. Output:

```
[tex-miss] frame=12027 misses=259 format=0 surface=0 address=0 size=0 other=259 total=658759
[tex-miss] top 1: surface loc=0 addr=0x059f3000 fmt=0x?? 8x8 pitch=64 mips=1 cube=1 n=... fp=0x... pipe=...
```

A summary is printed for every frame with a miss; the cumulative top-10 list
every 60 emitted frames and at engine shutdown. The "kind" tag (bits 8-15 of
the format argument) is used by the Vulkan backend for a kind mismatch.

Hooks: `eng_texture_slot`/`eng_texture_upload` set the reason, and
`sink_bind_textures` notes each unit whose handle is 0 or whose surface path
would hand a 2-D object to a cube unit. The backend notes a non-VIEW kind/cube
mismatch (VIEWs are already counted by the engine with the guest address).

Unit test: `libs/video/tests/test_rsx_tex_miss.c` (fixture
`test_rsx_tex_miss` in `tools/test_video_fixtures.py`) checks per-reason
counts, key aggregation (two binds of the same texture with different programs
keep the last program), the per-frame reset, the cumulative totals, the
shutdown flush, and the kind tag.

## 3. Runs

One game launch per line. The meter recipe is `ab_live.sh`'s: NAV_HOME=1 to
the pitch, X once, verify the meter, settle 10 s. The tool is
`recomp-work/ab-tools/tex_census_run.sh black|magenta`.

| run | config | result |
|---|---|---|
| black | `VK_DUMMY_BLACK=1`, census on | meter reached; **28.95 fps** over 10 s |
| magenta 1 | black unset, census on | `STOP: pitch not reached` after 4m40; team frame saved, no meter |
| magenta 2 (retry) | same | same `pitch not reached`; stopped after the second identical failure |
| present confirm 1 | `PRESENT_NEW_ONLY` on, completion edge | 23/s presents vs 42.8 `[flip]`; wrong edge |
| present confirm 2 | `PRESENT_NEW_ONLY` on, request edge | **42.57 rows/s vs 42.66 `[flip]`**, 426 shown / 10 s |

Logs: `recomp-work/visuals/tex_census_black/run.log`,
`.../tex_census_magenta/run.log` (the retry overwrote the first attempt's log),
`.../present_new_only/run.log`.

### 3a. Black arm: the census at the meter

1029 meter frames had misses (18:01:11-18:01:44); **201,000 misses** in that
window, mean **195/frame**, max **582**; run total **659,395**. Every one was
reason `other` in this build (the surface notes were added after this run), and
the top entries were 8x8 handles. The retry's surface notes show the guest
addresses behind them:

```
surface loc=0 addr=0x059f3000 fmt=<printed 0x00, see caveat> 8x8 pitch=64 mips=1 cube=1
        n=~655k fp=0x4cb28b50b25a9097 pipe=380
surface loc=0 addr=0x0dc82400 ... fp=0x580a2535064e2ee1 pipe=342
surface loc=0 addr=0x0de42800 ... fp=0x14ddb3ef8e1060f4 pipe=366
surface loc=0 addr=0x0db1d800 ... fp=0x18e4be5ec62d4144 pipe=302
```

Caveat: those runs printed the format byte after an extra `>> 8` (the engine
passes the 8-bit field, the display shifted it again), so the log says
`fmt=0x00`; the real byte is not recoverable from those logs. The census now
prints the field directly. Address, size, pitch, mips and cube are valid.

### 3b. The magenta arm

`pitch_magenta.ppm` (saved from the last frame of the failed nav) is **not the
meter**; it is a b-roll shot behind home plate. Against the black arm's
`pitch_black.ppm` (the pitcher view) it differs everywhere (mean diff
94/56/99 per channel), and **217,220 pixels (~24%) are pure magenta** in the
magenta frame. The crowd and the player uniforms/gear are the pink areas. This
proves the dummy surfaces are visible when the dummy is magenta, but it does
not identify the meter's player materials, because the scenes differ.

The nav failure is an infrastructure failure, not a game failure: `nav3.py`
and `poll_pitch.py` classify 640x360 luma against references recorded with
black dummies. The magenta frame changes the luma enough that the pitch screen
is not recognised (first attempt: nudged `options` and left the path; retry:
sat on `stadium`), so the script stops before X. Two identical failures: the
run campaign stopped there.

### 3c. Team select / logo (task 4)

`team_black.ppm` and `team_magenta.ppm`: **identical**, mean diff 0.0, max 0.
The centre 3-D logo is missing in both; the 2-D team crests and the `AT`
panel are present. `PS3RECOMP_WALKER_RESYNC_FIX=1` does not bring it back, and
the census shows no texture fallback on the frame. This is the park-resync
draw loss in `docs/notes/tick-and-resync.md`, not texture resolution (the earlier
WCB doc reached the same conclusion: WCB does not change the team frame).

## 4. What the dark players are (and the fix that is not contained)

The symptom (batter/catcher/umpire dark, jerseys flat) is consistent with the
one real miss class: a small cube environment/lighting lookup sampled by a
`TextureCube` program is bound a 2-D colour target, so it reads black
(`VK_DUMMY_BLACK=1`) and the material loses its environment term. The magenta
b-roll frame confirms those same units turn pink. It is **not** a format
decode (0 format misses), an unresolved address (0 address misses) or a size
rejection (0 size misses), and it is not the jersey texture itself.

It is not contained because the six cube faces must be assembled on the host:

1. In `sink_bind_textures`, when `t.cubemap` and a colour surface exists at
   `t.offset`, look for the six face surfaces at `t.offset + face * stride`
   (`stride = pitch * height` for a linear target; `rsx_texture_cube_face_stride`
   for the texture layout). Require all six to be current (handle + colour
   serial + dimensions).
2. New backend hook `cube_surface_view(six_handles, six_serials)`: copy the six
   faces into one 6-layer, cube-compatible image (one `vkCmdCopyImage`/blit per
   layer) and cache it by the six key pairs; release the image when the
   surfaces are evicted or rewritten, same lifetime as `surface_view`.
3. If fewer than six faces are current, keep the dummy and count a
   `surface-cube-incomplete` reason, so the census does not hide the fallback.
4. Gate behind `PS3RECOMP_ENG_CUBE_SURFACE=1`, default off. Add a null-backend
   fixture with six 8x8 colour targets and a cube-bound draw (cache hit, one
   face rewritten, one face missing), plus a replay gate on the captured pitch
   frame. Then one windowed A/B on the meter.

Rejected alternatives: read the cube from guest memory (under `WCB=2` ondemand
the faces exist in the host target, not in guest pages; the resolve is driven
by guest accesses, so this would sample stale bytes), and sample one face as
the whole cube (wrong orientation and five faces missing).

**Follow-up (2026-11-03):** implemented as
[`cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md) behind
`PS3RECOMP_RSX_CUBE_RT=1` (default off). The census rows were confirmed to be
six separate 8x8 targets at `base + face * 0x200`; at the meter the cube misses
drop to 0 and the frame has no pink with the dummy unset.

## 5. `PS3RECOMP_VK_PRESENT_NEW_ONLY=1` (task 5)

Implemented in `rsx_vulkan_backend.c` (`:2244-2272`): `vk_present_window` asks
`cellGcm_flip_frame_count()` (`cellGcmSys.c:119`) and returns without acquiring
a swapchain image if it has not advanced. The new counter is bumped in
`gcm_flip_request` (`:3568`), i.e. on SetFlipCommand / a FIFO flip marker,
**not** in `SetPrepareFlip` (which bumps `cellGcm_flip_request_count`, used by
the frame clock's outer present, but presents no frame). That was the source
of the extra presents.

Static responsiveness check: `rsx_backend_pump()` runs on the frame clock on
its own `PS3RECOMP_PUMP_MS=8` cadence and is independent of presents; SDL
events, the swapchain-lost path and the window close all go through the pump,
not `vkQueuePresentKHR`. Re-presenting an unchanged image is not needed for
input or compositor liveness under x11; the pump gap in the confirm run stayed
at 6-7 ms.

Confirm run (start screen, 10 s; one earlier attempt was discarded because it
gated on completions and showed 23/s):

| metric | value |
|---|---:|
| guest `[flip]` | **42.66 fps** (430 flips / 10.00 s) |
| MangoHud CSV rows (one per present) | **42.57 rows/s** (426 rows) |
| backend window `shown` | **426 / 10 s** |
| dropped (no-slot + not-ready) | **0** |
| max pump gap | **6-7 ms** |

`play_windowed.sh` (both `recomp-work/` and `mlb16-recomp/tools/`) now exports
`PS3RECOMP_VK_PRESENT_NEW_ONLY=1`; the engine switch stays default off.

## 6. Compare PNGs

Under `recomp-work/visuals/`:

* `tex_census_compare/team_black_vs_magenta.png` - identical team frames.
* `tex_census_compare/pitch_black_vs_magenta.png` - black pitch vs magenta
  b-roll (different camera; the pink is the dummy).
* `tex_census_compare/pitch_diff_x4.png` - 4x amplified difference.
* `tex_census_compare/meter_black.png` - meter, black arm (the player pose the
  user reported).
* `tex_census_compare/meter_black_players_2x.png`,
  `tex_census_compare/pitch_magenta_players.png` - player crops.
* `present_new_only/run.log` - the confirm numbers.

## Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work
ab-tools/tex_census_run.sh black          # PS3RECOMP_TEX_MISS_LOG=1, meter
ab-tools/tex_census_run.sh magenta        # VK_DUMMY_BLACK unset (nav references are black-only)
ab-tools/present_new_only_run.sh          # start screen, MangoHud vs [flip]

cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_video_fixtures.py      # test_rsx_tex_miss among the video fixtures
```
