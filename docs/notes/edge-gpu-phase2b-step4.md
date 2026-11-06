# c690 Edge on the GPU — phase 2b step 4 (0xB830 leaf kernel exact; the 47 % number was capture overhead)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. **Offline only: zero game launches.** The work is the
GPU kernel for the A730 heavy-body leaf (LS `0xB830`), its 64-pair differential
gate, a one-dispatch burst cost, a CPU-native AVX2 estimate, and the review of
the capture wrapper on the play build.

This follows [`edge-gpu-phase2b-step3.md`](edge-gpu-phase2b-step3.md),
[`edge-gpu-phase2b-step2.md`](edge-gpu-phase2b-step2.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`c690_function_map.md`](c690_function_map.md) and
[`spu-codegen-audit.md`](spu-codegen-audit.md).

## TL;DR

* **The 0xB830 leaf kernel is bit-exact over all 64 meter pairs.** 64/64 pairs,
  **0 differing LS bytes** (4,194,304 32-bit words, max ULP 0), and the nine
  GPRs that survive the `0xA660` tail call are also exact. The shader is a
  literal transliteration of the lifted 78-line body: 25 `shufb`, 22 `selb`,
  9 `fma`, 2 `rotqby`, 3 `addi`, 1 `add`, 5 x 16-byte loads, 4 x 16-byte
  stores. SPU byte order, FTZ and fused FMA are emulated in the shader.
* **One dispatch for the whole frame burst.** 4,394 calls in one
  `vkCmdDispatch`, device-local: **0.101-0.103 ms/frame** (0.023 us/call),
  CPU record 1.7 us + submit 2.25 us. The dispatch floor is ~0.08 ms; the
  marginal cost of the 4,393 extra calls is ~0.02 ms.
* **Premise correction: the step-3 9.06 us/call / 47.16 % is capture
  overhead, not leaf time.** The tail capture takes a full 256 KiB LS+GPR
  snapshot in `c690_tail_open` **before** the per-key pair cap is checked in
  `c690_tail_close`. At 1,270,050 calls that is ~312 GB of `memcpy` in the
  10 s window; a 256 KiB copy measures 6.75 us here. The same lifted leaf
  runs **47.8 ns/call** offline (785 x86 instructions): 4,379 calls/frame =
  **0.209 ms/frame** (all lanes), 60.7 ms/10 s, i.e. ~0.6 % of one core, not
  11.505 s. **0xB830 is not a 5.6 ms lever; its maximum saving is ~0.1-0.2
  ms/frame.**
* **Option D estimate.** A native AVX2 leaf is ~71 vector ops + 9 live vector
  outputs per call, about 80-120 uops, **~6-10 ns/call** (0.03-0.04 ms/frame,
  or ~4-6 ns/call two calls wide). It beats the GPU kernel on this leaf and
  still saves only ~0.17 ms/frame over the lifted body.
* **Wrapper review.** With capture off, the wrapper costs **0.54 ns/call
  median (1.1 % of the 48 ns leaf; range 0.0-3.5 %, 500k iters x 16 pairs)**.
  Per second at 127k calls that is ~0.07 ms/s, negligible end-to-end, but it
  is above the 1 % per-call review threshold. The 0x2A / 0x21 / 0xA730
  capture wrappers are now opt-in (`patch_cull_hooks.py --capture`), the play
  module was stripped, `build-play` rebuilt, and the replay result is
  unchanged (0x2A 64/64, 0x25 64/64, 0x26 64/64, 0xA7 64/64; the 0x21 rows
  keep the same 944,089 / 1,333,359 / 60,248 byte boundary diffs).

## 1. Kernel

`tools/replay/c690_b830_batch.comp` (new). It is generated from
`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`
`..._0000B830_body` (lines 14965-15042) by a small one-off transliterator, then
checked by hand. The lifted body is straight-line: no data-dependent control
flow, so the GPU kernel is one invocation per leaf call.

Representation:

* A `uvec4` is one SPU 128-bit register, component 0 = preferred slot.
* The Local Store is uploaded as raw little-endian words; `load128`/`store128`
  byte-swap each word into/out of the big-endian SPU value.
