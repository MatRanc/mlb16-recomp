# Shader disk cache and play-test hitch recorder

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh` / `ab_live.sh`
configuration (`METER=1 SKIP_CINE=1 PIN=1 WCB=2 VIDEODRIVER=x11`, batch-1
switches on, `PS3RECOMP_VK_PACE=1`, `PS3RECOMP_PREPARE_FLIP_RET=1`).

The census in [`hitch-census.md`](hitch-census.md) put 2.95 s of the 5.1 s
match-load frame and 0.83 s of the cinematic hitches in our own HLSL -> SPIR-V
translation (`vk_eng_translate`), while the persistent `VkPipelineCache`
removed only ~0.29 s of driver pipeline creation. This note records the
on-disk cache of the translated SPIR-V, the measurements cold vs warm, the
hitch recorder now enabled for play tests, and the async-compile question.

## TL;DR

* **`PS3RECOMP_SHADER_DISK_CACHE` (default off; on in both
  `play_windowed.sh` copies) caches the SPIR-V by the engine's pipeline
  identity plus a translator version and the glslang version.** Files are
  written atomically; a short, corrupt or version/key/HLSL/checksum
  mismatch is a miss and is rebuilt, never an error. 48/48 unit checks pass.
* **Cold load frame 5,089.6 ms, of which 3,302.0 ms is `shader.compile`.
  Warm load frame 2,056-2,090 ms, of which 22 ms is `shader.compile` in the
  load window** (260.8 ms total in the whole warm run, including one 239 ms
  cinematic first-use frame). The cache removes ~3.0 s, 58 % of the load
  frame, with no visual change.
* The cold run wrote **784 files / 14 MB** (`392` pipelines, VS+FS). The warm
  run added 22 files (11 pipelines) only because its cinematic-skip path
  differed; the load itself was served from disk.
* **Meter screenshots are identical in the meter region** (mean absolute
  difference 0.05-0.33 of 255 between the cold and warm runs); the full-frame
  difference (~4/255) is the animated crowd/players, not a shader change.
* **Hitch recorder (`PS3RECOMP_TIMELINE_HITCH_US=250000`) costs < 1 % at the
  meter** (mean frame time 30.86 ms with it, 30.95 ms without; p50 35.98 vs
  35.93 ms), the rings are fixed at 16 MB/thread, and the file is written next
  to the play output. It is now on in both play scripts; `tools/hitch_table.py`
  turns the file into a hitch table.
* **Async pipeline translation is feasible but not free.** It would need a
  placeholder pipeline, a draw skipped or delayed until the module is ready,
  and a thread-safe glslang instance. The disk cache already removes the
  translation on every warm run, so async compile would only help the first
  run (or a cache invalidation). Not implemented; see §6.

## 1. What was built

### 1.1 Switch and key

`PS3RECOMP_SHADER_DISK_CACHE` (`libs/video/rsx_shader_spirv.cpp`):

* unset/empty/`0`: off (the code default; the play scripts set it to `1`);
* `1`/`on`/`true`/`yes`: `$PS3_GAMEDATA_ROOT/shader_cache`, which is
  `recomp-work/shader_cache` in the play scripts;
* any other value: that directory (created if needed).

The key is the engine's 64-bit pipeline identity computed in
`rsx_draw_engine.c` (`eng_pipeline_get`): the vertex program bytes, the
fragment program's structural hash, the branch/export/cube/vertex-texture/
shadow/gamma/expand/user-clip/unnormalised/WPOS inputs, the input layout, the
render-target format/count and the render state. That is a superset of the
guest microcode plus every input that changes the HLSL and its translation, so
two pipelines with the same shaders but different blend/depth state simply get
separate files. The engine hands it to the backend through the optional
`set_pipeline_key` vtable entry (`rsx_draw_engine.h`), and
`vk_eng_pipeline_create` uses it for both stages
(`rsx_vulkan_backend.c:3990`).

The translator version is `RSX_SHADER_TRANSLATOR_VERSION` in
`rsx_shader_spirv.h:95`; bump it whenever the decompilers' HLSL, the binding
contract, the glslang options in `translate()` or the SPIR-V legalisation
changes. The glslang major/minor/patch is also in every file, so a glslang
update is a miss too.

### 1.2 File format and failure behaviour

One file per `(key, stage)` at `<dir>/<key16>_<stage>.spv`. Header (15 u32):
magic `RSCD`, format, translator version, glslang major/minor/patch, stage,
key (lo/hi), HLSL hash (lo/hi), HLSL length, word count, word checksum
(lo/hi). Then the SPIR-V words.

A load checks every header field, the HLSL hash/length and the word checksum
before copying anything. Any mismatch, a short file, a truncated header or a
zero/huge word count is a **miss**: the file is ignored and rewritten after
the normal translation. Nothing in the cache path can make the process fail.
Stores write `<file>.<pid>.<serial>.tmp` and `rename()` it, so a reader never
sees half a file; a failed write is silent because the translation is already
in hand.

The existing `PS3RECOMP_SPIRV_CACHE` (HLSL-text-keyed) is unchanged and
independent. With both on, the guest-keyed file is checked first; a miss then
goes through the HLSL-text cache and finally glslang.

## 2. Unit test

`test_shader_spirv` now covers the guest-keyed cache as well as the older
HLSL-text cache:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
ninja test_shader_spirv && ./ps3recomp_sdk/test_shader_spirv
# 48 passed, 0 failed
```

