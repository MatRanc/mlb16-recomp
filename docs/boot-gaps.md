# Boot and match-load gaps: the DXT lift already removed C6, and the shader cache never hit

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two automated boot -> pitch-meter launches (the
hard cap), X once per launch: `ab_live_bootgap_off_20261105_073251` and
`ab_live_bootgap_on_20261105_073812`. Both used the standard `ab_live.sh`
shape (`N=1 SEG=10 DROP=0 METER=1 PIN=0 VIDEODRIVER=x11 BOOT=40`), the
hitch recorder at **100 ms** and `PS3RECOMP_PIPE_TIMING=1`. The key was off
in the first launch and `PS3RECOMP_SHADER_CACHE_STABLE_KEY=1` in the second.
The binary was rebuilt between the two launches, so the second launch also
proves the key survives a build-stamp change. Clean room: the EBOOT, this
repo's runtime, the existing logs/timelines and the game's own files only.
No RPCS3 or other GPL emulator source was read or cited.

## TL;DR

* **C6 is stale.** The 237-654 ms boot gaps in
  [`hitch-census.md`](hitch-census.md) §4 were the **interpreted DXT
  kernel** on the four `Wws_Job/DXTJob` lanes, not a runtime wait. The lift
  in [`dxt-native-step3.md`](dxt-native-step3.md) (750cb5e9, default on)
  removed them: A/B at the start screen went from 11 flip gaps > 250 ms
  (7,623 ms, max 1,055 ms) to one 308 ms gap. Post-lift runs have **no
  237-654 ms asset-loading gap**.
* **The boot gaps that remain are the Bink intro.** 100-132 ms frames at
  16-28 s: the six `spurs pm` lanes are idle (`spu.run` 2-3 ms per window),
  the walker is idle, the display thread waits on its flip lwcond, and the
  main PPU polls `sys_timer_usleep(400)` at guest LR `0x00EE93E4` (Bink
  SDK) 200-273 times per frame. The Bink SPU task (image 5) runs on its own
  host thread in 28-32 ms rounds. This is decode compute, not a cheap
  runtime wait.
* **A real cheap bug is in the match load.** `PS3RECOMP_SHADER_DISK_CACHE`
  is on in the play scripts, but the cache **never hit**: the HLSL hash is
  seeded with `kTranslatorBuild = __DATE__ " " __TIME__`
  (`libs/video/rsx_shader_spirv.cpp:144`, `:266`), so every rebuild
  invalidates every file. Both recent cold runs report **0 hits, 856
  misses, 856 rejected**. The fix is an opt-in stable key
  (`PS3RECOMP_SHADER_CACHE_STABLE_KEY=1`, default **off**).
* **Measured match load:** 4,341 ms (key off, 0 disk hits) -> **1,239 ms**
  (key on, 862 disk hits, 0 glslang). The shader work inside the load frame
  falls from 3,292 ms to 36 ms. Boot -> meter: 167 s -> 152 s, with the
  caveat that the key-off run also contains an unrelated 6.8 s
  present/compositor stall.
* **Tests:** `test_shader_spirv` 67/67 pass, including the new
  old-build-file, version-bump and HLSL-length cases.

## 1. The census, from the existing logs first

### 1.1 What the old census found, and what replaced it

| run | date | boot gaps >= 250 ms | cause |
|---|---|---:|---|
| `ab_live_hitchcold2_20261103_155504` | 11-03 15:55 | 18 frames 353-589 ms | interpreted DXT on 4 SPU lanes |
| `ab_live_dxtliftA_20261103_225911` (lift off) | 11-03 22:59 | 11 gaps > 250 ms, 7,623 ms, max 1,055 ms | interpreted DXT |
| `ab_live_dxtliftB_20261103_230713` (lift on) | 11-03 23:07 | 1 gap 308 ms | none |
| `ab_live_bootgap_off_20261105_073251` | 11-05 07:32 | 0 (boot) | Bink intro 100-132 ms; one 6.8 s present stall |

The old `boot-spu-hitches.md` §1.2/§1.3 attribution is correct for its
build: the PPU polled `sys_timer_usleep(200)` in the title's job-chain loop
and the four DXT lanes ran interpreted code. The lift changed the second
half; the remaining poll in the intro is the Bink SDK's 400 us loop, not
the job chain.

### 1.2 Post-lift boot gaps (existing runs, `[fliplog]`)