* The GPR image is a host `u128` array, so its 32-bit words are already SPU
  word values; `load_gpr` does **not** byte-swap (this was the one bug found
  while bringing the kernel up).
* `shufb` is implemented on SPU byte positions: byte `P` of word `w` is
  `w >> (24 - 8*(P&3))`; selectors `0xE0/0xC0/0x80` produce `0x80/0xFF/0x00`.
* `selb` is `(a & ~c) | (b & c)`; `rotqby` rotates the 16 SPU bytes by
  `b.x & 15`; `a`/`ai` are per-word adds.

Float semantics (the build is `SPU_FAST_FLOAT` + fused FMA):

* `fma()` uses GLSL.std.450 `Fma`, which has no intermediate rounding
  (confirmed in the SPIR-V: one `OpExtInst Fma`). This matches
  `_mm_fmadd_ps` in `spu_helpers.h`.
* `ftz()` reproduces `spu__ff_ld`/`spu__ff_st`: any operand/result whose
  exponent field is zero becomes `+0`.
* The 64 pairs contain no denormal operand or result, so a no-FTZ variant is
  also bit-exact on this corpus and the FTZ check is inside the measurement
  noise (0.101 vs 0.101-0.103 ms/frame). It is kept for unseen inputs.

The leaf also leaves nine GPRs live for the caller: `0xA660` overwrites
`gpr[0]` and `gpr[80..127]` from LS, so only `7, 12, 18, 24, 50, 59, 60, 61,
77` survive. The kernel writes them to a separate output buffer and the gate
checks them against the captured OUT GPRs. (The LS-only form of the kernel is
0.041 ms/frame, but it is not the full leaf contract.)

## 2. Exactness gate

`tools/replay/test_c690_b830_batch.c` (new) parses every op `0xA7` IN/OUT pair
from a `PS3RECOMP_C690_TAIL_CAPTURE` spool, runs the shader on the IN LS/GPRs
and diffs the **full 256 KiB LS** plus the nine live GPRs.

Run on `/tmp/opencode/fncap2b3a730` (64 pairs):

| output | pairs | exact | differing bytes/words | max ULP | ULP histogram |
|---|---:|---:|---:|---:|---|
| Local Store | 64 | **64** | 0 / 4,194,304 | **0** | exact 4,194,304, all others 0 |
| live GPRs | 64 | **64** | 0 / 2,304 | 0 | exact |

Gate: `<= 1 LSB on <= 0.1 %` of output values. Result: **bit-exact**, so no
drift was accepted and no softening was needed. The harness exits non-zero on
any byte/word beyond the gate.

## 3. Burst cost

One `vkCmdDispatch`, one submission, device-local storage, RTX 3060 Ti. J
calls are tiled over the 64 captured state images; J=64 is the exactness run.

| J | GPU ms | GPU us/call | record us | submit us |
|---:|---:|---:|---:|---:|
| 1 | 0.080 | 80.5 | 1.7 | 2.3 |
| 64 | 0.088 | 1.38 | 1.7 | 2.25 |
| **4,394** | **0.101-0.103** | **0.023** | **1.7** | **2.25** |

The 4,394-call burst is **0.101-0.103 ms/frame** (median of 31 iterations, three
runs). The fixed dispatch/submit+fence floor is ~0.08 ms; the extra 4,393
calls add ~0.02 ms, i.e. ~4.6 ns/call marginal. Compare with the 0x2A kernel's
0.23-0.27 ms per 429-job burst: this leaf is smaller and even more
overhead-bound.

At the step-3 call rate (1,270,050 calls / 10 s = 127,005 calls/s; at 29 fps =
4,379 calls/frame) the GPU burst is **0.10 ms/frame**. The CPU record+submit
per burst is **~4 us**, far below the GPU wait.

## 4. Premise correction: where the 9.06 us/call came from

`c690_tail_open` (`ps3recomp-play/runtime/spu/c690_corpus.c:818`) runs for
every sampled entry:

