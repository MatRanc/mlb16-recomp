# What the RSX walker actually waits for

Date: 2026-11-02. One pitch run (`measure_fast.sh`, `PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1`,
`PS3RECOMP_TRACE` windowed at the pitch), 33.44 fps over the 10 s measurement (the trace window
itself: 393 frames, 11.80 s, 33.29 fps). Baseline without the new trace events was 34.63 fps, so the
instrumentation cost **−3.4 %** (≈1.1 ms/frame), inside the 10 % budget. Numbers below are as
measured; the fps estimates use the baseline frame time where noted.

## TL;DR

The walker's ~11.6 ms/frame of `rsx.put_wait` is **not** c690 throughput.

| source of wait time | ms/frame | share |
|---|---:|---:|
| a SPU PUT overwrote the parked slot, but the walker did not run again yet | **3.02** | 25.9 % |
| no SPU PUT touched the park at all (caught up with `put`, or the park was patched by a non-SPU writer) | **5.73** | 49.2 % |
| job not yet admitted by the SPURS scheduler when the wait began | 1.82 | 15.7 % |
| the attributed c690 job was running | 0.96 | 8.2 % |
| pushed, queued with a lane idle / all lanes busy | 0.11 | 0.9 % |

Only **~2.8 ms/frame** is actual c690 production (admission + running). The largest single block,
**~5.7 ms/frame**, is the PPU/park-patch side: `get == put` (the walker has consumed everything the
PPU has appended) or a self-JUMP park that was patched without any SPU DMA touching the park word.
Another **~3.0 ms/frame** is the walker's own resume cadence: the slot is filled p50 68 µs (p90
426 µs) after the wait starts, but the walker does not run again for p50 527 µs / p90 2.15 ms.

The workers are not saturated: c690 is **42.1 %** of lane time over the 6 configured lanes, 59 %
busy in total, the 6th lane is 88 % idle and ran **no** c690 jobs. Queueing while a lane was idle is
0.8 % of wait time and queueing with all lanes busy 0.1 %.

**Conclusion:** the 60 fps gap is a scheduling/ordering problem (producer earlier, walker resumed on
PUT, pipeline the producer with the walker), not a c690 throughput problem. Design B would remove at
most the ~2.8 ms/frame of c690 production wait and not the ~8.7 ms/frame of PPU/cadence wait; see
the lever table for estimates.

## Method

### Instrumentation (off by default)

Reused `PS3RECOMP_C690_STATS` (per-job wall, entry `0x135B8` → end `0x12AB8`), `VK_DRAWPROF`
(walker per-draw), and the existing `PS3RECOMP_TRACE` timeline (`rsx.put_wait`, `rsx.drain`,
`spu.policy_module`, `spu.workload`, `gcm.flip_request`). Added to `PS3RECOMP_TRACE`, all behind the
existing off-by-default hooks:

| event | where | meaning |
|---|---|---|
| `c690.job` (span) | `c690_stats.c` job entry/end | one c690 job: `desc` = descriptor EA from r3, `spu` = id |
| `spurs.kick` (instant) | SPURS coordinator, `cellSpurs.c` | workload admitted: `wid`, `lane_ready` = lane \| ready<<8 |
| `spurs.init` (instant) | SPURS scheduler start | configured physical lanes (6 here) |
| `gcm.spu_put` (instant) | `gcm_note_spu_put()` | an SPU DMA PUT landed in RSX-visible memory: `io`, `size` |
| `gcm.label_rel` (instant) | same | an SPU PUT into the GCM label window (0 fired this run) |
| `rsx.park` (instant) | walker self-JUMP park | `get`, `put` at the park |
| `spu.overlay_load` (instant) | `spu_overlay_note_get()` | a streamed job module became resident: `ea`, `image` |

`PS3RECOMP_TRACE_TRIGGER=<path>` + `PS3RECOMP_TRACE_SECS=<n>` start a fixed-length window when the
file appears (checked by the trace flusher every 5 ms) and then flush/close the file, so a run killed
with SIGKILL still has the whole window. Until the trigger, events are dropped.

### Run

`measure_fast.sh` was run unmodified with

```
EXTRA="PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/.../wait_trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/.../wait_go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=8 PS3RECOMP_TRACE_FLUSH_MB=8"
```

A watcher touched `wait_go` when `frame.ppm` matched the pitch reference (using the same
`same.py` box/threshold as `measure_fast.sh`). The first attempt was invalid: a stale `frame.ppm`
from the previous run matched at boot and opened the window 4 minutes early. The retry (with
`frame.ppm` removed and its mtime required to be after the run started) is the one reported; the
game, build and env were otherwise identical.

