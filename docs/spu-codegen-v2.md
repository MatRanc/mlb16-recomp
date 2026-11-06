# SPU codegen v2: the 3.3x is float semantics, not spills

Date: 2026-11-05. Branch `exp/spu-v2` in `ps3recomp-sync` (off `exp/spu-regs`),
build `recomp-work/build-sync`. Offline only: no live A/B. The title lift
configuration did not change; the new lift flag is off by default. This follows
[spu-codegen-audit.md](spu-codegen-audit.md),
[spu-regs.md](spu-regs.md), [spu-inline-callees.md](spu-inline-callees.md),
[c690-profile.md](c690-profile.md) and
[wws-native-pm.md](wws-native-pm.md) §9.

## TL;DR

* **The previous mission's premise is inverted.** The 3.3x is not intra-function
  spill traffic. On the 266,934-instruction `0xFDA0` stage, **63 % of host time**
  is SPU instruction emulation, of which **39 points are the per-operand
  FTZ/denormal normalisation** and **26 points are `frsqest`/`fi`/`frest`/LUT
  emulation**. Spills are **12.7 %**, local-store access **3.3 %**, shufb **3.7
  %**, branch/dispatch **0.2 %**.
* Weighted over the four float-bound measured kernels (29.0 % of c690 job
  time): local-store 3.8 %, register file/spills 18.8 %, SPU emulation/float
  66.6 %, branch/dispatch 0.4 %. Removing all spills would buy at most 1.23x;
  removing all emulation would buy 3.0x. That is why `--local-regs`,
  `--precise-regs` and `--inline-direct` measured 0 %. (The 0x2A gather write
  `0x7998` is the exception: it is LS/pressure-bound, see its row.)
* **Phase 2 implemented `--spu-ftz-hw`** (default off): the lifted float helpers
  assume the host thread runs with MXCSR FTZ|DAZ, so the hardware flushes
  denormals and only the sign of zero is canonicalised. The runtime enters the
  mode for the duration of a lifted run and restores the caller's MXCSR.
  * `0xFDA0` stage (median of 3): 268.9 / 132.9 / 444.8 / 267.5 us -> 197.4 /
    98.2 / 325.9 / 196.2 us (**1.36x**), LS and all 128 GPRs bit-identical on
    all four corpus pairs.
  * 0x21 loop `0xC018` 1.23x, `0xBED0` 1.25x, `0x131A0` 1.11x, `0x13140` 1.10x,
    0x2A `0x10F78` 1.06x, `0x0A730` 1.11x; every captured LS+GPR hash identical.
  * **Kill criterion not met**: the best hot function is 1.36x, below the 1.5x
    gate, so the flag stays off, no live A/B was run, and no title lift was
    changed.
* **The concrete next step is documented, not shipped**: lazy sign-of-zero
  canonicalisation (canonicalise a float result only before a consumer that can
  observe its sign) has a measured upper bound of **1.54x** on the stage; it
  needs a lifter dataflow pass because raw `-0` results differ in 3-4 GPR bytes.

## Phase 1: class -> share of hot-function time, upper bound if removed

Method: `perf record -e cycles:u` on the real captured input, `perf annotate`
per lifted symbol, every instruction classified by opcode and by the innermost
inlined frame (`llvm-symbolizer --inlines`). Region counts are not used; the
samples are the executed cost. The measured functions and their c690 job shares
from [c690-profile.md](c690-profile.md) §2.2:

