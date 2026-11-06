# WWS native PM head/tail: contract, the native ticket claim, and why Stage A is not enough

Date: 2026-11-05. Branch `exp/wws-native` in `ps3recomp-sync` (off
`exp/live-ab`), build `recomp-work/build-sync`. Offline only: no game launch and
no live A/B. This is roundtable item 2, Stage A
([roundtable-2026-11-05.md](roundtable-2026-11-05.md),
[roundtable-2026-11-05/C_recomp.md](roundtable-2026-11-05/C_recomp.md)).

## TL;DR

* **The PM head/tail contract is written down below**, from the lifted policy
  module and our own EBOOT: the entry ABI, the local-store pipeline and list
  layout, the ticket claim (GETLLAR/PUTLLC), the descriptor/code DMA, the job
  launch/return, and the exit-to-kernel tail.
* **One head section is native and byte-exact:** the WWS ticket claim
  (`0x23A8 -> 0x23BC -> 0x2460/0x2498/0x24C0/0x250C/0x2518/0x2534`) is
  reimplemented in `runtime/spu/wws_native_pm.h` with the SPU register file in
  host locals. A differential test runs the lifted chain and the native chain on
  identical LS + main memory and compares LS, main memory and the result
  registers; all five cases (claim, index 1, tag path, early-out, PUTLLC retry)
  match byte-for-byte. Offline cost: lifted ~160 ns/claim, native ~140 ns,
  **1.12-1.21x**.
* **That is not enough to meet the roundtable's Stage A kill criterion.** The
  claim is <1 % of the 17.4 us head (it is ~160 ns), so the saving is
  **~0.02 lane-ms/frame**, not the 4.6 lane-ms of head+tail. The remaining head
  (job staging `0x3080`, dispatch `0x31B0`) and the whole job submit/exit tail
  (`0x12AB8 -> 0x4A5C...`, `0x2D10 -> 0x3630`) are still lifted. The offline
  lane-gap sum therefore does not fall by 50 %, so **Stage B (a native runner)
  must not start** and **the key stays off**.
* `PS3RECOMP_WWS_NATIVE_PM` is an environment key, default **0**. The generated
  PM module is patched reproducibly by
  `tools/c690_ref/patch_wws_pm_native.py`; with the key off the lifted claim
  runs unchanged.
* Offline gates: the new differential test passes; `check_spu_context_abi.sh`
  passes (no `spu_context` change); `build-sync` builds incrementally;
  `tools/test_video_fixtures.py` is **34/34**. The latter needed a test-only
  weak-symbol stub (`libs/video/tests/fixture_runtime_stubs.c`) because
  `exp/live-ab`'s `PS3RECOMP_BINK_DMA_LOG` path left `rsx_draw_engine.c`
  referencing `g_spu_put_hook` and `ps3_ms_now`, which the fixtures do not link;
  that break was on the base commit, not this change.

## 1. What the PM is, and the guest-visible contract

The WWS job manager is SPURS workload policy module `wwsjob_pm`, 12,032 raw
bytes at guest EA `0x105BB80` (file offset `0x104BB80`). SPURS loads it at
local store `0xA00`; the runtime registers it as image 6 by content
fingerprint (`0x75BFE68AE96D7577`).

Entry follows `cellSpursModuleEntry` (`runtime/spu/spurs_policy.c`):

| reg / LS | meaning |
|---|---|
| `r0` | return-to-kernel link (`SPURS_PM_EXIT_TO_KERNEL_LS`) |
| `r1` | stack (`0x3FFB0`; the PM reloads its own) |
| `r3` | kernel context at LS `0x100` |
| `r4` | u64 workload argument in the preferred doubleword; word 1 is the job-list EA, word 0 an optional AUDIT buffer (0 for this title) |
| `r5` | poll status |

`SpursKernelContext` (big-endian) at LS `0x100`:

