# Native c690 Edge, step 1: perf profile, corpus, and the 0xFDA0 C reference

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. One successful meter launch (`edge1prof_20261103_012604`):
`METER=1 SKIP_CINE=1 WCB=2 PIN=1`, x11, the standard `play_windowed.sh`
environment (including `PS3RECOMP_TEX_DIRTY=1` and the four cheap-win live
switches), `PS3RECOMP_FLIP_LOG=1`, `PS3RECOMP_C690_CORPUS`, then a 20 s
whole-process `perf record` after the corpus finished. Analysis and
instrumentation only: no optimisation and no live-stage hook landed.

## TL;DR

* **Per-flip frame time in the 20.008 s perf window:** n=640 intervals,
  **p50 34.05 ms, p90 43.58, p99 49.35, mean 31.19, std 11.20 ms**, 32.0 fps
  (33.4 % of frames < 25 ms, 40.0 % 35-45 ms, 10.6 % > 45 ms).
* **The new perf profile re-confirms the old gdb ranking** but moves the
  cull-test blocks up: `0xFDA0` 19.7 %, `0xD4A8` 8.4 %, `0x6D68` 6.4 %,
  `0xFA58` 5.9 %, `0xAF98` 5.7 %, `0x8160` 5.1 %, `0x7C18` 5.0 % (already
  native), `0xD2B8` 4.9 %, `0xFCB8` 3.2 %, `0xF520` 2.8 %. The
  **tree/cull family (`0xFDA0` + `0xFA58` + `0xFCB8` + `0xF520` + the small
  `0xFAxx`/`0xFCxx` helpers) is 31.8 % of c690**, ~4.6 ms per worker per
  frame.
* **Corpus:** 2,000 jobs over 227 meter frames, 19 opcode-layout variants,
  12,138 DMA GETs (1,217 B/job, 16-1,936 B each). **0 PUTs were recorded**
  because the output DMA is in the submit epilogue after the job-end command;
  the corpus (like the c690 wall model) closes at the end command. The
  stage snapshots carry the output state for the 4 cull jobs.
* **Reference:** a 1,799-line executable C reference for the full `0xFDA0`
  state machine is **4/4 bit-exact** on the captured stage pairs: zero
  differing local-store bytes and zero differing GPRs per pair. It is in
  `ps3recomp-play/tools/c690_ref/`.
* **Plan order:** tree/cull -> unpack/pack (23.5 %) -> skin/transform (13.6 %)
  -> normalize (8.2 %) -> format/parser/other. Native tree/cull is expected to
  remove ~4.1 ms per worker (~20 ms of lane union) if it reaches 10x the lifted
  stage; at the measured 0.55 elasticity that is ~3-4 ms of p50, with the
  19.5 ms main-ref cycle as the floor.

## 1. Run and method

| run | tag | result |
|---|---|---|
| 1 | `edge1prof_20261103_011506` | stopped by the harness: `pgrep -x MyGameRecomp` stopped matching after the runtime renamed the main thread; no data |
| 2 | `edge1prof_20261103_011833` | corpus hook never enabled (the `g_c690_corpus_on` gate was set inside the first hook it gated); perf recorded during boot. No corpus |
| 3 | `edge1prof_20261103_012604` | **used here.** Meter at 156 s; corpus trigger at +14 s; 2,000 jobs and `DONE` in 5 s; perf 20 s after the corpus shut its hooks off |

Three launches total (one infrastructure failure, one instrumentation failure,
one retry that produced the data). The run stopped the game cleanly after the
perf window; no other game was running.

Perf: `perf record -F 999 --call-graph=dwarf,16384 -p <game>` for 20.008 s.
1.9 GB of perf data; 116,283 user samples plus 66 kernel samples. The report
below uses the first user IP in each sample (`perf script --max-stack 1`).
Function names are the lifted symbols; `jobmod_c690c2f7b09a_eF5B8_spu_func_00XXXXXX`
maps to LS `0xXXXXXX`.