| function | job share | share of that function's cycles by class (a-f) | upper bound |
|---|---:|---|---:|
| `0xFDA0` tree/cull (`0xFDA0` stage corpus) | 19.0 % | emulation 72.0 (float-sem 38.9, float-est 26.1, shufb 3.7, rot 2.4, cmp/sel 1.0); spills 12.8; LS 3.3; arith/other 10.7; branch 0.2 | 3.57x |
| `0xAF98` normalise (0x0A730 capture) | 6.2 % | emulation 58.9 (float-sem 31.1, float-est 13.1, shufb 11.4, selb 2.6); spills+regfile 29.8; LS 5.0; arith/other 4.9; branch 0.5 | 2.43x |
| `0xC9D0` transform (0x0BED0 capture) | 1.9 % | emulation 61.5 (float-sem 36.4, float-est 17.4, shufb 5.1, selb 2.2); spills+regfile 25.0; LS 2.8; arith/other 9.7 | 2.60x |
| `0xB830` transform (0x13140 capture) | 1.9 % | emulation 42.5 (shufb 19.5, float-sem 14.2, selb 6.6, rot 2.3); spills+regfile 36.6; LS 5.8; arith/other 13.7; branch 1.2 | 1.74x |
| `0x7998` 0x2A gather write (0x10F78 capture) | 0x2A | LS 21.3 (incl. mask 4.6); spills+regfile 42.2; other 26.4; shufb 4.4; branch 2.4 | 1.72x (pressure) |
| `0xC018` 0x21 loop tail (0x0C018 capture) | in 0x21 | emulation 56.2 (float-sem 40.4, shufb 8.0, float-arith 7.0, rot 2.3); regfile 19.0; spills 11.4; LS 6.4; branch 1.3 | 2.28x |
| **weighted over the four float-bound kernels (29.0 % of job time)** | | **emulation 66.6; spills+regfile 18.8; LS 3.8; arith/other 10.1; branch 0.4** | emulation 2.99x, pressure 1.23x, LS 1.04x |

The classes map to the mission's list as: (a) local-store = `ls`+`ls_mask`+
`ls_mem`; (b) register file = `spill`+`gprfile`; (c) scalar-in-128 is not
isolated as a separate class (it is inside `other`/`spill`; see the Phase 2
design); (d) SPU emulation = float-sem, float-est, shufb, rot, cmp, selb;
(e) branch/dispatch = `branch` (plus the 19 % `gprfile` write-back in the 0x21
loop); (f) channel/MFC is ~0 in these kernels.

The four functions without an existing entry capture are `0x6D68` (7.1 %),
`0xD4A8` (8.1 %), `0x8160` (5.2 %), `0xD2B8` (3.8 %) and `0xE750` (format
setup). The fncap21 set stops after the 0x0A unpack/transform block, and the
edge corpus contains only 0x25/0x26/0x2C cull jobs, so there is no real entry
state for them offline. Their static mix is the same shape (float `fma`,
`cflts`, `shufb`, LS) and the FTZ/DAZ result below applies to every lifted
float op; they are not claimed as measured.

### Why "spills" looked large before

The old 26-37 % figure is a static memory-operand count. Dynamically the
write-back blocks (`ctx->gpr[...] = srN`) sit on interrupt-check lines and are
mostly not taken; a line-level profiler attributes them anyway and overstates
the cost by ~3x. The executed spill share is 12.8-25.8 % depending on the
function, and the hot functions that are float-bound pay less.

## Ablations (upper bounds, never shipped)

| variant | what it removes | `0xFDA0` stage | exact? |
|---|---|---:|---|
| `--spu-ftz-hw` + MXCSR FTZ/DAZ | per-operand FTZ/denormal normalisation | **268.9 -> 197.4 us, 1.36x** | yes, 4/4 corpus pairs, LS+GPR=0 |
| LS mask off (`& ~0xF` only) | the 4 KiB-page address `and` | 270.3 -> 266.1 us, 1.016x | yes on the corpus |
| no result FTZ (unsafe) | result sign-of-zero canonicalisation too | 268.3 -> 175.0 us, **1.54x** | no: 3-4 GPR bytes differ |
| LS byte swap off (unsafe) | the per-access `vpshufb` | LS/GPR differ, path changes | no; perf says 2.6-16.8 % |
| `spu_shufb` -> `rb` (unsafe) | shufb emulation | path changes | no; perf says 3.7-19.5 % |
| `spu__xe_lut` -> AVX2 gather or 4 scalar loads | `frsqest`/`frest` LUT | 0.99-1.00x | yes; neutral on Zen+ |
| `--ib-goto-starts` (prior mission) | 0x21 host dispatches | 250 -> 187 dispatches | yes; dispatch is ~0.5 % of cycles |

