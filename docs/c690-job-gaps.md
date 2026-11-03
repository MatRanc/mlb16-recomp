# c690 job turnover: where the 60-85 us inter-job gap goes

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. One mapping launch with `PS3RECOMP_C690_GAP=1`,
`PS3RECOMP_C690_STATS=1`, a 45 s `PS3RECOMP_TIMELINE` window and a 25 s
`perf record` on the game process; one A/B launch (baseline vs the fix bundle)
that was stopped by a runtime crash in the second arm. The mission cap was two
game launches and has been used. Details of the run: section 6.

This is the follow-up to [`frame-timeline-2.md`](frame-timeline-2.md): the long
meter frame is one c690 burst, the five lanes are 64 % occupied inside it, and
the idle is per-job turnover. This document says what that turnover is, which
part is ours, and what was implemented.

## TL;DR

* **The gap is three phases, not one runtime lock.** For 628,783 inter-job
  gaps under 2 ms, on the same lane (timeline policy/job spans):

  | phase | mean us | p50 us | p90 us | p99 us |
  |---|---:|---:|---:|---:|
  | job end -> policy-run end (tail) | 30.7 | 28.6 | 35.9 | 80.0 |
  | policy-run end -> next policy-run start (host) | **52.5** | **27.5** | **87.8** | 471.1 |
  | policy-run start -> next job start (head) | 35.2 | 26.2 | 33.3 | 138.5 |
  | **inter-job gap** | **118.5** | **85.1** | **162.8** | 803.8 |

  At p50 the three phases are nearly equal (28.6 + 27.5 + 26.2 = 82.3 us).
  At the mean the host phase is the largest because of its long tail.
* **The policy module runs once per job.** Per lane: ~136k jobs, ~139k policy
  spans. Policy span p50 **196 us**, mean 240 us; c690 job span p50 **136 us**;
  inter-policy gap p50 **27.5 us**. The PM exits to the kernel after each job,
  and our SPURS coordinator thread dispatches it again.
* **The job module is re-DMA'd on every job.** In the 86 s meter window:
  `code_get` 7.74 M GETs / **85.4 GiB**, i.e. **70 KiB per gap**, the c690
  module size (67,648 B). `ovl_load`/`ovl_verify` = 1.376 M each: the streamed
  overlay is marked resident and the full 67 KiB code is compared again once
  per job. The copy is ours to avoid when the bytes are already identical.
* **Atomics are not the main cost.** GETLLAR 3.13 M and PUTLLC 3.11 M in the
  same window (about 2.5 per gap, matching the WWS ticket claim), with
  **217k failures (7 %)**. The lock-line fast path is already taken for a
  resident poll; the retries are not the 60-85 us gap.
* **Perf on the `spurs pm` threads (25 s, 57,853 samples):** c690 job
  functions are 47 % of lane samples, the PM functions 3 %, other job modules
  6 %; the rest is runtime, dispatch and scheduler. That is the expected
  split for a job-wall-dominated profile, not a spin in one runtime helper.
* **Implemented, default off:** `SPURS_PM_FAST_RESCHED=<n>` (re-run a still
  ready PM in the same worker, at most n times, before yielding to the
  coordinator) and `SPU_CODE_DMA_MEMO=1` (skip a code GET already byte-identical
  in local store; exact, unit-tested). Both are live control-file keys.
* **The live A/B did not complete.** The first B segment crashed with a
  double `spu_lockline_unlock()` in the fast-reschedule consume path
  (`pthread_mutex_lock` owner assertion). The defect is fixed and the tree
  rebuilt, but the two-launch cap was already used, so the fixed bundle was not
  re-measured. In the 13 s before the crash B looked better (gap p50 67.7 vs
  79.8 us, p90 112.7 vs 186.0, lane busy 0.548 vs 0.512), but that is one
  short, aborted segment and is **not** a result. No switch was enabled in
  either `play_windowed.sh`.

## 1. What "the gap" is

A c690 job is the span LS `0x135B8` -> `0x12AB8` on a `spurs pm` host thread.
Between the end command and the next entry of `0x135B8` on the same thread the
lane does:

1. **tail:** the job's submit epilogue (`0x12AB8` -> `0x4A5C`), including the
   output PUTs that carry the job's result, then the PM's post-job bookkeeping
   and its exit-to-kernel (`LS 0x9C0`);
2. **host:** our `spurs_pm_worker` finishes the policy run, releases the slot,
   the `spurs kernel` coordinator rescans the instance and wakes a worker,
   `spu_run_policy_module` re-enters the same persistent PM context;
3. **head:** the PM fetches/claims the next WWS ticket, DMAs the job descriptor
   and (again) the job code, builds the job context, and branches to
   `0x135B8`.