The c690 wall numbers are the last periodic `PS3RECOMP_C690_STATS` report
before the corpus `DONE` (elapsed 172.5 s, 515,091 jobs, mean **163.4 µs**,
p50 122 / p90 249 / p99 972, max 3.30 ms). The perf window itself ran with
c690 stats and corpus hooks off, so the periodic `clock_gettime`/file I/O do
not pollute the profile. Stage ms below are the profile share scaled to the
old 14.4 ms/worker/frame c690 wall shape; they are estimates, not per-stage
timers.

## 2. Perf profile

### 2.1 Per-flip frame time (required measurement note)

From `PS3RECOMP_FLIP_LOG` `[fliplog]` show timestamps inside the perf data
window (monotonic 92246.892075-92266.900678 s), **per game flip**, not the
every-10th `[flip]` decimation:

| metric | value |
|---|---:|
| intervals | 640 |
| fps | 32.0 |
| p50 | **34.05 ms** |
| p90 | 43.58 |
| p99 | 49.35 |
| mean | 31.19 |
| stddev | 11.20 |
| min / max | 6.04 / 69.40 |
| < 25 / 25-35 / 35-45 / > 45 ms | 214 / 102 / 256 / 68 frames |

The short/long alternation is still present, but the current build (texture
dirty revalidation, cheap wins) has more short frames than the 2026-11-02
timeline run. Present-side (`show`) deltas are p50 35.16, p90 45.63,
p99 55.57; the guest-flip deltas above are the primary number.

### 2.2 Thread groups (user samples)

| group | samples | share |
|---|---:|---:|
| `spurs pm` (5 c690 lanes + policy) | 80,627 | 69.3 % |
| `rsx walker` | 12,401 | 10.7 % |
| `ppu main` | 7,962 | 6.8 % |
| `spurs kernel` | 4,046 | 3.5 % |
| `mangohud-nvidia` | 3,555 | 3.1 % |
| `ppu Kinematics` | 2,306 | 2.0 % |
| `ppu IMFE PPU Th` | 1,278 | 1.1 % |
| `ppu snd_stream_` | 1,207 | 1.0 % |
| `present` | 341 | 0.3 % |

Of the `spurs pm` samples, **48,116 (59.7 %) are c690 lifted/native code**;
the rest is runtime glue: syscalls/futex 6.7 %, `__memmove` 2.9 %,
`spu_lifted_lookup` 2.0 %, `spu_indirect_branch_slow` 2.0 %,
`spu_drain_call` 1.5 %, `pthread_cond_broadcast` 1.3 %, other 25 %.

### 2.3 c690 stages, new ranking

Shares are of the 48,116 c690 user samples (native `0x7C18` included). The
ms column scales the share to 14.4 ms/worker/frame.

| rank | LS | share | ms/worker/frame | role |
|---:|---|---:|---:|---|
| 1 | `0xFDA0` | **19.70 %** | 2.84 | tree/cull state machine entry and hot continuation blocks |
| 2 | `0xD4A8` | **8.40 %** | 1.21 | matrix-palette skin/transform variant |
| 3 | `0x6D68` | **6.37 %** | 0.92 | 12/12/11 SNORM unpack |
| 4 | `0xFA58` | **5.85 %** | 0.84 | node/interval bounds test (tree/cull family) |
| 5 | `0xAF98` | **5.72 %** | 0.82 | normalize (`frsqest` + `fi`) |
| 6 | `0x8160` | **5.05 %** | 0.73 | snorm8 pack |
| 7 | `0x7C18` | **4.98 %** | 0.72 | snorm16 pack; **already native**, still hot |
| 8 | `0xD2B8` | **4.85 %** | 0.70 | skin/transform variant |
| 9 | `0xFCB8` | **3.15 %** | 0.45 | leaf evaluation (tree/cull family) |
| 10 | `0xF520` | **2.79 %** | 0.40 | LS-heavy index/stream block (tree/cull family) |
| 11 | `0xE750` | 2.24 % | 0.32 | format-context setup |
| 12 | `0xB268` | 2.18 % | 0.31 | normalize variant |
| 13 | `0x7998` | 1.91 % | 0.28 | gather/write (0x2A assembler) |
| 14 | `0xC9D0` | 1.34 % | 0.19 | transform/normalize variant |
| 15 | `0x6860` | 1.34 % | 0.19 | unpack driver |
| 16 | `0x4100` | 1.31 % | 0.19 | stream helper |
| 17 | `0x10700` | 1.27 % | 0.18 | command parser |
| 18 | `0x7540` | 1.11 % | 0.16 | pack helper |
| 19 | `0xA798` | 1.11 % | 0.16 | 3x4 affine transform |
| 20 | `0x10758` | 1.09 % | 0.16 | command parser helper |
| 21 | `0xA8A0` | 1.08 % | 0.16 | transform variant |
| 22 | `0x5BC0` | 0.97 % | 0.14 | stream/command block |
| 23 | `0xA730` | 0.84 % | 0.12 | affine setup |
| 24 | `0x76B8` | 0.81 % | 0.12 | gather/write |
| 25 | `0xB830` | 0.66 % | 0.09 | transform variant |