The FTZ ablation is the only one that is both large and exact. It is the
implemented Phase 2. The unsafe 1.54x variant is not a candidate because the
register file is architecturally visible; the lazy-canonicalisation design
below is how to recover it exactly.

## The WWS PM head/tail

The PM head/tail is 4.10 lane-ms per lane per frame
([wws-native-pm.md](wws-native-pm.md) §9). Components measured there: lifted
instruction execution 36 us/gap (pm_exit 20.1, head_stage 11.7, tail_submit 4.2
us), synchronous DMA copies 0.50 lane-ms, GETLLAR/PUTLLC 0.30 lane-ms, host
dispatch ~0.6 lane-ms. The lifted PM is float-light and branch/LS-heavy; the
claim microbench (`runtime/spu/tests/run_wws_native_pm_test.sh`) is 160 ns
lifted vs 125 ns native on identical LS/memory. There is no separate 3.3x float
class here: the head/tail is dominated by lifted integer/LS execution and the
job/submit chain, so a native PM exit translation (not FTZ) is its lever, as
already concluded.

## Phase 2: `--spu-ftz-hw`, measured, below the kill gate

Implemented in `ps3recomp-sync`:

* `tools/spu_lifter.py --spu-ftz-hw` (plumbed through `tools/lift_jobmods.py`)
  emits the six SPU float helpers on the assumption that the run has MXCSR
  FTZ|DAZ: plain 128-bit loads (DAZ flushes denormal operands), hardware
  `vaddps`/`vmulps`/`vfmadd*`, and a result canonicalisation
  (`vcmpeqps`/`vandnps` maps -0 to +0). It also emits a weak constructor that
  tells the runtime the module needs the mode; without `__FMA__` it falls back
  to the exact `spu_fa`/... helpers and does not ask for the mode.
* `runtime/spu/spu_channels.c` gains `spu_ftz_hw_enable()` and
  `spu_ftz_mode_enter/leave`: a lifted run sets FTZ|DAZ for its duration and
  restores the caller's MXCSR on exit (the same structure as the arm64
  `SPU_FLOAT_RZ` path), so a synchronous PPU-thread run does not change the
  PPU's floating point.
* Default off: no title lift passes the flag; `build-sync` still uses the
  existing lift.

Measurements (median, 1000 calls, pinned; hashes from the full captured
LS+GPR image):

| workload | old | new | speedup | LS/GPR hash |
|---|---:|---:|---:|---|
| `0xFDA0` sample 0 | 268.9 us | 197.4 us | 1.362x | identical |
| `0xFDA0` sample 5 | 132.9 | 98.2 | 1.354x | identical |
| `0xFDA0` sample 11 | 444.8 | 325.9 | 1.365x | identical |
| `0xFDA0` sample 18 | 267.5 | 196.2 | 1.363x | identical |
| `0x0C018` (250 dispatches) | 57.9 us | 47.2 | 1.225x | identical |
| `0x0BED0` (146) | 35.2 | 28.1 | 1.252x | identical |
| `0x131A0` | 39.1 | 35.3 | 1.108x | identical |
| `0x13140` | 17.7 | 16.1 | 1.098x | identical |
| `0x10F78` (0x2A) | 9.50 | 8.98 | 1.058x | identical |
| `0x0A730` | 35.3 | 31.8 | 1.111x | identical |

Exactness: the four `0xFDA0` corpus pairs are bit-identical in all 256 KiB of LS
and all 128 GPRs; every fncap21 capture above matches its old hash identically;
`runtime/spu/tests/run_tests.sh` is **11/11**;
`tools/test_spu_local_regs.py` and `tools/test_spu_ib_known_targets.py` pass;
`build-sync` builds. The kill gate is 1.5x on the hot functions; the best is
1.36x, so **the flag stays off, no live A/B was run**, and no title lift or
`build-play` build was touched.

## Phase 2 design if the gate is to be reached

