# Waking the RSX walker on the slot PUT

Date: 2026-11-02. Branch `feat/walker-wake` in `ps3recomp-probe`, off `mlb16-work` (not merged).
Two pitch runs with the fix enabled: one trace window (398 frames, 12.98 s) and one run where the
trace window missed its trigger. Compared with the 2026-11-02 attribution run in
[`walker-spu-wait.md`](walker-spu-wait.md).

## TL;DR

The walker's `e2` wait (slot PUT visible, walker not yet resumed) is **real and removable**: with the
fix the PUT->resume lag falls from **p50 527 µs / p90 2.15 ms to p50 8 µs / p90 11 µs**. But the
frame does **not** get faster. The wait reappears as "job not yet pushed": the walker is caught up
with the SPU producer, so waking it earlier just makes it wait for the next job in smaller pieces.
The extra passes cost more than the removed wait:

| run (same binary, `PS3RECOMP_WALKER_WAKE=1`) | fps (10 s) | trace window | waits/frame | wait/frame |
|---|---:|---|---:|---:|
| previous, fix off | **33.44** | 393 f, 11.80 s, 33.29 fps | 6.2 | 11.64 ms |
| this, fix on, traced | **30.95** | 398 f, 12.98 s, 30.67 fps | 35.4 | 11.28 ms |
| this, fix on, trace window missed (no trace) | **32.22** | — | — | — |

The fix is therefore **off by default** (`PS3RECOMP_WALKER_WAKE=1` enables it). The e2 lever in the
attribution doc's table ("3.0 ms -> ~37-39 fps") is an upper bound that does not survive the
measurement: removing the cadence only relocates the wait unless the producer is pipelined too.

## 1. How the walker waits and resumes

The FIFO walker runs on the present/frame-clock thread. Its loop is
`templates/project/main.cpp:220`: `cellGcm_fifo_kick_wait(idle_ms)` before draining, with
`idle_ms` 4 ms by default (`main.cpp:216-217`, `PS3_FRAME_CLOCK_MS`). The wait is
`cellGcmSys.c:1922` -> `WaitForSingleObject(s_gcm_kick_ev, ms)` (`cellGcmSys.c:1927`); it is a
bounded auto-reset event, not a spin, futex or condvar.

A pass that reaches a self-JUMP with `tgt == get` stops on the park (`cellGcmSys.c:2684-2696`), and
the next pass starts only when the idle wait returns. Two things end that wait early:

* a game thread whose `cellGcmFinish`/FIFO-space poll finds the fence queue dry
  (`cellGcm_ref_on_poll`, kick at `cellGcmSys.c:1958`), and
* now, with the fix, an SPU DMA PUT that overwrites the parked word.

Before the fix nothing in the SPU PUT path signalled it. The slot is filled p50 68 µs after the
wait starts, but the walker only re-reads it on the 4 ms timeout, or when an unrelated fence poll
happens to kick first. That residual is the measured **p50 527 µs / p90 2.15 ms** PUT->resume.

## 2. The fix (e2)

`gcm_note_spu_put` (`cellGcmSys.c:1604`) now calls `gcm_walker_wake_for_put(io, size)`
(`cellGcmSys.c:1908`) after resolving the PUT to an IO offset. The walker publishes the `get` it
parked on when it stops (`s_walker_park_get`, set at `cellGcmSys.c:2694`, cleared at the start of
every pass, `cellGcmSys.c:2318`). A PUT whose range contains that word signals the same kick event
the idle wait uses (`gcm_walker_kick`, `cellGcmSys.c:1894`). A PUT elsewhere does not, so this
cannot spin the walker for unrelated traffic; the auto-reset event coalesces a burst. The bounded
timeout remains as the fallback, so a missed signal costs at most one 4 ms tick, never a hang.

Toggle: `PS3RECOMP_WALKER_WAKE=1` (`=0` or unset disables; default off, see TL;DR). Unit test:
`tools/test_gcm_walker_wake.py` (`libs/video/tests/test_gcm_walker_wake.c`) checks both that a
covering PUT wakes the wait in tens of ms instead of its 2 s timeout, and that a PUT elsewhere
leaves it to the timeout.

## 3. What the measurement says

Trace window 398 frames, 12.98 s, 30.67 fps; 14,078 waits (35.4/frame), 11.28 ms/frame.

| class | previous (fix off) | this (fix on) |
|---|---:|---:|
| a job not yet pushed | 1.82 ms/f | **3.65 ms/f** |
| b queued, idle lane | 0.09 ms/f | 0.01 ms/f |
| c queued, all lanes busy | 0.02 ms/f | 0.00 ms/f |
| d job running | 0.96 ms/f | 0.12 ms/f |
| e2 PUT visible, walker not resumed | 3.02 ms/f | 2.09 ms/f |
| u caught up with put | 2.39 ms/f | 2.59 ms/f |
| u park patched by a non-SPU writer | 3.34 ms/f | 2.81 ms/f |
| total | 11.64 ms/f | 11.28 ms/f |