### Analysis

`tools/analyse_walker_wait.py` parses the line-oriented trace. For each `rsx.put_wait` it looks for a
`gcm.spu_put` whose DMA range **contains the parked `get`** (the write that overwrites the park
word), attributes it to the `c690.job` on that worker containing the PUT, and splits the wait:

* **a** `[wait start, job kick]` — the workload was not admitted yet;
* **b/c** `[kick, job start]` — queued; split by whether any configured lane was idle (>50 % of the
  segment) or all were busy;
* **d** overlap with `[job start, job end]` (split into >p90 "long-tail" and normal);
* **e1/e2** after the job: before the PUT is visible / after the PUT but before the walker resumes;
* **u** no PUT overwrote the park: `u_caught_up` when there was no park (`get == put`, waiting for the
  PPU to append) and `u_park_patched` when a park at that `get` was patched without an SPU PUT.

Worker utilisation is the union of that lane's `spu.policy_module` (+ `spu.workload`) spans per
inter-flip frame; c690 is the subset covered by `c690.job`; "other" is the rest of the policy run.
Lanes that never appear are counted idle using `spurs.init`.

## Results

Window: 393 frames, 11.80 s, 33.29 fps. 2,444 walker waits, 4.57 s total = **11.64 ms/frame**,
p50 777 µs, p90 4.68 ms. 6 configured lanes.

### Wait attribution

2,043 / 2,444 waits (83.6 %) have an SPU PUT that overwrites the park; all 2,043 attribute to a
`c690.job` on the same worker. The other 401 waits have no such PUT.

| class | ms | % of all wait | % of PUT-attributed wait | waits |
|---|---:|---:|---:|---:|
| a job not yet pushed | 716.05 | 15.7 | 30.8 | 1,997 |
| b pushed, queued with an idle lane | 36.89 | 0.8 | 1.6 | 1,258 |
| c pushed, queued with all lanes busy | 6.73 | 0.1 | 0.3 | 270 |
| d job running | 376.73 | 8.2 | 16.2 | 1,507 |
| e1 finished, PUT not yet visible | 0.30 | 0.0 | 0.0 | 46 |
| e2 PUT visible, walker not resumed | 1,186.39 | 25.9 | 51.1 | 868 |
| u caught up with `put` (no park; PPU append) | 939.45 | 20.5 | — | 256 |
| u park patched by a non-SPU writer | 1,312.42 | 28.7 | — | 145 |

Timing inside the PUT-attributed waits:

* wait start → slot-filling PUT: **p50 68 µs, p90 426 µs**;
* PUT → walker resumes: **p50 527 µs, p90 2.15 ms**;
* job end → its PUT: p50 −581 µs (the job emits the slot before its end command);
* job kick → job start (queue): p50 25 µs, p90 40 µs.

Long-tail split of `d`: 262 ms in jobs longer than p90 (260 µs), 114 ms in normal jobs.

### Worker utilisation and non-c690 work

| lane | busy | c690 | other | idle |
|---:|---:|---:|---:|---:|
| 0 | 72.7 % | 44.1 % | 28.6 % | 27.3 % |
| 1 | 67.1 % | 50.2 % | 16.9 % | 32.9 % |
| 2 | 67.0 % | 52.0 % | 14.9 % | 33.0 % |
| 3 | 67.0 % | 52.5 % | 14.5 % | 33.0 % |
| 4 | 67.4 % | 53.5 % | 13.9 % | 32.6 % |
| 5 | 11.9 % | **0.0 %** | 11.9 % | 88.1 % |

Mean over 6 configured lanes: **58.8 % busy, 42.1 % c690**. Per active lane c690 is 44–54 % of the
30 ms frame, i.e. ~13–16 ms; summed over the five active lanes that is **~75–77 ms/frame
cumulative**, matching the decision doc's 73.5 ms at 34.6 fps (this run is 3.4 % slower). There is
no throughput wall in the workers.

Non-c690 work inside the policy modules (14–29 % of lane time) is job modules 200 (`0x0105EE00`),
201 (`0x0106D380`), 202 (`0x01079180`) and 206 (`0x0117FC40`), plus policy module overhead. Their
loads in the window were 13,205 / 9,130 / 470 / 867; c690 (image 204, `0x01161300`) loaded 171,080
times. Durations of the non-c690 modules are not separately attributed.

### Cross-checks

* `PS3RECOMP_C690_STATS` over the same 12 s: 179,438 jobs (14,953/s), wall sum +30.88 s
  (77.3 ms/frame cumulative), p50 115 µs / p90 236 µs / max 3.83 ms. The trace (with its own event
  overhead) sees 171,076 jobs, p50 127 µs / p90 260 µs.