The job code itself is not part of the gap; the `PS3_TR_C690_JOB` span covers
only the job. The `PS3_TR_SPU_POLICY` span covers the whole PM quantum,
including the job, so policy-span end - job end is the tail and
next-policy-start - next-job-start is the head.

## 2. Measurement

Mapping launch (section 6): meter reached at 161 s, X once, timeline armed 20 s
later for 45 s, perf attached for 25 s. `/tmp/jg1.bin` (226 MB) held 7.68 M
events from 41 threads; it was deleted after analysis.

* 628,783 consecutive-job pairs on the five active lanes with
  `0 < gap < 2 ms`, paired to the policy span that contained the first job and
  the next policy span that started before the second job. Gaps >= 2 ms are
  the burst duty-cycle idle and are excluded from the turnover statistics.
* Percentiles are over all lanes pooled; per-lane p50s are within 1 us of each
  other (84.4-85.9 us).
* Instrumentation added for this mission (`PS3RECOMP_C690_GAP=1`) records the
  same boundaries independently: 1.221 M gaps in the 86 s meter window, p50
  81 us, p90 168 us, plus per-function exclusive time and counters for MFC,
  GETLLAR/PUTLLC, overlay load/verify and code re-load. This is measurement
  only and default off.
* `perf record -g --call-graph dwarf -F 400 -p <game>` for 25 s, 466 MB,
  deleted after analysis. Flat leaf profile per thread (no call graph) was
  used because the DWARF unwind of 466 MB is unusably slow on this host.

## 3. The breakdown in us per job

### 3.1 Phases (timeline, gaps < 2 ms)

| phase | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| tail | 30.7 | 28.6 | 35.9 | 80.0 |
| host | 52.5 | 27.5 | 87.8 | 471.1 |
| head | 35.2 | 26.2 | 33.3 | 138.5 |
| gap | 118.5 | 85.1 | 162.8 | 803.8 |

The p50 column is the typical dense-burst turnover. The host phase has the
heaviest tail: a preempted worker or a busy coordinator turns 27 us into
hundreds of microseconds. That tail is also why the mean gap (118 us) is much
larger than p50.

### 3.2 Where the lifted time sits (`C690_GAP`, 86 s window, delta of the 2 s reports)

Exclusive intervals use the same last-entry-owns-the-interval model as the job
counters, across all images while a gap is open. Direct host calls are not
entry hooks, so a function that calls another directly owns its callee's time;
the table is a lower bound per function.

| image | LS | share of gap | avg us/gap | what it is |
|---|---|---:|---:|---|
| 6 | `0x2D10` | **66.2 %** | 81.8 | PM exit subtree (`0x2D10` -> `0x3630` -> exit-to-kernel); because the host wait after the exit is charged to the last entry, this is **tail + host** together |
| 6 | `0x12AB8` | 5.4 % | 7.5 | c690 end/submit handler, the first part of the tail |
| 6 | `0x3FEC0` | 4.5 % | 0.7 | PM helper |
| 6 | `0x22D0` | 3.7 % | 0.5 | PM helper |
| 6 | `0x2030` | 2.9 % | 2.9 | PM staging |
| 6 | `0x34B8` | 1.9 % | 2.4 | PM staging |
| 6 | `0x5318` | 1.6 % | 13.1 | PM path with the ticket/descriptor work |
| 6 | `0x2498` | 1.6 % | 1.8 | PM helper |

`0x2D10` is a two-instruction wrapper that calls `0x3630`; both are below the
job-code buffer at `0x4000`, i.e. **PM code**, not the c690 job module. The
attribution therefore says: after the job ends, the c690 submit epilogue and
the PM's exit/host hand-off account for most of the gap, while the PM's
pre-job staging is spread over many small functions.

### 3.3 Runtime counters in the same window

| counter | 86 s | per gap |
|---|---:|---:|
| MFC GET | 40.3 M (103.7 GiB) | 33 GETs, 85 KiB |
| MFC PUT | 7.16 M (18.9 GiB) | 5.9 PUTs, 15.5 KiB |
| code GET into LS `0x4000..0x14840` | **7.74 M (85.4 GiB)** | **6.3 GETs, 70 KiB** |
| GETLLAR | 3.13 M | 2.56 |
| PUTLLC | 3.11 M (217k fail, 7.0 %) | 2.55 |
| overlay load / verify | 1.376 M / 1.376 M | 1.13 |

The code-GET figure is the strongest single result: **the engine re-streams the
whole c690 module into local store for every job**, and our overlay path then
re-validates it with a 67 KiB `memcmp`. On hardware the DMA is asynchronous and
the PM waits only on its tag; here the copy is a synchronous `memcpy` in the
head. Over the window this is 85 GiB of avoidable copy that does not change a
single local-store byte.