Family totals (same scale):

| family | share | ms/worker/frame |
|---|---:|---:|
| tree/cull (`0xF500..0x105C0`) | **31.80 %** | **4.58** |
| unpack/pack (`0x6000..0x8500`) | 23.48 % | 3.38 |
| skin/transform (`0xD000..0xDA00`) | 13.64 % | 1.96 |
| normalize (`0xAF00..0xB300`) | 8.24 % | 1.19 |
| format context (`0xE600..0xEA00`) | 2.54 % | 0.37 |

The old gdb profile had `0xFA58` at 3.3 % and `0xF520` at 2.4 %; the new
sample puts them at 5.9 % and 2.8 %. `0x7C18` dropped from 5.7 % to 5.0 % but
is still the sixth-largest c690 function; the native body has not removed its
hot spot. `0xE750` and `0x7998` are new in the top 15.

### 2.4 Hot basic blocks

Host offsets inside the lifted functions (sample count in the 20 s window).
The instruction at each offset is AVX2 byte-permute / LS access / float
select, matching the earlier static op census.

| function | offset | n | instruction at the offset |
|---|---:|---:|---|
| `0xF520` | +0x6D8 | 810 | stack spill next to `vmovdqu` LS read / `vpshufb` |
| `0xE750` | +0x12D3 | 623 | `vpshufb xmm0,xmm0,xmm15` (descriptor bytes) |
| `0xFDA0` | +0x2646 | 309 | `vpshufb xmm0,xmm3,xmm13` + `vpand` |
| `0xFDA0` | +0x2AD0 | 188 | `vandps` + `vpcmpgtd` (mask/select) |
| `0xFA58` | +0x16E7 | 185 | `vandps` + `vpcmpgtd` + `vpand` (interval test) |
| `0xFA58` | +0x163F | 178 | `vmovdqu` LS read + `and 0x3fff0` |
| `0xF520` | +0x6B4 | 157 | `vmovdqu` LS read + stack spill |
| `0xFDA0` | +0x2A1E | 132 | `vmovdqa` spill + `vmovdqu` + `vpshufb` |
| `0xFDA0` | +0x0A30 | 127 | `vpshufb` + `vmovdqu` |
| `0xFA58` | +0x09D4 | 122 | `vpblendvb` + `vmovups` |
| `0x8160` | +0x067D | 116 | `vmovdqu` + `and 0x3fff0` + `vpshufb` |
| `0xFDA0` | +0x098B | 116 | `vpshufb` + LS load |
| `0xFDA0` | +0x09E2 | 112 | `vpshufb` |
| `0xFA58` | +0x0972 | 112 | LS load + mask |

The tree/cull cost is therefore not one inner loop: it is a spread of
128-bit LS reads, `shufb` permutations, float compare/select and the
`vpblendvb` mask path across `0xFDA0`, `0xFA58`, `0xFCB8` and `0xF520`.

### 2.5 walker

Top `rsx walker` self symbols (12,401 samples): `[unknown]` 3,427 (27.6 %;
unsymbolized user IPs, mostly the backend/JIT regions), `rsx_vertex_remap_build`
2,372 (19.1 %), `__memmove_avx_unaligned_erms` 621 (5.0 %),
`gcm_rsx_process_fifo_unlocked` 553 (4.5 %), `__syscall_cancel_arch_end` 492
(4.0 %), `sink_end_work` 393 (3.2 %), `rsx_fp_structural_hash` 356 (2.9 %),
`eng_pipeline_get` 222 (1.8 %), `sink_bind_textures` 211 (1.7 %). The
texture-dirty change removed the old texture hashing hot spot; the walker is
now mostly vertex remap, FIFO decode and memmove. The c690 work is not the
walker's own bottleneck; it is the producer the walker waits on.

