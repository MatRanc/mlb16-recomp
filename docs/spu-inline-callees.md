# SPU inlining direct callees: implemented, measured, no gain

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, flag
`--inline-direct` (default off). Offline only: no game launch, no changes to
`build-play`, no `recomp-work` commit. This follows
[`spu-codegen-audit.md`](spu-codegen-audit.md) §6-8 and
[`targets-and-backlog.md`](targets-and-backlog.md).

## TL;DR

* **The lifter option is implemented**, behind `--inline-direct` (default off),
  and a c690 lift with it compiles. It inlines acyclic `SPU_DIRECT` tail
  targets in tail position, inlines `brsl` callees whose return is a clean
  return through the link they were given, keeps recursion and indirect edges
  on the trampoline, and runs `--local-regs` once over the combined body.
* **The static gate fails.** x86 per SPU did not fall by 10 % on the probes:
  `0xAF98` 2163 -> 2516 (+16 %), `0xA410` 1243 -> 1243 (0 %), `0xFDA0_body`
  5090 -> 5090 (0 %), `0x10F78` 1190 -> 1479 (+24 %). Only the 0x10F78 *path*
  improves once its inlined callee `0x10C70` (1281 x86) is counted.
* **Executed instructions do not move either.** On the four captured
  `0xFDA0` stage pairs the `0xFDA0` corpus runs **810.89 M vs 811.06 M host
  instructions** (+0.02 %, `perf stat instructions:u`, 3 alternating runs) and
  the same cycles; wall time is neutral within run noise (277-281 us, 138-141
  us, 468-469 us, 281-287 us per stage).
* **Why the audit's estimate did not hold.** The 26-37 % GPR/stack share is
  *intra-function* register pressure (100+ live 128-bit values through one
  body), not call-boundary save/reload. A tail transfer must publish the whole
  dirty GPR state at the chain's final exit whether the target is inlined or
  not, so inlining moves stores rather than removing them. A `brsl` boundary
  is only ~2 % of a hot function's x86, and merging bodies enlarges the union
  of live registers, so the compiler spills more.
* **No meter A/B was run.** Gate (a) is the first gate and it failed; the
  measured instruction count says a launch would spend one of the three runs
  to measure noise. `build-play` and the play scripts are untouched.

## 1. What was implemented

`tools/spu_lifter.py --inline-direct` (also plumbed through
`tools/lift_jobmods.py`). Before `--local-regs`:

* a `brsl`/`brasl` call is inlined when the callee (a) is not already on the
  inline stack, (b) makes no call of its own, (c) does not write the link
  register, and (d) returns only through that link (`SPU_RET` / `SPU_RET_REG`);
  every return becomes `goto <continuation>`;
* a direct `SPU_DIRECT` tail is inlined in tail position when its target cannot
  reach the function being expanded (inline-stack cycle check), so guest loops
  keep the musttail/trampoline edge;
* inside an inlined callee, any remaining tail or indirect exit becomes a
  local host dispatch + `spu_drain_call(ctx, ret)` + `goto continuation`, i.e.
  exactly the work the enclosing `brsl` drain did;
