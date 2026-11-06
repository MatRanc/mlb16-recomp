# Native c690 Edge, step 2: corpus fix, 0xFDA0 kernel, and the live attempt

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. This step produced a bit-exact native replacement for
the c690 tree/cull stage (LS `0xFDA0`) and proved it on 1,604 captured stage
pairs, but **did not land it live**: the two attempts to enable it at the meter
corrupted the guest's RSX FIFO. The switch exists but is default-off and is not
enabled in either `play_windowed.sh` copy. Details and the exactness caveat are
below; the ranked plan is in section 7.

## TL;DR

* **Corpus fixed.** Stage snapshots are now capped on outermost `0xFDA0`
  entries (not the job index), live in a per-lane spool independent of job
  sampling, and the job file stays open through the submit epilogue. A run with
  the spool collected **403 / 400 / 400 / 401** pairs; the last one is a meter
  burst (frames 9116-9125). The job-file run recorded **249,886 PUTs** (was 0
  in step 1) - the epilogue is captured now.
* **Native kernel is exact offline.** A flattened C kernel of the whole
  `0xFDA0` state machine is **bit-identical (full 256 KiB LS + all 128 GPRs) on
  all 1,604 captured pairs**, and on the 12 job-file pairs, when built with the
  shipped runtime TU flags. The remaining mismatches found in-game were
  harness bugs and one compile-flag mismatch, all fixed and re-proved offline
  (section 5).
* **It is not fast enough.** Lifted stage vs native kernel on 12 pairs:
  median **136.7 -> 106.1 us, 1.30x** (range 1.28-1.35x). The step-1 plan
  expected 10x and set a 5x kill criterion. At 31.8 % of c690 that is roughly
  **-1.0 ms per worker per frame**, below the measurement floor of the frame
  A/B.
* **Live: rejected for now.** With `PS3RECOMP_C690_NATIVE_CULL=1` at the meter
  the guest corrupts its FIFO and stops flipping within ~10 s (two attempts,
  same signature). The exactness gate only covers the stage input classes in
  the corpus; the meter class that wedged was not captured. The switch is left
  in the tree, default-off, for the follow-up described in section 7.

## 1. Corpus (Part 1)

### 1.1 What changed

`runtime/spu/c690_corpus.c`:

* **Stage cap on `0xFDA0` entries.** `PS3RECOMP_C690_CORPUS_STAGE_JOBS` now
  means the number of outermost `0xFDA0` stage pairs, not a job-index bound.
  Recursion is still folded (`stage_depth` only counts the outermost entry).
* **Per-lane stage spool.** A stage pair is written to `<dir>/stage_NNN.bin`
  from the lane that ran it, independent of whether that job was sampled for a
  job file. This removes the coupling that limited step 1 to 4 pairs: the
  c690 work is spread over five lanes and most jobs are not sampled.
  Record format: 8-byte magic `C69STG01`, `u32 version`, then the same
  24-byte record header as the job file (`type`, `size`, `ea`, `lsa`, `aux`)
  and the `STAGE_IN`/`STAGE_OUT` payload (LS + GPR + PC/image). `ea` carries
  `(pair_seq << 32) | frame`, `aux` the SPU id.
* **Spread controls.** `..._STAGE_STRIDE` (every Nth outermost entry) and
  `..._STAGE_GAP_MS` (minimum global gap) let a run trade count for spread.
* **Epilogue capture.** `c690_corpus_job_end()` no longer closes the file: it
  records the opcode counts and keeps the file open until the next job on the
  same lane (or process exit), so the submit epilogue PUTs are written. This
  is the `hash_tail` model the c690 hash already used. `c690_corpus_dma()`
  accepts PUTs in that tail state and still ignores epilogue GETs.
* `PS3RECOMP_C690_CORPUS_TRIGGER` now also gates the stage spool. The atexit
  destructor closes the last tail per process.

`tools/c690_corpus_report.py` parses `stage_*.bin` and `--extract-stage`
writes `spool_<file>_<seq>_{in,out}.bin` next to the job-file pairs.

### 1.2 Numbers