Scanning the recent `recomp-work/*.log` flip logs (the runtime prints one
`[fliplog]` per presented frame), every post-lift boot window has the same
shape: a 247-354 ms startup gap, then 100-160 ms intro frames, then no
asset-loading gap. The largest non-intro boot gap in the recent runs is
315 ms (`ab_live_pipeasync_on2`), at flip 15, before the intro.

| run | boot gap count >= 100 ms | max boot gap | match load |
|---|---:|---:|---:|
| `pipeasync_off` 06:12 | 13 intro + 1 startup | 289 ms | 4,727 ms (cold) |
| `pipeasync_on3` 07:07 | 13 intro + 2 startup | 270 ms | 895 ms (warm shader cache) |
| `lanes1` 07:11 | 8 intro + 1 startup | 296 ms | 4,421 ms (cold) |
| this run, key off | 16 intro + 1 startup | 294 ms | 4,341 ms |
| this run, key on | 14 intro + 1 startup | 294 ms | 1,239 ms |

The intro gaps are the same before and after the cache key: they are not
shader work.

### 1.3 What each side waits on in a boot intro gap

Representative 130 ms frame (key-off timeline, t = 17.12 s):

| thread / event | clipped in the frame |
|---|---:|
| `ppu DisplayFlip` `ppu.lwcond` | 130.0 ms |
| `rsx walker` `rsx.idle_wait` / `rsx.put_wait` | 128.2 / 129.5 ms |
| `host thread 8` (main PPU) `ppu.usleep` | 117.1 ms over 258 calls |
| six `spurs pm` lanes `spu.run` | 2.7 ms total (lane union / 6) |
| `spu.policy` | 2.7 ms |
| `rsx.drain` | 0.9 ms |
| `fs.read` | 0 |

The `ppu.usleep` spans are `a = usec`, `b = guest LR`
(`include/ps3emu/ps3_trace.h:105`); all 3,888 calls in 15.5-27 s are
**400 us** and LR **`0x00EE93E4`**, inside the statically linked Bink SDK
(the same region as the Bink IO wrappers in
[`bink-video.md`](bink-video.md) §2). The `spu_workload` log shows the
Bink task (`taskset 0x0147ED00`, image 5) entering `WAIT_SIGNAL` after
28-32 ms rounds. So the six general SPU lanes are idle, but the movie's
own SPU task is busy on a host thread; the PPU is in the SDK's poll, not
in a runtime sleep/timeout we could shorten. This is the Bink decode
cadence: the BIK header says 578 frames at 30 fps (1280x720), and the
100-132 ms frames are the decode rounds, not a fixed sleep.

### 1.4 The 6.8 s gap is presentation, not boot work

In the key-off run, flip log n=1862 (`07:33:34`) shows
`pres_to_show = 6,786,641 us` while `req_to_fin = 0` and
`fin_to_pres = 978 us`: the game presented frames on time and the
compositor did not show them for 6.8 s. The 100 ms hitch recorder has no
frame at 43 s because the guest frame boundaries were fine. This is the
`vkQueuePresentKHR`/X11 class in [`present-stalls.md`](present-stalls.md)
§1 class 3b, not SPU/PPU work. It is run-to-run variable (absent in the
key-on run) and must not be counted as a boot asset gap.

## 2. The match-load gap and the cache bug

### 2.1 The load frame

Both runs use the same shape; the only intended difference is the cache
key. The load frame is the first large frame after the options screen is
confirmed.

| metric (load frame) | key off | key on |
|---|---:|---:|
| flip gap (`show_gap`) | **4,341.2 ms** | **1,238.9 ms** |
| timeline hitch | 4,339.2 ms | 1,159.1 ms |
| `shader.compile` inside | **3,292.5 ms** (356 spans) | **36.4 ms** |
| `rsx.drain` | 3,945.6 ms | 739.5 ms |
| `pipe.create` | 31.1 ms | 37.1 ms |
| `tex.upload` | 363.3 ms | 371.6 ms |
| `spu.run` (lane union / 6) | 3,175.2 ms | 2,471.0 ms |
| `c690.job` | 291.7 ms | 280.3 ms |
| disk cache | **0 hits / 856 misses / 856 stores / 856 rejected** | **862 disk hits / 0 glslang / 0 mem** |

