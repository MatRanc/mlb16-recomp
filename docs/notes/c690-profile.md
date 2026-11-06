# c690 profile: where the Edge job time goes, and the ceilings

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Meter runs use `METER=1`, `SKIP_CINE=1`, `WCB=2`,
`PIN=1`, `CUBE_RT=1`, `PACE=1`, `PRESENT_NEW_ONLY=1`, x11, and the four
cheap-win live switches. Instrumentation added for this mission is
env/live-gated and default-off; no optimisation landed.

## TL;DR

* **The c690 job is 165 µs mean (p50 121, p90 248, p99 981 µs) and there are
  437 jobs/frame.** That is **72 ms/frame cumulative across the five busy
  lanes, 14.4 ms per worker**. The "35-40 ms" span is the frame / the union
  of lane busy time on the long frames, not each lane's busy time.
* **Top five hot functions (210 in-job gdb samples):** `0xFDA0` tree
  traversal/culling 19.0 %, `0xD4A8` skin/transform 8.1 %, `0x6D68` SNORM
  unpack 7.1 %, `0xAF98` normalize 6.2 %, `0x7C18` snorm16 pack 5.7 %
  (**already native**, `runtime/spu/job_c690_native.c:110`).
* **Category split while a lane is running:** 90.5 % lifted/native job
  compute, 5.2 % branch dispatch/indirect lookup, 1.9 % MFC/DMA + memcpy,
  1.4 % coherency/lockline, 1.0 % channel/clock. Inside the compute, the
  static op census is dominated by `shufb`/byte permutes (23 % of weighted
  ops), LS 128-bit reads (15 %), integer adds (12 %), LS writes (9 %) and
  float ops (16 %).
* **Only five lanes can run c690, by the game's own SPURS config:**
  `MeshJobs0/1` set `maxContention=5` and `priority[5]=0`. Overriding the
  sixth lane is not legal in practice: it wedged the guest within ~10 s.
* **Whole-job caching is dead:** in the meter window 40.3 % of sampled jobs
  had byte-identical input (25.9 % from an earlier frame), but only **0.07 %**
  had identical output. The output carries ring/slot state.
* **The ceiling probe works:** adding a synthetic 50 µs to every job
  (≈ +4.4 ms/worker/frame) cost 1.95 fps (CI 0.63-3.28) and raised the
  walker's SPU wait 6.7 -> 8.9 ms. Frame time follows c690 lane time with
  roughly 0.55 elasticity; below the main ref-wait cycle (~19.5 ms) further
  c690 cuts stop lowering p50.

## Runs and method

| run | tag | result |
|---|---|---|
| 1 | `ab_live_c690prof_20261102_235817` | killed at 126 s by the orchestration tool's 120 s command timeout; no data |
| 2 | `ab_live_c690prof2_20261103_000050` | complete. meter at 209 s; 35 s settle; six 10 s arms, A = `C690_SPIN_US=0`, B = 50 |
| 3 | `ab_live_c690prof3_20261103_000803` | meter at 158 s; A = lane6 off ran 8 s; B = lane6 on wedged (no flip) and the run stopped at rc=7 |