## 3. Corpus

### 3.1 Format

`PS3RECOMP_C690_CORPUS=<dir>` (default off). One file per sampled job,
`<dir>/job_<index>_spu<id>.c69`. The header is 40 bytes
(`"C69CORP1"`, version, job index, frame, SPU id, image id, descriptor EA,
start ns), then records of
`u32 type, u32 size, u64 ea, u32 lsa, u32 aux, <size bytes>`:

| type | payload |
|---|---|
| 1 JOB_DESC | bytes at the job's `r3` (the game passes an LS pointer, so this is **not** a guest EA; the descriptor is in the stage LS at `0x1330`) |
| 2 JOB_GPR | all 128 GPRs at job entry |
| 3 GET | guest bytes read by an MFC GET, with EA and LS destination |
| 4 PUT | LS bytes written by an MFC PUT, with EA and LS source (the epilogue limitation below) |
| 5/6 STAGE_IN/OUT | full 256 KiB LS + 2 KiB GPR + pc at the outermost `0xFDA0` entry/exit |
| 7 OPCODES | 256 command counts |
| 8 JOB_END | wall ns and recorded bytes |

Env: `PS3RECOMP_C690_CORPUS_JOBS` (2000), `..._STAGE_JOBS` (20),
`..._STRIDE` (1), `..._TRIGGER` (path). The step-1 run used stride 32 to
spread 2,000 captured jobs over ~5 s of meter frames. The corpus directory is
game data and stays under `recomp-work/` (not committed).

### 3.2 Counts and layout variants

Meter window: 2,000 jobs, 227 frames. 12,138 GETs, 2,434,288 bytes total,
mean 201 B/GET and 1,217 B/job, sizes 16-1,936 B, guest EA range
`0x01246880..0x1A0CAEB0`. **PUTs: 0.**

The 2,000 jobs fall into **19 opcode layouts** (all share the same descriptor
in LS `0x1330`). The top variants:

| jobs | opcode mix |
|---:|---|
| 344 | `04,07,08,0A*4,0C,13,1F,21,25,27,2A` |
| 339 | `04,07,08,0A*4,0C,13,1F,21,26,27,2A` |
| 311 | `04,07,08,0A*5,13,1F,21,2A` (no cull) |
| 252 | `04,07,08,0A*2,0C,13,1F,21,25,27,2A*2` |
| 210 | `04,07,08,0A*7,0C,13,1F,25,27,2A` |
| 115 | `04,07,08,0A*4,0C,13,1F,21,27,2A` |
| 93 | `04,07,08,0A*6,0C,13,1F,25,27,2A` |
| 81 | `04,07,08,0A*5,0C,13,1F,25,27,2A` |

Every job has one `04`, `07`, `08`, `13`, `1F` and `27`; the variable is the
number of `0A` binds (2-10), the presence of the `25`/`26` cull and `21`
emit, and the `2A` count (1-2). Only the `25`/`26` layouts enter `0xFDA0`.

### 3.3 What the corpus shows, and what it does not

* **The job descriptor is in LS, not guest memory.** At job entry
  `r3 = 0x1330`; `vm_base+0x1330` is zero. The descriptor bytes are in the
  stage image at LS `0x1330` (`00 00 00 02 00 00 00 08 ... 01 24 3E 00 ...`).
  The JOB_DESC record is therefore a zero page; the stage snapshot is the
  authoritative descriptor capture.
* **Output DMA is in the submit epilogue.** The job-end command `0x12AB8`
  closes the corpus, so the PUTs that follow it (the ring/vertex submit) were
  not recorded. This is the same boundary the c690 wall model uses. The
  stage snapshots do capture the cull/vertex output state for the 4 cull jobs
  (LS changes at `0x2Bxxx..0x2E1xx`; the `0x2F000` page is the vertex-ring
  image).