The new checks: first use is a miss that stores one file at the expected path;
a second process hits it and returns the same words; another guest key is a
separate miss; a corrupted word (header intact, checksum wrong) and a
truncated file are rejected misses that rebuild; a translator-version bump
rejects the old file, the bumped version hits its rewritten file, and the old
version then misses. No GPU or game is needed.

## 3. Cold vs warm

### 3.1 Runs

| run | folder | cache | recorder | result |
|---|---|---|---|---|
| cold | `ab_live_shadercache_cold_20261103_165656` | empty, then 784 files | on, 250 ms | boot -> load -> skip cinematics -> **pitch meter at 159 s** |
| warm 1 | `ab_live_shadercache_warm_20261103_170051` | 784 files | on, 250 ms | load reached at 127.6 s; the cinematic skip then opened the meter early and the old pitch poll waited for a `pitch_wait` frame that never came (nav miss, see §7) |
| warm 2 | `ab_live_shadercache_warm_off_20261103_170959` | 806 files | **off** | boot -> load -> skip -> **pitch meter at 158 s**; used for the recorder A/B |

The warm 1 run still measured the load (the part this cache targets); the
fixed harness in warm 2 reaches the meter.

### 3.2 The load frame

| metric in the load frame | cold (rec on) | warm 1 (rec on) | warm 2 (rec off) |
|---|---:|---:|---:|
| frame length | **5,089.6 ms** | **2,056.1 ms** | **2,063.8 ms** |
| `shader.compile` | **3,302.0 ms** | **22 ms** | (cache warm) |
| `pipe.create` | 24.1 ms | 18.2 ms | - |
| `tex.upload` | 323.3 ms | 340.4 ms | - |
| `c690.job` (six lanes) | 247.5 ms | 395.7 ms | - |
| `surface.create` | 51.1 ms | 103.3 ms | - |
| `rsx.drain` (parent) | 3,904.1 ms | 1,012.3 ms | - |

Frame lengths are from the `[fliplog] fin=` deltas and the cold/warm 1
timelines agree to 0.1 ms. The warm 1 timeline attributes 260.8 ms of
`shader.compile` to the whole run: 22 ms in the load window plus a 239 ms
first-use frame at 142.7 s (a cinematic pipeline the cold run's skip path did
not create). The cold run's 3,302 ms is all in the load window.

The remaining warm load frame is the title's own SPU asset work (`spu.run`
lane union ~960 ms in that window) and the walker drain, not translation. That
is the same boot/asset lever as the 0.35-0.6 s boot gaps in the census.

## 4. Visual check

The meter was confirmed at 159 s (cold) and 158 s (warm 2). Comparing the
saved meter screenshots (`meter_*.png`, 640x360):

| comparison | mean absolute difference (R,G,B of 255) |
|---|---:|
| meter region (370,95)-(395,120), cold vs warm | 0.05, 0.06, 0.33 |
| whole frame, cold vs warm | 4.00, 4.29, 4.75 |

The meter region is identical to the quantisation of the dump; the whole-frame
difference is the animated crowd/players. There is no shader-cache visual
change.

## 5. Play-test hitch recorder

The census's hitch-only timeline is now part of both `play_windowed.sh`
copies:

```sh
export PS3RECOMP_TIMELINE="$OUTDIR/hitches.pstl"
export PS3RECOMP_TIMELINE_HITCH_US=250000
export PS3RECOMP_TIMELINE_RING_MB=16
export PS3RECOMP_TIMELINE_FLUSH_MB=2
```

Only frames longer than 250 ms are kept, with the spans that ran inside them;
between hitches the flusher drops records, so the rings are fixed and the
file grows with hitches, not with session length. A 208 s cold run with 15
hitches wrote 11.9 MB; the 400 s warm 1 run with the same 15 hitches wrote
9.7 MB. No 30-minute session was run (launch cap), but memory is bounded by
the 16 MB/thread ring and the drop rule by construction.

The meter A/B is warm 2 (recorder off) vs the cold run (recorder on; at the
meter every pipeline it needs is already built, so the cache state does not
matter). MangoHud frame times over the meter window (159-205 s and 158-205 s):

| metric | recorder on (cold) | recorder off (warm 2) |
|---|---:|---:|
| mean frame time | 30.86 ms | 30.95 ms |
| p50 | 35.98 ms | 35.93 ms |
| p99 | 51.34 ms | 49.83 ms |
| frames > 50 ms | 1.7 % | 0.9 % |
| frames > 250 ms | 0 | 0 |

The difference is below run-to-run noise (< 1 %), so the recorder stays on.

