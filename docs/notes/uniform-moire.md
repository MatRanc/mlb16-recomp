# Uniform moire: the sampler census, and the 2x1 derivative candidate that did not pay

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two pitch-meter launches (the hard cap): the first
tried the 64-entry sampler-table hypothesis, the second the 2x1 derivative
scale. Offline work used the existing `meter_frame` capture and its shader
dump. No RPCS3 or other GPL emulator source was read or cited. The MIT NHL
Legacy and inFAMOUS 2 notes were checked first (section 1).

## TL;DR

* **The Vulkan sampler table is not overflowing at the meter.** The guest
  samples **54 distinct sampler register sets** in the meter capture. A live
  1.5-minute meter run never printed the sampler-cache-full warning, so the
  inFAMOUS 2 style "64 samplers, then the default nearest/level-0 sampler"
  bug does not happen in this title/scene. Raising the table is not the fix.
* **Mip chains are complete, LOD bias is zero, and the LOD clamps match the
  levels.** Every sampled meter unit has the guest's full mip count, uploaded
  level by level (BC1/BC3 and RGBA8 alike). `TEXTURE_FILTER[12:0]` is **0** on
  all 54 states, so an ignored bias changes nothing here. `CONTROL0` max LOD
  equals `mips-1` in the sampled states.
* **Anisotropy is requested and ignored.** The cloth units ask for 2x-8x
  (`CONTROL0[6:4]`), but the Vulkan sampler never sets `anisotropyEnable`; the
  D3D12 backend does (and the MIT ports keep a 16x path). That is a real
  divergence and the strongest remaining lead, but it is not tested live yet.
* **The 2x1 derivative-scale candidate is implemented but not proven.** The
  RSX shades once per 1280-pixel and derives per pixel; we hold the scene
  target at 2560x720 samples and derive per sample. `PS3RECOMP_FP_AA` scales
  DDX/DDY and implicit 2D samples by the sample grid, default **off**. Key off
  the replay digest is unchanged (`3c3b3b85532a2ff5` / gate
  `0a9220f49d76e843` with `FP_TXD=1`). Key on changes the digest (new shader
  text), all 51 dumped meter shaders compile with glslang, and the decompiler
  fixture covers the new text.
* **The A/B says no.** N=4 x 10 s per arm: fps **35.56 -> 30.44**, p50
  **27.2 -> 29.5 ms**, p90 **34.3 -> 38.0 ms**, paired **+5.13 fps**
  (95 % CI -1.37..+11.62). The saved per-arm player crops show no confirmed
  change on the pants/jersey (A/B crop mean-abs 1.0-1.5 vs A/A 0.7-4.6), so
  the candidate is left default off and marked unproven.
* **The capture thumbnails are not a reliable judge for this bug.** They are
  640x360 downscales of the frame and can create their own moire from a fine
  cloth texture. The user check in section 6 is the real visual gate.

## 1. What the MIT reference ports already answer

* NHL Legacy **#245** (and #176 before it): a nearest-*mag* unit was point
  sampled at mip 0 at every distance, ignoring the min filter; the face
  checkerboard / jersey weave was fixed by using the min filter when
  minified. Our cloth units are `magf=2` (linear), so that exact bug does not
  apply, but it is the same class of "sampler decodes one field wrong".
* NHL Legacy **#239** (hash every mip level) and **#240** (guest clamp modes):
  our engine already hashes the whole mip span and decodes the wrap modes.
