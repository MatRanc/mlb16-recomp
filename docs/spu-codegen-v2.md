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
