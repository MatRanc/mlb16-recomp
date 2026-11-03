# Backward frames: `cellGcmSetPrepareFlip` returned 0, so every flip named display buffer 0

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. Standard configuration: `play_windowed.sh`
(WCB=2, PIN=1, `PS3RECOMP_RSX_CUBE_RT=1`, `PS3RECOMP_VK_PACE=1`,
`PS3RECOMP_VK_PRESENT_NEW_ONLY=1`, `VK_DRAW_BATCH=1`,
`VK_FRAME_PIPELINE=1`, x11).

## TL;DR

* **The whole frame stepped back, not just animated geometry.** The title
  renders alternately into display buffer 0 (`offset 0x10000`) and buffer 1
  (`offset 0x890000`), but every flip asked for buffer 0. When buffer 0
  still held the frame from two frames ago, the window showed that image
  after already showing the newer buffer-1 frame; the next flip then went
  forward again. The camera, HUD and meter are all in the display buffer, so
  everything regressed together.
* **Cause:** `cellGcmSetPrepareFlip` returned `CELL_OK` (0). The title stores
  that return value in its own flip queue and its display thread later passes
  it, masked to 8 bits, to `cellGcmSetFlipImmediate`. So the flip after a
  frame rendered into buffer 1 still named buffer 0. The real SDK returns the
  buffer id; RPCS3's `gcmSetPrepareFlip` does the same
  (`return not_an_error(id)`).
* **Fix:** return the buffer id (`PS3RECOMP_PREPARE_FLIP_RET`, live key,
  default off, now on in both `play_windowed.sh` copies). One line in
  `cellGcmSys.c`.
* **Backward steps:** present-source serial went backward on **4.71 % of
  presents** without the fix and **0.038 %** with it (one duplicate-path
  event) in the same N=6 live A/B at the pitch meter. Dumped display images:
  **219.9 backward frames per 1000 moving frames** off, **6.2** on (42x
  fewer; the residual are scene cuts).
* **Cost:** none measurable. Guest fps A **30.34 +/- 0.29**, B
  **29.34 +/- 2.38** (B contains one 24.77 fps segment from a resync; its
  other five segments average 30.25), paired A-B **+1.00 fps**, 95 % CI
  **[-1.27, +3.28]**. Present intervals A p50 **35.24 / p99 57.45 ms**, B p50
  **34.91 / p99 61.14 ms**.

## 1. Static: the flip's buffer id comes from the SDK's return value

Ghidra on the EBOOT (`a58eb36f...`):

`Gfx_FrameEnd` (0x00066530) prepares the frame's flip and stores the *return
value* in its flip queue:

```c
uVar15 = cellGcmSetPrepareFlip(&DAT_01244e08, *puVar18 & 0xff);
puVar18 = DAT_01244e10;
*(undefined4 *)(&DAT_01245018 + iVar14) = uVar15;   /* iVar14 = (frame & 3) * 0x10 */
```

`Gfx_DisplayFlip` (0x000692dc) consumes that slot and flips exactly what was
stored:

```c
uVar5 = DAT_011203dc;
iVar3 = (uVar5 & 3) * 0x10;
cellGcmSetFlipImmediate(*(uint *)(&DAT_01245018 + iVar3) & 0xff);
```

Our HLE returned `CELL_OK` (0) on success, so the queue slot was 0 for every
frame and `SetFlipImmediate` always received 0. The title does alternate its
render target (the engine creates and draws into both display surfaces), which
is why half the displayed frames were the older buffer-0 image.

The real SDK returns the display buffer id from `cellGcmSetPrepareFlip`;
RPCS3's `gcmSetPrepareFlip` returns `not_an_error(id)` and the old RPCS3
wrapper stores the same value. Our return value was the only divergence in
this path.

### Instrumentation used to find it

`PS3RECOMP_PRESENT_TRACE=1` (default off), `rsx_draw_engine.c`: one line per
present with the guest flip index, requested buffer id, display offset, source
surface slot, the source surface's last colour-write serial (`wseq`) and a
`back` flag when that serial is older than the previous present's source. It
also keeps a summary (`... stale-source (backward)`). A per-surface
`last_write_serial` is bumped by 3D writes and by 2D transfers into a target;
the extra `color_serial` bump a 2D write makes is trace-only, so the default
path is unchanged.

