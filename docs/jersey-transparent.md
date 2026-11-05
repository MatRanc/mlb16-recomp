# The see-through jersey torso: two dropped character draws, not a texture

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Offline work on the existing `meter_frame` capture,
one pitch-meter A/B (`ab_live.sh`, N=4 x 10 s per arm). No RPCS3 or other GPL
emulator source was read or cited. The MIT NHL Legacy and inFAMOUS 2 notes were
checked first for a similar alpha/render-to-texture fix (section 1).

## TL;DR

* The symptom is **not** a missing/odd texture. The whole-jersey draw is
  **dropped before it reaches the backend**: its fragment program is rejected
  by our decompiler.
* The character fragment programs contain `DDX`/`DDY` followed by
  **`TXD` with a gamma-flagged texture unit**. The decompiler handled neither
  opcode, and for gamma `TXD` it returned `-1`, which fails the whole fragment
  program. The engine then drops every draw through it
  (`rsx_draw_engine.c`, "FAILED (draw dropped)").
* At the pitch meter this is **23 draws per frame** (offline replay: 502
  submitted with the fix off, 525 with it on). That is the missing torso.
* The fix translates `TXD` as `SampleGrad(coord, ddx, ddy)`, implements
  `DDX`/`DDY`, and runs a gamma `TXD` through the existing gamma and
  sign-expand wrappers. It is behind the live key **`PS3RECOMP_FP_TXD`**,
  default **off**.
* Key off, the offline digest is byte-identical to the documented gate
  (`meter_frame` 64 frames: hash `461ff3a834c62331`, gate `089e7b1082b4bcc3`).
  Key on the digest changes and the draw count rises by 23/frame, which is the
  fix. A unit test (`test_fp_decompiler`) covers both arms.
* Live meter A/B: fps **37.88 -> 36.38**, p50 **34.14 -> 34.15 ms**,
  p90 **36.72 -> 38.62**, p99 **39.79 -> 41.78**, walker **7.55 -> 9.60 ms**
  (paired fps -1.50, 95 % CI -3.68..+0.68, N=4). The cost is the 23 draws that
  used to be missing; the bug was hiding ~4 % of the meter's character work.
* The saved per-arm crops are unambiguous: with the key off the pitcher's
  torso is field/background, with the key on it is the white jersey with the
  name and number.

## 1. What was ruled out first

The prior work in this tree already narrowed the field:

* `textures-dummy-census.md`: no texture format/address/size miss at the
  meter; the only dummy fallbacks were cube probes, fixed by
  `cube-rt-and-frames-in-flight.md`. The jersey is not a dummy texture.
* `visuals-wcb-gamma.md`: WCB is the main colour/lighting fix and is on
  (`WCB=2`); it does not bring the jersey back. WCB also does not change a
  draw that was never submitted.

The MIT reference ports were checked for a similar fix:

* NHL Legacy `#212` (the fixed-function alpha test was never applied): our
  engine already appends the alpha test to the fragment program
  (`rsx_fp_apply_alpha_test_buffered`), and the A/B scene does not show an
  alpha-test-only change.
* NHL Legacy `#169` (runtime-composited equipment/name textures never written
  back): a plausible class, but the census resolves every sampled jersey
  texture and the symptom is a whole-mesh hole, not a black/blank material.
* NHL Legacy `#174`/`#103` (alpha-to-coverage on a single-sampled backend):
  the jersey is not an A2C primitive here; the draws are ordinary blended
  opaque character parts.

None of those matched. The answer was in our own decompiler, and the
`[RSX fp] gamma fetch opcode 25 unsupported on unit 4` lines in the play logs
were the fingerprint.

## 2. The evidence: the draws are dropped

### 2.1 Offline replay of the existing meter capture

`replay_rsx_engine` replays `recomp-work/meter_frame` through the shipping
engine with the null backend. With the shader dump on:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
REPLAY_NULL_HASH=0 PS3RECOMP_WALKER_PROF=0 \
  RSX_ENGINE_SHADER_DUMP=$PWD/captures/faildump \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 1 --warmup 0
```

The run prints two failures:

```
[RSX fp] sign-expand fetch opcode 25 unsupported on unit 6 (read unexpanded)
[RSX fp] gamma fetch opcode 25 unsupported on unit 4
[rsx engine] pipeline 47a2efa08d28069d: guest vp 38, fp -1, 101 constants,
             fp 4e49b63161c5f0f7 -> FAILED (draw dropped)
[RSX fp] sign-expand fetch opcode 25 unsupported on unit 6 (read unexpanded)
[RSX fp] gamma fetch opcode 25 unsupported on unit 4
[rsx engine] pipeline db07246e9b1c516a: guest vp 38, fp -1, 101 constants,
             fp b73644bdf2f72eeb -> FAILED (draw dropped)