| run | purpose | result |
|---|---|---|
| `corpus_edge2_20261103_050839` | job files + epilogue | 8,000 jobs, 21 layouts, **249,886 PUTs**, 8 job-file stage pairs |
| `corpus_edge2shadow_20261103_051831` | shadow + spool | 403 spool pairs (trigger fired early: frames 9166-9176) |
| `corpus_edge2shadow_20261103_052630` | shadow + spool | 400 pairs (frames 8066-8649) |
| `corpus_edge2shadow_20261103_053251` | shadow + spool | 400 pairs (frames 7965-8394) |
| `corpus_edge2shadow_20261103_053932` | shadow + spool | **401 pairs at the meter** (frames 9116-9125) |

The meter burst is real: `0xFDA0` entries arrive in a ~10-frame burst (about
400 pairs) when the mesh updates, then the meter goes quiet. Capturing 300+
pairs *spread over >= 2 s of meter frames* is not possible on the static meter;
the 401 meter pairs span ~0.3 s. The other runs spread pairs over 5-20 s but
not at the meter. The layouts in the meter burst were captured and validated.

Layout variants in the job-file run: 21, up from 19; all 0x25/0x26 cull
layouts observed in the window are represented in the spool.

## 2. What `0xFDA0` computes (plain words)

`0xFDA0` is the geometry job's **per-batch tree/cull and vertex-emit stage**.
It is not a single loop: it is a 45-block cooperative state machine spread
over LS `0xF500..0x105C0`; the reference in `tools/c690_ref/ref_c690_stage.c`
is the executable specification.

Entry state: `r1` is the LS stack, `r3`/`r4`/`r5` are batch/vertex/output
pointers, `r6`/`r7` an item range, `r8` the node-record base. The stage:

1. makes a 96-byte stack frame and saves the incoming register file;
2. runs two passes (`r6..r7` in both directions) over packed node records
   (`r8 + 8*i`). Each record carries counts and byte offsets extracted with
   `cwd/chd/cdd/cwx` and `shufb/rotqby`;
3. for each record transforms three vertices with the 4x4 matrix assembled in
   the stack frame (`fma`/`fms`, no contraction), builds two edge vectors,
   computes a normal with cross products, and normalizes with the SPU
   `frsqest` + `fi` Newton refinement (and `frest`/`fi` for the reciprocal);
4. tests the triangle's signed area / facing (`fceq` on the sign lane,
   `xor 0x80000000`) and writes the transformed vertices, the normalized
   normal and, when the facing test passes, the triangle's indices into the
   output region starting at `r5`. Only the kept items advance the output
   pointer;
5. for the larger/general path (`0x10190..0x105B8`) evaluates node bounds
   (`0xFA58`), expands leaves (`0xFCB8`/`0xFCE4`), emits vertex/index runs,
   and recurses into the two child ranges at `0x103A0` (two recursive
   `0xFDA0` calls). The recursion is the tree traversal; the previous bullets
   are the leaf work.

The outputs are therefore (a) transformed/normalized vertex floats and
indices in the cull output region (typically `0x2Bxxx..0x2E1xx`, 3.5-6.5 KiB
per call) and (b) the final GPR state; the stage also leaves its stack scratch
below `r1`, which the caller does not read.

## 3. Native kernel (Part 2)

`tools/c690_ref/gen_native_stage.py` flattens the reference into
`runtime/spu/c690_native_stage.inc`:

* one C function, `sr0..sr93` live in locals for the whole traversal;
* every continuation tail call becomes a `goto`; no `ctx->gpr` save/reload per
  block and no PC dispatch;
* indirect entries are direct labels; the 0xE630 path keeps its original abort;
* all SPU arithmetic and LS access still goes through `spu_helpers.h`, so the
  semantics are the lifted semantics by construction.

Two deliberate deviations, both proved on the corpus:

1. **Hardware denormal mode.** The native TU defines `SPU_FF_FTZ_HW` and runs
   under MXCSR `FTZ|DAZ` (saved/restored per call). The lifted helper masks
   every operand; the hardware does the same for denormals. The only remaining
   difference is the sign of a flushed zero, so the native helper canonicalises
   the sign of zero on the result instead (`spu__ff_ftz` in that TU). This is
   the single biggest win: it removes 3 ops per operand.
