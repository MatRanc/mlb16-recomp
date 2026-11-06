# SPU codegen audit: how fast is the lifted c690 code, and where do the host instructions go?

Date: 2026-11-04. Static/offline only: no game launches, no live A/B, no changes to
`exp/live-ab` or `build-play`. The measured object is the one in
`recomp-work/build-play` (clang 22, `-O2 -march=native -mavx2 -mfma -mssse3
-ffp-contract=off`, `SPU_FAST_FLOAT`, `SPU_LIFTED_FAST_LS`, `SPU_WRCH_INLINE`,
`SPU_USE_DISPATCH_MT`). The c690 module was lifted with `--merge-chunks
--ib-known-targets --ib-goto --const-bi --local-regs`
(`mlb16-recomp/tools/lift_spu.sh`).

This follows [`c690-profile.md`](c690-profile.md),
[`edge-native-step5.md`](edge-native-step5.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690-offload-decision.md`](c690-offload-decision.md) and
[`nhl-lessons.md`](nhl-lessons.md).

## TL;DR

* **Yes, the lifted c690 code is slower than a real SPU, by about 3.3x per
  instruction on the hottest captured stage.** A captured `0xFDA0` tree/cull
  stage executes **0.95-1.06 ns per SPU instruction** (median **1.03 ns**) on
  this Ryzen 5 2600X. A real 3.2 GHz SPU dual-issues one instruction per
  **0.3125 ns**. The lift is therefore at **~3.3x** the SPU's issue interval.
* **The cause is instruction count, not stalls.** The compiler emits **~8-13
  x86 instructions per SPU instruction** for the hot functions; at 1.03 ns and
  ~3.6 GHz that is ~3.7 host cycles per SPU instruction, i.e. **~3.1 IPC** —
  the host is already issuing near its sustainable rate. There is no single
  pathological helper: there are simply 10x more host instructions than SPU
  instructions.
* **The top overhead is the 128-entry SPU register file.** **26-37 %** of the
  emitted x86 instructions in the hot functions are memory operands on
  `ctx->gpr` or on the host stack (register spills). Byte permutation/LS
  endianness is **2.5-10.5 %**, LS address masking **0.6-5.6 %**, and
  branch/dispatch **0.1-5 %** (low in the leaves, higher in the assembler).
* **Per-stage native rewrites cannot fix this.** The existing gates put native
  stages at **0.99-1.53x**, not 3x; the compiler already emits the same
  `vpshufb`/`vpsllvd`/`vfmadd` mix. The lever is the recompiler's code
  generation for the whole module.
* **The measured build is already on the cheap codegen switches** (AVX2/FMA,
  fast LS, fast float, inline MFC channel writes). The older claim that the
  AVX2 SPU blocks are compiled out does not apply to `build-play`; enabling
  them is not a remaining lever there.
* **The four named branches are already merged except the lock-line stripes.**
  `port-local-regs`, `port-spu-int-simd` and `port-spu-runtime-cuts` are
  ancestors of `exp/live-ab`; their flags are in the c690 lift. The 64-stripe
  lock-line change is unmerged and measured neutral (18.8 vs 18.5 fps); the
  0x21 path performs no atomics, so it cannot help c690 compute.

## 1. Method

All numbers are offline and reproducible without booting the game.

**Static inventory.** The c690 job module is the raw 67,648-byte image at LS
`0x4000`. Its SPU instruction ranges come from the flat disassembly of that
image (`/tmp/opencode/c690_disasm.txt`), with function boundaries from the
lifted source and the module's own `spu_function_table`. Host instruction
counts come from `objdump -d --disassemble=...` on the shipped
`build-play` object:
`CMakeFiles/MyGameRecomp.dir/.../jobmod_c690c2f7b09a_eF5B8/spu_recomp.c.o`.
Memory operands are classified by base register and displacement; `ctx->gpr`
is at offset 0 (`2048` bytes), `ctx->ls` at `0x40800`, so `0..0x7ff` on a
non-stack base is the GPR file and `%rsp/%rbp` is the host spill area.

**Dynamic microbenchmark.** Two kinds of state were used:

1. **Captured c690 stage state (the primary result).** The existing
   `PS3RECOMP_C690_CORPUS` captures `STAGE_IN`/`STAGE_OUT` pairs at the
   outermost `0xFDA0` entry, each a full 256 KiB LS + 2 KiB GPR image. Four
   pairs exist in `recomp-work/corpus_edge1_20261103_012604/stage`. The shipped
   `build-play` object's `jobmod_..._spu_func_0000FDA0_body` was linked into a
   small host program and run on each pair; all four reproduce the captured
   `STAGE_OUT` **bit-for-bit** (`ls_diff=0`, `gpr_diff=0`).
2. **Synthetic zero GPR state for the leaf kernels.** `0xA410` and `0xAF98`
   have no data-dependent control flow on their hot path, so an all-zero GPR
   image exercises the same instruction sequence. They are used to separate a
   straight-line kernel (`0xA410`, no calls) from a kernel with one direct call
   (`0xAF98` -> `0xB238`).

**Dynamic SPU instruction count.** The denominator is not estimated. A copy of
the lifted module was instrumented to increment a counter once per lifted SPU
statement, compiled with the same clang flags, and run on the same state. It
agrees exactly with the runtime interpreter (`spu_interp_run_until`, counted by
`g_spu_interp_steps`): **266,934** SPU instructions for the `0xFDA0` sample.
The instrumented run is used only for counting; timing uses the uninstrumented
`build-play` object.

Timing is the median of seven alternating function/reset runs, pinned to one
CPU (`taskset -c 8`), minus the measured reset-only baseline.

## 2. Lifted vs a real SPU

| workload | state | SPU instr/call | ns/call | ns per SPU instr | vs SPU (0.3125 ns) | static x86 | x86 per SPU |
|---|---|---:|---:|---:|---:|---:|---:|
| `0xA410` selector-7 kernel | synthetic | 148 | 81-83 | **0.55** | **1.8x** | 1,243 | 8.4 |
| `0xAF98` normalize (+`0xB238`) | synthetic | 182 | 177-193 | **1.00** | **3.2x** | 2,163 | 12.7 |
| `0xFDA0` stage, sample 0 | captured | 266,934 | 274.7 us | **1.03** | **3.3x** | 5,090 | 11.6 |
| `0xFDA0` stage, sample 5 | captured | 142,456 | 135.7 us | **0.95** | **3.0x** | 5,090 | 11.6 |
| `0xFDA0` stage, sample 11 | captured | 434,469 | 460.0 us | **1.06** | **3.4x** | 5,090 | 11.6 |
| `0xFDA0` stage, sample 18 | captured | 266,934 | 276.2 us | **1.04** | **3.3x** | 5,090 | 11.6 |

The four stage samples span 0.95-1.06 ns per SPU instruction despite a 3x range
in instruction count, so the rate is a stable property of the code, not of one
input. `0xA410` is the best case (straight-line, no call): 0.55 ns. `0xAF98`
pays about **1.8x more per SPU instruction than `0xA410` for only 1.5x the
static x86/SPU ratio**; the extra cost is the call boundary (a full save of
`0xAF98`'s live GPRs and a full reload in `0xB238`) plus more stack spills.

At ~3.6 GHz (the 2600X was at 92 % of its 4.25 GHz maximum during the run),
1.03 ns is ~3.7 cycles. With 11.6 x86 instructions per SPU instruction that is
~3.1 IPC; `0xA410`'s 0.55 ns / 8.4 x86 is ~4.2 IPC. **The host is issuing
close to its width; the slowdown is the 8-13x instruction count.** Removing a
latency or a helper call will not move much; removing emitted instructions
will.

### What this does and does not say about the frame

The c690 job wall is 72 ms/frame cumulative across five lanes (14.4 ms per
worker) and the meter burst is ~33-35 ms. The PS3 fits "the same c690 work" in
~20 ms on five SPUs. Those two end-to-end numbers are **not** a controlled
per-instruction comparison: the captured-stage rate says that the *same
instruction stream* would take ~3.3x the SPU issue time, i.e. a 20 ms SPU
budget would become ~66 ms on this host if the host executed exactly the same
320M SPU instructions. The observed 33 ms burst therefore means the host
workload is about half the PS3 budget's instruction count (scene/workload
difference), not that the lift is only 1.65x slower. The per-instruction ratio
is the robust measurement; the frame comparison is not.

## 3. Static inventory of the named hot functions

`SPU static` is the number of SPU instructions in the lifted function body
(count-instrumented source; merged chunks included). `x86` is the number of
instructions `objdump` emits for that symbol in `build-play`. For the `0x21`
selector-1 path the direct-call chain is listed as well.

| LS | role | SPU static | x86 | x86/SPU | notes |
|---|---|---:|---:|---:|---|
| `0x13140` | `0x21` selector-1 entry | 23 (path 14) | 210 | 9.1 | `andi` + `brz`/`hbrr` + tail call; other exits in the body |
| `0xBB80` | selector-1 setup | 6 | 36 | 6.0 | loads four constants, calls `0xBBB0` |
| `0xBBB0` | setup/constant block | 66 | 525 | 8.0 | 34 LS reads, spills 65 GPRs |
| `0xBCB4` | kernel entry | 11 | 89 | 8.1 | calls `0xBCDC` |
| `0xBCDC` | kernel prologue | 18 | 130 | 7.2 | |
| `0xBD20` | kernel prologue | 38 | 251 | 6.6 | |
| `0xBDB4` | kernel | 10 | 136 | 13.6 | high ratio: mostly GPR/LS moves |
| `0xBDD8` | kernel | 19 | 68 | 3.6 | |
| `0xBE20` | kernel | 16 | 90 | 5.6 | |
| `0xBE5C` | kernel -> loop | 29 (span) | 142 | 4.9 | 28 mask ops: LS-address masking |
| `0xBF00` | outer loop body | 71 | 598 | 8.4 | inner loop `0xBF00..0xBF54` is 22 SPU instructions |
| `0xC018` | loop tail | 56 | 556 | 9.9 | ends in `ctx->pc = gpr[102]; spu_indirect_branch` |
| `0xA3E0` | selector-7 entry | 6 | 39 | 6.5 | calls `0xA410` |
| `0xA3F8` | selector-7 merged body | 299 | 1,308 | 4.4 | includes `0xA410`; calls `0xA8A0` |
| `0xA410` | selector-7 kernel | 148 | 1,243 | 8.4 | straight-line, 93 LS ops |
| `0x10F78` | `0x2A` vertex assembler | 187 | 1,190 | 6.4 | 21 calls/indirect branches, 30 masks |
| `0x10B88` | `0x0A` convert dispatcher | 57 | 51 | 0.9 | the work is in `0x7540`/`0x7840`/`0x7AC0`/`0x7D40`/`0x7FC0` |
| `0xAF98` | normal/tangent normalize | 170 | 2,163 | 12.7 | 102 GPR loads, 438 stack memory ops; calls `0xB238` |
| `0x11A60` | `0x13` 16-byte copy | 6 | 28 | 4.7 | real copy loop is `0x11990` -> `0x5680` |
| `0xFDA0_body` | tree/cull stage | 439 | 5,090 | 11.6 | the measured stage; 1,030 stack memory ops |

### Call-path counts

For the `0x21` selector-1 path (`0x13140 -> 0xBB80`), the **static path to the
loop** is:

```
0x13140 (14) + 0xBB80 (6) + 0xBBB0 (66) + 0xBCB4 (10)
+ 0xBCDC (17) + 0xBD20 (37) + 0xBDB4 (9) + 0xBDD8 (10)
+ 0xBE20 (15) + 0xBE5C (29) = 213 SPU instructions, 1,677 x86 instructions
```

and the **outer loop body** is `0xBF00` (70) + `0xC018` (56) = **126 SPU
instructions, 1,154 x86 instructions per iteration**. The loop back edge is the
computed `bi $r102` at `0xC0F4`; `0xC018` stores about 30 GPRs and the next
`0xBF00` reloads about 41, so every iteration crosses a lifted-call boundary
even though it stays inside the same guest function.

For selector 7 (`0x131A0`), the two bodies are `0xA3E0 -> 0xA410` (154 SPU,
1,282 x86) and `0xA3F8 -> 0xA8A0` (299 + 642 source instructions; `0xA8A0`
alone is 467 x86 and calls `0xA660`/`0xB238`). The measured 51.7 us/call is
dominated by whichever body the flag selects and by its batch loop; the static
counts do not capture that loop.

The measured per-call walls from `c690-emit-ring.md` line up with these static
bodies once loops are included: `0x21` selector 1 is 390.4 us, selector 7 is
51.7 us; `0x2A` (`0x10F78`) is 22.55 us; `0x13` (`0x11A60`) is 16.2 us; the
`0x0A` handler (which enters `0x10B88`) is 2.66 us; `0xAF98`'s exclusive
counter is 345 ns/entry, which the profile's own 1.52x over-billing puts within
~1.5x of the 185-193 ns measured here.

## 4. Where the x86 instructions go

Share of the emitted x86 instructions in the `build-play` object. "GPR/stack"
is memory operands on `ctx->gpr` or `%rsp/%rbp`; "byteperm" is
`vpshufb`/`vperm*`/`bswap`/`movbe`; "mask" is an `and` with a `0x3ffff`/
`0x3fff0` style immediate; "branch" is any `j*`/`call`/`ret`; the remainder is
real vector/integer arithmetic, constant loads and register-to-register moves.
The categories are not exclusive.

| function | x86 | GPR/stack mem | byteperm | mask | branch/call | other |
|---|---:|---:|---:|---:|---:|---:|
| `0xAF98` | 2,163 | **28.0 %** | 7.3 % | 1.1 % | 0.4 % | 63.2 % |
| `0xA410` | 1,243 | **36.1 %** | 10.5 % | 5.6 % | 0.1 % | 47.7 % |
| `0xBBB0` | 525 | **37.1 %** | 10.1 % | 5.9 % | 3.4 % | 43.5 % |
| `0xBF00` | 598 | **28.9 %** | 7.2 % | 1.3 % | 1.3 % | 61.3 % |
| `0xC018` | 556 | **26.4 %** | 6.3 % | 0.7 % | 0.2 % | 66.4 % |
| `0x10F78` | 1,190 | **34.1 %** | 5.3 % | 2.5 % | 5.0 % | 53.1 % |
| `0xFDA0_body` | 5,090 | **36.6 %** | 2.5 % | 0.6 % | 1.1 % | 59.2 % |

The GPR/stack share is a lower bound on spill cost: it counts the loads and
stores, not the register-to-register moves that feed them or the address
arithmetic. It is the single largest non-arithmetic category everywhere.

## 5. The four branch worktrees

| worktree / branch | tip | what it does | in `exp/live-ab`? | measured | useful for c690? |
|---|---|---|---|---|---|
| `ps3recomp-lift` / `port-local-regs` | `84c18f9f` | `--local-regs` keeps a function's SPU registers in C locals; `--const-bi` turns `il/ila ... bi` into a direct goto | **yes**, ancestor | no isolated number; landed in the 2026-10-28 codegen set | **already active**: the c690 lift passes both; the remaining spills are compiler stack traffic, not the old `ctx->gpr`-per-statement model |
| `ps3recomp-helpers` / `port-spu-int-simd` | `571c83c7` | SSE4.1/AVX2 forms for the scalar integer helpers (`sumb`, `cgx`, `bgx`, `xswd`, `orx`, `gb*`, `fsm*`, `clz`, `shlh`, `roth*`, `shlqbi`, `rotqbi`, `rotqmbi`) | **yes**, ancestor | inFAMOUS 2 port: 112 -> 123 fps; test suite passes; no isolated MLB number | **yes**: c690 uses `orx` 24, `fsm*` 34, `shlqbii` 56, `rotqmbii` 32; already active |
| `ps3recomp-spurt` / `port-spu-runtime-cuts` | `ad0b5b23` (+ `bbdef542`, `1b26e4a1`) | fast indirect-branch resolver for job modules, latched MFC parameter channels, `u128` built as two 64-bit halves | **yes**, ancestor | no isolated MLB number; part of the 2026-10-28 runtime cuts | **small**: dispatch is ~5 % of c690; the 0x21 loop has one indirect branch and one 30/40-GPR spill per batch iteration |
| `ps3recomp-ll` / `port-lockline-stripes` | `518e49e3` | 64-stripe lock-line lock keyed by guest line | **no** (1 commit ahead, 127 behind) | 18.8 vs 18.5 fps at the pitch: neutral | **no**: `0x21` has 0 GETLLAR/PUTLLC/atomics; coherency/lock-line is 1.4 % of job time |

The shufb-without-byte-swaps change (`58587e93`, +4 % fps) is in `exp/live-ab`
even though its branch name sits under the same worktree; it is part of why
byteperm is already only 2.5-10.5 % of the emitted code. The striped lock-line
change is the only unmerged one and it cannot move c690 compute.

## 6. Top three overheads and what removal would buy

These are estimates from the measured x86 mix and the measured 1.03 ns/SPU
instruction, not A/B results. They are not additive.

| # | overhead | share of x86 | what it is | expected gain if removed |
|---|---|---:|---|---:|
| 1 | **GPR file / register spills** | 26-37 % | `ctx->gpr` traffic plus host stack spills for the 128-entry SPU register file; every lifted call saves live GPRs and the callee reloads them | **15-25 %** (1.15-1.25x) realistic; ~30 % upper bound if the whole category vanished |
| 2 | **Per-call boundary + indirect dispatch** | ~5-10 % of the hot paths | `SPU_DIRECT` tail calls, the `0xC018 -> 0xBF00` computed `bi` loop edge, and the GPR save/reload around each lifted entry | **5-10 %** on call-heavy paths; ~2-4 % of the c690 wall (0x21 is 43 % of it) |
| 3 | **LS byte-swap + address masking** | 3-11 % byteperm + 0.6-5.6 % mask | `vpshufb` endian swaps and `and 0x3fff0/0x3ffff` LS address masking | **3-6 %** overall; low risk, and partly taken already (+4 % from `58587e93`) |

The arithmetic itself is 45-60 % of the x86 and is already the same
AVX2/FMA/SIMD mix a native kernel would use. That is why removing the
overheads, not rewriting the math, is the lever.

## 7. Cost, risk and verification for lifter changes

Any change here is a **lifter/codegen** change and therefore touches every
lifted module (8 job modules, 5 SPU images). Keep it behind a lifter flag,
default off, and verify in this order:

1. **Static/codegen check (no game):** relift with the flag, compare
   `x86 instructions per SPU instruction` and build size per function against
   the baseline. A change that does not cut x86/SPU by >=10 % on `0xAF98`,
   `0xA410` and `0xFDA0_body` is not worth testing further.
2. **SPU equivalence:** `tools/test_spu_local_regs.py`, the runtime's
   `runtime/spu/tests` (`test_spu_int_neon`, `test_spu_shufb`, float/LS tests),
   and the per-function replay harness. The `0xFDA0` corpus is an exact gate:
   the lifted stage must still produce `ls_diff=0`, `gpr_diff=0` against all
   four `STAGE_OUT` images.
3. **Whole-module corpus:** relift all job modules and run the existing
   per-module replay/corpus tests; compare LS+GPR hashes bit-for-bit. The
   interpreter is the correctness floor and already agrees with the lift on
   instruction count (266,934 for the sample).
4. **Only then a live A/B** (outside this task): one meter run with
   `PS3RECOMP_C690_HASH`, one without, same scene; kill at <5 % c690-wall
   change or any hash mismatch.

Risk is concentrated in (1): `SPU_DIRECT` uses clang `musttail` for correct
tail-call semantics, which also **prevents inlining** of direct callees and
forces the register save/reload at every `brsl`. Inlining acyclic direct
callees is the most promising concrete change (it lets the compiler keep
values in registers across the call), but recursion (`0xFDA0`) and guest loops
that rely on the trampoline must keep the tail-call path. A flag that inlines
only non-recursive, non-indirect callees is the safe form.

Compiler flags are not the lever: rebuilding the same source with `-O3`
produced 2,280 x86 for `0xAF98` (vs 2,163 at `-O2`) and no timing change
(171.8 ns vs 177-193 ns), and `-fno-stack-protector` changed nothing.

## 8. Recommended plan

1. **Stop per-stage native rewrites for c690.** The existing gates are
   0.99-1.53x; even a perfect 3x on all geometry stages leaves the burst at
   ~27-31 ms and costs weeks per stage. The host instruction count, not the
   math, is the gap.
2. **Do one offline codegen spike (highest value, no game run):**
   - relift c690 in a scratch worktree with and without `--merge-chunks` and
     `--local-regs`, and with a "split long live ranges" variant, and measure
     x86/SPU plus ns per SPU instruction on the four existing `0xFDA0` corpus
     pairs;
   - add a lifter option that inlines acyclic direct callees instead of
     `musttail`-ing every `SPU_DIRECT`, so the compiler can allocate registers
     across the call;
   - keep it only if it cuts x86/SPU by >=15 % on the three probes and keeps
     the stage hashes exact.
3. **If the spike works, apply it to the hot 0x21 loop next:** extend
   `--const-bi`/`--ib-goto` to the `bi $r102` back edge at `0xC0F4` so
   `0xBF00 <-> 0xC018` stays in one lifted function and keeps its registers.
   That removes one save/reload set and one resolver per batch iteration.
4. **Helper micro-opts last:** the remaining byteperm and mask instructions are
   small and low-risk, and `58587e93` already took the biggest shufb win.
5. **Do not** pursue GPU-offload Design A/B, the sixth lane, or lock-line
   striping for c690; all three are priced or measured elsewhere and none
   addresses the emitted-instruction count.

A realistic combined outcome of (2)-(4) is **1.2-1.4x on c690 compute**
(1.03 -> ~0.75-0.85 ns/SPU instruction, still ~2.4-2.7x a real SPU), which is
worth ~3-6 ms of the 33-35 ms burst. Reaching parity with a real SPU would
require a different backend (a register-mapped SPU-to-host compiler or
whole-job native code), not incremental changes to this lift.

## 9. Reproduce

The benchmark programs are outside the repo. The essential steps:

```sh
# corpus input: 256 KiB LS followed by 2 KiB GPR, captured at 0xFDA0
STAGE=recomp-work/corpus_edge1_20261103_012604/stage/job_000000_in.bin
OBJ=recomp-work/build-play/CMakeFiles/MyGameRecomp.dir/\
home/m/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c.o

# 1. static host instruction counts
objdump -d --disassemble=jobmod_c690c2f7b09a_eF5B8_spu_func_0000AF98 "$OBJ" | grep -c '^ *[0-9a-f]*:'

# 2. dynamic SPU instruction count: compile the lifted source with a counter
#    per statement (same clang flags as build-play) and run FDA0_body on $STAGE;
#    the count is 266,934 for sample 0 and matches spu_interp_run_until.

# 3. timing: link FDA0_body from $OBJ with stubs and run it on $STAGE,
#    resetting the 258 KiB state each iteration and subtracting the reset-only
#    baseline. Median of seven runs pinned to one CPU.
```

## 10. Clean room and licence

This is our own static analysis of our own EBOOT/lifted c690 module, our own
runtime's counters and interpreter, and our own corpus captures. The
stage/corpus method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. No RPCS3 or other GPL emulator source was
read or cited.
