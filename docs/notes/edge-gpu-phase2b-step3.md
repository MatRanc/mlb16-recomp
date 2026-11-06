# c690 Edge on the GPU — phase 2b step 3 (A730 body unblocked; cull/A3E0 kernels blocked)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. One automated pitch-meter launch (`ab_live.sh`,
governor `performance`, tag `fncap2b3a730`) plus offline replay of every
captured pair. Nothing is wired into the walker, no runtime key was added and
nothing is default-on.

This follows [`edge-gpu-phase2b-step2.md`](edge-gpu-phase2b-step2.md),
[`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`edge-holes-fix.md`](edge-holes-fix.md),
[`edge-native-step4.md`](edge-native-step4.md) and
[`c690_function_map.md`](c690_function_map.md).

## TL;DR

* **A730 is unblocked at the right body.** The sub-selector `0x13140` only
  runs a 12.9 us prefix. Its heavy body is reached through the trampoline
  chain `0x13140 -> 0x13250 -> 0xA3C0/0xA3F8 -> 0xA410 -> (bi gpr[82])
  0xA730 -> 0xA798 -> (bi gpr[82]) 0xB830`. The leaf `0xB830` is the
  per-vertex transform/normalize body. A default-off wrapper at `0xB830`
  (op key `0xA7`) now captures real IN/OUT pairs. Its unit test passes.
* **One meter capture, 64/64 A730 body pairs replay byte-exact.** The
  wrapper saw **1,270,050** `0xB830` calls in the 10 s meter window and
  **11.505 s (47.16 %)** of the 24.398 s tail. The `0x13140` prefix is
  **0.201 s (0.83 %)**: that is the measurement that was missing in step 2.
  The captured `0xA7` pairs, and the existing 0x2A/0x25/0x26 pairs, all
  replay with **0 differing LS bytes**.
* **The cull kernel is not built; it is not a screen test.** The 0x132E0
  body's heavy stage is the `0xD080` subtree: **24 blocks / 1,551 lifted
  lines**, with the per-vertex skin/transform loop `0xD2B8`, the
  `0xD4A8/0xDA60/0xDB58/0xDB88/0xDC48/0xDD48` index pack, and the
  `0x49D4`/`0xE6B8` allocators in the caller. A bit-exact GPU port is a
  C-to-GPU translation of that state machine, not a compact screen-space
  formula. The 64/64 exact meter pairs are ready as the test corpus, but no
  kernel/diff/burst number is claimed.
* **The A3E0 kernel is not built; it is a 19-function chain.** The clean
  A3E0 pairs reach `0x131A0 -> 0xBB98 -> 0xA3E0 -> 0xA410 -> 0xA730 ->
  0xA798 -> 0xAF98/0xB238` (normalize) plus the pack chain. The statically
  reachable graph is **19 functions / 2,493 lines** with indirect branches.
  Again this is a multi-day translation, not a compact formula.
* **Signed off as GPU-exact is still only 0x2A.** The running total is
  **23.1 % of the step-2 meter tail** (18.2 % in the step-3 window), at
  **0.23–0.27 ms per 429-job burst** device-local. A730 is unblocked but has
  no kernel; the culls and A3E0 have no kernel. **The full tail cannot move
  with this mission's result**, so option C is not viable without the
  multi-day kernel port described in §7.

## 1. A730 trampoline map (static + captured)

The 0x21 sel1 A730 path is not `0x13140` itself. The lifted sources show:

```
0x13140 body
  r6 & 16 == 0 -> 0x13250
    0xA3C0: load constants LS 0x143D0/0x143E0/0x143F0/0x14430 -> 0xA410
    0xA3F8: load constants LS 0x14410/0x14420/0x14450/0x14480 -> 0xA410
0xA410: builds the transform state, sets gpr[82], indirect branch
  captured target: 0xA730
0xA730: 78-line prefix, SPU_DIRECT 0xA798
0xA798: per-vertex-group loop, indirect branch gpr[82]
  captured targets: 0xA798 (self) and 0xB830
0xB830: 78-line self-contained per-vertex transform/normalize leaf,
        returns through gpr[86]
```

The captured targets are from the replay of a real A730 command pair with
the trampoline log enabled (offline, no game run). The same trace for A3E0
shows `0x13200 -> 0xA3E0 -> 0xA410 -> 0xA730 -> 0xA798 -> 0xAF98/0xB238`;
the A3E0 target from `0xA798` is `0xAF98`, not `0xB830`.

The important distinction for the capture:

* a wrapper on `0x13140` records only its **12.9 us prefix**;
* a wrapper on `0xA730` records only the `0xA730` prefix, because the loop
  continues through `0xA798`'s indirect branch;
* a wrapper on `0xB830` records the **real per-vertex body** inputs/outputs.

## 2. Capture hook: the A730 leaf

`tools/c690_ref/patch_cull_hooks.py` now also wraps LS `0xB830`:

```c
void jobmod_..._0000B830_body(spu_context* ctx);
void jobmod_..._0000B830(spu_context* ctx) {
        extern int g_c690_tail_on;
        extern void c690_corpus_tail_cmd_enter(const spu_context*, uint32_t, uint32_t);
        extern void c690_corpus_tail_cmd_exit(const spu_context*);
        if (__builtin_expect(g_c690_tail_on, 0))
            c690_corpus_tail_cmd_enter(ctx, 0x0B830u, 0xA7u);
        jobmod_..._0000B830_body(ctx);
        if (__builtin_expect(g_c690_tail_on, 0))
            c690_corpus_tail_cmd_exit(ctx);
}
```

It rides the existing `PS3RECOMP_C690_TAIL_CAPTURE` spool. The key is the
opcode `0xA7`, so `tail_hist.txt` and `c690_tail_report.py` show it
separately. When `g_c690_tail_on` is 0 (the default), the only added cost is
one predicted-not-taken branch. No `spu_context` field was added; the ABI
check still reports `shared field offsets agree`.

`tools/test_c690_tail_capture.py` now also covers the `0xA7` pair: the
opcode, LS `0xB830`, key `0xA7`, the one-line write bitmap, and the
`0x000000A7 1` histogram entry. It passes.

`tools/replay/test_c690_tail_replay.c` maps `0xA7` to `0xB830` and compares
the captured OUT at the leaf's return (the leaf returns through `gpr[86]`;
the caller's continuation is not drained).

## 3. The one meter capture

One launch, `ab_live.sh`, build `build-play`, at the pitch meter
(`METER=1 SKIP_CINE=1`, one X, 8 s settle), governor `performance`:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
TAG=fncap2b3a730 N=1 SEG=2 DROP=0 SETTLE=8 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 PITCH_THRESH=35.0 LIMIT=420 BOOT=40 ARMS='A=' \
  PS3RECOMP_C690_TAIL_CAPTURE=/tmp/opencode/fncap2b3a730 \
  PS3RECOMP_C690_TAIL_PAIRS=64 PS3RECOMP_C690_TAIL_STRIDE=1 \
  PS3RECOMP_C690_TAIL_TRIGGER=/tmp/opencode/fncap2b3a730.trigger \
  PS3RECOMP_C690_OP21=1 ./ab-tools/ab_live.sh
```

The log (`ab_live_fncap2b3a730_20261104_105323.log`) shows the pitch reached
at 152 s, the X press, the meter confirmed at 155 s, and
`c690 tail capture armed at the meter` followed by `trigger seen; sampling`.
The trigger file is non-consuming and was still present after the run.

Meter-window histogram (10 s, 395,784 commands in the step-2 window; this
run's window has 1,603,179 commands because the A730 leaf is captured at
every call):

| key | class | pairs | calls | jobs % | time ms | time % | avg us |
|---|---|---:|---:|---:|---:|---:|---:|
| `0x000000A7` | A730 body leaf `0xB830` | 64 | 1,270,050 | 79.22 | 11,505.25 | 47.16 | 9.06 |
| `0x0000002A` | 0x2A assembler | 64 | 147,466 | 9.20 | 4,444.82 | 18.22 | 30.14 |
| `0x0131A007` | A3E0 | 64 | 61,901 | 3.86 | 3,693.98 | 15.14 | 59.68 |
| `0x00000025` | 0x25 cull | 64 | 69,037 | 4.31 | 2,473.30 | 10.14 | 35.83 |
| `0x00000026` | 0x26 cull | 64 | 24,069 | 1.50 | 1,056.75 | 4.33 | 43.91 |
| `0x0131A017` | BB98 | 64 | 15,046 | 0.94 | 1,021.97 | 4.19 | 67.92 |
| `0x01314001` | A730 sub-selector prefix | 64 | 15,610 | 0.97 | 201.46 | 0.83 | 12.91 |

The `0xA7` key is the leaf call count, not the A730 command count; it is
the first measurement that includes the body time. The step-2 command-level
A730 share (32.41 %) was measured at the command boundary and is not
comparable to the leaf key.

## 4. Offline replay gate

`test_c690_tail_replay` (built against the patched generated module) on the
new spool:

| class | pairs | exact | diff bytes |
|---|---:|---:|---:|
| `0xA7` A730 body | 64 | **64** | 0 |
| 0x2A | 64 | **64** | 0 |
| 0x25 | 64 | **64** | 0 |
| 0x26 | 64 | **64** | 0 |
| 0x21 A3E0 (command boundary) | 64 | 0 | 944,089 |
| 0x21 BB98 (command boundary) | 64 | 0 | 1,333,359 |
| 0x21 A730 (command boundary) | 64 | 0 | 60,248 |

The 0x21 command-boundary rows are the known parser-byte/boundary issue
(`+2` at `0x1F0F3` or a following direct dispatch); they are not the A730
body corpus. The `0xA7` pairs are one leaf call each and are exact.

An earlier offline check also exercised the wrapper without a game run: the
step-2 A730 command pair was replayed with the capture env on, the wrapper
produced 64 `0xA7` pairs (108 leaf calls), and all 64 replayed byte-exact.
That is the unit-level end-to-end test of the wrapper.

## 5. Culls: why there is no kernel (kill decision)

The mission's cull kill criterion is "kept-index lists identical, zero
changes". The corpus side is ready: the step-2 run's **64/64 0x25 and 64/64
0x26 pairs replay byte-exact**, and the step-3 run repeats that. The missing
piece is the GPU kernel, and the body is not a compact screen test.

Evidence from the lifted module and the replay trace:

* `0xD080` is a 24-block tail-call state machine; the flattened exact stage
  `runtime/spu/c690_native_screen_stage.inc` is **1,551 lines**.
* The executed path is a per-vertex loop `0xD2B8` (88 iterations for 523
  candidate triangles on one captured pair) followed by
  `0xD4A8 -> 0xDA60 -> 0xDB58 -> 0xDB88 -> 0xDC48 -> 0xDD48`.
* `0xD2B8` is a skin/transform kernel: it reads the transform rows from
  LS `0x272F0`, the viewport row `{595,640,-335,360}` from LS `0x27330`,
  and uses `fma`/`shufb`/`rotqby` over 4-vertex groups. It is not a single
  4x4 matrix times vertex formula; the format/skin constants are
  load-bearing.
* The final blocks write the kept index list as **4 u16 per triangle**
  (three indices plus a zero pad) into the cull's index buffer and the
  caller finalises the count at `+0x114`.
* The command body also calls the shared `0x49D4`/`0xE6B8` allocators before
  `0xD080`; a full 0x132E0 port includes those.

A bit-exact GPU port therefore means translating ~1,550 lines of SPU
semantics (including `fma`/`selb`/`shufb`/`rotqby`, the `frest`/`fi`
normalize path, and the block state machine) to a GPU kernel, then the
0x132E0 glue. That is a multi-day C-to-GPU translation, not an offline
spike. **The cull GPU kernel is killed for this mission.** The culls stay on
the SPU; no kernel, diff or burst number is claimed. This also means the
full-tail option C cannot move the culls.

## 6. A3E0: why there is no kernel

The clean A3E0 command-boundary pairs (21/32 in the step-2 corpus, 0/64
exact in the step-3 replay because of the parser byte/boundary rule) show
the real path:

```
0x131A0 -> 0xBB98 -> 0x13178
         -> 0x13200 -> 0xA3E0 -> 0xA410 -> 0xA730 -> 0xA798
         -> 0xAF98/0xB238 (normalize) -> the pack chain
```

The statically reachable graph from `0xA8A0` (the A3E0 continuation) is
**19 functions / 2,493 lifted lines** with indirect branches. It is the same
class of heavily unrolled format/transform chain that blocked A730 in step
2, not a compact formula. The clean pairs are real meter inputs/outputs and
are the test corpus for a future kernel, but no kernel/diff/burst number is
claimed here.

## 7. Port plan and effort

### A730 body (`0xB830`)

* Scope: one 78-line self-contained leaf, four vertices per call, reads
  pointers from GPRs and writes LS; `fma`, `selb`, `shufb`, `rotqby`.
* Test corpus: the 64 `0xA7` pairs from this run (all exact).
* Work: a GPU kernel for the leaf plus a bit-exact differential test; then
  the caller loop `0xA798`/`0xA730` and the 0xA410 glue; then the pack/format
  path if the kernel must produce the final arrays.
* Honest estimate: **~2–3 days** for the leaf kernel and its 64-pair gate;
  **~5–8 days** for a bit-exact A730 command kernel including the caller and
  pack chain. The leaf is the only part that is clearly tractable; the
  command-level contract (output arrays, ring writes) still needs the
  caller.

### Cull `0xD080`

* Scope: the 24-block state machine, the `0xD2B8` transform loop, the
  `0xD4A8..0xDD48` index pack, and the `0x49D4`/`0xE6B8` allocator glue.
* Test corpus: 64/64 exact 0x25 and 64/64 exact 0x26 meter pairs.
* Honest estimate: **~1–2 weeks** for a bit-exact port and gate. A
  high-level "4x4 transform + viewport + triangle bbox" rewrite is not
  recommended: the captured `0xD2B8` path is not a single affine transform
  and would need a pair-by-pair differential to prove bit-exactness.

### A3E0

* Scope: the 19-function / 2,493-line chain above.
* Test corpus: the clean A3E0 pairs (21/32 in step 2; the step-3 command
  pairs are parser-contaminated and must be re-captured at the body with a
  wrapper like the A730 one).
* Honest estimate: **~1–2 weeks** after the A730 caller/leaf work, because
  it shares `0xA730`/`0xA798` but adds `0xAF98`/`0xB238`.

## 8. Coverage and running total

The step-2 coverage arithmetic is unchanged. The minimal 5 % job-fallback set
is `0x2A + 0x25/0x26 + A3E0 + A730` (BB98 is the 4.52 % fallback). The safe
set is all six keys plus the culls.

What is actually signed off as GPU-exact after this mission:

| class | kernel | exactness | burst cost | tail time signed off |
|---|---|---|---|---|
| 0x2A | `c690_2a_batch.comp` | bit-exact (phase 2a/2b) | 0.23–0.27 ms / 429 jobs | **23.1 %** (step-2 meter window; 18.2 % in step 3) |
| A730 body | none (unblocked) | 64/64 body pairs replay exact, no kernel | — | 0 |
| 0x25/0x26 | none (killed) | 64/64 pairs replay exact, no kernel | — | 0 |
| A3E0 | none (blocked) | clean pairs exist, no kernel | — | 0 |
| BB98 | none | — | — | 0 |

The **running total of tail time signed off as GPU-exact is 23.1 %** (0x2A
only). The A730 body is now measurable and exact but not ported; the culls
and A3E0 are not ported. The full tail (0x21 + culls + 0x2A) cannot move
with this result, so the 35.0 -> ~17.6 ms full-tail model is not reachable
yet. The next honest lever is the A730 leaf port (§7), not another capture.

## 9. Reproduction

```sh
# unit test (capture hook, A730 leaf pair)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_c690_tail_capture.py

# patch the generated module (0x2A/cull/0x21 wrappers + A730 leaf)
python3 tools/c690_ref/patch_cull_hooks.py
ninja -C ../recomp-work/build-play MyGameRecomp

# offline replay of the step-3 meter spool
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I ../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8 \
  -I runtime/spu -I include -I libs/video \
  -o /tmp/opencode/test_c690_tail_replay \
  ../mlb16-recomp/tools/replay/test_c690_tail_replay.c \
  ../mlb16-recomp/tools/replay/reg_10f78.c \
  ../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  runtime/spu/c690_corpus.c runtime/spu/c690_cull_fix.c \
  runtime/spu/c690_native_cull.c runtime/spu/c690_native_screen.c \
  runtime/spu/job_c690_native.c -lm
/tmp/opencode/test_c690_tail_replay /tmp/opencode/fncap2b3a730

# histogram
python3 tools/c690_tail_report.py /tmp/opencode/fncap2b3a730
```

The generated module is game-derived and is not committed; the capture
spools are in `/tmp/opencode` and are not committed.

## 10. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our
runtime/walker code and our own captures. The stage/corpus/differential
method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern
is from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors),
design reference only. No RPCS3 or other GPL emulator source was read or
cited.