### 3.4 Perf (25 s of `spurs pm`, flat leaf samples)

57,853 samples total on the process; 42,228 on `spurs pm`.

| class | samples | share |
|---|---:|---:|
| c690 job functions (`jobmod_c690...`) | 19,896 | 47.1 % |
| other job modules (DXT/Crowd/...) | 2,475 | 5.9 % |
| PM functions (`wwsjob_pm...`) | 1,312 | 3.1 % |
| dispatch/resolve/drain | ~4,000 | ~9.5 % |
| `c690_gap` instrumentation + c690 counters | ~4,000 | ~9.5 % |
| memcpy/memcmp, locks, futex, scheduler, unknown | remainder | remainder |

The profile confirms the gap is not a spin in one runtime helper: the c690
job body dominates lane time, and the PM + runtime pieces are small leaves.
The gap is a **structural turnover** (exit -> host dispatch -> re-enter ->
stage), not a single lock. The instrumentation overhead here is real (~9 % of
lane samples) and is why the A/B was planned without `C690_GAP`.

## 4. What is ours, and what is not

Ours:

* the **host phase**: one condition-variable round trip and the coordinator's
  instance scan per job. The PM is persistent and its local store is intact,
  so re-running the same workload in the same worker is semantically the same
  dispatch the coordinator would perform.
* the **synchronous code re-DMA**: 70 KiB per job that is already in local
  store. Skipping it when source and local store are byte-identical is exact.
* the overlay verification that follows every re-DMA (67 KiB `memcmp` per
  job), which the memo avoids when the re-DMA is a no-op.

Not ours (guest protocol / hardware behaviour):

* the c690 submit epilogue and the output PUTs after the end command;
* the PM's ticket claim and its own staging code;
* the WWS job list protocol and the GETLLAR/PUTLLC contention. Its 7 % failure
  rate is the game's claim pattern, not a livelock.

## 5. Implemented switches (default off)

### 5.1 `SPURS_PM_FAST_RESCHED=<n>` (`libs/spurs/cellSpurs.c`)

After `spu_run_policy_module` returns, `spurs_pm_worker` checks whether the same
workload is still dispatchable on the same lane (enabled, runnable state,
priority bit, `maxContention`, ready count or signal bit, and the
`SPURS_READY_LEVEL` rule). If it is, the worker mirrors exactly the
coordinator's dispatch bookkeeping -- consume one ready unit in count mode or
clear the signal bit, refresh `wklCurrentContention`, reset idle, emit the
`spurs.kick` instant -- and runs the PM again without releasing the slot or
waking the coordinator. It does at most `n` such re-runs (clamped to 64) before
returning to the normal path, so a workload that exits with stale ready work
cannot monopolise the lane. `n = 0` is off (the default); `n = 8` was the A/B
arm. Live key `SPURS_PM_FAST_RESCHED`.

Correctness argument: the PM context is already persistent and re-entrant;
the worker does not change the PM image, data, or local store; the guest job
order is still decided by the PM's own ticket protocol; and the only guest
state the coordinator would write is written identically. The bound preserves
cross-workload fairness.

### 5.2 `SPU_CODE_DMA_MEMO=1` (`runtime/spu/spu_channels.c`, `spu_dma.h`)

For a GET that lands entirely inside a **VERIFIED** streamed-code span, at the
matching source offset (`ea == source_ea + (lsa - base)`), compare the source
bytes with the local-store bytes. If they are identical the GET cannot change
local store: skip the copy and do **not** mark the overlay pending, so the span
stays verified and the next dispatch skips the 67 KiB verification too. This is
exact by construction -- the bytes are compared, not assumed from the source
address. Changed source or changed local store falls back to the normal copy
and pending/verify path.

Unit test: `runtime/spu/tests/run_code_dma_memo_test.sh`, which builds the real
runtime units and checks identical/full, identical/partial, wrong offset,
outside span, changed source and changed local-store cases. Passes.

Live key `SPU_CODE_DMA_MEMO`.

Both keys are seeded from their environment variable and can be flipped from
`PS3RECOMP_CTL`; both default off, so the standard play configuration is
unchanged.

## 6. Runs, and why the A/B has no result

| run | tag | result |
|---|---|---|
| 1 | `ab_live_jobgap1_20261103_074128` | mapping launch, complete. Meter at 161 s; timeline 45 s from meter+20 s; perf 25 s. 1.221 M gaps in the 86 s window; tables above. |
| 2 | `ab_live_jobgap2_20261103_080547` | A/B launch, aborted. Segment 0 A completed; segment 1 B ran ~13 s and died with `Fatal glibc error: pthread_mutex_lock.c:88 ... __owner == 0` on `spurs pm`. `ab_live.sh` stopped with rc=7 (no flips). |