* unreachable merged chunks are dropped from the inlined copy (they would
  otherwise add their registers to the caller's prologue);
* `runtime/spu/spu_context.h` gains `SPU_DIRECT_INLINE_OK`, the `SPU_DIRECT`
  fast-path test; the generated source also defines a fallback so it compiles
  against older runtime headers (the generated `spu_recomp.h` includes
  `../../runtime/spu/spu_context.h`, which in `recomp-work` resolves to the
  `ps3recomp-native` runtime, not this checkout).

The flag is off by default and changes nothing in a normal lift. The
committed code is the option, not a default-on configuration.

## 2. Static result (same flags as `build-play`)

`objdump -d --disassemble=<symbol>` on the freshly compiled c690 object,
baseline (`build-play` source) vs `--inline-direct`. Both were compiled with
`-O2 -march=native -mavx2 -mfma -mssse3 -ffp-contract=off -DSPU_FAST_FLOAT
-DSPU_LIFTED_FAST_LS -DSPU_WRCH_INLINE -DSPU_USE_DISPATCH_MT` and the c690
lift flags `--merge-chunks --ib-known-targets --ib-goto --const-bi
--local-regs`, against the same runtime header the play build uses.

| symbol | baseline x86 | inline x86 | change |
|---|---:|---:|---:|
| `0xAF98` (normalize) | 2,163 | 2,516 | **+16.3 %** |
| `0xA410` (sel-7 kernel) | 1,243 | 1,243 | 0 % |
| `0xFDA0_body` (tree/cull) | 5,090 | 5,090 | 0 % |
| `0x10F78` (0x2A assembler) | 1,190 | 1,479 | **+24.3 %** |
| `0xBBB0` (0x21 setup) | 525 | 2,051 | +291 % (now contains the chain) |
| `0xBF00` (0x21 loop head) | 598 | 1,197 | +100 % (now contains `0xC018`) |
| `0xA3E0` (sel-7 entry) | 39 | 1,303 | now contains `0xA410` |

Counting the whole executed path rather than one symbol does not rescue the
gate:

| path | baseline x86 | inline x86 | baseline x86/SPU | inline x86/SPU |
|---|---:|---:|---:|---:|
| `0xAF98` + reachable `0xB238` chunk | 2,297 | 2,516 | 12.6 | 13.8 |
| `0xA3E0` + `0xA410` | 1,282 | 1,303 | 8.3 | 8.5 |
| `0x10F78` + `0x10C70` | 2,471 | 1,479 | 8.9 (277 SPU) | 5.3 |
| `0xBF00` + `0xC018` | 1,154 | 1,197 | 9.1 | 9.4 |

The only path with a real static cut is `0x10F78 -> 0x10C70` (-40 %), because
that callee's prologue/epilogue is large relative to its body. It is one of
three calls in the 0x2A assembler, and 0x2A is 13.5 % of the c690 wall.

## 3. Dynamic result on the captured `0xFDA0` corpus

Four `STAGE_IN` images from
`recomp-work/corpus_edge1_20261103_012604/stage/`, the lifted `0xFDA0` entry
linked with the bench stubs, median of seven alternating runs pinned to CPU 8.
This is the audit's own microbenchmark, with the two objects built from the
same header.

| stage | baseline `0xFDA0` | inline `0xFDA0` | change |
|---|---:|---:|---:|
| sample 0 | 278.7 us | 281.9 us | +1.1 % |
| sample 5 | 139.4 us | 137.8 us | -1.1 % |
| sample 11 | 468.2 us | 469.3 us | +0.2 % |
| sample 18 | 280.9 us | 286.8 us | +2.1 % |

`perf stat -e instructions:u,cycles:u -x,` on sample 0, 3 alternating runs:

| arm | instructions | cycles |
|---|---:|---:|
| baseline | 810,894,309 / 810,894,789 / 810,894,622 | 439.8 M / 448.7 M / 448.3 M |
| inline | 811,056,098 / 811,056,110 / 811,056,207 | 443.8 M / 444.0 M / 447.8 M |

The executed instruction count differs by **0.02 %** and cycles by less than
run-to-run noise. The inlined object is 5.0 MB of text vs 1.35 MB for the
baseline, so a live A/B would also carry a larger I-cache footprint for no
instruction saving.

## 4. Why there is no win

1. **The spills are intra-function.** `0xAF98` uses ~100 SPU registers in one
   straight-line body; the host compiler must spill most of them because x86
   has 16 vector registers. Inlining its tiny tail target `0xB238` (the
   reachable first chunk is ~12 SPU instructions before a computed branch)
   cannot reduce that live set, and the merged prologue is a superset of both
   sets, so x86 goes up.
2. **A tail transfer does not get cheaper.** The caller's write-back before
   `SPU_DIRECT` publishes the dirty GPRs; the chain's final exit must publish
   them again for the indirect dispatcher. Inlining removes the reload in the
   target's prologue but keeps (and enlarges) the final write-back.
3. **A `brsl` boundary is small.** `spu_drain_call` + the write-back/reload
   around it is ~2 % of a hot body. Removing it, while keeping the same values
   live across the callee, adds spills; the net on `0xFDA0` is zero.
4. **The option does help one shape**: `0x10F78 -> 0x10C70`, where a large
   prologue/epilogue surrounds a short body. If the 0x2A class is ever worth
   a dedicated change, that is the form to keep, not a global lift flag.

The audit's conclusion that instruction count (not stalls) is the lever still
holds; the specific lever is a register-mapped backend or a native stage, not
call-edge inlining.

## 5. What was not done

* **Gate (b) exactness and gate (c) meter A/B were not run.** Gate (a) is the
  first gate and it failed (negative, not just short). The corpus run in §3
  already exercises the lifted `0xFDA0` path for this question.
* **The `0xC0F4` `bi $r102` back-edge item was not attempted.** It was
  conditional on 2c showing a gain.
* **`build-play` was not rebuilt and no play script was changed.** The
  play-test build is still the existing one.
* The runtime `spu_context` layout is unchanged; `SPU_DIRECT_INLINE_OK` is a
  macro only (ABI check unaffected).

## 6. Reproduce

```sh
cd /home/m/Documents/mlb16-online
EXTRA=0x8ec4,0x918c,0x91a8,0x9200,0x9df8,0x9e34,0xb238,0xbcb4,0xbcdc,0xbd20,\
0xbdb4,0xbdd8,0xbe20,0xbe5c,0xc70c,0xc74c,0xc764,0xc78c,0xc7a0,0xc7d4,0xc7e8,\
0xc98c,0xd2b8,0xd4a8,0xd7a0,0xd810,0xd878,0xd9d0,0x4b24,0x4b6c,0x5034,0x5318,\
0x57b4,0x6860,0x82fc,0xa8a0,0xc290

# inline lift (separate output; the play tree is not touched)
LBP_JOBMOD_EXTRA_FUNCS=$EXTRA python3 ps3recomp-play/tools/lift_jobmods.py \
  --engine-elf EBOOT.elf --out /tmp/inline/spu_lifted \
  --register /tmp/inline/spu_lifted/jobmods_register.c --symbol-prefix mlb16 \
  --only-ea 0x1161300 --merge-chunks --ib-known-targets --ib-goto --const-bi \
  --local-regs --inline-direct

# compile with the play build's flags (source must sit at <x>/spu_lifted/<mod>/
# so ../../runtime resolves the same way it does for build-play), then count:
objdump -d --disassemble=jobmod_c690c2f7b09a_eF5B8_spu_func_0000AF98 "$OBJ" |
  grep -c '^ *[0-9a-f]*:'
```

Bench: link the lifted `0xFDA0` entry with the `tools/c690_ref` stubs and run
the four `STAGE_IN` images, pinned, alternating baseline and inline; count with
`perf stat -e instructions:u,cycles:u`.

One pre-existing test-harness nit found while checking the lifter:
`tools/test_spu_local_regs.py` fails to link on this tree because its link
line does not include the `c690_corpus`/`dxt_capture` stubs
(`undefined reference to g_c690_corpus_on`, `g_dxt_capture_on`,
`c690_corpus_dma`, `dxt_capture_dma`). It is unrelated to this change and
happens with the flag off.

## 7. Clean room and licence

Own static analysis and benchmarks of our own EBOOT/lifted c690 module and
corpus captures. The local-register/callee-summary model follows the inFAMOUS 2
port (MIT, ThiagoFraga761 and contributors) and the stage/corpus method follows
NHL Legacy Native (MIT, (c) 2026 HeckYouMan), design reference only; no code
was copied from either. No RPCS3 or other GPL emulator source was read or
cited.