The remaining 1.16-1.24 s is the game's own load work: SPU asset jobs,
texture uploads and c690, not shader translation. The `[pipe-timing]`
lines confirm the source: key off has 213 `src=glslang` and 643 `src=mem`
(no `src=disk`); key on has **862 `src=disk` and zero `glslang`/`mem`**.

### 2.2 The bug

`PS3RECOMP_SHADER_DISK_CACHE` stores one SPIR-V file per `(guest key,
stage)` (`libs/video/rsx_shader_spirv.cpp:415-540`). The header carries the
translator version, the glslang version, the guest key, the HLSL length and
the HLSL hash. The hash is seeded with `kTranslatorBuild`:

```c
/* rsx_shader_spirv.cpp:144 */
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;
/* rsx_shader_spirv.cpp:266 */
const u64 seed = fnv1a64(kTranslatorBuild, sizeof kTranslatorBuild, ...);
return fnv1a64(hlsl, strlen(hlsl), seed);
```

`__DATE__`/`__TIME__` change on **every build of this translation unit**.
`disk_load` compares the hash words (`:468-472`), so after any rebuild every
file is `rejected`, even when the translator, glslang version, guest key and
HLSL are identical. The files are then overwritten with the new stamp, so
the cache is always one build deep. The stats line proves it:

```
07:36:10 [shader-cache] .../shader_cache: 0 hits, 856 misses, 856 stores,
         856 rejected (translator v1)
```

The directory had 2,684 files (48 MB) before the run; they were all
rejected. This is a wrong-looking number (a full cache with zero hits) and
it is a cheap key bug, not translation work.

### 2.3 The fix (default off)

`PS3RECOMP_SHADER_CACHE_STABLE_KEY=1` (`rsx_shader_spirv.cpp:399`) makes
the cache:

* hash the HLSL without the build stamp (`hlsl_hash_stable`, `:275`);
* on load, compare magic/format/translator/glslang/stage/guest key/HLSL
  length (`:468-472`) and accept a file whose hash was written by an older
  build;
* still reject a translator-version bump, a glslang change, a different
  guest key, a different HLSL length, a bad checksum or trailing bytes.

The normal path is unchanged when the key is unset/0. The startup line says
`(translator v1, stable key)` so a run is self-identifying.

`RSX_SHADER_TRANSLATOR_VERSION` remains the invalidation contract (bump it
when the HLSL/decompiler/binding changes). The stable key deliberately
trades the build-stamp belt-and-suspenders for cross-build hits; the guest
key plus the translator version and the HLSL length are the safety net.

### 2.4 Tests

`libs/video/tests/test_shader_spirv.c` gained a build-stamp case: seed a
file with the normal key, corrupt its stored hash to simulate a file from
an older build, then check that the normal key rejects it and the stable
key accepts it with the same words. It also checks that a translator
version bump and a wrong HLSL length still miss with the stable key.
`ninja test_shader_spirv && ./ps3recomp_sdk/test_shader_spirv`:
**67 passed, 0 failed**.

## 3. The two launches

| | key off | key on |
|---|---|---|
| run | `ab_live_bootgap_off_20261105_073251` | `ab_live_bootgap_on_20261105_073812` |
| binary | built 07:31 | rebuilt 07:37 (new build stamp) |
| timeline | `captures/bootgaps_off_20261105_073251.pstl` (26.8 MB) | `captures/bootgaps_on_20261105_073812.pstl` (23.3 MB) |
| boot -> intro open | 12.0 s | 12.0 s |
| nav start (harness `BOOT=40`) | 40.0 s | 40.0 s |
| options confirmed / load start | 132.0 s | 124.0 s |
| pitch reached | 165 s | 150 s |
| meter confirmed (X once) | 167 s | 152 s |
| gaps >= 100 ms, whole run | 57 (19.2 s) | 50 (7.1 s) |
| boot intro gaps (16-28 s) | 16, 101-129 ms | 14, 100-132 ms |
| startup gap | 253 ms | 294 ms |
| present/compositor stall | 6,802 ms | none |
| match load | **4,341 ms** | **1,239 ms** |
| shader.compile in the whole run | 4,154.7 ms | 36.4 ms |

The key-off run's 167 s boot -> meter includes the 6.8 s compositor stall;
excluding it the path is 160 s, so the cache key accounts for about 8 s of
the 15 s difference, and the rest is the load/cinematic shader work it
removes. The boot-to-menu time (launch -> first menu screen) is unchanged
by the key: both runs spend the same 12 s to the intro and the same
100-132 ms intro frames. The intro is Bink decode, not shader work.