```

A third failed program (`fp 35eca74f078c08e6`) hit the same gamma-`TXD` path
after the first 32 pipeline log lines. The dumped prefix of each fragment
program ends at the `TXD`:

```
/* TODO: unhandled FP opcode 0x15 (DDX) words 15400680 1C9DC904 ... */
/* TODO: unhandled FP opcode 0x16 (DDY) words 16400680 1C9DC904 ... */
/* TODO: unhandled FP opcode 0x19 (TXD) words 192C0684 1C9D5D0C 00015D08 0001C90C */
```

The last line is the instruction after which the decompiler returned `-1` for
the gamma-`TXD` on unit 4; the program never reaches its colour output, so the
draw is dropped.

### 2.2 What the TXD words say

Decoding the instruction with the documented NV40 layout:

| field | value |
|---|---|
| opcode | `0x19` TXD |
| destination | `h14` (mask `xyzw`) |
| SRC0 | `h3.zwzz` — the texture coordinate |
| SRC1 | `h2.zwzz` — d(coord)/dx |
| SRC2 | `h3.xyzw` — d(coord)/dy (its `.xy` are the derivatives) |

The program computes the derivatives immediately before with `DDX`/`DDY` and
packs them into `.zw` and `.xy`, which matches the NV_fragment_program2
definition of `TXD`: "texture sample with partials", operands
`coord, ddx, ddy`. Both character programs use the same pattern for a normal
map (`EXP_TEX`, units 6/8) and a gamma-flagged colour map (units 4/7).

### 2.3 The fix compiles

The generated HLSL for the real failing program (6320 bytes, 101 constants)
now translates to SPIR-V with glslang (18,050 words, no log output). The
live A/B confirms it on the GPU: the B arm shows the jersey (section 4).

## 3. The fix

All of it is behind the live key **`PS3RECOMP_FP_TXD`** (seeded from the
environment, flippable through `PS3RECOMP_CTL`), default **off**.

* `libs/video/rsx_fp_decompiler.c`:
  * `DDX`/`DDY` emit `ddx(...)`/`ddy(...)`.
  * `TXD` emits `SampleGrad(rsx_samp[u], coord, ddx, ddy)`; cube/volume units
    use `.xyz`, 2D units `.xy`.
  * a gamma `TXD` is no longer rejected; the existing `_rsx_gamma_input`
    wrapper is applied to the sampled result, and the `EXP_TEX` / sign-expand
    wrapper applies to `TXD` like the other fetch ops.
  * key off, every path emits exactly the previous text (the old `TODO`
    comments), so the offline gate is unchanged.
* `libs/video/rsx_draw_engine.c`: the key value is part of the pipeline
  identity, so a live flip builds the new programs instead of reusing the
  cached rejected ones.
* `libs/video/ps3_live_ctl.{c,h}`: new `PS3_LIVE_FP_TXD` /
  `PS3RECOMP_FP_TXD`.
* `libs/video/tests/test_fp_decompiler.c`: key off rejects the gamma `TXD`,
  key on produces `SampleGrad`, `ddx`, and the gamma wrapper (real meter
  instruction words).

The pass-dump "arm at first draw" trigger
(`RSX_ENGINE_PASS_DUMP_NOW=1`) and the per-program failure dump name
(`fail<fp>.ps.hlsl`) are diagnostic additions made while finding this; they
are inert unless the corresponding environment variable is set.

## 4. Live pitch-meter A/B

One launch, pitch meter, X once, `ab_live.sh`, N=4 segments x 10 s per arm,
2 s dropped, WCB=2, PIN=0, x11, the usual play defaults (including
`RSX_CUBE_RT=1`). The control file flipped `PS3RECOMP_FP_TXD`; every switch
logged `[live-ctl] ... PS3RECOMP_FP_TXD=...`.

```
TAG=jerseytxd PITCH_THRESH=35 N=4 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  ARMS='A=PS3RECOMP_FP_TXD=0|B=PS3RECOMP_FP_TXD=1' \
  BOOT_ARM='PS3RECOMP_FP_TXD=0' ./ab_live.sh