| off | field |
|---|---|
| `0x1C0` | SPURS EA (u64) |
| `0x1C8` | `spuNum` (lane 0-5) |
| `0x1CC` | DMA tag |
| `0x1D0` | current PM base (u64, low word `0xA00`) |
| `0x1D8` / `0x1DC` | current unique id / current workload id |
| `0x1E0` | `exitToKernel` address (intercepted: ends the run) |
| `0x1E4` | `selectWorkload` address (intercepted: HLEs the kernel's re-dispatch) |

The PPU observes only the job list in main memory, through
`Mesh_WaitPrevJobList` -> `WwsJobList_Wait` -> `WwsJobList_SpinUntilDone`
(our EBOOT, `0x6ED24` -> `0x9FBD70` -> `0x9FB898`): the list base is the first
descriptor word; for the mesh stage (`param_2 = 3`) the stage header is at
`base + 0x30 + 0x40`; `numJobs` at stage-header `+0x40`; per-lane completion
counters at `+0x42, +0x44, ..., +0x4c`; list counter at `+0x4e`. The PPU waits
for the minimum counter to reach the submitted count, then reads the job array
at `*(base + stage*4 + 0x30) + count*8` and adds chain lengths (a descriptor
with non-zero `+2` continues the chain). `sync(1)` (`lwsync`) precedes each
read. **A native runner must publish exactly these counters and cursors.**

## 2. Local-store state the PM keeps between quanta

The PM context is persistent per `(instance, workload, lane)`; a real PM keeps
its LS across quanta. The pipeline and list state the head/tail touches:

| LS | meaning |
|---|---|
| `0x080` | 128-byte GETLLAR landing pad (list line shadow) |
| `0x0C0..` | per-lane cursor rows in that line (`lane * 16`) |
| `0xD00` | job-record/staging area read by the claim (`jobState`, high half 0=empty, 1=allocated, 2/3/4 = later states) |
| `0xDF0` | per-slot records (32 B each) |
| `0xEB0` | staging ring |
| `0x11B0` | logical-to-buffer bindings |
| `0x12A0` | `loadJobState` (preferred halfword) |
| `0x12B0` | dispatch flag |
| `0x12C0` | `nextLoadJobNum` |
| `0x12D0` | `loadJobNum` (`0xFFFFFFFF` empty) |
| `0x12E0` | `runJobNum` |
| `0x12F0` | `storeJobNum` |
| `0x1300` | ring slot index |
| `0x1330` | list base copy |
| `0x13E0` | allocation serial |
| `0x13F0` | `eaWorkLoad` (job-list EA from `r4`) |
| `0x1410` / `0x1420` / `0x1430` | job index / job header / flags from `AllocateJob` |
| `0x1440` | per-slot record table |
| `0x14A0` / `0x14B0` / `0x14D0` | list-line / cursor-cache / tag-cache words |
| `0x14E0` | PM return address |
| `0x1510` | saved job return |
| `0x1540` / `0x1550` / `0x1570` / `0x1590` | timer and exit bookkeeping |
| `0x1600` / `0x1610` | job context words |
| `0x1A0..0x4A0` (relative to the job context) | job output/ring state used by the submit epilogue |

## 3. The head: claim, descriptor, staging, dispatch

### 3.1 Ticket claim (`0x23A8`, native now)

`0x23A8` is the WWS ticket claim. Entry `r3` = list line EA, `r4` = 2 * job
index, `r6` = return link. Steps (all addresses below are the lifted code's):

1. `r50 = 0x80` (GETLLAR LSA), `r51 = 0`, `r52 = r3` (list EA),
   `r53 = 0x3000`, `r54 = 2*r4`.
2. `0x23BC`: write `MFC_LSA=r50`, `MFC_EAL=r52`, issue `MFC_Cmd=0xD0`
   (**GETLLAR**, 128 bytes of the list line into LS `0x80`); read
   `MFC_RdAtomicStat`. The reservation is now on the line.
3. Choose the lane row at `LS[0x80]` word 2: `r5 = rotqbyi(line, 8)`,
   `r58 = r5<<4`; read `LS[r58+0xC0]` (the lane's cursor row) and
   `LS[0xB0]` (row control). Compare the cached line `LS[0x14B0]` with the
   fetched line (`r68`) and the cached row `LS[0x14A0]` with the computed row
   base (`r62`).
4. On a cache miss (`r68==0 || r62==0`), `0x2534` issues a 128-byte `GET`
   (`MFC_Cmd=0x40`) of the row into LS `0xD00` and caches `0x14A0/0x14B0`;
   then `0x2460` reads the job state at `LS[0xD00 + offset]`.
5. Depending on the state, `0x250C` (empty: write `0x14A0/0x14D0 = 0` and
   return), `0x2518` (state 4: patch the line word), `0x24C0` (state 2/3: a
   16-byte `GET` of the next row), or `0x2498`.
6. `0x2498`: write the new cursor back to `LS[r58+0xC0]`, write
   `MFC_LSA=r50`, `MFC_EAL=r52`, issue `MFC_Cmd=0xB4` (**PUTLLC**), read
   `MFC_RdAtomicStat`; on failure (bit 0 or `r68==0`) retry from `0x23BC`;
   on success write `0x14D0 = 0` and return via `r6`.
7. Results: `r3/r4/r5` (job header/index/flags) and the updated list line in
   main memory (`PUTLLC`), plus the LS caches above.

The claim is the only point in the head that takes the list-line **reservation**
(GETLLAR/PUTLLC). The measured failure rate is ~7 % (217k of 3.11M in the
86 s window, [c690-job-gaps.md](c690-job-gaps.md) §3.3) and the retry is
included in the chain; [c690-lane-gaps.md](c690-lane-gaps.md) puts lock-line
retry at 0.035 lane-ms/frame, so contention is not the cost.

### 3.2 Descriptor, job context and dispatch (still lifted)

`0x3020` (`AllocateJob`) calls the claim twice, then `0x23A8`'s results are
stored to `0x1420/0x1410/0x1430`; it builds the per-slot record at
`0x1440 + 32*slot`, clears the staging ring (`0xDF0`, `0xEB0`, `0x11B0`) and
issues the job-header `GET`. `0x3080` then:

* sets `0x12A0 = jobState`, `0x12D0 = loadJobNum`, `0x12C0 = nextLoadJobNum+1`,
  `0x13C0/0x13B0/0x13E0` bookkeeping;
* writes the slot record at `0x1440 + 32*loadJobNum`, the ring entry at
  `0xEB0 + 256*loadJobNum`, and clears `0xDF0` records;
* issues the job descriptor/code **GET** (`MFC_LSA=0xC00`,
  `MFC_EAL = line + offset`, `MFC_Size`, `MFC_TagID`, `MFC_Cmd=0x40`) and
  waits for its tag (`0x2854`); the 67 KiB job code is re-DMA'd into LS
  `0x4000..0x14840` on every job ([c690-job-gaps.md](c690-job-gaps.md) §3.3);
* writes the job context words at `0x1600/0x1610` and the return in `0x1510`.

`0x31B0` (`DispatchJob`) writes `0x12B0=1`, `0x12D0=-1`, `0x12E0=loadJobNum`,
`0x12A0=0xF00`, saves `r0` to `0x1510`, records the queue entry
(`0x25F8/0x2628`), builds the job context (`0x3570`), and **indirect-branches
to `r5 = list EA + descriptor offset`** (the c690 job entry, LS `0x135B8`).
On return it continues at `0x3288` and `0x25F0`.

### 3.3 The job

c690 job entry `0x135B8` writes `0x14840/0x14850`, calls `0x135F0` (command
loop) and runs to the end command `0x12AB8`, which tail-calls `0x4A5C`.

## 4. The tail: submit epilogue and exit

The job's submit epilogue is `0x4A5C -> 0x4AAC/0x4AD0/0x4AEC ->
0x494C/0x49D4 -> 0x4E7C/0x4E90 -> 0x4B24 -> ...` (all in the c690 job
module). It:

* reads the job context at `LS[0x14840]` and the output descriptor at
  `ctx+0x1D0`;
* issues the output **PUTs** (`MFC_LSA` from the job context, `MFC_EAL`,
  `MFC_Size`, `MFC_TagID`, `MFC_Cmd` `0x20/0x40`) and waits for the tag
  (`0x494C` / `0x49D4`);
* updates the ring/cursor words at job-context `+0x420`, `+0x480`, `+0x1A0`,
  `+0xA0`, `+0x340` and `LS 0x14840/0x14850`;
* increments the per-lane completion counter that the PPU's
  `WwsJobList_SpinUntilDone` polls (`+0x42..+0x4c`, list counter `+0x4e`);
* returns to the PM through `0x1510` (the dispatch saved it).

The PM tail is `0x2D10`: write `SPU_WrEventMask`, clear `LS 0x0`, call
`0x3630`, which loads `exitToKernel` from LS `0x1E0` and indirect-branches.
The runtime intercepts that address in `policy_mode`, sets
`g_spurs_pm_exited`, and ends the run. If the run is not exiting, `0x3648`
rebuilds the work list, `0x3688` returns, and `0x2D20/0x2D48` update the
`0x1500/0x1540` timer/exit state.

The observed tail (job end -> PM end) is 2.519 lane-ms/frame and the head is
2.075; together they are 84 % of the lane idle
([c690-lane-gaps.md](c690-lane-gaps.md) §3). The single largest item is the
`0x2D10 -> 0x3630` exit subtree (66 % of gap in the exclusive-time view,
[c690-job-gaps.md](c690-job-gaps.md) §3.2), which includes the host wait
after exit; the actual lifted code is spread over the job submit chain and
the PM staging helpers.

## 5. Implementation

| file | change |
|---|---|
| `runtime/spu/wws_native_pm.h` | native claim (transliterated chain, locals instead of `ctx->gpr`) + `PS3RECOMP_WWS_NATIVE_PM` (default 0) |
| `tools/c690_ref/patch_wws_pm_native.py` | reproducible patch of the generated PM: include the header, rename `0x23A8` to `_body`, add the keyed wrapper |
| `runtime/spu/tests/test_wws_native_pm_main.c` | differential test (lifted vs native) and microbenchmark |
| `runtime/spu/tests/run_wws_native_pm_test.sh` | builds the generated PM as the oracle with a memory-backed MFC model |
| `libs/video/tests/fixture_runtime_stubs.c` + `tools/test_video_fixtures.py` | weak `g_spu_put_hook`/`ps3_ms_now` stubs so the fixtures link without the runtime |
| `recomp-work/spu_lifted/wwsjob_pm/spu_recomp.c` | generated, patched locally (not committed) |

Exactness: every line of the native claim is the lifted line with `ctx->gpr[n]`
replaced by a host local `rN`; the same `spu_*` helpers, channel writes and
LS accesses are used. Only `r3/r4/r5` are written back (the caller,
`0x3020`, reads only those); the scratch registers the lifted body leaves in
`ctx->gpr` are not guest-visible. The code is compiled into the generated PM
TU, so the key-off binary is the original lifted code.

## 6. Offline verification

```
tools/c690_ref/patch_wws_pm_native.py          # idempotent
runtime/spu/tests/run_wws_native_pm_test.sh
```

The test builds the generated PM together with a stub MFC model that
implements GETLLAR (copy 128 B, take the reservation), PUTLLC (copy back if
the reservation matches, else fail), GET/PUT and tag updates. Both paths are
run on a byte-identical LS + memory image and the test compares all 256 KiB of
LS, the whole memory model and `r3/r4/r5`.

| case | result | coverage |
|---|---|---|
| `claim-base` | PASS | 1 GETLLAR, 1 PUTLLC |
| `claim-index1` (job index 1) | PASS | 1 GETLLAR, 1 PUTLLC |
| `claim-tag-path` (cache miss) | PASS | 1 GETLLAR, 2 tag GETs |
| `claim-early-out` (jobState 0) | PASS | 1 GETLLAR, no store |
| `claim-putllc-retry` | PASS | 2 GETLLAR, 2 PUTLLC (one failed reservation) |

Microbenchmark (`bench_claim`, 200k iterations, three runs): lifted
156.7-164.1 ns, native 135.4-142.6 ns, **1.12-1.21x**.

## 7. Kill criterion and the estimate

The roundtable's Stage A gate is "after Stage A the offline lane-gap sum must
fall by more than 50 %". This change cannot meet it:

* the claim is ~160 ns; the head is 17.4 us p50 and the tail 21.1 us, so the
  claim is <1 % of the 4.59 lane-ms of head+tail per lane;
* a 1.15x claim saves ~20-25 ns per claim, i.e. **~0.02 lane-ms/frame**, or
  about 0.004 ms of burst wall at five lanes;
* the rest of the head (`0x3020`, `0x3080`, `0x31B0` and the DMA helpers) and
  the whole job submit/exit tail are still lifted and dominate the 4.59
  lane-ms.

**Stage B (native runner) must not start, and the key stays 0.** The useful
next step is a mechanical translation of the *whole* head (`0x3020/0x3080/
0x31B0` and their DMA/tag helpers) and the *whole* job submit chain
(`0x12AB8 -> 0x4A5C...`), each with the same differential harness. Only when
that sum falls by >50 % does a runner make sense.

A live A/B was not run. The only offline gate that could show a signal is the
claim, and its estimated effect (~0.02 lane-ms) is far below the ~0.4 fps
cross-launch drift and the 1.09 fps paired resolution of the meter
([meter-scene-baseline.md](meter-scene-baseline.md)). A/B etiquette also says
not to spend a launch when the question is already answered offline.

## 8. Clean room

This is our own reverse engineering of the title's WWS job list and policy
module, our own EBOOT (Ghidra), our own lifted SPU code and our own trace
stream. The stage/corpus and differential-harness method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied. No
RPCS3 or other GPL emulator source was read or cited.

## 9. Follow-up 2026-11-05: where the head/tail microseconds actually go

The Stage A claim measurement above answered only ~160 ns of the 4.59 lane-ms
head+tail. This section measures the rest. One automated pitch-meter launch,
build `recomp-work/build-sync` (this branch + the standard play configuration:
`SPURS_PM_FAST_RESCHED=64`, `SPURS_PM_NATIVE_DISPATCH=6`), new opt-in
`PS3RECOMP_C690_STEPS=1` (default **0**; measurement only):

* run `ab_live_wwssteps_20261105_181358`; meter confirmed 184 s after launch;
  window 18:17:13-18:17:41 (28 s, 1,060 flips, **37.9 fps**), 5 lanes,
  433,361 jobs (86,757 per lane -> **81.8 jobs/lane/frame**), 429,533 short
  (in-burst, <2 ms) gaps.
* `PS3RECOMP_C690_STEPS` is implemented in `runtime/spu/c690_stats.c` and uses
  the same two entry hooks as `C690_GAP` (`spu_lifted_lookup` and `SPU_DIRECT`)
  plus the DMA and atomic call sites. Each gap is split into
  `tail_submit`, `pm_exit+host`, `head_alloc+claim` and `head_stage`; the
  synchronous DMA copies are timed by category (code GET / data GET / PUT) and
  the GETLLAR/PUTLLC operations are timed while the step is active. Only gaps
  under 2 ms are merged, so the inter-burst idle does not skew the means.

| step (LS boundary) | us/gap mean | p50 | p90 | share | lane-ms per lane/burst |
|---|---:|---:|---:|---:|---:|
| tail submit (job end 0x12AB8 -> first PM entry) | 7.72 | 7 | 10 | 15.4 % | **0.63** |
| PM exit + host (0x2D10/0x2D20 -> head start) | 22.34 | 17 | 28 | 44.6 % | **1.83** |
| head alloc + claim (0x23BC/0x2498/0x3020) | 2.18 | 1 | 2 | 4.4 % | **0.18** |
| head stage + dispatch (0x3080 -> job entry; 0x31B0 is a host call, so it lands here) | 17.90 | 16 | 21 | 35.8 % | **1.46** |
| **total turnover** | **50.06** | **41** | | | **4.10** |

Components measured inside those steps (mean us/gap):

| component | tail_submit | pm_exit+host | head_alloc+claim | head_stage |
|---|---:|---:|---:|---:|
| code re-DMA (GET, 67,846 B / 5.17 GETs) | 0 | 0 | 0 | **2.79** |
| other GET (12,402 B / 21.9 GETs) | 0.09 | 0.01 | 0.10 | **3.34** |
| PUT output (13,125 B / 4.0 PUTs) | **3.24** | 0.05 | 0 | 0 |
| GETLLAR + PUTLLC | 0.24 | **2.15** | 1.31 | 0 |
| lifted instruction execution (remainder) | 4.2 | **20.1** | 0.8 | 11.7 |

Reading the table:

* **The head is 19.8 us/gap and the tail is 30.1 us**, i.e. the same 4.1
  lane-ms/frame shape as `c690-lane-gaps.md`; the new split resolves it.
  `pm_exit+host` is the single largest step (1.83 lane-ms), and inside it the
  host dispatch portion is only ~0.6 lane-ms (the measured host class in
  `c690-lane-gaps.md`); the rest is the lifted PM exit/re-entry path plus the
  SPURS workload-line atomics.
* **All removable copy work is small.** Every synchronous DMA copy in the whole
  turnover is ~0.50 lane-ms (code 0.23, data 0.27) plus 0.27 lane-ms of output
  PUTs; all lock-line atomics are ~0.30 lane-ms. Even deleting the 67 KiB
  job-code re-DMA outright saves **0.23 lane-ms per lane per burst**, which is
  far below the 1 ms kill criterion, and `SPU_CODE_DMA_MEMO` already skips that
  copy when the source is unchanged (and was measured flat on frame time).
  Tag waits are zero because this runtime's MFC is synchronous; what the code
  calls a tag wait is the copy timed above.
* **The dominant time is lifted instruction execution**, not a removable
  sub-step: pm_exit 20.1 us, head_stage 11.7 us, tail_submit 4.2 us. That is
  the `spu-codegen-audit.md` lever (8-13 x86 instructions per SPU instruction,
  1.03 ns/instr) or a native translation of the remaining PM exit/staging
  chain (the Stage A proper, multi-day work), not a memo, a memcpy or a tag
  batch.

**Kill criterion reached.** No sub-step with a safe local fix can save 1 ms of
the meter burst; the only step above 1 lane-ms (`pm_exit+host`) needs the
native PM exit translation, not a copy/atomic cut. Nothing was implemented in
this follow-up beyond the opt-in measurement key; `PS3RECOMP_C690_STEPS`
defaults to 0 and changes no guest behaviour.

Reproduce:

```sh
cd recomp-work/ab-tools
TAG=wwssteps BUILD=build-sync N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 LIMIT=500 SETTLE=10 PITCH_THRESH=35 ARMS='A=' \
  BOOT_ARM='' SCENE_SHOTS=0 PS3RECOMP_C690_STEPS=1 ./ab_live.sh
# [c690-step] lines in the log at the 2 s cadence; the exit report has the
# cumulative table and the per-step p50/p90 histograms.
```

Clean room unchanged: our own trace/counters, our own lifted code and our own
EBOOT analysis; no RPCS3 or other GPL emulator source was read or cited.