1. **Lazy sign-of-zero canonicalisation** (target 1.54x on `0xFDA0`). Emit float
   results raw and track which registers may hold `-0`. Canonicalise only
   before a consumer that can observe the sign (bitwise `selb`/`and`/`or`,
   integer compares, `shufb`/rotates, LS stores) and at write-backs. The
   compiler pass belongs in `localize_registers`/a new dataflow over
   `f.body_lines`, with the same conservative fallback (canonicalise on any
   unknown consumer). The measured 1.54x upper bound comes from removing all
   result canonicalisation; the pass keeps the 3-4 GPR bytes exact.
2. **Scalarisation** (class c): prove which registers only ever hold
   preferred-slot scalars and keep them in 32/64-bit locals. The hot functions
   carry addresses/counters in 128-bit vectors (`sr3`, `sr40`, ...); this is the
   remaining pressure (20 % weighted) and would compose with (1).
3. **Host-order local store** (class a, 4.9 %): store LS in host layout with
   exact conversion at DMA/MFC boundaries. Small on the stage but 21 % of
   `0x7998`; do it only if the DMA boundary plumbing is already in hand.
4. **Cheaper `frsqest`/`fi`** was measured neutral on Zen+ (gather and scalar
   LUT both 0.99-1.00x); do not spend more there without a different table
   formulation.

A realistic combined result of (1)+(2) is 1.4-1.6x on the stage; parity with a
real SPU still needs a register-mapped backend or whole-job native code.

## Reproduce

```sh
cd /home/m/Documents/mlb16-online
EXTRA=0x8ec4,0x918c,0x91a8,0x9200,0x9df8,0x9e34,0xb238,0xbcb4,0xbcdc,0xbd20,\
0xbdb4,0xbdd8,0xbe20,0xbe5c,0xc70c,0xc74c,0xc764,0xc78c,0xc7a0,0xc7d4,0xc7e8,\
0xc98c,0xd2b8,0xd4a8,0xd7a0,0xd810,0xd878,0xd9d0,0x4b24,0x4b6c,0x5034,0x5318,\
0x57b4,0x6860,0x82fc,0xa8a0,0xc290

# FTZ lift (separate output; the play tree is not touched)
LBP_JOBMOD_EXTRA_FUNCS=$EXTRA python3 ps3recomp-sync/tools/lift_jobmods.py \
  --engine-elf EBOOT.elf --out /tmp/spuv2/c690_ftz \
  --register /tmp/spuv2/c690_ftz/jobmods_register.c --symbol-prefix mlb16 \
  --only-ea 0x1161300 --merge-chunks --ib-known-targets --ib-goto --const-bi \
  --local-regs --spu-ftz-hw

# tests
bash ps3recomp-sync/runtime/spu/tests/run_tests.sh
python3 ps3recomp-sync/tools/test_spu_local_regs.py
python3 ps3recomp-sync/tools/test_spu_ib_known_targets.py
```

The per-function harness is scratch (not committed): it links the generated
module against `spu_context.h`, runs a captured `ls.bin`+`gpr.bin` entry, drains
`g_spu_trampoline_fn`, and compares the final 256 KiB LS + all 128 GPRs. The
FTZ run sets MXCSR FTZ|DAZ in the harness because the runtime wiring is not
enabled in the offline harness.

## Clean room

Own static analysis of our own EBOOT/lifted modules, own corpus and fncap
captures, own runtime tests and perf counters. The stage/corpus method follows
NHL Legacy Native (MIT, (c) 2026 HeckYouMan); the local-register/callee model
follows the inFAMOUS 2 port (MIT, ThiagoFraga761 and contributors); design
reference only, no code copied. No RPCS3 or other GPL emulator source was read
or cited.

---

# v3 (2026-11-05): FTZ live-switchable, measured at the meter

Branch `exp/spu-v3` in `ps3recomp-sync` (off `exp/spu-v2`), build
`recomp-work/build-sync`. One automated pitch-meter launch. No title lift
configuration changed; `PS3RECOMP_SPU_FTZ` defaults to 0 and the default arm is
the old module unchanged.

## What shipped: one binary, two variants, one live key

The v2 flag produced a module whose float helpers only make sense while the host
thread runs MXCSR FTZ|DAZ. Shipping that as the only code would have made
`build-play` (which has no FTZ runtime) inexact, and would have made the switch
relaunch-only. v3 instead keeps **both variants in one binary**:

