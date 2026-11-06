# Native c690 Edge, step 3: the live wedge is a missing return PC; the cull family is 0x25/0x26

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Three launches (one wedge attempt, one diagnostic
corpus/shadow run, one live A/B). All runs used the standard
`play_windowed.sh`/`ab_live.sh` environment plus `METER=1 SKIP_CINE=1`,
`PIN=1 WCB=2`, governor `performance`. This is an incremental result: stage 1
is fixed and proved live; stage 2 identifies the cull and builds the offline
harness, but the safe sample-grid fix is **not** landed; stage 3 measured the
native stage as neutral.

## TL;DR

* **Stage 1 is fixed.** The live wedge was not an output mismatch: the
  flattened `0xFDA0` kernel saved the final 128 GPRs and LS, but did not
  publish the return PC / indirect-branch trampoline that the lifted body's
  terminal `SPU_RET` publishes. The corpus and shadow gates compared LS+GPR
  only, so they could not see it. Publishing `ctx->pc = gpr[0]` (and
  `g_spu_trampoline_fn` at `host_depth == 0`) in the live path, plus a pc
  comparison in shadow, fixes it: **130,304 live native calls, 0 fallbacks,
  0 mismatches, no wedge over six 20 s segments.**
* **The holes are the 0x25/0x26 cull, not 0x2C.** `0x2C` is **0.0 %** of the
  command histogram in both the user's 20:26 play test and our 21:26 meter
  run; `0x25` is 3.8 % and `0x26` 1.0-1.2 %. `MLB16_CULL_BYPASS=3` is the
  family to test; `=2` should be a no-op at the pitch.
