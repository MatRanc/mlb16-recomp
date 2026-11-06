# SPU lifter register pressure: what is left after `--local-regs` (roundtable item 4)

Date: 2026-11-05. Branch `exp/spu-regs` in `ps3recomp-sync` (off
`exp/wws-native`). Offline only: **no game launch** and no live A/B; no title
lift config changed and no flag enabled by default. This is roundtable item 4
([roundtable-2026-11-05.md](roundtable-2026-11-05.md),
[roundtable-2026-11-05/C_round2.md](roundtable-2026-11-05/C_round2.md)),
following [spu-codegen-audit.md](spu-codegen-audit.md) and
[spu-inline-callees.md](spu-inline-callees.md).

## TL;DR

* **The call-boundary half of the premise is already done.** `--local-regs`
  writes back only registers a path may have written, and reloads after a call
  only what the callee's transitive write set can change
  (`callee_summaries`). `spu-inline-callees.md` then showed that the remaining
  26-37 % GPR/stack share is *intra-function* register pressure, not the
  `srN = spu_u2v(ctx->gpr[N])` reload after every call.
* **New, safe lifter options (both default off):**
  - `--precise-regs` (with `--local-regs`): a dataflow pass loads a register in
    the prologue only if the body may read it before writing it, and reloads
    after a call only registers that are read before being overwritten.
    Write-backs at exits and before syncs stay complete, so the architectural
    register file is exact whenever an interrupt can be taken.
  - `--ib-goto-starts` (with `--ib-known-targets --ib-goto`): a computed
    backward branch may target any function entry within reach, not only an
    entry the image loads with `il`/`ila`. Each candidate is an exact address
    compare; a miss falls through to the resolver as before. This adds the
    `bi $r102` self-back-edge at `0xC0F4` (`goto loc_0000C018`) that
    [spu-codegen-audit.md](spu-codegen-audit.md) §8 step 3 asked for.
* **Static result on the c690 job module (clang -O2, the build-sync flags):**
  `--precise-regs` alone changes the compiled hot functions by **0** x86
  instructions (clang already deletes the dead entry loads and dead reloads;
  source-level prologue loads fall 14,157 -> 7,401 but the object does not
  move). The listed hot functions are unchanged:
  `0x6D68` 554, `0x8160` 1,702, `0xD2B8` 955, `0xAF98` 1,619, `0xE750` 1,222,
  `0x7998` 1,660, `0xD4A8` 1,804. `--ib-goto-starts` adds the loop-edge goto
  (`0xC018` 468 -> 554 x86) and costs +7.1 % module text.
* **Kill criterion reached.** A correct version does **not** cut emitted spill
  traffic by 30 % on the hot functions: the spill instructions are inside one
  function's live set (100+ live vectors against 16 YMM registers), and the
  call-edge traffic was already removed. No live A/B was run.
* **The goto does have a real, measured effect on dispatches.** On four
  captured 0x21 kernel states the new module reaches the identical final
  LS+GPR state with roughly half the host dispatches (146 -> 79, 153 -> 71,
  190 -> 101, 250 -> 187); each removed dispatch is one write-back + tail hop +
  reload. That is a dispatch/control-flow win, not an emitted-spill win, and it
  is left off by default pending a live A/B.

## 1. Why the "per-function read/write summary" is already there

`tools/spu_lifter.py` `localize_registers()` maintains both sides:

* a **may-dirty** forward pass over the plan (labels/gotos) so a sync or an exit
  stores only registers written on some path;
* `callee_summaries()` builds a fixed point `(may-write, returns-through)` over
  direct `brsl` callees and direct tail transfers, and `_lr_sync_changes()`
  reloads only what a call can change. A call whose callee can leave through an
  unknown path reloads everything.

That is exactly the write-set side of the design in the task. It is why the
`26-37 %` figure was never all call edge: on `0xAF98` a `brsl` boundary is
about 2 % of the x86 (spu-inline-callees.md §4).

The **read-set side of a call write-back cannot be narrowed safely.** An SPU
interrupt is taken in `spu_drain_call` after the caller returns and before the
callee runs, and `spu_irq_regs_save()` snapshots `ctx->gpr`. Skipping the store
of a dirty register because "the callee does not read it" would make that
snapshot stale: the handler sees the pre-call value, and the iret restores it.
`--precise-regs` therefore never touches the pre-sync write-back set.

## 2. What `--precise-regs` does (and what it cannot buy)

Implemented in `localize_registers()` / `_lr_live_in()`:

* `_lr_read_mask()` separates a pure line's reads from its single plain
  assignment target.
