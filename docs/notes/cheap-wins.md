# Cheap wins from the NHL audit, and a faster scripted nav

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play` (reconfigured to the winning build options below).
Meter runs use `METER=1`, `WCB=2`, `CUBE_RT=1`, `PIN=1`, x11,
`PRESENT_NEW_ONLY=1`, `PACE=1`, and the scripted nav. All runs are one game
launch each, on the usual Ryzen 5 2600X / RTX 3060 Ti machine.

This is the implementation and measurement pass for items 1-5 and 7-9 of
[`nhl-lessons.md`](nhl-lessons.md), plus the "skip every pregame cinematic with
X" navigation change.

## TL;DR

* **Nav: boot-to-meter 245 s -> 153-157 s** (about -90 s, -37 %) with
  `SKIP_CINE=1`. X is pressed only on a frame that matches no menu reference and
  is not the pitch camera; the pitch-menu match is still the trigger for the
  one meter X, so a second X is never sent.
* **Live items (SPU MFC memo, NOP-run skip, immediate fence publish, texture
  victim pin): B won by 1.37 fps** (95 % CI -2.66..-0.07) with walker wait
  7.75 -> 6.78 ms. The same-run A/A check is not at the A/A level (the first A
  segment was a 26.4 fps warm-up outlier), so treat the size as 0.9-1.4 fps,
  not as 1.37 exactly. A single-item probe of `SPU_MFC_LOOKUP_CACHE` alone was
  inside noise (+0.24 fps, CI -1.22..+1.69).
* **Build items: all-on won both pairings.** 22.35 vs 28.35 fps and 26.76 vs
  28.68 fps; the c690 job wall fell 65.5 -> 48.5 ms and 60.8 -> 47.7 ms, and
  the walker's SPU wait fell 11.6 -> 7.2 ms and 10.9 -> 7.8 ms. The main driver
  is `PS3RECOMP_SPU_FAST_LS`; `-mavx2`/`-mfma` were already implied by
  `-march=native`, but `-ffp-contract=off` was not set for the app's SPU TUs.
* **Correctness:** the AVX2 exactness test is bit-identical over 446.8 M lanes,
  the FAST_LS helper suite is 72/72, the VMX op oracle and an old-vs-new
  accessor fuzz are clean, and the walker replay hash is unchanged
  (`416abee2c5bff47d`, gate `67f03d06377de89b` with the texture pin off and on).
* **Kept:** all four live switches (now exported by `play_windowed.sh`) and the
  all-on build options (now the configuration of `build-play`). No visual
  change in the meter frames.

## What was added

All runtime switches are live keys in `libs/video/ps3_live_ctl.c` and default
off unless noted; `play_windowed.sh` sets the winners to 1.

| switch | item | what it does |
|---|---|---|
| `SPU_MFC_LOOKUP_CACHE=1` | 1 | Per-host-thread MFC engine memo (the existing code was default-off and env-only; it is now a live key read on each MFC access). |
| `GCM_NOP_SKIP=1` | 4 | Skips runs of NV4097 `NO_OPERATION` (`0x0100`, count 0). Zero runs were already skipped; the method decodes to nothing here, so the whole run is stepped over with the same trace-ring/park bookkeeping. Never active while a walker capture is recording. |
| `GCM_REF_IMMEDIATE=1` | 5 | On `SET_REFERENCE`, drains the fence queue and writes `ctrl->ref` immediately instead of one value per drain tail. The queue is kept, so publication order cannot regress. |
| `PS3RECOMP_RSX_TEX_PIN=1` | 7 | Texture victim rule: never evict an entry whose `last_use_serial` is from this frame; if every entry is pinned, fall back to LRU. |
| `PS3RECOMP_SPU_FAST_LS=ON` | 2 | Existing option, now the build config. Vector local-store access and no LS diagnostics (`-mssse3`). |
| `PS3RECOMP_SPU_AVX2=ON` | 3 | New option: `-mavx2 -mfma -ffp-contract=off` for the lifted SPU TUs. |
| `PS3RECOMP_PPU_X86_64_V3=ON` | 8 | New option, a deliberate no-op under `-march=native` (see below). |
| `PS3RECOMP_VMX_FAST_ACCESSORS=ON` | 9 | New option: `memcpy` + `__builtin_bswap*` for `vmx_h/w/sh/sw` instead of a byte at a time. |

`-march=native` was already in `build-play`. That makes the audit's items 3
and 8 mostly or wholly redundant, which is why the options are written to say
so rather than to look like new wins:

* **Item 2 is not a no-op.** `-mssse3` is implied by native, but the
  `SPU_LIFTED_FAST_LS` define changes the generated code, so the option still
  selects a different path. It was already ON in this build tree.
* **Item 3 is half a no-op.** `-mavx2` and `-mfma` are implied by native (the
  AVX2 blocks in `spu_helpers.h` were already compiled). `-ffp-contract=off`
  was *not* set for the app's SPU TUs, so the new option's only real effect is
  that flag. The exactness test was rerun with it.
* **Item 8 is a no-op here.** Zen+ native is a superset of x86-64-v3, and the
  TLS-guard switch is an MSVC/clang-cl flag with no Linux equivalent. The
  option checks the configured flags and does not override `-march=native`.
* **Item 9 is a real helper change**, verified against the old accessors.

## Part 0: skipping the pregame cinematics

`nav3.py` gained `SKIP_CINE` (default 1, `CINE_GAP=1.6`) and a `--to-pitch`
mode; `ab_live.sh` and `measure_fast.sh` call it after the lineup screen.
The classifier presses X only when the frame matches no menu reference, is not
black, and is not the pitch camera or the meter. `--to-pitch` reports the meter
already up with exit code 10 so the caller does not press X again.

Two false-positive paths were found and fixed while measuring:

* The first version pressed X on the loose pitch-camera match. That X can be
  consumed by the batter-view transition; the pitch menu then appeared with no
  meter. The safe trigger for the one X is the exact `pitch_wait` menu match
  (the pre-existing reference), and that is what `ab_live.sh` uses now.
* If the loose match fires a few seconds before the menu, the skip simply
  stops early; the exact-match poll then catches the menu. Never press on the
  pitch camera or on a meter-looking frame.

| run | launch -> meter |
|---|---:|
| before (2026-11-02 `ab_live_exp_b_cuts`, same nav) | **245 s** |
| `cheap_nav` (first skip check) | 153 s |
| `cheap_live2` | 154 s |
| `cheap_build_base1` / `all1` / `all2` | 154 / 157 / 155 s |
| `cheap_build_base2b` (early loose match, menu waited) | 183 s |

The two base runs with the early match are the reason the final flow waits for
the exact menu even after `--to-pitch` returns: 183 s is what "stop skipping at
a transition frame" costs.

## Part 1: correctness, offline

| check | result |
|---|---|
| `tools/test_ppu_vmx_lanes.py` (10 VMX ops x 300 cases vs the AltiVec model) | pass |
| old-vs-new VMX accessors, 200,000 random 16-byte vectors | 0 differences |
| `test_spu_float_exact` with `-mavx2 -mfma -ffp-contract=off` | 446,840,832 lanes, 0 mismatches |
| `test_spu_helpers` with `-DSPU_LIFTED_FAST_LS -mssse3` | 72 passed, 0 failed |
| `tools/test_template_spu_options.py` | pass |
| `replay_rsx_engine walker_frame` with `PS3RECOMP_RSX_TEX_PIN=0/1` | same `last_hash 416abee2c5bff47d`, `gate 67f03d06377de89b`, 668 draws, 64,128 read hits |

The NOP skip is gated off when `wcap` is active, so the walker capture stream
is byte-identical by construction; the replay above is the texture-pin half of
the same gate. The replay engine is the shipping draw engine with the null
backend.

## Part 2: measurements

### Live A/B at the meter

`ab_live_cheap_live2_20261102_225439`: N=6, 10 s segments, 2 s dropped per
switch (6 x 8 s per arm), same binary. A = all four off, B = all four on.
Log: `recomp-work/ab_live_cheap_live2_20261102_225439.log`.

| arm | guest fps | p50 ms | p90 ms | p99 ms | std ms | walker ms | wait ms | main ms | mesh ms | c690 us/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A off | 28.54 +/- 1.22 | 38.3 | 51.1 | 60.1 | 21.0 | 27.33 | 7.75 | 10.15 | 11.93 | 46,910 |
| B on | **29.90 +/- 0.73** | **37.2** | 51.4 | 60.6 | **15.3** | **26.65** | **6.78** | 10.12 | **11.43** | 46,685 |

Paired difference A-B: **-1.37 +/- 1.30 fps**, 95 % CI **[-2.66, -0.07]**.
The report's A/A self-check says "not an A/A level": segment 0 (A) is a 26.4 fps
warm-up outlier; without it A is 28.96 and B 29.90 (+0.94 fps). The table's
frame times are the MangoHud present frametimes over the measured windows; the
guest fps is from the `[flip]` counter.

A first run toggled only `SPU_MFC_LOOKUP_CACHE` (the arm strings were written
on one line, so the control file applied one key): 29.33 +/- 0.88 vs
29.57 +/- 0.77 fps, paired **-0.24 +/- 1.46** (CI covers 0). The memo alone is
inside noise; the bundle's wait reduction comes from the group.

### Build-time A/B

Two binaries, everything else held: `build-play-base` = FAST_LS/AVX2/VMX/V3
off, `build-play-all` = all four on (FAST_FLOAT on in both). One run each,
alternated base, all, base, all, same nav and meter. Each row is the two 10 s
segments of that run (same arm both halves).

| run | binary | guest fps | p50 ms | p90 ms | p99 ms | std ms | walker ms | wait ms | main ms | mesh ms | c690 us/frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base1 | base | 22.35 | 44.3 | 62.8 | 72.0 | 13.5 | 33.18 | 11.57 | 12.84 | 14.26 | 65,456 |
| all1 | all | **28.35** | **37.7** | **52.2** | **61.8** | 27.1* | 28.49 | 7.20 | 11.05 | 12.12 | **48,526** |
| base2b | base | 26.76 | 32.2 | 55.1 | 63.6 | 12.5 | 26.46 | 10.90 | 8.42 | 13.01 | 60,780 |
| all2 | all | **28.68** | 38.3 | **51.3** | **61.5** | 14.0 | 27.04 | 7.83 | 10.14 | 11.99 | **47,734** |

\* all1's first arm contains one ~0.5 s present stall; its second arm has
std 13.6 ms and p99 60.7 ms. The p50/p90/p99 columns are combined over both
arms; the p99 is much less sensitive to that one stall than the std.

Pairing: base1 -> all1 **+6.0 fps**, base2b -> all2 **+1.9 fps**. The all-on
binary wins both times. c690 wall is **-24 %** and **-21 %**, the walker's SPU
wait **-4.4 ms** and **-3.1 ms**, and the walker own time falls with it. base1
is the slower of the two base runs (its walker own is 33.2 ms and c690 65.5 ms
vs 26.5 / 60.8 ms in base2b); even against the faster base, all-on is +1.9 fps
and -21 % c690 wall. The ~1 fps session noise noted in the plan is optimistic
here: the two base runs differ by 4.4 fps, which is why the c690/wait mechanism
numbers matter more than the single fps pair.

The winning build is the all-on configuration; `build-play` was reconfigured
to it (FAST_LS, SPU_AVX2, VMX_FAST_ACCESSORS, PPU_X86_64_V3; the last is a
no-op under native) and rebuilt.

## Frametime PNGs

Each plot is the MangoHud present frametime over the whole run; the tinted
bands are the measured A/B windows (the label in each band is that window's
guest fps).

* `recomp-work/ab_live_cheap_live2_20261102_225439_ft.png` - live A/B. Menus
  sit near 60, the meter bands start at ~160 s. Both arms are bursty
  (p50 ~37-38 ms, p90 ~51, p99 ~60); B's bands read 29-31 fps and its std is
  visibly lower (15.3 vs 21.0 ms).
* `recomp-work/ab_live_cheap_build_base1_20261102_230103_ft.png` - baseline.
  The meter bands are ~45-50 ms (22 fps) with a 128 ms y-range and several
  80-110 ms spikes; p99 72 ms.
* `recomp-work/ab_live_cheap_build_all1_20261102_230411_ft.png` - all-on.
  The meter bands drop to ~35-40 ms (28-29 fps); p50 37.7 ms, p99 61.8 ms.
  One ~0.5 s stall remains at the start of the first arm.
* `recomp-work/ab_live_cheap_build_base2b_20261102_231806_ft.png` - the faster
  baseline run. Meter bands 27 fps, p50 32.2 ms but p99 104 ms: choppier than
  all2 despite the similar p50.
* `recomp-work/ab_live_cheap_build_all2_20261102_231026_ft.png` - all-on.
  Meter bands 29 fps, p50 38.3 ms, p90 51.3 ms, p99 61.5 ms, the tightest p90
  and p99 of the four.

The whole-run `avg fps` printed at the top of each plot includes the menus and
is not the meter number; use the band labels and the table.

Meter screenshots: `ab_live_cheap_build_{base1,all1,base2b,all2}_*/meter_*_1.png`.
They are the same camera, matchup and HUD; the mean absolute difference between
a base and an all meter frame (3-4) is smaller than between the two base runs
(9.7), i.e. pose/crowd timing, not the build. No missing geometry or texture
regression was seen.

## What was kept

* `play_windowed.sh` (both copies, `mlb16-recomp/tools` and `recomp-work`)
  exports `SPU_MFC_LOOKUP_CACHE=1`, `GCM_NOP_SKIP=1`, `GCM_REF_IMMEDIATE=1`,
  `PS3RECOMP_RSX_TEX_PIN=1`; each can be set to 0 in the environment.
* `build-play` is now built with `PS3RECOMP_SPU_FAST_LS=ON`,
  `PS3RECOMP_SPU_AVX2=ON`, `PS3RECOMP_VMX_FAST_ACCESSORS=ON`,
  `PS3RECOMP_PPU_X86_64_V3=ON`, so the winning build is the default for
  `play_windowed.sh` (`BUILD=build-play`).

## What was not done

* The optional second live run toggling one item at a time was not run: the
  planned budget was five game runs plus one retry, and the build-time A/B
  used four of them. The single-item `SPU_MFC_LOOKUP_CACHE` probe from the
  first (malformed-arm) run is reported above instead.
* `PS3RECOMP_SPU_FAST_FLOAT` (audit item 6) was held at its existing ON in every
  arm and is not part of this comparison.
