# Depth targets as textures: keep them on the GPU

Date: 2026-11-05. Branch `exp/depth-gpu` (off `exp/crowd-prof`) in worktree
`ps3recomp-depth`, build `recomp-work/build-depth`. **Zero game launches**:
the crowded capture `captures/walker_replay_fixed` replayed offline through
the real Vulkan backend (`PS3RECOMP_REPLAY_VK=1`). Follows
[`crowded-walker-profile.md`](crowded-walker-profile.md). No RPCS3 or other
GPL emulator source was read or cited.

## TL;DR

* The ~225 `dguest` binds per frame are **one** depth target: a 2048x1024
  Z24S8 zeta (the shadow map) sampled as a linear DEPTH24_D8 texture
  (`fmt 0xB0`, pitch 8192, 2048x1024) by 222 draws a frame. In the crowded
  capture it is **cleared to 1.0 and never drawn into** (no shadow casters),
  so `had_write` is 0 and `eng_zdepth_snapshot` refuses it ("a clear-only zeta
  falls through to guest memory"). Every bind then takes the guest path:
  one `submit_and_wait` + 8 MB `depth_readback` + store per frame (the GPU
  wait drains the whole open draw batch), plus a decode + 8 MB re-upload,
  plus a plain texture-slot lookup for each of the 222 binds.
* The format is not the problem (the device snapshot handles D32 and this is
  DEPTH24_D8, the same bind the other 480 `dsnap` binds a frame use). The
  gate is. Fix: with `PS3RECOMP_ENG_DEPTH_GPU=1` a zeta that is cleared and
  not drawn into, whose guest copy is still unstored (`guest_dirty`), is
  snapshotted on the GPU like a written one. The guest store stays lazy:
  `guest_dirty` is untouched, so every existing guest reader (forced store,
  WCB, texture-dirty) still gets the bytes when it asks.
* Offline, real Vulkan, 12 frames + 4 warm-up, 3 runs per arm: **walker
  61.3 -> 42.2 ms, `tex` 23.6 -> 5.4 ms**, `dguest` 222 -> 0, `dsnap`
  480 -> 702, `uploads` 1.0 -> 0.0. The final frame is **byte-identical**
  (PPM, 2,764,816 bytes, md5 equal across off, off-again and on); the
  measurement-only `DEPTH_NOGUEST` arm differs in 60,154 bytes, so the
  metric would have seen a wrong picture.
* Live key (default 0): `PS3RECOMP_ENG_DEPTH_GPU` in the `PS3RECOMP_CTL` file,
  or the env var.

## What the reads are

Logged with a temporary per-bind print (not kept) on the capture:

```
zfmt=2 (Z24S8) zsize=2048x1024 used=2048x1024 zpitch=8192
  <- tex base=0x90 (DEPTH24_D8) fmt=0xB0 (linear) 2048x1024 pitch=8192 mips=1 dim=2 remap=0xAAE4
```

All 888 binds in 4 frames are this one combination. The event stream for the
target per frame: `clear depth=1.0`, then 222 reads interleaved with writes to
the main depth target only. So it is a depth-compare shadow lookup into an
empty shadow map (all texels = the clear value 1.0), sampled by the world
fragment programs; not soft particles, DOF or SSAO. Why it missed the GPU
path: `eng_zdepth_snapshot` requires `had_write` (a depth-writing draw since
the last clear); the clear-only case was sent to guest memory by design.

## The change

`libs/video/rsx_draw_engine.c`:

* `eng_depth_gpu()`: the key (`PS3_LIVE_ENG_DEPTH_GPU`, env fallback,
  `rsx_draw_engine_set_depth_gpu()` for unit tests).
* `eng_zdepth_snapshot`: `had_write || (eng_depth_gpu() && guest_dirty)`.
* `eng_zdepth_wrote`: with the key on, a depth write or clear drops
  `snapshot_valid`, so a snapshot taken at the clear is refreshed by a later
  draw (the old code only dropped it on a clear).

`libs/video/ps3_live_ctl.[ch]`: the live key. `tools/replay_rsx_engine_core.c`
and `rsx_draw_engine_replay_present_last()`: with `PS3RECOMP_VK_DUMP` set the
replay presents the last-written colour surface and completes the present, so
`PS3RECOMP_VK_DUMP=file.ppm` writes the final frame (the capture has no
display buffer, so it never presented before). Timing arms leave it unset.

Exactness: the snapshot is the device depth (a float), the guest store
quantised it to 24 bits and the texture decode read it back. For the clear
value this is the same number; the byte-identical frame confirms it. When the
guest copy has been stored (`guest_dirty` 0) and the target was not written,
guest memory is authoritative again (a PPU/SPU writer may have changed it) and
the old path applies unchanged. Not proven for a clear to an arbitrary
non-0/1 value: that goes through the same 24-bit quantisation difference
(at most 1 LSB of 2^24).

## Numbers

Real Vulkan replay, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0`,
`--frames 12 --warmup 4`, 3 runs per arm, median of the per-report values:

| ms/frame | key 0 | key 1 |
|---|---:|---:|
| walker | 61.33 | 42.19 |
| tex | 23.55 | 5.44 |
| draw | 13.22 | 12.64 |
| fetch | 10.47 | 10.37 |
| pipe | 5.26 | 5.21 |
| other | 5.34 | 5.23 |
| tool median frame | 59.9 / 60.9 / 60.4 | 38.7 / 38.5 / 38.8 |

`[tex-stats]`: key 0 `dsnap=480 dguest=222 uploads=1.0`; key 1
`dsnap=702 dguest=0 uploads=0.0`. This recovers the whole of the NOGUEST
bound (walker 43.7 ms) with a correct picture.

Image: `PS3RECOMP_VK_DUMP` final frame, 3 measured + 2 warm-up frames: off vs
off-again 0 differing bytes; off vs on 0 differing bytes; off vs NOGUEST
60,154 of 2,764,801 bytes. Digests with the key off are unchanged:
`0e8f7c71ca8be27c` / gate `a9a80aa0392953cd` (null path, 5 + 2 frames).
`tools/test_video_fixtures.py` 34/34 (with `test_gcm_tick_in_drain`, a known
flake, rerun once). New unit test:
`test_depth_gpu_clear_only` in `libs/video/tests/test_rsx_draw_engine.c`.

Caveat: this capture's shadow map is empty. Frames where the game does draw
shadow casters already took the `had_write` snapshot path and are not changed
by this key (the `eng_zdepth_wrote` invalidation aside).

## Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_ENG_DEPTH_GPU=1 PS3RECOMP_TEX_STATS=1 PS3RECOMP_REPLAY_VK=1 \
VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 REPLAY_NULL_HASH=0 \
  ./build-depth/ps3recomp_sdk/replay_rsx_engine ./captures/walker_replay_fixed \
  --frames 12 --warmup 4
# image: add PS3RECOMP_VK_DUMP=captures/depthgpu/x.ppm and --frames 3 --warmup 2
```

## Live check (USER)

One launch, about 3 minutes. Boot the depth-gpu build with the key off, get
to the scene, then flip it live (the control file is rewritten whole, as in
`edge-e1-holes.md`):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
CTL=$PWD/depthgpu.ctl
printf '%s\n' PS3RECOMP_ENG_DEPTH_GPU=0 > "$CTL"
BUILD=build-depth PS3RECOMP_CTL="$CTL" ./play_windowed.sh
# zoomed-out replay / fielding with a crowd; after ~60 s:
printf '%s\n' PS3RECOMP_ENG_DEPTH_GPU=1 > "$CTL"
# after ~60 s more, back to 0 to confirm the picture and fps return:
printf '%s\n' PS3RECOMP_ENG_DEPTH_GPU=0 > "$CTL"
```

Look at: the on-screen fps (expect up ~8-13 ms/frame in the crowded scene), the
`[walker-prof]` lines (`tex` ~28 ms toward ~10, `walker` down by ~10-13 ms),
`[tex-stats]` (`dguest` ~228/frame to ~0, `dsnap` up by the same, `[live-ctl]`
prints each flip), and that player and ground shadows and any shadow-map
effect look identical at each switch: no flicker, no missing or doubled
shadows, no black or white patches. Any difference is a regression; report it
with the scene and the flip time.