* A backward liveness pass over the same plan computes, per entry, which locals
  can be read before being defined. A sync defines a local only through its
  reload (the call line writes `ctx->gpr`, not the caller's C local). Write-backs
  count as uses, so a register that may be stored on a path where it was not
  written is still loaded.
* The prologue loads `live_in[0]`; all other used registers are declared but not
  initialised. After a sync only `changes & used & live_in[i+1]` is reloaded.

On the synthetic `tools/test_spu_local_regs.py` programs this cuts the
*executed* reloads by about half, including the interrupt case:

| program / flags | executed reloads, `--local-regs` | with `--precise-regs` | outputs |
|---|---:|---:|---|
| calls, plain | 116 | 43 | identical |
| calls, `--merge-chunks` | 137 | 67 | identical |
| calls, `--merge-chunks --ib-known-targets --ib-goto` | 144 | 67 | identical |
| irq, `--merge-chunks` (interrupt taken on pass 5) | 86 | 41 | identical |

On the real c690 module at `-O2`, clang already removes those dead loads and
reloads, so the object is unchanged (207,802 -> 207,623 x86, 45.8 % spill both).
For a less aggressive compiler or `-O1` the pass would still save source and
executed traffic. It is a correctness-preserving option, not the lever.

## 3. What `--ib-goto-starts` does

`_ib_goto_targets()` only considered entries the image materialises with
`il`/`ila`. The `bi $r102` at `0xC0F4` computes its target arithmetically
(`ilhu` + `rotqby`), so `0xBF00`/`0xC018` were never tested and every iteration
crossed a write-back + drain hop + reload. The new option adds every function
start **at or before** the branch within `+-0x800` (backward only, to keep the
code growth bounded; forward entries already have `--const-bi` /
`--ib-known-targets`). Every candidate is an exact compare guarded by
`SPU_GOTO_OK`, so a miss is the old resolver path.

The c690 lift gains 220 goto sites (590 -> 810) and +7.1 % text. The self-loop
`0xC018 -> 0xC018` now stays in one host frame. `0xBF00` remains a separate
function: it is a chunk/call boundary, so making `0xBF00 <-> 0xC018` one lifted
function is a merge-policy change, not a goto target change, and was not done.

## 4. Measurements

### 4.1 Static (c690 job module, clang 22, `-O2 -march=native -mavx2 -mfma
-mssse3 -ffp-contract=off`, `SPU_FAST_FLOAT`, `SPU_LIFTED_FAST_LS`,
`SPU_WRCH_INLINE`, `SPU_USE_DISPATCH_MT`; c690 flags `--merge-chunks
--ib-known-targets --ib-goto --const-bi --local-regs`)

| lift | x86 | stack mem | GPR-file mem | spill share |
|---|---:|---:|---:|---:|
| baseline | 207,802 | 38,846 | 56,412 | 45.8 % |
| `--precise-regs` | 207,623 | 38,780 | 56,271 | 45.8 % |
| `--ib-goto-starts` | 222,539 | 48,473 | 57,775 | 47.7 % |

Hot functions (`objdump` per symbol), baseline -> `--ib-goto-starts`:

| function | baseline x86 | goto-starts x86 |
|---|---:|---:|
| `0x6D68` SNORM unpack | 554 | 554 |
| `0x8160` | 1,702 | 1,702 |
| `0xD2B8` | 955 | 955 |
| `0xD4A8` skin/transform | 1,804 | 1,856 |
| `0xAF98` normalize | 1,619 | 1,619 |
| `0xE750` | 1,222 | 1,222 |
| `0x7998` | 1,660 | 1,660 |
| `0xC018` loop tail | 468 | 554 (adds the goto; the writeback/reload it bypasses stays as the fallback) |

The spill instructions on the hot functions do not move. This is the audit's
conclusion reproduced on the task's function list: the remaining pressure is
the function's own live set.

### 4.2 Correctness on real inputs

* **0xFDA0 stage corpus** (`recomp-work/corpus_edge1_20261103_012604/stage`,
  four STAGE_IN/STAGE_OUT pairs): baseline, `--precise-regs`,
  `--ib-goto-starts` and both together all replay to `ls_diff=0`,
  `gpr_diff=0`, matching the captured LS and all 128 GPRs.
* **Captured 0x21 kernel states** (`recomp-work/captures/fncap21`): with a
  bounded drain and the captured LS+GPR, the new module reaches the same final
  `pc`, LS hash and GPR hash as baseline on every capture
  (`0x13140`, `0x131A0`, `0xBED0`, `0xB830`, `0x0A730`, `0x10F78`, `0x0C018`),
  while issuing fewer host dispatches where the loop gotos fire:

| capture | baseline dispatches | goto-starts dispatches | final `pc` | LS/GPR |
|---|---:|---:|---|---|
| `0xBED0` | 146 | 79 | `0x131EC` | identical |
| `0xB830` | 153 | 71 | `0x1327C` | identical |
| `0x0A730` | 190 | 101 | `0x132CC` | identical |
| `0x0C018` | 250 | 187 | `0x131EC` | identical |

  Single-shot CPU time on those four: 0.072 -> 0.062, 0.047 -> 0.042,
  0.070 -> 0.063, 0.098 -> 0.091 ms. The harness models channels as inert, so
  the absolute time is not guest time; the dispatch count and the hashes are
  the result.

Tests run: `runtime/spu/tests/run_tests.sh` **11/11**,
`tools/test_spu_local_regs.py` (now also lifts with `--precise-regs`; all
programs and flag sets identical, and the link line gains `c690_corpus.c` /
`dxt_capture.c`, the pre-existing missing stubs),
`tools/test_spu_ib_known_targets.py` (new `--ib-goto-starts` case),
`runtime/spu/tests/run_wws_native_pm_test.sh` (all five claim cases match; its
harness now defines `g_spu_direct` / `g_spu_stack_reset_hint`, which a PM lifted
with `--ib-goto`/`--const-bi` references).

## 5. Kill criterion and the WWS PM probe

The task's gate was >=30 % less emitted spill traffic on the hot functions.
The measured change there is 0 %. The call-edge traffic was already removed by
`--local-regs`, and the intra-function live set is a register-allocation
problem, not a lifter bookkeeping one. **No live A/B was run** and no title lift
flag was enabled.

A side probe of the WWS PM (4.1 lane-ms/frame head+tail) with the job-module
flags (`--merge-chunks --ib-known-targets --ib-goto --const-bi --local-regs`
`--precise-regs`) is not a win either: the native-claim differential still
passes, but the lifted claim measured 160-190 ns baseline vs 168-177 ns
(median, 5 runs each, overlapping) and the PM's static spill share only moves
from GPR-file memory to stack memory. Not adopted.

Next lever, unchanged from the audit: a register-mapped backend or splitting a
long live range (the 5,090-x86 `0xFDA0_body`), or - as a separate, live-gated
experiment - `--ib-goto-starts`, whose measured effect is dispatches, not
spills.

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online
EXTRA=0x8ec4,0x918c,0x91a8,0x9200,0x9df8,0x9e34,0xb238,0xbcb4,0xbcdc,0xbd20,\
0xbdb4,0xbdd8,0xbe20,0xbe5c,0xc70c,0xc74c,0xc764,0xc78c,0xc7a0,0xc7d4,0xc7e8,\
0xc98c,0xd2b8,0xd4a8,0xd7a0,0xd810,0xd878,0xd9d0,0x4b24,0x4b6c,0x5034,0x5318,\
0x57b4,0x6860,0x82fc,0xa8a0,0xc290

# relift one module with the new options (output outside recomp-work/spu_lifted)
LBP_JOBMOD_EXTRA_FUNCS=$EXTRA python3 ps3recomp-sync/tools/lift_jobmods.py \
  --engine-elf EBOOT.elf --out /tmp/opencode/spuregs/c690_new \
  --register /tmp/opencode/spuregs/c690_new/jobmods_register.c \
  --symbol-prefix mlb16 --only-ea 0x1161300 --merge-chunks --ib-known-targets \
  --ib-goto --ib-goto-starts --const-bi --local-regs --precise-regs

# tests
bash ps3recomp-sync/runtime/spu/tests/run_tests.sh
python3 ps3recomp-sync/tools/test_spu_local_regs.py
python3 ps3recomp-sync/tools/test_spu_ib_known_targets.py
```

The 0xFDA0/capture differential harness is scratch (not committed): it loads
`ls.bin` + `gpr.bin`, calls the lifted entry through the module's function
table, drains `g_spu_trampoline_fn`, and compares the final 256 KiB LS + all
128 GPRs (and the `pc`/dispatch count) between two lifted modules.

## 7. Clean room

Own static analysis of our own EBOOT/lifted modules, our own corpus/capture
files, and our own runtime tests. The local-register / callee-summary model
follows the inFAMOUS 2 port (MIT, ThiagoFraga761 and contributors) and the
stage/corpus method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan),
design reference only; no code was copied. No RPCS3 or other GPL emulator
source was read or cited.