`PS3RECOMP_PRESENT_DUMP_DIR=<dir>` (default off): a 160x90 grayscale PGM of
every **displayed** present (written after the `PRESENT_NEW_ONLY` gate, so the
files are the display sequence), `present_%05u.pgm`, capped by
`PS3RECOMP_PRESENT_DUMP_MAX`. `[pdump] n= hash=` lines carry a content hash.

The first diagnostic run (`ab_live_backtrace_20261103_125238`, fix off) showed
`SetFlipImmediate(bufferId=0)` 6830 times and never 1, while `SetPrepareFlip`
alternated 0/1 (1981 each): the smoking gun. Its trace reported
**946 stale-source presents of 11100** eng_present calls; a per-flip pass
(first call per flip) still had 176 regressions.

## 2. The fix

`libs/video/cellGcmSys.c`, `cellGcmSetPrepareFlip`:

```c
const int ret_id = gcm_live_get(PS3_LIVE_PREPARE_FLIP_RET,
                                "PS3RECOMP_PREPARE_FLIP_RET");
...
return ret_id ? (s32)bufferId : CELL_OK;
```

It deliberately still does **not** queue a flip into the FIFO. RPCS3 also
emits a flip command there, but this title's display thread performs the flip
through `cellGcmSetFlipImmediate`; queueing here as well would flip every
frame twice. The return value is the only behavioural change.

Switch: live key `PS3RECOMP_PREPARE_FLIP_RET`, seeded from the environment,
default 0 in code. Both `play_windowed.sh` copies now export it as 1; set it
to 0 for the old behaviour. `ab_live.sh` arms can A/B it live.

## 3. Run A: live A/B at the pitch meter (N=6, no dumps)

One launch (`ab_live_fixab_20261103_125855`), X once for the meter, 6 x 10 s
per arm, 2 s dropped, arms switched through `PS3RECOMP_CTL`:

```sh
cd recomp-work/ab-tools
TAG=fixab METER=1 WCB=2 PIN=1 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
ARM_A=$'PS3RECOMP_PREPARE_FLIP_RET=0' \
ARM_B=$'PS3RECOMP_PREPARE_FLIP_RET=1' \
BOOT_ARM=$'PS3RECOMP_PREPARE_FLIP_RET=0' PS3RECOMP_PRESENT_TRACE=1 \
./ab_live.sh
```

The live-ctl self-check passes for all 12 segments.

| arm | guest fps | walker ms | wait ms |
|---|---:|---:|---:|
| A off | **30.34 +/- 0.29** (29.98-30.79) | 18.64 | 14.03 |
| B on | **29.34 +/- 2.38** (24.77-30.77) | 18.74 | 15.55 |

B's standard deviation is one segment: segment 7 ran at 24.77 fps with
23.1 ms wait (the same resync hiccup shows as a 462 ms present interval in
segment 8). The other five B segments are 29.73, 30.35, 30.34, 30.34, 30.07
(mean 30.17). Paired A-B **+1.00 fps**, 95 % CI **[-1.27, +3.28]**.

### Present trace per measured window

| metric | A (off) | B (on) |
|---|---:|---:|
| eng_present calls | 2717 | 2662 |
| stale source (`back=1`) | **128 (4.71 %)** | **1 (0.038 %)** |
| source buf 0 / buf 1 | 2403 / 414 | 1370 / 1292 |

### Display intervals from `[fliplog] show` (measured windows)

| metric | A (off) | B (on) |
|---|---:|---:|
| p50 | 35.24 ms | 34.91 ms |
| p90 | 47.87 ms | 48.22 ms |
| p99 | 57.45 ms | 61.14 ms |
| mean | 34.87 ms | 35.50 ms |
| n | 1240 | 1205 |

The display interval distribution is unchanged within run-to-run noise; the
fix changes *which* buffer a flip names, not how often frames are produced or
presented.

## 4. Runs B/C: image monotonicity on a moving scene

Two launches with `PS3RECOMP_PRESENT_DUMP_DIR` on, dumps from boot through the
pregame cinematics and the pitch screen (`SKIP_CINE=0`, `PRESS_SETTLE=8`), all
arms the same value, so the whole run is one arm:

```sh
TAG=backoff METER=1 SKIP_CINE=0 PRESS_SETTLE=8 WCB=2 PIN=1 N=1 SEG=10 DROP=2 \
VIDEODRIVER=x11 BOOT_ARM=$'PS3RECOMP_PREPARE_FLIP_RET=0' \
ARM_A=$'PS3RECOMP_PREPARE_FLIP_RET=0' ARM_B=$'PS3RECOMP_PREPARE_FLIP_RET=0' \
PS3RECOMP_PRESENT_TRACE=1 \
PS3RECOMP_PRESENT_DUMP_DIR=$W/present_fixoff \
PS3RECOMP_PRESENT_DUMP_MAX=9000 ./ab_live.sh
# TAG=backon with ..._RET=1 -> $W/present_fixon
```