```c
s->sampled = (s_tail_stride <= 1u || entry % stride == 0);
if (!s->sampled) return;
memcpy(s->in_ls, ctx->ls, SPU_LS_SIZE);      /* 256 KiB */
memcpy(s->in_gpr, ctx->gpr, C690_CORPUS_GPR_SIZE);
```

The per-key cap is only checked later in `c690_tail_close`
(`c690_corpus.c:782`), so with `PS3RECOMP_C690_TAIL_STRIDE=1` every one of the
1,270,050 leaf calls copies 256 KiB. That is ~312 GB in the 10 s window. An
offline 256 KiB `memcpy` on this host is **6.75 us**, which accounts for
~8.6 s of the reported 11.505 s; the rest is the timestamp/atomic/close path.
The same run's `0xA7` average was 9.06 us, not a leaf body time.

Offline, the lifted `0xB830` body on the captured state is **47.8 ns/call**
(785 x86 instructions, 71 SPU vector ops; pinned, median of 9 alternating
runs, wrapper vs body). Therefore:

| quantity | value |
|---|---:|
| step-3 reported leaf time (capture on) | 11.505 s / 10 s, 47.16 % of tail |
| true lifted body, 1,270,050 calls | 60.7 ms / 10 s |
| per frame (4,379 calls) | **0.209 ms/frame**, all lanes |
| real-SPU estimate (71 ops x 0.3125 ns) | ~22 ns/call = 0.096 ms/frame |
| GPU full kernel, one dispatch/frame | **0.101 ms/frame** |

So the GPU is **~2x the lifted host body** for this leaf and about at parity
with a real SPU; the absolute saving over the lift is ~0.10 ms/frame, not
~5.6 ms. The step-3 "47 % of the tail" and the step-2-derived 5.6 ms lane
saving are not valid for this leaf.

This is a capture bug, not a game bug: `c690_tail_open` should look up the
key's pair count and set `sampled = 0` once the cap is reached, before the
snapshot. That was not changed here (it would need a new meter capture to
re-measure, and this task is offline-only). Until it is fixed, tail-capture
histogram *times* for high-frequency hooks are not body times.

## 5. Option D: CPU-native AVX2 estimate (not built)

The leaf is 71 vector operations for four vertices. A native AVX2 version
would be:

| work | count |
|---|---:|
| `vpshufb` (shufb / rotqby) | 27 |
| `vpblendvb` (selb) | 22 |
| `vfmadd` (fma) | 9 |
| word add / add-immediate | 4 |
| 16-byte loads / stores | 9 (LS) + 9 (live GPR vectors) |
| **uops** | **~80-120** |

At ~3.6 GHz and 3-4 uops/cycle, with `vpshufb`/`vfmadd` sharing the FP pipes,
that is **~20-35 cycles/call = 6-10 ns/call** for one 128-bit call. Two calls
in one 256-bit AVX2 register (the shuffles stay lane-local) are ~4-6 ns/call.
Per frame (4,379 calls): **0.03-0.04 ms** one call at a time, **0.02-0.03 ms**
two wide. Memory is 80 B read + 64 B written per LS call plus the GPR outputs;
4379 calls stay in L1/L2.

That is faster than the GPU kernel (0.10 ms/frame including dispatch) and ~6x
the lifted body, but the saved time is still only ~0.17 ms/frame. Option D is
the better implementation *if* this leaf is ever ported; it is not a reason to
port it.

## 6. Wrapper review and opt-in change

The play build carried the step-3 capture wrapper at `0xB830`. The wrapper is
not inlined (`spu_recomp.c.o` has a 61-byte wrapper calling the 0xe3a-byte
`_body`), so with capture off it adds one call/ret and two predicted-not-taken
global tests.

`tools/replay/test_c690_b830_wrapper.c` (new) times wrapper vs `_body` on the
captured state with `g_c690_tail_on == 0`, 500,000 iterations, pinned to one
CPU, 16 pairs, alternating order:

| | ns/call |
|---|---:|
| wrapper | 48.5-51.9 |
| `_body` | 47.9-50.6 |
| delta | **median 0.54 ns (1.1 %)**; mean 0.75 ns; range 0.00-1.76 ns (0-3.5 %) |