* PUT->resume: **p50 8 µs, p90 11 µs** (was 527 µs / 2,151 µs).
* Waits per frame: **6.2 -> 35.4**. The fix turns one 4 ms-batched pass into a pass per c690 job.
* The total wait is unchanged within noise; `d` collapsed and `a` grew because the causal PUT is now
  visible long before the wait begins. The walker still waits on the same producer.
* Frame rate fell **~7.5 %** (33.29 -> 30.67 fps in-window; 33.44 -> 30.95 on the 10 s counter). The
  run without a trace window measured 32.22 fps against the previous 34.63 no-trace baseline, also
  ~7 %. The extra passes are the cost; the frame cannot use the earlier start because the next job
  is not ready.

Rendering was unchanged at the pitch: the end-of-window `frame.ppm` differs from the previous traced
run's pitch frame by 7.4 grey levels over the full 640x360 image and 2.1 in the pitch box
(380,100)-(500,200), and matches the pitch reference within the harness threshold.

## 4. The `u` class: who patches the park

The 145 `u_park_patched` waits have p50 **11.7 ms**, end next to a `gcm.flip_done` or `spurs.kick`,
and the next pass moves `get` from the high-IO park (0x1300000-0x1580000 region) straight to `put`
in the low main ring (0xC2C8-0xE3xx). No SPU PUT covers the park: this is a **PPU store**.

Static path (EBOOT, Ghidra):

* The park word lives in the mesh command buffer. `Gfx_FrameEnd` (guest **0x00066530**) links that
  buffer into the main pushbuffer with `oris r3,r24,0x2000; stw r3,0x0(r5)` at **0x00066ae0**, where
  `r5 = *(mesh_context + 8)` is the buffer's current word. The value is `0x20000000 | io`, a JUMP to
  the main ring's current position. The equivalent main-ring store is at 0x00066a74.
* The `put` register itself is written by `FUN_00065760` (guest **0x00065760**):
  `lwz r4,0x3b00(r4); stw r5,0x0(r4)` at **0x00065af8**. `DAT_01123b00` is the control-register
  pointer stored once by `FUN_0006de94` (0x0006de94, from `cellGcmGetControlRegister`).
  `FUN_00065760` is called by `Gfx_FrameEnd` and by the "Async Kick" thread `FUN_00068fa4`; the
  `DisplayFlip` thread `Gfx_DisplayFlip` (0x000692dc) reads the same control block.
* `Gfx_FrameEnd` does not link the mesh buffer until `Mesh_WaitPrevJobList` (0x0006ed24) has waited
  out the previous SPU job list, so the PPU is on the frame's critical path.

So the walker is **early**, not the PPU late in the wake sense: it has consumed the frame's SPU-built
segment and parks on its end before `Gfx_FrameEnd` has produced and linked the next one. A wake from
the PPU store would only trim the residual 0-4 ms timeout; the ~12 ms is the PPU producer itself.

**Proposed fix for `u` (not implemented, per the task):**

1. give the runtime visibility of the PPU's put/park stores (a store hook on
   `GCM_CONTROL_GUEST_ADDR` for `FUN_00065760`'s store, or a page guard on the parked word), and
   signal the same kick event; and
2. more importantly, **pipeline the PPU producer**: start the next frame's `Gfx_FrameEnd`/mesh link
   before the walker reaches the end of the current segment, instead of after `Mesh_WaitPrevJobList`
   drains the whole list. The trace says the second is where the time is; a wake alone cannot move
   it.

## Reproduce

```
# traced (fix on); trigger the trace window when the pitch reference matches
EXTRA="PS3RECOMP_WALKER_WAKE=1 PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/ww_trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/ww_go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=8 PS3RECOMP_TRACE_FLUSH_MB=8" \
  recomp-work/ab-tools/measure_fast.sh ww_traced.log

python3 tools/analyse_walker_wait.py /tmp/ww_trace.json \
  --manifest recomp-work/spu_lifted/jobmods_manifest.json --md ww_report.md
```

## Caveats

* Two runs, one pitch window each. The first trace window missed its trigger (the frame watcher
  stopped before the pitch screen), so the "no trace" fps is from a run with the watcher competing
  for CPU; it agrees with the traced A/B (~7 %), which is the stronger comparison.
* The analyser's `e_walker` is not purely `PUT->resume`: when the PUT precedes the job's end it
  counts from the job end. The `resume_lag` line (p50 8 µs) is the direct PUT->resume number.
* The `u` writer is identified statically; no PPU write-side trace was added. `FUN_00065760` and
  `Gfx_FrameEnd` are Ghidra names for stripped EBOOT functions; the guest addresses are the entry
  points.