* `tools/spu_ftz_twin.py` derives `<module>_ftz/spu_recomp.c` from the already
  generated, already patched base module: the symbol prefix gains `_ftz`, the
  six helpers `fa/fs/fm/fma/fms/fnms` become the `spu_hw_*` forms, and the
  registration loop calls `spu_register_function_var(addr, fn, 1)`. Control
  flow, native hooks and the cull/Pm patches are inherited byte for byte, so the
  only difference between the arms is the float helper selection. The script
  also patches `jobmods_register.c`, `jobmods_register_dxt.c` and
  `spu_workloads.c` to register the twin under the same image id and, for the
  waypoint images, the twin entry. It is re-runnable and idempotent.
* The runtime (`spu_channels.c`) stores a `variant` on each registry entry.
  `spu_lookup` prefers variant 1 when the current run's sampled key is on and
  otherwise returns the first registered (variant 0) function exactly as before;
  the lookup memo and the dispatch cache include the variant in their key.
  `spu_run_with_halt` samples `PS3RECOMP_SPU_FTZ` once per run (live control file
  or environment), stores it per host thread, sets MXCSR FTZ|DAZ for the run and
  restores the caller's MXCSR and variant on the way out (nested runs restore the
  outer variant too). `spu_workload.c` chooses the twin entry when the key is on.
* `build-play` safety: the twin's registration is behind a **weak**
  `spu_register_function_var` (absent in the play runtime, so the twin registers
  nothing) and the generated workload registration behind a weak
  `spu_workload_register_img_ftz`. With the key off, dispatch never selects
  variant 1, so the play tree is byte-identical in behaviour. All 14 twins were
  syntax-checked against the play tree's headers.

`tools/spu_lifter.py --spu-ftz-hw` now emits the same twin shape (no eager
"enable FTZ" constructor); a fresh twin lift registers itself as variant 1.

## Offline gates

Lifted-old vs lifted-twin, bit-identical in all 256 KiB of LS and all 128 GPRs
(and final pc / dispatch count), replayed with the twin arm under MXCSR FTZ|DAZ:

| input | cases | result |
|---|---:|---|
| 0xFDA0 stage corpus (`corpus_edge1_20261103_012604/stage`) | 4 pairs | identical, and equal to the captured output |
| `fncap21` captures (0x0A730, 0x0B830, 0x0BED0, 0x0C018, 0x10F78, 0x13140, 0x131A0, 0x13518, 0x13520) | 9 | identical |
| DXT job entries (`dxt_capA`/`dxt_capB`, entry 0x4030) | 6 | identical |
| WWS PM claim cases (run on the twin module) | 5 | all PASS; the PM is float-light, this is the native-claim differential, not a full old-vs-twin state diff |
| 0x6D68 (new entry capture, `captures/fncap_v3`) | 1 | identical |
| `runtime/spu/tests/run_tests.sh` | 11 | 11/11 |
| `tools/test_spu_local_regs.py`, `tools/test_spu_ib_known_targets.py` | | pass |
| `check_spu_context_abi.sh` | | pass (no `spu_context` field added) |
| `tools/test_video_fixtures.py` | 34 | 34/34 |

The differential harness is committed: `tools/spu_ftz_diff.py` +
`tools/spu_ftz_diff_main.c` (one executable per arm; the twin arm sets MXCSR).
The 5 workload images and the 6 jobmods with no real entry corpus are the same
mechanical twin (same instruction stream, helper selection only) and are
compile/registration-checked, not dynamically differential-tested.

## Live A/B at the pitch meter (one launch)

`ab_live.sh BUILD=build-sync`, 6 alternating 10 s windows inside one launch
(A = `PS3RECOMP_SPU_FTZ=0`, B = 1), pin 1, WCB 2, meter scene. The live-ctl log
shows the key flipping `0->1->0->1->0->1` during the run and the A/B band
self-check passes.