```

Log `recomp-work/ab_live_jerseytxd_20261105_001148.log`, marks
`..._20261105_001148.marks`.

| metric | A (off) | B (on) |
|---|---:|---:|
| fps (segments pooled) | **37.88** | **36.38** |
| p50 / p90 / p99 ms | 34.14 / 36.72 / 39.79 | 34.15 / 38.62 / 41.78 |
| std / mean ms | 11.33 / 26.41 | 11.42 / 27.48 |
| walker / wait ms | 7.55 / 15.84 | 9.60 / 16.26 |
| c690 lane jobs/s | 3,034-3,065 | 3,143-3,207 |

Paired per-segment fps differences: -2.76, -0.37, -3.88, +1.00; mean
**-1.50** (sd 2.22, 95 % CI -3.68..+0.68, N=4). The p50 is unchanged (the
meter frame is c690-bound); the B arm pays ~2 ms of walker time for the 23
character draws. The B windows also happened to run slightly more c690 work,
so the fps delta is an upper bound, but the mechanism is direct: the fix
executes draws that were previously dropped, so it can only cost, never gain,
fps.

### 4.1 The pixels

Scene thumbnails are saved every 2 s by the harness. Matched 5 s-into-segment
frames (`A_seg0..3`, `B_seg0..3`) were cropped around the pitcher/batter:

* `recomp-work/visuals/jersey_txd/torso_seg0_A_vs_B.png` (and seg1..3)
* `recomp-work/visuals/jersey_txd/A_seg*_torso_4x.png`,
  `B_seg*_torso_4x.png`

With the key off the pitcher's torso is the field: the green/dirt line runs
straight through where the jersey should be, and only the head/hands are
drawn. With the key on the torso is the white jersey with "PLOUFFE" and "36"
on the back.

Frame statistics (mean absolute difference per channel, 0-255):

| pair | mean abs |
|---|---:|
| A/A, different segments | 3.45 |
| B/B, different segments | 4.32 |
| A/B, matched segments | 3.36 |
| A/B, torso crop only | 5.3-8.9 |

The whole-frame numbers are at the A/A level because the scene animates and
the jersey is white against a pale background; the crop is what shows the fix.

## 5. Exactness and tests

```sh
cd /home/m/Documents/mlb16-online/recomp-work
# key off: the documented digest, unchanged
REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 PS3RECOMP_FP_TXD=0 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 64 --warmup 10
# output hash 461ff3a834c62331, gate 089e7b1082b4bcc3, 502.0 draws/frame

# key on: 23 draws/frame more, a different digest (intended)
REPLAY_NULL_HASH=1 REPLAY_NULL_VSRANGE=1 PS3RECOMP_FP_TXD=1 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame --frames 64 --warmup 10
# output hash 3c3b3b85532a2ff5, gate 0a9220f49d76e843, 525.0 draws/frame
```

* `libs/video/tests/test_fp_decompiler.c`: 92 checks pass, including the new
  `txd_gamma_off_rejects`, `txd_gamma_on_translates`, `txd_sample_grad`,
  `txd_ddx`, `txd_gamma_wrap`.
* `tools/test_video_fixtures.py`: **30/30** when run serially (the same
  fixtures, one at a time). Under the default parallel runner the
  pre-existing timing-sensitive `test_gcm_tick_in_drain` failed on three
  attempts (`flips == before + 2`); it passes standalone and in the serial
  run, and it does not compile any file touched here. This is the flake
  already noted in `walker-tex-cost.md` §7.
* No `spu_context` field was added or moved, so the ABI check is unaffected.

## 6. User command: 5-minute check (key off vs on, live flip)

Pitch meter and a batter walk-up in the same session. The key is live, so one
launch is enough.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
printf '%s\n' PS3RECOMP_FP_TXD=0 > jersey.ctl
PS3RECOMP_CTL=$PWD/jersey.ctl ./play_windowed.sh

# At the pitch screen, press X once to stop the idle cinematics and show the
# meter (never a second time; that starts the throw). Watch the pitcher's
# torso. Then flip the fix on:
printf '%s\n' PS3RECOMP_FP_TXD=1 > jersey.ctl
# The log prints: [live-ctl] flip=... PS3RECOMP_FP_TXD=1 (was 0)

# Later, in a batter walk-up / any player close-up, flip it off and on again:
printf '%s\n' PS3RECOMP_FP_TXD=0 > jersey.ctl
# then
printf '%s\n' PS3RECOMP_FP_TXD=1 > jersey.ctl
```

What to look for:

* **Key off:** the pitcher/batter torso is transparent -- field, dirt or the
  crowd is visible through the shirt; only head, arms and pants are drawn.
* **Key on:** the torso is the team jersey, with its colour and the back
  name/number; the silhouette is solid from belt to shoulders.
* **On:** no new black/white patches, shimmer, missing arms, or z-fighting on
  the character; the rest of the scene is unchanged.
* The live-ctl line names the key and the flip; if it does not appear, the
  control file was not read (check `PS3RECOMP_CTL` and the path).
* Cost: about 1-2 fps at the meter (the 23 restored draws); p50 is unchanged.

## 7. Files

* Code: `libs/video/rsx_fp_decompiler.{c,h}`,
  `libs/video/rsx_draw_engine.c`, `libs/video/ps3_live_ctl.{c,h}`,
  `libs/video/tests/test_fp_decompiler.c` (branch `exp/live-ab`).
* Captures/logs: `recomp-work/ab_live_jerseytxd_20261105_001148*`,
  `recomp-work/visuals/jersey_txd/`, `recomp-work/captures/faildump/`
  (not committed).

## 8. Clean room and attribution

This is our own replay capture, our own runtime and our own EBOOT. The TXD
operand layout was checked against the public NV_fragment_program2
specification and the Mesa/nouveau `nvfx_shader.h` opcode table (MIT-style,
not an emulator); no RPCS3 or other GPL emulator source was read or cited. The
MIT NHL Legacy and inFAMOUS 2 notes were consulted for the alpha /
render-to-texture classes above; no code was copied.
