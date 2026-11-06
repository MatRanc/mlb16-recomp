# Native c690 Edge, step 5: the real meter stage order, and why the per-stage SIMD plan is at parity

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh`/`ab_live.sh`
configuration (`PIN=0`, `WCB=2`, x11, the four cheap wins, the shader disk
cache, the hitch recorder, `SPURS_PM_FAST_RESCHED=8`, `CUBE_RT=1`, `PACE=1`)
plus `METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`, governor `performance`. Two
measurement launches, one 90 s meter segment each:

| launch | tag | what it adds |
|---|---|---|
| 1 | `ab_live_stage5meas_20261104_040459` | the 62-entry named-function hot list |
| 2 | `ab_live_stage5meas2_20261104_041417` | per-command inclusive time |

This follows [`c690-profile.md`](c690-profile.md),
[`c690-job-gaps.md`](c690-job-gaps.md),
[`pin-and-burst-path.md`](pin-and-burst-path.md),
[`edge-native-step2.md`](edge-native-step2.md),
[`edge-native-step3.md`](edge-native-step3.md) and
[`edge-native-step4.md`](edge-native-step4.md).

## TL;DR

* **The old stage shares do not describe the meter.** The profile's
  "tree/cull 31.8 %, unpack/pack 23.5 %, skin 13.6 %, normalize 8.2 %" came
  from a gdb sample of a different scene (pitch `job_01`). Measured at the
  meter, the c690 job wall is dominated by the **0x21 emit/ring command
  (42.9 %, 102 us/call)**, then **0x2A assembler (15.7 %, 22.6 us)**, **0x25 +
  0x26 cull (12.7 %, 25.7 / 37.2 us)**, **0x13 copy (9.7 %, 16.2 us)** and
  **0x0A bind/convert (7.5 %, 2.66 us)**. Entry/parse is 5.6 %; every other
  command is below 2 %.
* **The geometry stages are a minority.** `0x0A + 0x2A` (where unpack, skin,
  normalize and pack live) are **23.2 %** of the job wall; adding the cull
  family and the 0x13 copy reaches 45.6 %. The 0x21 emit/ring path alone is
  **42.9 %** and is not one of the named stages.
* **The named-function model still cannot see the biggest chain.** The
  exclusive report names `0xAF98` 6.5 %, `0x10B88` 5.7 %, `0xB268` 2.6 %,
  `0xF520` 2.5 %, `0xC9D0` 1.4 %, `0xA798` 0.9 %, `0xB830` 0.8 %; **75.3 %**
  stays in `other`, because the 0x21/0x2A chains call their leaves through
  plain C calls (`brsl`), not `SPU_DIRECT`.
* **Per-function SIMD rewrites are at parity, not 3x.** The existing offline
  gates already measure `0x7C18` **0.99x** (this session, 8-wide AVX2 kernel),
  `0x6D68` **1.00x** and `0xAF98 + 0xB238` **1.03x** (commit `e3386c5`), and the
  live-validated `0xFDA0` **1.30x** / `0xD080` **1.53x**. The compiler already
  emits the same `vpshufb` / `vpsllvd` / `vfmadd` mix. **No stage reaches 3x.**
* **Even 3x on every geometry stage does not reach 25 ms.** At the measured
  shares, 3x on `0x0A + 0x2A + cull + 0x13` removes ~7.4 ms of the 24.4 ms
  critical-lane busy (35.0 -> ~27.6 ms). The remaining 0x21 emit/ring path is
  the largest term and has no native version. The <= 25 ms target needs a
  different lever (dispatch/overlap or the emit path), not the planned stages.
* **Nothing was implemented, enabled or A/B'd.** The measurement switches are
  kept, default off outside `PS3RECOMP_C690_STATS`; no `spu_context` field was
  added; no game file changed. Two of the three launches were used; the third
  was not spent on a stage the numbers and the existing gates do not support.

## 1. Measurement

### 1.1 What was added

Two measurement-only changes in `runtime/spu/c690_stats.c`:

1. **A 62-entry named hot list.** The meter's 0x0A command path
   (`0x12478 -> 0x10B88 -> 0x7540/0x7AC0/0x7D40/0x7FC0`), the 0x2A assembler
   chain, the skin/normalize variants, the cull and the tree helpers are named
   instead of falling into `other`. Only entries that reach an
   `SPU_DIRECT`/indirect boundary can be charged separately.
2. **Per-command inclusive time.** The handler entry already knows the opcode
   (`s_op_map`). The model charges `now - cmd_start` to the previous command
   when the next handler is entered, and closes the last command at the job
   end. `entry/parse` is the time from the job entry to the first handler.
   The sum-check is exact: **339,647,285 us of command time vs 339,646,527 us
   of job wall (-0.0002 %)** on 2,038,861 jobs.

The per-command timer reuses the `now` the exclusive model already takes, so
it adds no clock call; the named list only changes the report. Both are gated
by `PS3RECOMP_C690_STATS` (already 1 in `ab_live.sh`, not in
`play_windowed.sh`).

### 1.2 Per-command time (meter, 2,038,861 jobs, 274 s cumulative)

| op | handler | commands | share of commands | inclusive us | share of c690 wall | avg us |
|---:|---|---:|---:|---:|---:|---:|
| 0x21 | `0x13080` emit / ring packet | 1,429,093 | 4.6 % | 145,833,807 | **42.9 %** | **102.05** |
| 0x2A | `0x10F78` vertex assembler | 2,377,054 | 7.7 % | 53,604,279 | **15.7 %** | 22.55 |
| 0x13 | `0x11A60` 16-byte copy | 2,038,863 | 6.6 % | 33,031,967 | **9.7 %** | 16.20 |
| 0x25 | `0x13520` screen cull | 1,139,915 | 3.7 % | 29,267,237 | **8.6 %** | 25.67 |
| 0x0A | `0x12478` bind/convert | 9,690,085 | 31.5 % | 25,796,878 | **7.5 %** | 2.66 |
| 0x26 | `0x13518` screen cull | 376,969 | 1.2 % | 14,016,059 | 4.1 % | 37.18 |
| — | entry/parse | — | — | 19,172,450 | 5.6 % | — |
| 0x27 | `0x11790` index copy/size | 1,720,118 | 5.6 % | 5,403,919 | 1.5 % | 3.14 |
| 0x1F | `0x12948` finalize | 2,038,863 | 6.6 % | 4,509,240 | 1.3 % | 2.21 |
| 0x04 | `0x11F58` stream setup | 2,038,863 | 6.6 % | 2,804,279 | 0.8 % | 1.38 |
| 0x0C | `0x12830` index decompress | 1,720,120 | 5.6 % | 2,609,676 | 0.7 % | 1.52 |
| 0x08 | `0x11BF8` stream end | 2,038,863 | 6.6 % | 2,042,210 | 0.6 % | 1.00 |
| 0x07 | `0x11DF0` stream end | 2,038,863 | 6.6 % | 1,429,752 | 0.4 % | 0.70 |
| 0x09 | `0x05000` stream setup | 12,126 | 0.0 % | 125,524 | 0.0 % | 10.35 |
| 0x2C | `0x12DC0` extra cull | 0 | 0.0 % | 0 | 0.0 % | — |

`0x21` is 4.6 % of commands but 42.9 % of the time: one emit/ring call is
~38x the average command. The 0x0A path is the opposite: 31.5 % of commands,
7.5 % of time, 2.66 us each.

### 1.3 Named functions (same run, exclusive-by-entry model)

| LS | us | share | entries |
|---|---:|---:|---:|
| `0xAF98` normalize | 22,320,960 | 6.5 % | 64,614,337 |
| `0x10B88` 0x0A convert | 19,627,548 | 5.7 % | 4,073,333 |
| `0xB268` transform/normalize | 8,921,567 | 2.6 % | 36,509,839 |
| `0xF520` index/stream | 8,652,109 | 2.5 % | 2,075,571 |
| `0x12478` 0x0A handler | 6,078,511 | 1.7 % | 9,690,085 |
| `0xC9D0` transform/normalize | 4,774,374 | 1.4 % | 16,564,018 |
| `0xA798` 3x4 affine | 3,328,832 | 0.9 % | 28,717,274 |
| `0xB830` transform/normalize | 2,795,481 | 0.8 % | 17,446,603 |
| `0x10F78` 0x2A assembler | 1,753,279 | 0.5 % | 2,377,054 |
| `0x12830` index decompress | 1,589,616 | 0.4 % | 1,720,120 |
| other | 255,696,265 | **75.3 %** | 455,332,075 |

The `other` bucket is not idle: it is the 0x21/0x2A/0x13 chains, whose leaves
are reached by plain C calls inside a lifted function. The 0x21 handler
`0x13080` is only 289 ms exclusive (0.09 %), so almost all of its 145.8 s is
in those callees.

## 2. Why the old shares are wrong

`c690-profile.md`'s percentages came from 210 gdb samples of a pitch `job_01`
capture on `build-o2`, not from the meter, and it said so (±2-3 points at 210
samples). The meter's command mix is different: 0x21 emit/ring dominates, and
the 0x0A bind/convert path (31.5 % of commands) is cheap per call. The old
"tree/cull 31.8 %" also does not match the measured cull family: 0x25 + 0x26
are **12.7 %** of the job wall at the meter, and `0x2C` is zero.

The step-2 plan's order (unpack/pack, then skin, then normalize) therefore
optimises the wrong 23 % of the job. The measured order is **0x21 emit/ring,
0x2A assembler, cull, 0x13 copy, 0x0A bind/convert**.

## 3. The per-stage SIMD premise is already falsified offline

All numbers are on this host, against the shipped lifted module, with the
game's own flags (`-O2 -march=native -DSPU_FAST_FLOAT
-DSPU_LIFTED_FAST_LS`):

| stage | native form | measured | source |
|---|---|---:|---|
| `0x7C18` snorm16 pack | 8-wide AVX2 kernel, bit-exact | **0.99x** | `ps3recomp-native/build/test_job_c690_native`, this session |
| `0x6D68` 12/12/11 SNORM unpack | AVX2/FMA kernel, bit-exact | **1.00x** | `tools/replay/native_6d68.c`, commit `e3386c5` |
| `0xAF98 + 0xB238` normalize | AVX2/FMA kernel, bit-exact | **1.03x** | `tools/replay/native_af98_b238.c`, commit `e3386c5` |
| `0xFDA0` tree/cull | flattened C, live-validated | **1.30x** | `edge-native-step2.md` |
| `0xD080` screen cull | flattened C, live-validated | **1.53x** | `edge-native-step4.md` |

The 0x6D68 commit's own conclusion is the point: "the compiler already emits
the same vpshufb/vpsllvd/vfmadd mix. This closes the per-function native path
as a performance lever." The 0x7C18 kernel is the 8-wide form (two iterations
at a time) and is still at parity; the per-call work is dominated by LS
traffic and byte permutation, not by scalar SPU semantics. A bit-exact
hand-written stage is not a 3x lever on this lift.

## 4. What 3x would buy, even if it existed

Baseline from `pin-and-burst-path.md`: a big burst is 35.00 ms wall with
24.39 ms critical-lane busy. Scaling the measured shares:

| target family | share | 3x saving on 24.39 ms | burst wall |
|---|---:|---:|---:|
| 0x0A + 0x2A (unpack/pack/skin/normalize) | 23.2 % | 3.8 ms | ~31.2 ms |
| + cull 0x25/0x26 | +12.7 % | +2.1 ms | ~29.1 ms |
| + 0x13 copy | +9.7 % | +1.6 ms | ~27.6 ms |
| + 0x21 emit/ring | +42.9 % | +7.0 ms | ~20.6 ms |

The <= 25 ms target needs ~10 ms off the critical lane, i.e. 41 % of the
24.39 ms busy. The planned geometry stages are 23.2 % of the job wall, so
even a perfect 3x leaves the burst at ~31 ms. Reaching 25 ms requires the
0x21 emit/ring path (or the dispatch/overlap levers in
`pin-and-burst-path.md`), which the step-2 plan does not cover and which has
no native version. And the measured per-stage speedups are 0.99-1.53x, not
3x, so the realistic stage saving is below the ~1 ms burst noise already seen
in steps 3-4.

## 5. Decision

* **Implement no native stage and run no A/B.** The ordering premise is
  contradicted by the measurement, and the >= 3x premise by the existing
  offline gates. Spending the third launch on a known-parity stage would not
  answer a useful question.
* **Keep the measurement changes.** The 62-entry list and the per-command
  inclusive timer are measurement-only, default off outside
  `PS3RECOMP_C690_STATS`, and cost no extra clock call. They are the tool for
  the next c690 investigation.
* **Nothing is enabled by default.** `PS3RECOMP_C690_NATIVE_CULL`,
  `..._NATIVE_SCREEN`, `..._CULL_FIX`, the SPURS fast keys and the flip keys
  keep their previous defaults. No `spu_context` field was added; the ABI
  check passes.
* **Next lever, if the user wants one.** The measurement says the emit/ring
  path (0x21, 42.9 %) and the 0x2A assembler (15.7 %) are where the meter's
  c690 compute is. A native 0x21 would be a ring/packet writer, not a SIMD
  vertex kernel; the first question is whether its 102 us is LS/packet work or
  a wait. `PS3RECOMP_C690_GAP`/the timeline can answer that without a new
  stage. The alternative remains the dispatch/overlap lever: the burst is 30 %
  inter-job gaps and the frame is producer-bound.

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work
TAG=stage5meas2 N=1 SEG=90 DROP=0 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 PITCH_THRESH=35 LIMIT=500 \
  ARMS='A=' BOOT_ARM='' ./ab-tools/ab_live.sh
# final report: the last [c690-stats] periodic block in ab_live_stage5meas2_*.log
# (opcode histogram + exclusive functions + sum-check)
```

The per-command table is the `op 0x..` block; the named-function table is the
`exclusive functions` block. Both are cumulative from the first c690 job, so
the run also contains the pre-meter pitch frames; the two runs agree on the
shares within a point.

## 7. Clean room and licence

This is our own static analysis of the EBOOT / lifted c690 module and our
runtime's own counters. The stage-split/corpus method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied.
No RPCS3 or other GPL emulator source was read or cited.