| arm | segments | fps (mean +/- range) | c690 job time | c690 lane time |
|---|---:|---|---:|---:|
| A, FTZ off | 3 | 34.60 +/- 1.78 (33.90-35.33) | 204.52 us/job | 3124 ms/s |
| B, FTZ on | 3 (2 with periodic sample) | 35.09 +/- 1.73 (34.37-35.76) | **183.07 us/job** | **2926 ms/s** |

* **c690 job wall time falls 1.117x** (204.5 -> 183.1 us/job) and c690 lane time
  falls 6.3 %. This is the FTZ result on real work, measured live: the twin was
  selected (a fallback to variant 0 would have left the job time unchanged).
* **Frame rate is flat within the meter's resolution**: paired A-B -0.49 +/- 2.94
  fps (95 % CI [-3.43, +2.45]). The pitch meter is walker/PPU-bound and the
  walker's c690 wait is only ~2.7 ms/frame, so a 10 % cut of c690 job time is
  mostly hidden. At the pitch and in crowded play the producer is longer
  (`targets-and-backlog.md`), so the same 1.117x should show there.
* SPU-thread CPU from `tools/thread_sched.py` was not obtained: the sampler
  attached to the `timeout` wrapper (the documented pgrep trap) instead of the
  game process, so only the wrapper thread was sampled. The c690 per-job wall
  time above is the per-arm SPU-lane measure that the frame clock actually sees.

User command to try it in play (crowded scenes):

```sh
cd /home/m/Documents/mlb16-online/recomp-work && PS3RECOMP_SPU_FTZ=1 \
  ./build-sync/MyGameRecomp ../EBOOT.elf            # or: play_windowed.sh (env inherited)
# live toggle: echo PS3RECOMP_SPU_FTZ=1 >> $PS3RECOMP_CTL   (0 to undo, no relaunch)
```

## Phase B status

Item 3 was partially captured with the second (and last) automated launch:
`SPU_FN_CAPTURE=0x6D68,0x8160,0xD2B8,0xD4A8,0xE750` at the pitch meter produced a
real entry state for **0x6D68** (differential passes); the other four did not
dispatch through `spu_lifted_lookup` in that window, so they remain unmeasured.
Items 1 (lazy sign-of-zero canonicalisation) and 2 (preferred-slot scalarisation)
are not implemented: both are a new dataflow/representation pass over the lifter,
and the v2 measurement already shows the remaining sign-of-zero work (1.54x
upper bound on 0xFDA0) is smaller than the emulation already removed. Nothing
non-exact was shipped; the key stays off by default.

## Reproduce

```sh
# twins + registrations (idempotent; regenerated from the patched base)
python3 ps3recomp-sync/tools/spu_ftz_twin.py --spu-dir recomp-work/spu_lifted

# exactness (one executable per arm; twin runs MXCSR FTZ|DAZ)
python3 ps3recomp-sync/tools/spu_ftz_diff.py --module jobmod_c690c2f7b09a_eF5B8 \
  --entry 0xFDA0 --inputs 'recomp-work/corpus_edge1_*/stage/job_*_in.bin' --want-suffix _out.bin
python3 ps3recomp-sync/tools/spu_ftz_diff.py --module jobmod_c690c2f7b09a_eF5B8 \
  --capdirs recomp-work/captures/fncap21 --capdirs recomp-work/captures/fncap_v3
python3 ps3recomp-sync/tools/spu_ftz_diff.py --module jobmod_647a2bf49000_e0030 \
  --capdirs /tmp/opencode/dxtcapA

# live A/B (one launch, key alternated in-run)
cd recomp-work/ab-tools
TAG=spuv3 BUILD=build-sync N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=1 \
  ARMS='A=PS3RECOMP_SPU_FTZ=0|B=PS3RECOMP_SPU_FTZ=1' BOOT_ARM='PS3RECOMP_SPU_FTZ=0' \
  ./ab_live.sh
```

Clean room unchanged: our own EBOOT, lifted modules, captures and runtime tests.
The stage/corpus method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan); the
local-register model follows the inFAMOUS 2 port (MIT, ThiagoFraga761 and
contributors). No RPCS3 or other GPL emulator source was read or cited.