2. **Rounding mode at entry.** The stage body can leave a truncating MXCSR
   behind (the SPU's mode); the native run uses the mode captured at stage
   *entry*, not the exit mode.

`runtime/spu/c690_native_cull.c` adds the switch, the layout guard and the
shadow comparator:

* `PS3RECOMP_C690_NATIVE_CULL=1` / `PS3RECOMP_C690_NATIVE_SHADOW=1` (env and
  live control keys `PS3RECOMP_C690_NATIVE_CULL` / `..._SHADOW`);
* outermost-only tracking (critical for a live switch that can flip mid-stage),
  and a fallback counter for any entry that fails the layout guard;
* shadow runs both on the entry image and compares the full LS and all 128
  GPRs, logging the first mismatch with values and optionally dumping
  `entry/lifted/native` images (`PS3RECOMP_C690_NATIVE_DUMP`).

**Exactness gate (offline): 1,604 / 1,604 spool pairs and 12 / 12 job-file
pairs bit-identical, full LS and all GPRs.** The check uses the *shipped
runtime translation unit* (compiled with the library's flags, not a test
harness). The initial failures were: (a) the shadow comparator comparing
through a stale stash after a layout fallback, (b) using the exit MXCSR, and
(c) the runtime TU not defining `SPU_FAST_FLOAT`, so it used the exact-float
helpers while the lifted module uses the fast path. All three are fixed; the
two mismatching dumps now reproduce exactly with the shipped TU
(`recomp-work/c690_mismatch_vfy/`).

## 4. Benchmark (offline)

Lifted stage loaded from the game's own `spu_recomp.c.o` object, native from
the shipped `c690_native_cull.c.o`, 3,000 iterations per pair after 50 warm-up,
input LS/GPR restore subtracted:

| pair | lifted ns | native ns | speedup |
|---|---:|---:|---:|
| 4 step-1 pairs | 277,826 / 136,820 / 464,091 / 279,378 | 213,992 / 106,318 / 356,898 / 214,220 | 1.29-1.30x |
| 8 run-2 pairs | 66,466-137,327 | 49,678-105,975 | 1.28-1.35x |
| **median (12)** | **136,677** | **106,146** | **1.30x** |

Per-stage saving is 17-107 us; the kernel is compute-bound in `spu_fi`,
the `frest`/`frsqest` LUT and `shufb`, so the flattened form alone cannot
reach the 5x kill line. At the c690 profile's 31.8 % tree/cull share
(4.58 ms/worker/frame at the 14.4 ms shape), 1.3x is about **-1.0 ms per
worker per frame**, inside the noise of the frame A/B.

## 5. Hook, shadow and live A/B (Part 3)

* **Wrapper.** The generated lifted module's `0xFDA0` function has the same
  6-line corpus wrapper as step 1, now also calling
  `c690_native_cull_maybe()` / `_enter` / `_leave`. It is regenerated game
  data under `recomp-work/spu_lifted/` and is not committed.
* **Shadow.** In-game shadow found three problems, all in the harness/TU
  (section 3): a fallback compared a stale stash, the native run used the exit
  MXCSR, and the runtime TU's float path did not match. After the fixes the
  exact shipped TU is bit-identical on every captured pair and on both
  failing dumps. A final in-game shadow run was not completed (the live
  attempts were stopped first).
* **Live A/B.** Two attempts, both at the meter with the live control file
  (A = cull off, B = cull on, 6 x 10 s per arm):
  * attempt 1 never ran the native kernel (a depth-tracking bug left the
    outermost flag set after the first lifted entry);
  * attempt 2 ran it and **wedged the guest**: `[cellGcmSys] FIFO resync
    (jump-park) 0x0136E970` with `w=2136E970` (the FIFO pointer written as a
    command word) and `STOP: no flips during segment 1 (B)` ~10 s after the
    switch. The same signature appeared in the first attempt, which also
    flipped the switch mid-stage.
  The run was stopped there. Per the mission's "same failure twice" rule no
  third attempt was made, and the switch was **not** enabled in
  `play_windowed.sh`.

What this means: the kernel is exact on the captured stage inputs, including a
401-pair meter burst, but the meter input class that the live arm hit is not
proven. The likely gap is a cull layout that only occurs after the meter burst
(the stage is bursty; the corpus cap filled inside the first burst), or a
resume path that does not enter at `0xFDA0`. Both are measurable with the
fixed spool: trigger at the meter with a 10x cap and a gap, and shadow for
60 s.

## 6. Instrumentation caveat

The step-1 corpus wrapper lived in generated game data. This step keeps the
same arrangement and documents it rather than teaching the lifter to emit the
hook. The runtime kernel, switch and corpus module are committed in
`ps3recomp-play`; the wrapper snippet is in the step-1 README and
`tools/c690_ref/`.

## 7. Updated per-stage plan (Part 4)

Measured stage speedup and the next order. "ms" is per worker per frame at the
step-1 14.4 ms c690 shape; union ms is the five busy lanes.

| # | stage | share | ms/worker | this step | next target | expected |
|---:|---|---:|---:|---|---|---:|
| 1 | **tree/cull** `0xFDA0`+`0xFA58`+`0xFCB8`+`0xF520` | 31.8 % | 4.58 | **1.30x, -1.0 ms/worker; not live** | 5x after a low-level rewrite of the `fi`/`frest`/`shufb` inner loops, or accept | 4.58 -> 0.9 (5x), union 35 -> ~26 ms |
| 2 | **unpack/pack** `0x6D68`,`0x8160`,`0x7C18`,`0x7998`,`0x6860`,`0x7540` | 23.5 % | 3.38 | - | native 5-10x | 3.38 -> 0.4-0.7; union -> ~23 ms |
| 3 | **skin/transform** `0xD4A8`,`0xD2B8`,`0xB268`,`0xC9D0`,`0xA798` | 13.6 % | 1.96 | - | native 5-8x | 1.96 -> 0.3-0.4; union -> ~20 ms |
| 4 | **normalize** `0xAF98`,`0xB238` | 8.2 % | 1.19 | - | native 5x | 1.19 -> 0.24; union -> ~19 ms |
| 5 | **format/parser** `0xE750`,`0x10700`,`0x10758`,`0x5BC0` | 5.5 % | 0.79 | - | native 3-5x | union -> ~18.5 ms |
| 6 | **codegen tail** shufb/LS/float/dispatch | tail | 0.5-1.5 | - | 1.2-1.5x | union -> ~18 ms |
| 7 | **hand-off/overlap** | - | - | - | after c690 < 19.5 ms main cycle | floor ~20-21 ms |

Order for the next missions: **finish proving tree/cull on the live meter
class** (cheap: the corpus spool already exists; one shadow run with a 10x cap)
and only then decide whether to invest in a hand-written vector version; if
the 1.3x flattened form cannot be made live-safe, go to **unpack/pack** next
because it is 23.5 % with a much simpler data flow and a 5-10x target, then
skin/transform, then normalize. Do not reorder on this stage's 1.3x.

## 8. Runs and deviations

Nine launches were used, far above the planned three (details in the progress
entry): one corpus attempt lost to a watcher path bug, one corpus retry that
produced the epilogue data, four shadow/spool runs while fixing the shadow
harness and the TU float mode, one A/B lost to a depth bug, and one A/B that
wedged. All game runs were on the standard `play_windowed.sh` environment
(`TEX_DIRTY=1`, `PIPE_MEMO=1`) plus `METER=1 SKIP_CINE=1`, governor
`performance`; no run was started while another game was live.

## 9. Caveats

* The "300 pairs over >= 2 s at the meter" target is not reachable on the
  static meter: the stage is bursty (401 pairs in 10 frames, then quiet). The
  1,604-pair corpus is spread over four runs and several scenes; only the last
  401 are meter frames.
* The exactness result is for the *stage* (full LS + GPRs), not for the whole
  job. The job ABI, ring allocation, submit epilogue and DMA remain lifted.
* The live wedge was not root-caused to a specific instruction; the native
  kernel's output is identical on every input actually captured.
* The fallback path was exercised only in unit tests; the live meter class
  never reached a fallback count in a completed run.