`tools/hitch_table.py` reads the PSTL file and prints one row per hitch
(seconds since the recording base, duration, dominant cause and its cost) and
a cause ranking:

```sh
python3 mlb16-recomp/tools/hitch_table.py \
    recomp-work/play_20261103_163747/hitches.pstl --top 40
# or, in the play output folder:
python3 /home/m/Documents/mlb16-online/recomp-work/ab-tools/hitch_table.py hitches.pstl
```

The cold run's table is the same shape as the census: 14 boot hitches of
282-693 ms dominated by `spu.run`, then the 5,089.6 ms load frame dominated by
`rsx.drain` (3,904.1 ms) with `shader.compile` 3,302 ms inside it.

## 6. Async translation for a first-use pipeline (report only)

**Where the walker blocks.** The engine calls `eng_pipeline_get` from the
walker drain (`rsx_draw_engine.c:6350`). On a miss it decompiles both programs
and calls `g.be->pipeline_create` synchronously (`:4551`), which runs glslang
for VS and FS (`rsx_vulkan_backend.c:4088`, `:4095`) and creates the modules.
The handle is then bound and the draw is emitted in the same drain
(`rsx_draw_engine.c:6476`, `:6557`/`:6563`). If `pipeline_create` returns 0,
the engine returns and **drops the draw** (`:6356`).

**What moving it off the walker would require.**

* A placeholder pipeline handle (or an "in progress" result) that the engine
  can cache and bind; the real pipeline is created on a worker and swapped in.
* A rule for the frame(s) before the pipeline is ready: skip the draw (a
  visible pop / missing geometry, as the current translation-failure path
  already does) or delay the draw until the pipeline is ready (needs a
  draw queue and a way to keep the render pass and its resources alive).
* A thread-safe translator. glslang's process state is not documented as
  thread-safe and the current cache serialises every translation under one
  mutex; a worker needs its own instance (or a second process), which is a
  larger change than the cache.
* The pipeline also depends on the render pass/layout chosen from the current
  render state, so the worker must be handed a snapshot of the pipeline key
  and the HLSL, not the live register file.
* Ordering: a deferred pipeline must not be used before its modules exist,
  and releasing the placeholder while a compile is in flight needs lifetime
  tracking.

**What it would buy.** Only the first-ever run, or a run after a translator
version bump / cache deletion: the disk cache already removes the 3.3 s on
every warm run. The cold run's translation is ~8.4 ms mean per shader, so a
worker would overlap it with the title's own asset loading only if the draw
can be skipped for a frame or two; the visible cost is a pop at first use.
The cinematic `c690.job` frames and the remaining warm load (SPU asset jobs)
are not touched by this. **Recommendation: keep translation synchronous for
now; revisit only if a cold-start pop is acceptable or the cache cannot be
shipped.**

## 7. Reproduction

```sh
cd /home/m/Documents/mlb16-online
ninja -C recomp-work/build-play MyGameRecomp

# cold: empty cache, recorder on
rm -rf recomp-work/shader_cache
rm -f /tmp/hitch_cold.pstl
TAG=shadercache_cold N=1 SEG=20 DROP=0 SETTLE=10 LIMIT=900 REPORT=0 \
  METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
  PS3RECOMP_SHADER_DISK_CACHE=1 \
  PS3RECOMP_TIMELINE=/tmp/hitch_cold.pstl \
  PS3RECOMP_TIMELINE_HITCH_US=250000 \
  bash recomp-work/ab-tools/ab_live.sh

# warm: same command with TAG=shadercache_warm and the cache present
python3 recomp-work/ab-tools/hitch_table.py /tmp/hitch_cold.pstl --top 40
```

The play scripts now do the equivalent for a real session: cache on, recorder
on, timeline at `<play output>/hitches.pstl`.

## 8. Caveats and deviations

* Three launches were used: cold, warm 1 (the cinematic skip opened the meter
  early and the old pitch poll missed it; the load was still measured), and
  warm 2 (fixed poll, recorder off). The cap was respected; no fourth launch.
* The warm 1 nav miss is a harness bug, not a cache effect. The pitch poll in
  `ab_live.sh` now also recognises an already-open meter and never presses a
  second X. Warm 2 reaches the meter with that fix.
* The recorder A/B is cold+recorder vs warm+no-recorder, not a same-launch
  toggle (the timeline cannot be toggled live). At the meter the cache state
  is irrelevant (all needed pipelines exist), and the difference is below
  noise.
* No 30-minute session: memory boundedness is argued from the fixed rings and
  the drop rule plus the 208 s/11.9 MB and 400 s/9.7 MB files, not measured
  over 30 minutes.
* The disk cache is keyed by the engine's full pipeline identity, so the same
  shader with different render state is a separate file. This is deliberate:
  it cannot miss an input that changes the HLSL.
* Raw PSTL timelines are in `/tmp`; the derived tables are in the run
  folders. The cache directory is `recomp-work/shader_cache/` and may be
  deleted at any time (the next run rebuilds it).