* **Only 4 stage pairs.** The stage capture was limited to the first 20
  sampled jobs, and only 4 of those were cull jobs (`0x25`/`0x26`). They are
  frames 9815-9817, so they are close in time; a later corpus should key the
  stage cap on `0xFDA0` entries, not on job index, to get 20 spread cull
  samples. The 4 are enough to lock the reference semantics, not enough to
  estimate layout coverage.

## 4. The 0xFDA0 C reference

`ps3recomp-play/tools/c690_ref/`:

* `ref_c690_stage.c` - 1,799 lines covering the `0xFDA0` state machine and
  its 45 continuation blocks. It was transliterated from the lifted module
  with the runtime plumbing removed (no interrupts/dispatch trampoline/call
  brackets/DMA); indirect calls go through a PC dispatcher. LS and arithmetic
  still use `spu_helpers.h`, the same implementation the game runs, so the
  comparison is bit-exact rather than "within tolerance".
* `test_ref_c690_stage.c` - loads a captured LS+GPR image, runs the reference,
  and compares the full 256 KiB LS and all 128 GPRs with the captured exit.
* `run_corpus_test.sh` - extracts the stage pairs from a corpus directory,
  builds with the game's exact SPU flags
  (`-DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS -mavx2 -mfma -ffp-contract=off`),
  and runs every pair.

Result on the step-1 corpus: **4/4 bit-exact**, 0 differing LS bytes and 0
differing GPRs in every pair. The stage writes/keeps 225-409 16-byte LS lines
(3.6-6.5 KiB) per job in the cull output region; the reference reproduces
those and every scratch byte the lifted stage leaves behind.

The native rewrite should preserve the helper semantics (no FMA contraction,
SPU rounding, SPU denormal/FTZ behaviour). The reference is deliberately not
optimised: it is the executable specification and the fallback oracle.

Attribution: the stage split and corpus/differential method follow NHL Legacy
Native (MIT, (c) 2026 HeckYouMan). No NHL code was copied into the reference;
the c690 layout and cull math are our own. See
`ps3recomp-play/THIRD_PARTY_NOTICES.md`.

## 5. Ranked plan for the native c690 path

Ordering is expected **ms saved at the meter** per unit of effort, using the
new profile. "Family" means the stage plus its continuation/helper blocks.
All shares are of the 48,116 c690 samples; ms is per worker per frame at the
14.4 ms wall shape. Union ms is the five busy lanes.

| # | stage | share | ms/worker | native target | union if 10x | effort | exactness gate |
|---:|---|---:|---:|---|---:|---|---|
| 1 | **tree/cull** `0xFDA0`+`0xFA58`+`0xFCB8`+`0xF520`+helpers | 31.8 % | 4.58 | 10x | 35 -> ~27 ms; p50 -3 to -4 | XL | corpus full-LS+GPR diff (4/4 now); in-game shadow hash; fallback <= 10 % |
| 2 | **unpack/pack** `0x6D68`,`0x8160`,`0x7C18`,`0x7998`,`0x6860`,`0x7540` | 23.5 % | 3.38 | 5-10x | 27 -> ~23 ms | M/L | `ref_spu_func_7c18.c` hash; snorm byte equality; per-job output hash |
| 3 | **skin/transform** `0xD4A8`,`0xD2B8`,`0xB268`,`0xC9D0`,`0xA798`,`0xA8A0` | 13.6 % | 1.96 | 5-8x | 23 -> ~20 ms | L | positions <= 1e-5; packed bytes <= 1 LSB; hash |
| 4 | **normalize** `0xAF98`,`0xB268`,`0xB238` | 8.2 % | 1.19 | 5x | 20 -> ~19 ms | M | `frsqest`+`fi` bit-exact; per-job hash |
| 5 | **format/parser** `0xE750`,`0x10700`,`0x10758`,`0x5BC0` | 5.5 % | 0.79 | 3-5x | 19 -> ~18.5 ms | M | descriptor/opcode capture; per-job hash |
| 6 | **remaining lifted codegen** dispatch/shufb/LS/float (AVX2 exact, PGO) | tail | 0.5-1.5 | 1.2-1.5x | 18.5 -> ~18 | M | SPU float/LS suites; c690 wall -5 % kill |
| 7 | **hand-off/overlap** once c690 < 19.5 ms main cycle | - | - | - | floor ~20-21 ms frame | M | frame p50/p99 A/B |