* inFAMOUS 2 `docs/NOTES.md`: the Vulkan backend cached **64** sampler states;
  past that, new states got the default sampler ("wrong filtering, wrap modes
  and LOD range"), and the fix was 4096 states capped by
  `maxSamplerAllocationCount`. That is why the 64-entry table was the first
  hypothesis here. inFAMOUS 2 also forces 16x anisotropy as an enhancement.
* No code was copied. The sampler-cache size and the anisotropy check are the
  reused ideas, credited here.

## 2. Offline sampler census, and what it rules out

`PS3RECOMP_ENG_SAMPLER_DUMP=400` (diagnostic, default off) on the meter
replay prints each distinct `(FILTER, ADDRESS, CONTROL0)` set once:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
REPLAY_NULL_HASH=0 PS3RECOMP_WALKER_PROF=0 PS3RECOMP_FP_TXD=1 \
  PS3RECOMP_ENG_SAMPLER_DUMP=400 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 8 --warmup 4
```

The meter uses **54 distinct states**. Their distribution:

| field | values seen |
|---|---|
| min filter `[18:16]` | 6 x38, 2 x6, 4 x6, 1 x3, 7 x1 |
| mag filter `[26:24]` | 2 x50, 1 x3, 4 x1 |
| max anisotropy `[6:4]` | 2x x34, 8x x13, 1x x4, 4x x3 |
| LOD bias `[12:0]` | **0 on all 54** |
| max LOD | matches `mips-1` on every multi-mip unit |

The three character programs that the jersey fix restored
(`4e49b63161c5f0f7`, `b73644bdf2f72eeb`, `35eca74f078c08e6`) use
BC3 colour + BC1 normal maps, e.g. `4e49...` units 4/7 are BC3 2048x256/
2048x128 with 9/8 mips and units 6/8 are BC1 the same size, all with
`minf=4, magf=2, aniso=2, bias=0`.

The engine's `eng_texture_upload_work` uploads every level of
`rsx_texture_mip_chain()` (and the backend creates the image with that level
count), so the "only mip 0" arm of hypothesis 1 is ruled out for these units.
The live run's sampler log never reported the 64-entry table filling, so the
inFAMOUS-style overflow is ruled out too: at 54 states the legacy table has
10 slots to spare even before boot/menu states are counted, and the warning
would have fired on the first overflow.

That leaves two live candidates: ignored anisotropy, and a texture-content/
filtering mismatch that a thumbnail cannot show.

## 3. The 2x1 derivative candidate (`PS3RECOMP_FP_AA`, default off)

The RSX runs the fragment program once per **pixel** and computes derivatives
per pixel. This renderer keeps the 2x1 antialiased scene target at its sample
resolution (2560x720) and shades each **sample**, so `ddx()` is half the
per-pixel x derivative. The candidate makes the derivative paths use the
pixel grid:

* `libs/video/rsx_fp_decompiler.c`: explicit `DDX`/`DDY` results are scaled by
  `float4(aa_sx, aa_sy, 1, 1)`; a 2D implicit `TEX` becomes
  `SampleGrad(coord, ddx(coord)*float2(aa_sx,aa_sy), ddy(...))`. Units whose
  coordinate is later rescaled by the unnorm post-pass keep their implicit
  `Sample` so that pass still finds them. Cube/volume and explicit
  `SampleBias`/`SampleLevel` are unchanged.
* `libs/video/rsx_draw_engine.c`: the sample grid is set before the decompile
  and is part of the pipeline identity when the key is on, so a live flip
  builds the new variants.
* `libs/video/ps3_live_ctl.{c,h}`: `PS3_LIVE_FP_AA` / `PS3RECOMP_FP_AA`,
  appended at the end of the enum.
* `libs/video/tests/test_fp_decompiler.c`: key off keeps the old `Sample`
  text exactly; key on emits the scaled `SampleGrad` and scaled `DDX`; a UN
  unit keeps `Sample`.

Offline checks:

```sh
# key off: the documented FP_TXD-on digest, unchanged
REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 PS3RECOMP_FP_TXD=1 PS3RECOMP_FP_AA=0 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 64 --warmup 10
# 3c3b3b85532a2ff5 / 0a9220f49d76e843, 525.0 draws/frame
# key on: a different digest (new shader text), same draw count
```

All 51 fragment programs dumped from the meter compile with
`glslangValidator -D -S frag --hlsl-iomap -e main -V` with the key on
(51 ok, 0 fail). `test_fp_decompiler` passes; one run of the fixture set was
29/30 with the pre-existing timing flake `test_gcm_tick_in_drain` (it also
fails standalone intermittently on this machine and touches no file here).

## 4. Live pitch-meter A/B

One launch, pitch meter, X once, `ab_live.sh`, N=4 x 10 s per arm, 2 s
dropped, `WCB=2`, `PIN=0`, x11, `FP_TXD=1`, `RSX_CUBE_RT=1`,
`PS3RECOMP_FP_AA` flipped by the control file:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=moire_aa N=4 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 WCB=2 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 PITCH_THRESH=35 \
  PS3RECOMP_FP_TXD=1 \
  ARMS='A=PS3RECOMP_FP_AA=0|B=PS3RECOMP_FP_AA=1' \
  BOOT_ARM='PS3RECOMP_FP_AA=0' ./ab_live.sh
```

Log `recomp-work/ab_live_moire_aa_20261105_010818.log`, marks
`..._010818.marks`; every switch logged `[live-ctl] ... PS3RECOMP_FP_AA=...`.

| metric | A (off) | B (on) |
|---|---:|---:|
| fps (segments pooled) | **35.56 +/- 2.53** | **30.44 +/- 6.22** |
| p50 / p90 / p99 ms (flip intervals) | 27.2 / 34.3 / 36.5 | 29.5 / 38.0 / 47.1 |
| walker / wait ms | 10.27 / 16.69 | 10.70 / 18.51 |
| engine / tex / draw ms | 9.298 / 0.408 / 3.000 | 9.761 / 0.432 / 3.166 |

Paired per-segment fps differences (A-B): 10.89, 1.67, 5.00, 2.93; mean
**+5.13** (95 % CI -1.37..+11.62, N=4). The first B segment is the worst
(25.18 fps): flipping the key changes the generated shader text, so the
in-run recompiles every affected pipeline (the run ends with 988 shader-cache
misses, 838 rejected). Later B segments are 34.49/31.79/30.29 fps, still
below A, so there is a steady cost as well.

The saved scene thumbnails were cropped around the pitcher/batter
(`recomp-work/visuals/moire_aa/A_seg*.png`, `B_seg*.png`). Matched-segment
mean absolute differences: A/A 0.70-4.60, B/B 4.71 (valid pairs), **A/B pants
crop 1.03/1.47/1.04**. The A/B difference is at the A/A level, i.e. no
confirmed change on the uniform. The thumbnails are also downscales that
manufacture their own twill moire, so they cannot prove the absence either;
the in-window user check is the gate.

**Verdict:** the derivative scale is not shown to fix the ripple and costs
fps. Keep the key default off; do not turn it on in `play_windowed.sh`.

## 5. What is still open (next mission)

1. **Guest anisotropy.** The Vulkan sampler ignores `CONTROL0[6:4]`
   (2x/4x/8x is requested on the cloth units). The D3D12 backend decodes it
   (`rsx_d3d12_backend.c`, `aniso[] = {1,2,4,6,8,10,12,16}`). Implement
   `anisotropyEnable`/`maxAnisotropy` behind a live key, capped by
   `VkPhysicalDeviceLimits.maxSamplerAnisotropy`, and include the field in the
   sampler key. Judge it at a player close-up, not in 640x360 thumbnails.
2. **Dump the pants program's texture at a close-up.** The meter capture does
   not carry the character texture pages the replay did not re-read (78 read
   misses), so the texture content cannot be decoded offline. A live
   `LD_TEXRGBA_DUMP` / a small Vulkan upload dump at the batter walk-up would
   show whether the twill is the diffuse/normal content (then the bug is the
   mip/LOD choice) or an artifact of our decode (then the bug is format/
   swizzle).
3. Only after one of those shows a change: re-run the meter A/B and judge the
   crop at full resolution, not the thumbnail sheet.

## 6. User command: 3-minute close-up check (key off vs on)

The candidate is live-flippable; one launch is enough. Use a player close-up
(batter walk-up, replay, or the pitch meter). The pitch screen cuts to idle
cinematics after ~30 s; press X once at the pitch to stop them and show the
meter (never twice -- that starts the throw).

```sh
cd /home/m/Documents/mlb16-online/recomp-work
printf '%s\n' PS3RECOMP_FP_AA=0 > moire.ctl
PS3RECOMP_CTL=$PWD/moire.ctl ./play_windowed.sh

# In a close-up, watch the pants and jersey for the fine diagonal ripple.
# Flip the candidate on:
printf '%s\n' PS3RECOMP_FP_AA=1 > moire.ctl
# The log prints: [live-ctl] flip=... PS3RECOMP_FP_AA=1 (was 0)
# Flip back after ~10 s:
printf '%s\n' PS3RECOMP_FP_AA=0 > moire.ctl
```

What to look for:

* **No change** when flipping: the 2x1 derivative/LOD is not the cause. This is
  the expected result from the A/B; report it and the next mission takes the
  anisotropy + texture-dump path.
* **Smoother cloth with the key on**: the derivative scale is the cause; note
  the cost (about 5 fps at the meter in the automated run) before making it a
  default.
* **Jersey/torso disappears, letters or numbers fragment, or new shimmer**:
  the `SampleGrad` rewrite broke that program. Note which uniform and flip the
  key back off; this is a shader-text bug, not a game setting.
* The live-ctl line must appear or the control file was not read (check
  `PS3RECOMP_CTL` and the path).

## 7. Files and artifacts

* Code: `libs/video/rsx_fp_decompiler.{c,h}`,
  `libs/video/rsx_draw_engine.c`, `libs/video/ps3_live_ctl.{c,h}`,
  `libs/video/tests/test_fp_decompiler.c` (branch `exp/live-ab`).
* Diagnostic: `PS3RECOMP_ENG_SAMPLER_DUMP=<max>` (default off) prints the
  distinct sampler states once each.
* Logs: `recomp-work/ab_live_moire_aa_20261105_010818*` (the tested A/B) and
  `recomp-work/ab_live_moire_samp_20261105_005645*` (the first launch, which
  showed no sampler-table overflow).
* Crops: `recomp-work/visuals/moire_aa/` (A_seg*, B_seg*, diff_A0_B1_x4.png).
* Shader dump: `/tmp/opencode/aadump` (not committed; 51 PS files).

## 8. Clean room and attribution

This is our own runtime, replay capture, trace and EBOOT. The reinstated
sampler-table size and the anisotropy check follow the MIT inFAMOUS 2 port
(`docs/NOTES.md`: 64 cached samplers -> 4096 capped by the device limit;
16x anisotropy as an enhancement) and the MIT NHL Legacy notes (#245 min
filter when minified, #239 all-mip hash, #240 clamp modes). No RPCS3 or other
GPL emulator source was read or cited, and no code was copied from either
port.

## 9. 2026-11-06 evening: anisotropy does not fix it; derivative grid still the lead

* `PS3RECOMP_ENG_ANISO` (ps3recomp `exp/visual-fixes` 38b522ba, default 0, live) applies the guest's
  `CONTROL0[6:4]` max anisotropy in the Vulkan engine sampler (sampler cache 64 -> 256 so the extra states
  do not fall back to the nearest default). Key off byte-identical on the meter replay.
* Live mound-visit A/B (one launch, full-res shots, `captures/mtrans/r2/moire_abc.png`): anisotropy on
  looks the same as off; the stipple stays. In theory missing anisotropy blurs, it does not alias.
* `PS3RECOMP_FP_AA=1` (the 2x1 derivative scale) visibly smooths the cloth (and the grass). Camera moved
  between arms, so this is indicative only. Switching it on stalls ~6 s (pipeline rebuilds; ab_live
  stopped on "no flips") and it cost ~5 fps before.
* The moire reproduces offline in the mound capture `captures/mtrans/r3/cap` (close players), so a
  cheaper equivalent of FP_AA can be judged by replay next.

## 10. 2026-11-06 night: root cause found -- polygon offset was dropped (`PS3RECOMP_VK_DEPTH_BIAS`)

Offline, on the mound capture `recomp-work/captures/mtrans/r3/cap` (the stipple reproduces there):

* A +2 mip LOD bias on every sampler (`PS3RECOMP_ENG_LOD_BIAS=2`) blurs the logo but keeps the stipple:
  not a mip choice.
* Replacing the R32F shadow map (`0x06600000`, DEPTH24_D8 read with the emulated PCF compare) by a
  constant 1.0 (`PS3RECOMP_DBG_DUMMY_VKFMT=100`) removes the stipple; the R8 gloss texture does not.
  So it is shadow acne.
* The game sets SET_POLY_OFFSET for its shadow casters (factor 1.1 / 2.2, units 10000 / 20000 in the
  capture's FIFO). The engine passes it to `set_depth_bias`, but no backend implemented it: the Vulkan
  pipelines drew every caster unbiased.

Fix: ps3recomp `exp/visual-fixes`, `PS3RECOMP_VK_DEPTH_BIAS=1` (startup key, default 0): engine
pipelines get `depthBiasEnable` + dynamic depth bias, set per draw from the guest offset (units scaled
for a Z16 zeta). Mound replay: the stipple is gone from jerseys and pants, cast shadows (strap, arm) stay
(`captures/mtrans/r3/db_cmp.png`). Meter replay byte-identical with the key on and off; replay frame
time unchanged. Limit: the host D32F unit equals the guest D24 unit only for depth >= 0.5, so near
casters get less bias than on RSX. Not yet checked live.

Live (2026-11-06 21:27, one mound-visit launch, `build-sync` 5726ebe3, full play key set +
`PS3RECOMP_VK_DEPTH_BIAS=1`): uniforms are smooth at the mound view, cast shadows on the mound intact
(`recomp-work/captures/mtrans/r5/mound_*.png`; compare `captures/mtrans/t_08.png`, key off). fps not
measured (startup key, needs paired launches).