Three game launches total (the mission's cap). `perf` is not installed and
`perf_event_paranoid=2`; no system setting was changed, so the planned
`perf record` was replaced by the existing `recomp-work/ab-tools/psample.c`
(PERF_COUNT_SW_CPU_CLOCK, `exclude_kernel=1`), which is allowed at
paranoid=2. The watcher that should have driven psample selected the
`timeout` wrapper instead of the game in run 3, so **no fresh pc-sampling
profile exists**. The per-function data below is from the existing gdb stack
profiles `recomp-work/prof_spu2.prof` and `prof_spu3.prof` (built from the same
lifted job module) plus the in-process c690 exclusive-time counters and a
static op census of the generated code.

## 1. Cheap checks

### 1.1 Thread placement under `PIN=1`

`runtime/platform/thread_affinity.c:98-183` maps:

| thread | CPU(s) | core |
|---|---|---|
| `rsx walker` | 0 | core 0 (sibling 6 left idle) |
| `ppu main` | 1 | core 1 (sibling 7 left idle) |
| every other named thread, including all six `spurs pm`, `present`, `ppu DisplayFlip`, `spurs kernel`, `gcm intr`, `audio mix` | mask `0xCFC` = 2,3,4,5,8,9,10,11 | cores 2-5, both SMT siblings |

The Ryzen 5 2600X sibling pairs are (0,6), (1,7), (2,8), (3,9), (4,10),
(5,11). The walker and main get whole cores; **the five busy c690 lanes,
`present` and `DisplayFlip` share four physical cores**. With five compute
lanes and only four cores there is necessarily at least one SMT pair among
busy threads, and nothing in the current mapping chooses which pair. A
consistent signal: lane 0 runs **18-20 % fewer c690 jobs** than lane 1 in
both runs (run 2: 111,916 vs 137,491; run 3: 122,982 vs 159,138), which is
what a lane sharing a core (or competing with another busy guest thread)
looks like. The exact `psr` table is not available because of the watcher bug
above.

**Proposed layout** (only if an A/B shows >1 ms): keep walker on cpu 0 and
main on cpu 1; pin lane 0 -> 2, lane 1 -> 3, lane 2 -> 4, lane 3 -> 5,
lane 4 -> 10 (sibling of core 4); pin `present` -> 8, `ppu DisplayFlip` -> 9,
`spurs kernel` -> 11. That guarantees four lanes on four distinct physical
cores and puts the unavoidable shared pair on the lane that shares with the
mostly-blocked `present`/`DisplayFlip`, not on two compute lanes. Keep the
walker/main siblings (6,7) free. Expected 0-2 ms; kill at <1 ms.

### 1.2 Why only five workers

It is the game's SPURS configuration, not ours:

* `recomp-work/bm_1.log:31`: `Initialize "" ea=0x01243E00 nSpus=6` (the
  instance has six physical lanes).
* `recomp-work/bm_1.log:62,74`: `MeshJobs0` and `MeshJobs1` are added with
  `maxC=5`.
* `recomp-work/bm_1.log:59`: the workload priority table at `+0x18` is
  `04 04 04 04 04 00` — **lane 5 has priority 0**, which makes it ineligible
  for these workloads. The runtime honours both the priority
  (`libs/spurs/cellSpurs.c:1837-1841`) and `maxContention`.

Lane 5 is not spare capacity: `DXTJob` (`maxC=4`), `CrowdJobList` and the
`maxC=1` jobs (`nextsynthJobLst`, `LibMP3`, `KinematicsJobs`) use it. A c690
job therefore cannot legally run on the sixth lane. The throwaway
`SPURS_LANE6=1` live key (which relaxes both checks for `MeshJobs0/1`) was
tried in run 3: the guest stopped flipping within ~10 s. That is a hard no.

### 1.3 Repeated work

New `PS3RECOMP_C690_HASH=<stride>` (default off) folds every DMA GET
(input) and every PUT (output, including the submit epilogue that follows the
job-end command) into FNV-1a, plus the LS output page range
`0x2F000..0x33000`; it samples 1 in `stride` jobs and records whether the
input hash was seen before and whether the output also matched.

Meter window 159.2-197.3 s in run 3 (60,190 samples, 1.1 KiB input and
34 KiB output per job):

| counter | samples | share |
|---|---:|---:|
| input hash seen before | 24,251 | **40.3 %** |
| — seen in an earlier frame | 15,578 | 25.9 % |
| — seen in the same frame | 8,673 | 14.4 % |
| input **and output** identical | 42 | **0.07 %** |

So about 40 % of c690 jobs re-read identical input bytes, but essentially
none produce an identical output; the output encodes ring allocation/slot
state. **No whole-job memoisation is worth building.** A stage-level cache
(e.g. the pure unpack/skin stages keyed by input bytes) would have to leave
the ring/pack epilogue out; that is a much smaller, unmeasured opportunity.

## 2. Where c690 time goes

### 2.1 Job cost histogram (in-process, run 3 at the meter)

| metric | value |
|---|---:|
| jobs (cumulative at 195 s) | 743,905 |
| wall p50 / p90 / p99 | **121 / 248 / 980 µs** |
| wall mean / max | **164.6 µs / 3.14 ms** |
| jobs/frame (437 jobs × 29 fps) | 437 |
| cumulative wall/frame (437 × 165 µs) | **72 ms** |
| per-worker busy (÷ 5 lanes) | **14.4 ms** |
| per-worker jobs | 122,982 / 159,138 / 155,608 / 149,758 / 156,419 |

Run 2 at the meter gives the same shape (121 / 248 / 981 µs, mean 164.8,
max 4.11 ms). The per-lane imbalance (lane 0 low) repeats.

Command mix (run 3): `0x0A` 31.7 %, `0x2A` 7.7 %, `0x00/04/07/08/13/1F`
6.6 % each, `0x0C/0x27` 5.6 %, `0x21` 4.5 %, `0x25` 3.8 %, `0x26` 1.1 %,
`0x09` and `0x2C` ~0 %.

### 2.2 Per-function distribution

The in-process exclusive counter can only name functions entered through
`spu_lifted_lookup`/`SPU_DIRECT`; it reports `0xAF98` 6.1-6.4 %, `0xA798`
0.8-0.9 %, `0x10F78` 0.5 %, other 92 %. The functions entered by `goto` or
the native swap (`0xFDA0`, `0x6D68`, `0x7C18`, `0x8160`, ...) are invisible
there. The gdb stack profiles do see them. Merging `prof_spu2` (25 samples)
and `prof_spu3` (120 samples), 210 samples whose stack contains the c690 job:

| rank | LS | share | generated code | guest work |
|---:|---|---:|---|---|
| 1 | `0xFDA0` | **19.0 %** | `spu_recomp.c:23881` | recursive spatial-tree/BVH traversal and bounding-interval culling (calls `0xFA58`, `0xFCB8`) |
| 2 | `0xD4A8` | **8.1 %** | `spu_recomp.c:18007` | matrix-palette skin/transform variant; 63 `shufb`, 24 LS writes |
| 3 | `0x6D68` | **7.1 %** | `spu_recomp.c:5372` | 12/12/11 SNORM dequantise (fma + bit unpack; [V] in `c690_function_map.md`) |
| 4 | `0xAF98` | **6.2 %** | `spu_recomp.c:13787` | normal/tangent normalise: `frsqest` + `fi` refinement |
| 5 | `0x7C18` | **5.7 %** | `spu_recomp.c:7612` → `runtime/spu/job_c690_native.c:110` | snorm16 pack (`cflts 157`) and interleave; **already native** |
| 6 | `0x8160` | 5.2 % | `spu_recomp.c:7935` | snorm8 pack (`cflts 142`) |
| 7 | `0xFCB8` | 4.8 % | `spu_recomp.c:23763` | leaf evaluation of the tree |
| 8 | `0xD2B8` | 3.8 % | `spu_recomp.c:17878` | skin/transform variant |
| 9 | `0xFA58` | 3.3 % | `spu_recomp.c:23421` | bounding-box / interval test |
| 10 | `0xF520` | 2.4 % | `spu_recomp.c:22720` | LS-heavy index/stream block |
| 11 | `0x76B8` | 1.9 % | `spu_recomp.c:7208` | gather/write |
| 12 | `0xC9D0` | 1.9 % | `spu_recomp.c:17278` | transform/normalise variant |
| 13 | `0xB268`, `0xB830` | 1.9 % each | `spu_recomp.c:14569`, `:14953` | transform/normalise variants |
| 14 | `0x5BC0` | 1.4 % | `spu_recomp.c:3251` | stream/command block |
| 15 | `0x10700` | 1.0 % | `spu_recomp.c:25273` | command parser (dispatch) |

`0x7C18` being already native and still 5.7 % is the important negative
result: the one native stage that exists has not removed its hot spot. The
native body (`job_c690_native.c`) does the same work with AVX2 but still
spends the time in `native_cflts256` + shuffles + the per-iteration `ctx->pc`
checks.

### 2.3 Categories

Measured by the innermost frame of the 210 in-job samples:

| category | share |
|---|---:|
| lifted/native job compute | **90.5 %** |
| branch dispatch / indirect lookup (`spu_indirect_branch`, `spu_lookup`, `spu_drain_call`) | 5.2 % |
| MFC/DMA + memcpy (`mfc_*`, `__memmove`) | 1.9 % |
| coherency / lockline (`spu_coh_*`, `spu_lockline_*`) | 1.4 % |
| channel / clock (`spu_rdch`, `clock_gettime`) | 1.0 % |

There are no separate LS-bound/endianness frames because those helpers are
inlined. A static op census of the same hot functions (instruction counts
weighted by gdb share) gives the inside of the 90 %: `shufb`/`rotqby`
23 %, LS 128-bit reads 15 %, integer adds 12 %, LS writes 9 %, float
(`fma`/`fi`/`frsqest`/`cflts`) 16 %, select 5.7 %, shifts 4.9 %, bit ops
4.8 %, direct calls 4.3 %, `goto` 2.9 %, compares 2.1 %, indirect dispatch
0.6 %. That is a static mix, not time, but it says the levies are byte
permutation, LS access and float, and that branch dispatch is a small tail.

## 3. Ceilings

### 3.1 Synthetic c690 cost (run 2)

Live key `C690_SPIN_US` busy-waits N µs inside every c690 job wall. A = 0,
B = 50 µs, three 10 s segments each:

| arm | guest fps | frame p50/p90/p99 (MangoHud) | mean/std | walker wait |
|---|---:|---|---:|---:|
| A 0 µs | **29.53 ± 1.14** | 37.39 / 51.01 / 60.76 ms | 34.92 / 33.62 ms* | 6.71 ms |
| B 50 µs | **27.57 ± 1.03** | 34.97 / 54.22 / 63.07 ms | 36.00 / 13.44 ms | 8.91 ms |

\* A has one 829 ms present outlier; without it the std is ~14 ms. Guest
frame means from `[flip]`: 33.9 -> 36.3 ms. Paired A-B: **1.95 fps, 95 % CI
[0.63, 3.28]**.

The spin adds 50 µs × 437 jobs / 5 lanes = **+4.4 ms per worker per frame**.
The frame mean moved +2.4 ms (0.55 elasticity) and the walker's SPU wait
+2.2 ms. Halving the 165 µs job would remove ~7.2 ms of per-worker time;
at the measured elasticity that is ~4 ms of p50, but on the long frames the
c690 union would fall from ~25-35 ms toward the ~19.5 ms main-ref cycle, so
the p50/p99 collapse is larger than the linear slope (the long/short
alternation disappears). Once c690 is below main's cycle, further c690 cuts
do not lower p50 — main becomes the floor.

### 3.2 Sixth lane (run 3)

`SPURS_LANE6=1` lets `MeshJobs0/1` use priority lane 5 and raises the
effective cap to 6. The A segment ran normally (guest 28.67 fps, present p50
39.1 / p90 51.7 / p99 58.6 ms). After switching B the flip counter stopped
and `ab_live.sh` stopped with rc=7 (no flips for 13 s). **Rejected**: the
guest job/ring protocol assumes five concurrent instances. Do not ship this
key as a fix.

### 3.3 Pinning

Not measured (run budget exhausted). See 1.1 for the proposed layout and the
reason it is at best a ~1-2 ms lever.

## 4. Plan: from ~35 ms to <= 15 ms of c690 union

The ranked plan below is ordered by expected ms at the meter per unit of
effort. Exactness for every item is tested with `PS3RECOMP_C690_HASH`
(per-job input/output hash, duplicate/mismatch counters) plus a shadow lifted
run of the same job while a native stage runs: a native stage is accepted
only if the shadow output hash matches bit-for-bit (or within the stated
float tolerance) on a captured corpus.

| # | change | expected c690 union (and p50) | effort | kill criterion | exactness test |
|---|---|---|---|---|---|
| 1 | **Native tree/cull stage**: `0xFDA0` + `0xFCB8` + `0xFA58` + `0xF520` (30 % of job time) with the NHL stage split/corpus method | 35 -> **~26 ms**; p50 long frames ~-4 ms | XL (2-4 wk) | stage < 5x faster, or fallback > 10 % of segments | per-job output hash vs lifted; corpus diff; cull differs on <= 1 borderline tri/segment |
| 2 | **Native skin/transform**: `0xD4A8`, `0xD2B8`, `0xB268`, `0xB830`, `0xC9D0` (~17 %) | 26 -> **~20 ms** | L (1-2 wk) | < 4x on the stage | positions <= 1e-5 max error, packed bytes <= 1 LSB, hash |
| 3 | **Pack/unpack**: optimise the existing native `0x7C18` and port `0x8160`/`0x6D68` (~18 %) | 20 -> **~17 ms** | M (1 wk) | < 2x on the stage | replay `ref_spu_func_7c18.c` hash; snorm byte equality |
| 4 | **Lifter codegen for the remaining hot code**: `shufb`/`rotqby`, 128-bit LS access, float `fma`/`fi`/`frsqest`, dispatch tail (the 16 %/15 %/23 % static mix) | ~1-3 ms (10-15 % of the remaining lifted code) | M | c690 wall -< 5 % | existing SPU float/LS exactness suites; per-job hash |
| 5 | **Explicit lane pinning** (1.1): four lanes on distinct cores, shared pair on `present`/`DisplayFlip` | 0-2 ms | S | < 1 ms | same-scene A/B; per-lane job counts even out |
| 6 | **Hand-off/overlap**: once c690 is under ~19.5 ms, overlap the producer with the walker drain / main ref cycle | 17 -> **20-21 ms frame** (floor, not c690) | M | no p50 gain once c690 < main cycle | frame p50/p99 A/B |
| — | **Whole-job cache** | **0** | — | measured: 0.07 % identical output | — |
| — | **Sixth lane** | **rejected** (wedge) | — | run 3 | — |

Milestones: after 1-3 the per-worker busy is ~7 ms and the c690 union should
be ~17 ms; item 4 is what can push it to ~15 ms. Reaching <= 15 ms **and**
keeping it there means the c690 union is no longer the frame's limiter, so
the last step is item 6, not more c690 work. The measured 0.55 elasticity and
the 19.5 ms main cycle are the two numbers that bound the payoff.

## Caveats / deviations

* `perf record` was not run: `perf` is absent and `perf_event_paranoid=2`;
  no system setting was changed. `psample` was built and works, but the
  run-3 watcher matched the `timeout` wrapper (`pgrep -f build-play/...`)
  instead of `MyGameRecomp`, so its only sample file is the wrapper's header.
  The per-function table is therefore the existing gdb profiles plus the
  c690 counters, not a fresh 20 s pc sample.
* Run 1 was killed by the orchestration tool's 120 s command timeout at
  ~126 s (the game was still booting); it produced no measurement.
* Run 2's hash output counter was zero because the job's output leaves in the
  submit epilogue; run 3 uses the fixed tail hashing. Only run 3's
  `dup_output` is meaningful.
* `c690-stats`' `frames=` counter is cumulative from boot and is not a
  per-frame job count; jobs/frame above is derived from jobs/s ÷ guest fps.
* The per-lane `psr` table was not captured (watcher bug). Section 1.1 is the
  static mapping plus the repeated lane-0 job deficit.
* The gdb profiles are from 2026-11-01 `build-o2`, not `build-play`; the
  lifted job module is unchanged between them. Shares are ±2-3 points at 210
  samples.