* **The sample-grid fix is not landed.** We captured 400 cull pairs from the
  meter and built an exact offline replay (the unpatched replay matches the
  game's captured LS output on all 400 pairs). The cull's screen transform is
  a four-float block `{scale_x, offset_x, scale_y, offset_y}` (720p values
  `{595, 640, -335, 360}`, four per-lane copies). Scaling x into sample space
  changes the kept-index list for 35/400 invocations, but it also **drops**
  triangles on 77-104 pairs because the cull has a separate fixed bound.
  That is not a safe fix; the switch is default-off and marked experimental.
* **Speed: native `0xFDA0` is neutral at the meter.** A/B (N=3 x 20 s,
  2 s dropped): A 32.36 fps, B 32.68 fps, paired difference -0.32 +/- 1.97 fps
  (95 % CI covers 0). Per-flip `fin`: p50 34.15 both; p90 41.61 -> 40.95;
  p99 48.36 -> 46.78; std 10.80 -> 10.63 ms. Keep it off by default. The
  stage is safe now, but 1.3x on 31.8 % of c690 is below the frame noise.

## 1. Stage 1: why the live switch still wedged, and the fix

Launch `ab_live_edge3s1_20261103_205745` repeated the step-2 failure: native
live at the meter stopped flipping within ~10 s (`FIFOSTEP io=013AED70
w=213AED70 method=0x0D70 count=78` repeating). A 110 s shadow run in
`ab_live_edge3s2_20261103_210626` reported **no mismatch** (no
`[c690-native] SHADOW MISMATCH`), so the kernel's LS and GPRs were already
exact on the live class.

The difference between shadow and live was the terminal return. The lifted
body ends in `SPU_RET`: it sets `ctx->pc = gpr[0]` and, at `host_depth == 0`,
sets `g_spu_trampoline_fn = spu_indirect_branch`. The native kernel only
stored the final register file; `c690_native_run` restored MXCSR and
returned without touching `pc` or the trampoline. The caller's
`spu_drain_call` then compared a stale `ctx->pc` with its return address.
The shadow comparator checked full LS + all 128 GPRs, but not `pc`, so the
corpus and shadow gates could never catch it.

Fix (`runtime/spu/c690_native_cull.c`):

* live: after `c690_native_stage(ctx)`, set `ctx->pc = ctx->gpr[0]._u32[0]`
  and, at `host_depth == 0`, `g_spu_trampoline_fn = spu_indirect_branch`;
* shadow: compare `t_nat->gpr[0]` with `ctx->pc` and report
  `pc_native`/`pc_lifted` in the mismatch line;
* report progress every 256 shadow/native calls so a killed run still leaves
  counters in the log (the atexit report is lost when the harness kills the
  process).

Evidence (`ab_live_edge3s3_20261103_212337`, six 20 s segments):

| item | value |
|---|---:|
| native calls | **130,304** |
| fallbacks (layout guard) | **0** |
| shadow mismatches | 0 |
| wedge / no-flip segment | none |
| live control self-check | PASS (6/6 bands) |

The fallback count of 0 also proves the live meter class passes the layout
guard; the step-2 worry about an uncaptured layout is not what wedged it.

## 2. Stage 2: which cull, and why the obvious fix is not enough

### 2.1 Which family

The c690 command histogram at the meter and in the user's 20:26 play test:

| opcode | meter 21:26 (our run) | user play 20:26 |
|---:|---:|---:|
| 0x25 (`0x13520` -> `0x132E0`) | 3.7-3.8 % | 3.8 % |
| 0x26 (`0x13518` -> `0x132E0`) | 1.0-1.2 % | 1.0 % |
| 0x2C (`0x12DC0` -> `0x12C08`) | **0.0 %** | **0.0 %** |

So `MLB16_CULL_BYPASS=2` (0x2C only) cannot be the fix at the pitch; the
holes are the 0x25/0x26 screen cull. This matches the user's `=1` result.

### 2.2 Corpus and exact replay

Added a cull spool to `runtime/spu/c690_corpus.c`
(`PS3RECOMP_C690_CULL_PAIRS`, `_STRIDE`, `_GAP_MS`) and reproducible wrappers
around `0x132E0` and `0x12C08` (`tools/c690_ref/patch_cull_hooks.py`). The
meter run captured **400 pairs** (5 `cull_NNN.bin`, all 0x25/0x26) plus
4,000 `0xFDA0` stage pairs.

The offline harness is the existing `mlb16-recomp/tools/replay/
test_job_c690_10f78_replay.c` model with `FF_ENTRY=132E0`; the cull is pure
LS/GPR (0 GET, 0 PUT, 0 channel ops), so a capture replays exactly. For all
400 pairs the replay's LS output equals the game's captured exit image
(**0 differing 16-byte lines**). That is a clean exactness gate for any
native cull work.

### 2.3 The transform block and the negative result

The cull's screen transform is a four-float block:

```
{ scale_x, offset_x, scale_y, offset_y } = { 595.0, 640.0, -335.0, 360.0 }
```

for the 720p pitch camera. It appears in four 16-byte-aligned copies per
lane (e.g. `0x17180`, `0x1F680`, `0x21B90`, `0x2A200`; the base moves by
0x10 with the command buffer alignment). The block is in the job context
pointed to by the command record at LS `0x1F000` (`[r3+0x10] = 0x17000`).

Changing the two x fields changes the cull's kept-index list, so this is the
right lever in principle. The output list pointer/count is at
`r3+0x110`/`r3+0x114` (u16 index list). Scaling both x fields:

| factor | pairs with more kept | fewer kept | same |
|---:|---:|---:|---:|
| 1.25 | 129 | 77 | 194 |
| 1.50 | 139 | 104 | 157 |
| 2.00 | 153 | 92 | 155 |

Scaling only `scale_x` is no better (1.5: 148 more / 80 fewer / 172 same).
At 2.0 the count collapses to 0 on some pairs. The cull therefore has a
separate fixed bound that is not in this block; doubling the transform
pushes geometry past it. A safe fix must either find and scale that bound, or
run the cull at both grids and union the index lists. Neither is landed.

The experimental switch (`PS3RECOMP_C690_CULL_FIX`,
`PS3RECOMP_C690_CULL_FIX_2C`) is in `runtime/spu/c690_cull_fix.c`. It is
**default off** and the header marks it experimental; do not enable it for
normal play.

## 3. Stage 3: speed A/B

Launch `ab_live_edge3s3_20261103_212337`: A = native off, B = native
`0xFDA0` on, N=3 segments x 20 s per arm, 2 s dropped, alternating.
Governor `performance`, standard play environment.

| metric (per-flip `fin`) | A (lifted) | B (native) |
|---|---:|---:|
| samples | 1704 | 1745 |
| p50 | 34.15 ms | 34.15 ms |
| p90 | 41.61 | **40.95** |
| p99 | 48.36 | **46.78** |
| stddev | 10.80 | **10.63** |
| mean | 30.91 | **30.53** |
| fps (MangoHud segment mean) | 32.36 | 32.68 |
| walker own | 18.22 ms | 17.99 ms |
| walker wait | 12.69 ms | 12.38 ms |

Paired A-B difference: **-0.32 +/- 1.97 fps** (95 % CI [-2.29, 1.65]),
A/A self-check PASS. The direction is faintly positive but inside noise, as
the 1.3x stage speedup on a 31.8 % share predicts. No clean winner: keep
the native switch off in both `play_windowed.sh` copies and `ab_live.sh`.

## 4. Play-test command for the holes

The safe diagnostic is the family bypass, not the experimental scale fix:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
# 0x25/0x26 only: expected to remove the pitcher's eye holes
MLB16_CULL_BYPASS=3 ./play_windowed.sh
# control: 0x2C only; expected to change nothing at the pitch
MLB16_CULL_BYPASS=2 ./play_windowed.sh
```

A second pass with the now-safe native stage is useful for a longer soak:

```sh
PS3RECOMP_C690_NATIVE_CULL=1 ./play_windowed.sh
```

Do not set `PS3RECOMP_C690_CULL_FIX=1` for this test; it is not safe yet.

## 5. Method and artifacts

* Runs: `ab_live_edge3s1_20261103_205745` (wedge), `ab_live_edge3s2_
  20261103_210626` (shadow + cull/0xFDA0 corpus), `ab_live_edge3s3_
  20261103_212337` (A/B). Three launches, no launch while the user's game
  was running.
* Corpus: `recomp-work/corpus_edge3s2/` (2.2 GB, game data, not committed;
  4,000 stage pairs and 400 cull pairs).
* Replay: `mlb16-recomp/tools/replay/test_job_c690_10f78_replay.c` with
  `FF_ENTRY=132E0`, `FF_ZERO_GET=1`, built against the generated module and
  the current runtime.
* Repro patch for the generated module:
  `ps3recomp-play/tools/c690_ref/patch_cull_hooks.py` (idempotent; renames
  the generated `0x132E0`/`0x12C08` bodies to `*_body` and inserts the cull
  spool/fix wrapper).

## 6. Deviations from the mission plan

* The live A/B is **N=3 x 20 s** per arm, not N=6 x 10 s: one launch was
  spent proving the wedge fix, and 20 s segments give the same total window
  per arm. The report tool's A/A self-check passes.
* The cull capture runs **after** the corpus trigger at the meter and is
  independent of job sampling; the 4,000 `0xFDA0` stage pairs are from the
  same window. This is a larger corpus than the plan's "20 cull jobs".
* The holes fix is **not** enabled anywhere. The experiment showed the
  transform-only approach is unsafe; shipping it would trade pin-holes for
  missing triangles.

## 7. Blockers / next step

The missing piece is the cull's fixed screen bound (or, equivalently, a
conservative union of the original and sample-grid index lists). The next
step is to locate the bound in `0xD080`/`0xD540` with the same replay harness
(watch the `fcgt`/`selb` threshold around the screen min/max) and re-run the
400-pair gate: accept only a change that never decreases the kept-index count
and adds the triangles that cover a 2x1 sample.

## 8. Licence / clean room

The c690 work is our own reverse engineering. The stage-split/corpus method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. No RPCS3 or other GPL emulator source was read or cited.