* Jobs/frame 435.3 vs 429 in the decision doc.
* Walker drain spans: 12 passes/frame, 18.4 ms/frame active (p50 22.2 ms) with tracing; the
  no-trace prior measurement is ~14–15 ms. `drain active + put_wait ≈ frame time` (30.04 vs
  30.04 ms), so the walker thread is the serial resource: it is either draining or waiting.

## What this means for 60 fps

The wait is dominated by **who has not produced yet** and **when the walker is scheduled**, not by
how much SPU math the c690 job does:

* **49 %** of the wait has no SPU PUT at the park. 20.5 % is `get == put` — the walker has drained
  everything the PPU has appended. 28.7 % is a self-JUMP park patched by something other than an SPU
  DMA (the PPU filling/linking the command ring). These are PPU-producer waits.
* **26 %** is the walker not running again after the slot was filled (p50 0.53 ms, p90 2.15 ms).
  The slot is filled 68 µs (p50) after the wait starts; the remaining time is mostly the pass
  cadence, not c690.
* **24 %** is c690 admission (`a`) plus running (`d`). Admission is short (kick→start p50 25 µs);
  the `a` time is the walker arriving before the workload was kicked at all.
* **0.9 %** is queueing, so more contention / more workers would not help. The 6th lane is idle and
  c690 uses at most 5.

### Levers, rough upper bounds

Frame times below start from the measured 30.0 ms (trace) and are also given against the 28.9 ms
no-trace baseline. They are upper bounds: a removed wait only shortens the frame if the producer
work it was hiding can overlap something else; they are not additive.

| lever | wait removed | frame | fps |
|---|---:|---:|---:|
| wake/resume the walker when an SPU PUT fills a slot | 3.0 ms | 30.0 → 27.0 (28.9 → 25.9) | **~37–39** |
| pipeline the PPU producer with the walker (stop catching up to the write head) | 5.7 ms `u` + up to 1.8 ms `a` | 30.0 → 22.5 (22.0) | **~44–46** |
| both, walker CPU unchanged (18.4 ms traced) | 8.7 ms | ~21.3 | **~47** |
| both, walker CPU at the no-trace ~14–15 ms | 8.7 ms | ~14.5–15.5 | **~60–65** |
| Design B (remove the c690 job) alone | ~2.8 ms (`a`+`d`) | 30.0 → 27.2 (28.9 → 26.1) | **~38** |
| more workers / 6th lane | 0.1 ms queue | — | ~0 |
| reorder jobs earlier in the frame | up to ~1.8 ms (`a`) | 28.9 → 27.1 | ~37 |

The realistic path to 60 is therefore: (1) resume the walker on the slot PUT (or shorten the pass
cadence), (2) overlap the PPU's command production with the walker instead of serialising them, and
(3) shave the walker's own CPU (already the subject of the walker work). Design B alone does not
reach the 55–60 fps claimed in `c690-offload-decision.md`; its measured ceiling here is ~38 fps
unless it is combined with the producer/pipeline changes, and it does not touch the PPU wait or the
walker cadence that make up ~8.7 of the 11.6 ms.

### Caveats

* One run, one window (393 frames). The 12 s window and ~2,400 waits are enough for the split, but
  this is not a benchmark campaign.
* "PPU/park-patch" waits do not identify the writer: the trace proves no SPU DMA overwrote the park
  word, not that the PPU wrote it. Adding a write-side hook for the FIFO ring would settle it.
* Slot attribution requires a PUT range that contains `get`; 83.6 % of waits meet it. The remaining
  waits are reported as `u`, not silently assigned.
* `gcm.label_rel` fired 0 times; the job/ring slot is identified by the job descriptor EA and the
  IO offset, not by the ring label. The SPU label release path is apparently not a write into the
  HLE label window.
* The trace adds ~1.1 ms/frame; the wait ms above are from the traced run. The lever table scales to
  the 28.9 ms baseline.
* Long-tail jobs are rare: `d` is 0.96 ms/frame, of which 0.67 ms in >p90 jobs.

## Reproduce

```
# one run, at the pitch; touch the trigger when the pitch reference matches
EXTRA="PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/wait_trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/wait_go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=8 PS3RECOMP_TRACE_FLUSH_MB=8" \
  recomp-work/ab-tools/measure_fast.sh wait_trace.log

python3 tools/analyse_walker_wait.py /tmp/wait_trace.json \
  --manifest recomp-work/spu_lifted/jobmods_manifest.json \
  --md wait_report.md --json wait_summary.json
```