**Hook choice.** Replace the stage at the `0xFDA0` **outermost call**, not the
whole job. The lifted job framework (entry `0x135B8`, command parser
`0x10700`, index decode, ring allocation `0xE0A8`, submit `0x12AB0` region)
stays lifted, so the job ABI, ring/slot state and epilogue PUTs remain
untouched. The native stage receives the same `spu_context`: read the cull
batch/context from LS (the captured entry has `r3 = 0x1330`, `r6/r7` the range
and `r4/r5/r8/r9` the batch pointers), write the same LS output and return
registers. **Fallback rule:** before running, check the c690 layout key (the
LS `0x1330` descriptor words, the batch range and the vertex format). If it is
not the captured layout, call the lifted `0xFDA0` body and count a fallback;
log the first 16 with `[C690-NATIVE] lifted: <reason>`. Stop and fix coverage
if fallbacks exceed 10 % of cull jobs.

**Exactness gates (each stage):**

1. Offline corpus: full LS + 128 GPRs bit-exact (the test above), then extend
   to a 20-cull-job corpus.
2. In-game shadow: for sampled jobs run the lifted stage after the native one
   on a copy of the entry LS and compare the output hash and the written LS
   ranges; count mismatches. A stage is accepted only at 0 mismatches.
3. `PS3RECOMP_C690_HASH` per-job input/output hash against the lifted path;
   duplicate-input jobs must produce identical output.
4. Replay/visual gates: the meter and pitch replay hashes unchanged, and a
   scripted pitch frame diff with the native stage on/off.

**A/B that proves each stage:** one meter run, native stage off/on as the two
arms, `PS3RECOMP_FLIP_LOG=1` per flip, 6 x 10 s per arm, 2 s dropped. Report
per-flip p50/p90/p99/stddev, `[c690-stats]` job wall, the walker's c690 wait,
and the fallback count. Kill criterion: stage < 5x faster, or fallback
> 10 %, or p50 within noise on a stage that should have moved it.

**Expected path:** stages 1-3 take the c690 union from ~35 ms to ~23 ms and
per-worker busy from ~14.4 to ~9 ms; stage 4 and the codegen tail take it to
~19 ms; at that point the c690 union is below the 19.5 ms main-ref cycle and
the frame floor is the main/ref hand-off, not c690. The measured 0.55
elasticity says do not expect the full 20 ms of lane-union saving in p50.

## 6. Caveats and deviations

* **Three launches, not two.** Run 1 failed on process detection
  (`pgrep -x` after the runtime renamed the main thread); run 2 failed because
  the corpus enable flag was set inside the hook it gated, so no job was
  sampled; run 3 is the retry and produced all data. The first failure is
  infrastructure, the second is instrumentation; both were fixed before the
  successful run.
* **The 20 s perf window has c690 stats off.** The corpus `DONE` deliberately
  cleared `g_c690_stats`/`g_c690_corpus_on` so the profile is free of
  `clock_gettime` and file I/O. The c690 wall numbers are from the last
  periodic report (172.5 s, mean 163.4 µs), and stage ms are scaled shares.
* **0 PUTs** - the submit epilogue is after the job-end command. Capturing it
  needs the corpus to stay open until the next job on the lane (the
  `hash_tail` model the c690 hash already uses).
* **4 stage pairs** - the stage cap was on job index, not on `0xFDA0`
  entries. A follow-up corpus should key it on the stage.
* **No live hook** was added in this mission, as requested.
* **Stage-snapshot instrumentation is in the generated lifted tree.**
  The corpus stage hooks need a 6-line wrapper around the lifted
  `0xFDA0` body (it calls `c690_corpus_stage_enter`/`_exit`, and
  tail-calls the body when the corpus is off). It lives in
  `recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`
  and is deliberately not committed: that tree is regenerated game
  data. Reproduce it by renaming the generated body to `_body` and
  adding the wrapper shown in `tools/c690_ref/README.md`.
* Perf data is 1.9 GB under `recomp-work/perf_edge1_20261103_012604.data`
  (not committed). The corpus is 21 MB under
  `recomp-work/corpus_edge1_20261103_012604/` (19 MB of job files plus
  2.2 MB of extracted stage images; not committed).