Each dumped frame is reduced to a 9x16 block-mean signature. A frame is
counted as **backward** when it is at least as close to some frame 2-8
back as to the previous frame (best older match < 0.5x the consecutive
difference) and the consecutive difference is >= 0.7 (moving). Static menu
frames and scene cuts therefore do not count.

| run | fix | displayed frames | moving frames | backward | per 1000 moving |
|---|---|---:|---:|---:|---:|
| `ab_live_backoff_20261103_130504` | off | 8686 | 955 | **210** | **219.9** |
| `ab_live_backon_20261103_131023` | on | 8696 | 804 | **5** | **6.2** |

The off run's backward frames peak in the pregame cinematics (79 and 90 per
minute in the two pitch/cinematic minutes); most are *exactly* the frame two
back (dk = 0.00), i.e. a re-shown buffer, which is the double-buffer repeat the
pitch meter description predicted. The on run's five are all at cuts (best
older match not 0.00 except one 7-frame-back title-card repeat). The two runs
also had dumps enabled, which costs fps; their ~27.9 fps is not used for the
A/B.

Example window (frames 5298-5305, the intro logo moving to the left):

* off: 5300 and 5302 are pixel-identical, 5301 between them is a different
  image -> `... 5299, 5300, 5301, 5300, 5303 ...` (backward step).
* on: the same indices advance every frame with no repeat.

Images (PNG, `recomp-work/visuals/backward_frames/`):

| file | what |
|---|---|
| `fixoff_strip_5298_5305_big.png` | off, moving logo; frame 5302 repeats 5300 |
| `fixon_strip_5298_5305_big.png` | on, same phase; monotonic |
| `fixoff_diff_5302_vs_5300.png` | off: 5302 vs 5300 is all black (identical) |
| `fixoff_diff_5302_vs_5301.png` | off: 5302 vs 5301 differs |
| `fixoff_strip_1625_1632_big.png` | off, menu/transition window with a repeat |
| `fixon_strip_1625_1632_big.png` | on, same indices |
| `fixoff_diff_1629_vs_1627.png` | off: 1629 vs 1627 all black |

Raw PGMs: `recomp-work/present_fixoff/present_*.pgm` and
`recomp-work/present_fixon/present_*.pgm` (160x90).

## 5. Reproduce

```sh
# build
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp

# offline fixtures (test_gcm_fifo_user_command fails to link on this checkout
# for pre-existing missing rsx_draw_engine_set_* stubs; the others pass)
cd /home/m/Documents/mlb16-online/ps3recomp-play && python3 tools/test_video_fixtures.py

# A/B (one launch), then the two dump runs above
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=fixab METER=1 WCB=2 PIN=1 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
ARM_A=$'PS3RECOMP_PREPARE_FLIP_RET=0' \
ARM_B=$'PS3RECOMP_PREPARE_FLIP_RET=1' \
BOOT_ARM=$'PS3RECOMP_PREPARE_FLIP_RET=0' PS3RECOMP_PRESENT_TRACE=1 \
./ab_live.sh
```

The frame-signature analyser is a short Python script over the PGMs (9x16
block means, nearest match among the previous 8 frames). It is described
above so it can be re-created without the run-specific scratch file.

## Caveats

* The A/B is one launch, N=6 per arm, with a passing live-ctl self-check; the
  fps drift between runs is ~0.5 fps, and one B segment caught a resync. The
  backward-step effect (4.71 % -> 0.038 %, 42x in the image domain) is far
  larger than the drift.
* The image check samples the whole boot; its "moving" threshold excludes the
  static meter. It can still flag a scene cut where the previous frame is
  closer to an older title card; those are the five residual cases in the on
  run.
* `PS3RECOMP_PRESENT_TRACE` and `PS3RECOMP_PRESENT_DUMP_DIR` are diagnostics,
  default off. The dump holds `present_mu` while it writes, so it perturbs
  timing; do not measure fps with it on.
* The fix is in the HLE return path shared by every title that uses
  `cellGcmSetPrepareFlip`. Returning the buffer id matches the SDK and RPCS3,
  but the switch defaults off for a title that might rely on the old 0; the
  MLB `play_windowed.sh` copies opt in.