## 4. What this means for the backlog

* Close **C6** as already fixed by the DXT lift (750cb5e9). Do not spend a
  run on `PS3RECOMP_SPURS_MAXC=6`; the lanes are not the problem anymore.
* The intro's 100-132 ms frames are Bink decode on one SPU task. A faster
  decode means SPU codegen or a native Bink path, not a runtime wait. The
  `sys_timer_usleep(400)` loop at `0x00EE93E4` is the SDK's poll; leave it.
* Keep the shader-cache stable key as the match-load lever until the
  first-use translation is cached by construction. With it, a rebuilt
  binary still hits the files a previous build wrote; the load drops from
  4.34 s to 1.24 s and the post-load cinematic shader hitches (765/946 ms)
  become 211/305 ms.
* The 6.8 s `pres_to_show` stall is the separate present/compositor class
  in [`present-stalls.md`](present-stalls.md), not part of this mission.

## 5. Caveats

* Two launches, no within-run A/B. The key-off and key-on runs used
  different nav paths (the key-off run reached the options screen 8 s
  later), so the boot -> meter comparison includes path variation; the
  load-frame comparison is the controlled number (same options -> pitch
  transition, same binary source, only the build stamp and the key differ).
* The key-on run was deliberately made after a rebuild, to show that the
  stable key hits files written by the previous build. A second key-off run
  with the same binary would also be warm; that is the point of the bug:
  the key-off cache is warm only until the next rebuild.
* The stable key accepts an old-build file by design when the translator
  version, glslang, guest key and HLSL length agree. If the translator
  changes without a `RSX_SHADER_TRANSLATOR_VERSION` bump, the stable key
  can serve stale SPIR-V. That is why it is opt-in and default off.
* The 100 ms hitch recorder keeps whole windows; the intro gaps are below
  the old 250 ms threshold, which is why the old census did not show them
  as a separate class.
* The present/compositor stall is visible only in `[fliplog]`
  (`pres_to_show`), not as a guest frame, so it is not in the hitch table.

## 6. Reproduction

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
ninja -j8 MyGameRecomp test_shader_spirv && ./ps3recomp_sdk/test_shader_spirv

# key off (cold after a rebuild)
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=bootgap_off N=1 SEG=10 DROP=0 METER=1 PIN=0 VIDEODRIVER=x11 BOOT=40 \
  PS3RECOMP_PIPE_TIMING=1 PS3RECOMP_TIMELINE=../captures/bootgaps_off.pstl \
  PS3RECOMP_TIMELINE_HITCH_US=100000 PS3RECOMP_TIMELINE_RING_MB=32 \
  ./ab_live.sh

# rebuild (changes the build stamp), then key on
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja -j8 MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=bootgap_on N=1 SEG=10 DROP=0 METER=1 PIN=0 VIDEODRIVER=x11 BOOT=40 \
  PS3RECOMP_PIPE_TIMING=1 PS3RECOMP_SHADER_CACHE_STABLE_KEY=1 \
  PS3RECOMP_TIMELINE=../captures/bootgaps_on.pstl \
  PS3RECOMP_TIMELINE_HITCH_US=100000 PS3RECOMP_TIMELINE_RING_MB=32 \
  ./ab_live.sh
```

Then `python3 mlb16-recomp/tools/hitch_table.py <timeline> --top 30` and
the `[fliplog]`/`[shader-cache]`/`[pipe-timing]` lines in the two logs.

## 7. Artifacts

* Code: `ps3recomp-play` branch `exp/live-ab` (pushed to `fork` only),
  `libs/video/rsx_shader_spirv.cpp` and
  `libs/video/tests/test_shader_spirv.c`.
* Timelines: `recomp-work/captures/bootgaps_off_20261105_073251.pstl`,
  `recomp-work/captures/bootgaps_on_20261105_073812.pstl`.
* Logs: `recomp-work/ab_live_bootgap_off_20261105_073251.log`,
  `recomp-work/ab_live_bootgap_on_20261105_073812.log`.
* Pre-lift evidence: `docs/boot-spu-hitches.md`,
  `docs/hitch-census.md`, `docs/dxt-native-step3.md`,
  `docs/shader-disk-cache.md`, `docs/present-stalls.md`.