The crash is a double unlock in `spurs_pm_fast_consume`: it called
`ef_line_end()` (which already unlocks the lock-line mutex) and then
`spu_lockline_unlock()` again. The second unlock left the mutex in an invalid
state; the next `spu_lockline_lock()` from another path hit the glibc owner
assertion. Fixed by deleting the extra unlock (the function now mirrors the
coordinator's two-line critical section exactly) and rebuilt. `run_code_dma_memo_test.sh`
still passes.

Because the mission's hard cap is two game launches and both were used, the
fixed bundle was not re-run. What run 2 did show before the crash:

* A segment (valid, one 15 s segment, 2 s dropped): **28.37 fps**, walker
  19.36 ms, wait 15.94 ms; gap p50 79.8 us, p90 186.0, lane busy 0.512.
* B segment (invalid: 13 s then crash): 30.93 fps, gap p50 67.7 us, p90 112.7,
  lane busy 0.548.

The B direction is consistent with both switches (fewer host round trips and
no code copy/verify), but one short aborted segment at N=1 is not evidence.
**No switch was enabled in either `play_windowed.sh`.** Both scripts still run
the standard configuration; the mission's "keep winners" step has nothing to
keep.

## 7. Next steps (in order)

1. Re-run the A/B with the double-unlock fix: `A = both 0`, `B =
   SPURS_PM_FAST_RESCHED=8, SPU_CODE_DMA_MEMO=1`, N=6, 15 s segments, 2 s
   dropped, timeline off, `C690_GAP` off (its 9 % lane overhead is not needed
   for the A/B; the 2 s `c690-gap-window` lines are enough). Kill criteria:
   no wedge, no guest-visible job-order/atomic change; keep a switch only if
   the paired frame p50/p99 and lane busy both move outside the A/A spread.
2. If fast reschedule wins, try `n = 32/64` and measure whether the bound is
   reached (it should not be for MeshJobs). If the coordinator wake is still
   visible, consider a short park/spin in the worker instead of the full CV
   wait.
3. If the code memo wins, measure the remaining head with `code_get` and
   `ovl_verify` counters; the next target is the synchronous copy itself
   (hardware MFC is asynchronous, ours blocks the PM until the tag).
4. Do not attack the 7 % PUTLLC failures yet: they are a claim pattern, not a
   runtime livelock, and the gap phases show no lock-dominated tail.

## 8. Reproduce

```sh
# Mapping launch (as run): timeline + perf + gap counters.
cd recomp-work/ab-tools
TAG=jobgap1 N=1 SEG=75 DROP=0 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 LIMIT=800 ARMS='A=' BOOT_ARM='' \
  PS3RECOMP_TIMELINE=/tmp/jg1.bin PS3RECOMP_TIMELINE_TRIGGER=/tmp/jg1.go \
  PS3RECOMP_TIMELINE_SECS=45 PS3RECOMP_C690_GAP=1 ./ab_live.sh
# after "pitch meter confirmed", wait 20 s, touch the trigger, then
#   perf record -g --call-graph dwarf -F 400 -p <game pid> -- sleep 25

# A/B (after the fix): no timeline, no C690_GAP.
TAG=jobgap2 N=6 SEG=15 DROP=2 METER=1 SKIP_CINE=1 PIN=1 WCB=2 \
  VIDEODRIVER=x11 REPORT=1 SETTLE=10 LIMIT=800 \
  ARMS='A=SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0|B=SPURS_PM_FAST_RESCHED=8;SPU_CODE_DMA_MEMO=1' \
  BOOT_ARM='SPURS_PM_FAST_RESCHED=0;SPU_CODE_DMA_MEMO=0' ./ab_live.sh

# Gap phase split (timeline): pair job spans (ev 33) with policy spans (ev 4)
# per spurs pm thread; gaps < 2 ms. See section 2.
```

## Caveats

* One mapping launch, one partial A/B launch. The phase split is from 628,783
  gaps in the 45 s timeline window; the 86 s counter window includes the
  settling time after the meter.
* The `C690_GAP` exclusive-time table lumps direct calls into their caller and
  charges the host wait to the last lifted entry (`0x2D10`). The phase split
  from policy/job spans is the authoritative decomposition; the function table
  says which PM code surrounds each phase.
* Perf was taken process-wide; only `spurs pm` leaf samples are tabulated.
  DWARF call graphs were recorded but not usable within the time budget on this
  host, so there is no per-caller tree.
* The A/B B segment is invalid: 13 s, then a crash. Its numbers are quoted only
  to show the direction, not as a win.
* The job-code re-DMA rate is measured from GET destinations in
  `0x4000..0x14840`; a GET that lands there for data rather than code would be
  counted too, but the 70 KiB/gap match to the 67,648 B module makes that
  unlikely.