At 127k calls/s the extra is ~0.07 ms/s, i.e. ~0.007 % of wall, so it is
irrelevant end-to-end. But it is above the 1 % per-call review threshold, and
the capture wrappers have no job in a play build, so they were made opt-in:

* `tools/c690_ref/patch_cull_hooks.py` now takes `--capture` to install the
  0x2A / 0x21 / 0xA730 capture wrappers and strips them by default (and with
  `--strip`). The cull wrapper is always applied: it also carries the
  sample-grid fix and the live cull bypass.
* The play module was stripped (`... spu_recomp.c`, capture wrappers off) and
  `ninja -C recomp-work/build-play MyGameRecomp` rebuilt.
* Replay after the rebuild: 0x2A 64/64, 0x25 64/64, 0x26 64/64, 0xA7 64/64
  exact; the 0x21 command-boundary rows are unchanged (0/64 exact with
  944,089 / 1,333,359 / 60,248 differing bytes). No `spu_context` field was
  added; `check_spu_context_abi.sh` reports `shared field offsets agree`.

`test_c690_b830_wrapper.c` requires a `--capture` module (it calls `_body`),
so it is a research harness for the capture build, not a test of the stripped
play build.

## 7. What this means for option C

The leaf kernel is correct and cheap, but the premise that it is 47 % of the
tail is wrong. With the real 0.1-0.2 ms/frame, porting this leaf cannot move
the 35 ms burst, and the GPU is not even the best engine for it (AVX2 is
faster). The large-time classes in the step-2 histogram were measured with the
same snapshot-per-entry capture, so their absolute times are also inflated;
the 0x21 command share is less distorted because those hooks run far less
often, but a corrected capture is needed before any further coverage
arithmetic.

Recommended next step (not done here, needs one meter launch): move the cap
check into `c690_tail_open` before the snapshot, recapture, and redo the
histogram. Only then is it meaningful to decide whether A730/A3E0/cull GPU
work is worth the remaining effort.

## 8. Reproduction

```sh
# kernel + 64-pair gate + burst cost (offline, needs Vulkan)
clang -O2 -o /tmp/opencode/test_c690_b830_batch \
  mlb16-recomp/tools/replay/test_c690_b830_batch.c -l:libvulkan.so.1 -lm
/tmp/opencode/test_c690_b830_batch --jobs 4394 --iters 31 --device-local \
  mlb16-recomp/tools/replay/c690_b830_batch.comp \
  /tmp/opencode/c690_b830_batch.spv /tmp/opencode/fncap2b3a730

# wrapper cost (requires a capture-mode module: --capture)
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I ps3recomp-play/runtime/spu -I ps3recomp-play/include \
  -I ps3recomp-play/libs/video \
  -o /tmp/opencode/test_c690_b830_wrapper \
  mlb16-recomp/tools/replay/test_c690_b830_wrapper.c \
  mlb16-recomp/tools/replay/reg_10f78.c \
  recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  ps3recomp-play/runtime/spu/c690_corpus.c \
  ps3recomp-play/runtime/spu/c690_cull_fix.c \
  ps3recomp-play/runtime/spu/c690_native_cull.c \
  ps3recomp-play/runtime/spu/c690_native_screen.c \
  ps3recomp-play/runtime/spu/job_c690_native.c -lm
/tmp/opencode/test_c690_b830_wrapper /tmp/opencode/fncap2b3a730 500000 0

# capture wrappers opt-in / strip
cd ps3recomp-play
python3 tools/c690_ref/patch_cull_hooks.py --capture   # capture build
python3 tools/c690_ref/patch_cull_hooks.py --strip     # play build
ninja -C ../recomp-work/build-play MyGameRecomp
```

The spool is `/tmp/opencode/fncap2b3a730` (not committed); the generated module
is game-derived and is not committed.

## 9. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code and our own captures. The stage/corpus/differential method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2
PC port (MIT, ThiagoFraga761 and contributors), design reference only. No
RPCS3 or other GPL emulator source was read or cited.
