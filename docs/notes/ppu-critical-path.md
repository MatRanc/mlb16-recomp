# The PPU critical path per frame

Date: 2026-11-02. One traced pitch run (`measure_fast.sh`, same trigger method as
[`walker-spu-wait.md`](walker-spu-wait.md)): **34.58 fps over the 10 s counter**, trace window 12.06 s / 411 frames
(34.08 fps), steady part 10.47 s / 361 frames (34.47 fps). This document maps what each frame-producing guest thread
does between flips, where it blocks, and which link sets the ~29 ms frame. Analysis only; no optimisation.

## TL;DR

The frame is set by the **RSX FIFO walker host thread**, not by the PPU main thread. Per frame (mean, steady window):

| link | ms/frame | share |
|---|---:|---:|
| **walker drain** (its own CPU, 12 passes/frame) | **17.25** | 59 % |
| walker waits for the PPU's mesh-ring link / park patch (`u_park_patched`) | 4.14 | 14 % |
| walker resume cadence after the slot PUT (`e2`) | 2.80 | 10 % |
| walker caught up with the PPU append (`u_caught_up`) | 2.02 | 7 % |
| c690 admission + running (`a` + `d`) | 2.71 | 9 % |
| queueing (lane idle / all busy) | 0.10 | 0 % |
| **walker frame = drain + put_wait** | **29.02** | **100 %** |

`ppu main` runs **9.3 ms** (mean; 13.4 ms p50) of guest logic and pushbuffer construction, then blocks **19.7 ms** in
one place: `FUN_000655cc` (0x000655cc), waiting on the ref condition (lwcond EA `0x1245090`) for the RSX walker to
consume the buffer it is about to reuse. Of that wait, the walker spends **11.6 ms draining** and **8.1 ms waiting for a
producer** — 6.2 ms of which is the PPU's own append/park work. That mutual wait (PPU waits on walker, walker waits on
PPU) is the structural cost the levers below attack.

The fine split of the walker's 11.8 ms producer wait (`analyse_walker_wait.py`, same trace) is: `u` PPU/park **6.16 ms**,
`e2` resume lag **2.80 ms**, c690 admission+running **2.71 ms**, queueing **0.10 ms**. It matches the previous run
(11.64 ms: 5.7 / 3.0 / 2.8 / 0.9), so the shape is stable.

**Answer in one line:** frame ≈ walker drain (17.3 ms) + walker producer wait (11.8 ms); the PPU's own 9.3 ms is
already overlapped, and its 19.7 ms block is the walker's remaining work it cannot overlap. Shrink the drain, pipeline
the PPU append with the walker, then shave c690 — in that order.

## 1. Threads that produce a frame

| guest thread | trace track | entry / guest address | what it does between flips |
|---|---|---|---|
| main | `ppu main` | game `_start` → frame loop `FUN_00062c1c` (0x00062c1c) | game logic; mesh-job submission; `Gfx_FrameEnd` (0x00066530) links the mesh ring into the main pushbuffer, waits the reused buffer's ref, writes put |
| DisplayFlip | `ppu DisplayFlip` | entry OPD 0x010B2D28 → `Gfx_DisplayFlip` (0x000692dc) | waits for a queued flip; waits the ref of the buffer; `cellGcmSetFlipImmediate`; waits the flip to complete |
| Async Kick | `ppu Async Kick` | entry OPD 0x010B2D00 → `FUN_00068fa4` (0x00068fa4) | waits an event (0x012465E0); writes put via `FUN_00065760`; signals 0x012465E4. Idle at the pitch (0 events in the window): `Gfx_FrameEnd` takes the synchronous kick path |
| IMFE PPU Thread | `ppu IMFE PPU Thread` | entry OPD 0x010B2D60 → `Gfx_CmdBuildWorkerThread` (0x00070894) | drains the command-build queue (cond EA `0x1246A98`) and submits mesh work items; feeds the job list the main waits on |
| Display Engine | `ppu Display Engine` | entry OPD 0x010B2BA8 | video-out / display state machine; blocked on lwcond `0x163D3870` for essentially the whole window, not on the frame path |
| — | `rsx walker` | `templates/project/main.cpp:207-260` (`frame_clock`) | the RSX FIFO walker: drains passes, waits for put / SPU slots. **The serial resource** |
| SPURS workers | `spurs pm` ×6, `spurs kernel` | `libs/spurs`, `runtime/spu/` | run the job modules, including c690 (edge geometry). 6 lanes, c690 41 % of lane time |
| others | IMFE/kinematics/sound/data-stream/GEM/FS | — | blocked ~the whole window; not on the frame path |

The frame path is therefore: **PPU main builds and links → Async Kick or main writes put → walker drains → DisplayFlip
flips → flip completes**. The SPU workers fill the walker's slots in between.

## 2. Every blocking point on that path

Guest addresses are Ghidra entry points; "runtime" is what the wait lands in.

### ppu main (render/game)

| # | guest site | wait | runtime | measured |
|---|---|---|---|---|
| 1 | `FUN_000655cc` 0x000655cc: `Mutex_LockOwned(&DAT_01245068)` 0x00065628, ref read 0x0006564c, `Cond_Wait(&DAT_01245068,9000)` **0x00065690**, unlock 0x000656a0. Called from `FUN_00065760` 0x00065798 | lwcond EA `0x1245090`, until RSX ref == the reused buffer's ref | `sys_lwmutex_lock` / `sys_lwcond_wait` (libsre, `runtime/ppu/ppu_sysprx.cpp`; the guest wrappers are `Mutex_LockOwned` 0x009a2da8 and `Cond_Wait` 0x009a3060) | **19.69 ms/frame mean** (p50 19.33); **the PPU's only per-frame block** |
| 2 | `Gfx_FrameEnd` 0x00066530, mesh branch: `FUN_0006f96c` 0x0006f96c, `FUN_0006f998` 0x0006f998, `Mesh_WaitPrevJobList` 0x0006ed24 | mesh-job counters +0x3e4/+0x3ec and WWS job-list counters 0x42-0x4e; `sys_timer_usleep(200)` at `sc` 0x0006edb0 (lr 0x0006edb4), up to 10000 tries, then `WwsJobList_Wait` 0x009fbd70 → `WwsJobList_SpinUntilDone` 0x009fb898 | `sys_timer_usleep` (`runtime/syscalls/sys_timer.c`); `cellSpursGetWorkloadInfo` | **not hit at the pitch** (no main usleep spans): the previous job list is already done when checked |
| 3 | `Gfx_FrameEnd` flip throttle: `Mutex_LockOwned(&DAT_012450d8)` 0x000673a8, `Cond_Wait(&DAT_012450d8,17000)` 0x000673c8 | lwcond EA `0x1245100` while submitted − completed flips > 2 | `sys_lwcond_wait` | **not hit at the pitch** (no main 0x1245100 spans) |
| 4 | `Gfx_FrameEnd` ref spin after an async kick (`while (ctrl->ref != iVar12) usleep(200)`) | ref register 0x03002008 | `sys_timer_usleep` | **not hit at the pitch** (`uVar23 == 0` path) |
| 5 | `Gfx_FrameEnd` flip-queue lock/unlock 0x00067210 / 0x00067238 | lwmutex `0x12450a0` | `sys_lwmutex_lock` | < 0.01 ms/frame |

`FUN_00065760` 0x00065760 is the put writer: it calls `FUN_000655cc` first (the wait above), then stores the new put
value into the GCM control register at **0x00065af8** and bumps the submitted-flip counter. `Gfx_FrameEnd` calls it at
0x000675bc; the Async Kick thread calls it at 0x0006901c. The mesh-ring link stores are 0x00066ae0 (mesh ring) and
0x00066a74 (main ring).

### ppu DisplayFlip

| # | guest site | wait | runtime | measured |
|---|---|---|---|---|
| 6 | `Gfx_DisplayFlip` 0x000692dc: flip-queue `Mutex_LockOwned/Cond_Wait(&DAT_012450a0,9000)` | lwcond EA `0x12450c8` while submitted == completed | `sys_lwcond_wait` | rarely blocks (a flip is usually already queued) |
| 7 | `Gfx_DisplayFlip`: ref `Mutex_LockOwned/Cond_Wait(&DAT_01245068,9000)` (0x000693c8 area) | lwcond EA `0x1245090` until ref == expected | `sys_lwcond_wait` | **9.31 ms/frame mean** (p50 1.06, p90 31.7) |
| 8 | `Gfx_DisplayFlip`: `Mutex_LockOwned/Cond_Wait(&DAT_012450d8,17000)` (0x00069510 area) | lwcond EA `0x1245100` until the vblank/flip handler completes the flip | `sys_lwcond_wait`; `Gfx_FlipHandler` 0x000691b0 signals it | **19.63 ms/frame mean** (p50 16.9, p90 33.9) |

DisplayFlip's two waits sum to ~29 ms, i.e. it is blocked for the whole frame and runs ~0.5 ms (the actual
`cellGcmSetFlipImmediate`). It is a display pacer, not the main thread's limiter: `ppu main` does not wait on flip
completion at the pitch.

### Async Kick / walker / SPU

| # | site | wait | runtime | measured |
|---|---|---|---|---|
| 9 | `FUN_00068fa4` 0x00068fa4: `FUN_009a31c4(&DAT_012465e0,0)` / signal `FUN_009a3224` | event flag/queue `0x12465E0` | `sys_event` (`runtime/syscalls/sys_event.c`) | idle at pitch |
| 10 | `cellGcm_fifo_kick_wait` (`libs/video/cellGcmSys.c:1922`) | `WaitForSingleObject(s_gcm_kick_ev, PS3_FRAME_CLOCK_MS)` (4 ms default, `templates/project/main.cpp:216-220`) | cellGcm present thread | bounded 4 ms; part of the walker cadence |
| 11 | FIFO walker park/catch-up and semaphore | `rsx.put_wait` / `rsx.sema_wait`; `gcm_note_spu_put` (fix off) does not wake it | `cellGcm_rsx_process_fifo`, `cellGcm_ref_on_poll` | **11.77 ms/frame mean** total put_wait |
| 12 | SPURS policy/kick, SPU mailboxes, lockline | `spurs.kick`, `spu.*_wait` | `libs/spurs/cellSpurs.c`, `runtime/spu/` | workers 57 % busy, c690 41 % |

## 3. Per-frame measurement

Tool: `tools/analyse_ppu_path.py` (new) over the trace; steady window drops 30 leading and 20 trailing frames.
Frame boundaries are `gcm.flip_request` instants. `ppu main` blocked = union of its lwcond/lwmutex/cond/semaphore/
event/usleep spans; `ppu.gcm_finish` is a run-of-ref-reads heuristic, not a wait, and is excluded.

| per frame (ms) | p50 | p90 | mean |
|---|---:|---:|---:|
| frame (flip to flip) | 33.84 | 39.22 | **29.01** |
| ppu main blocked | 19.34 | 24.37 | 19.70 |
| — ref cond `0x1245090` | 19.33 | 24.33 | **19.69** |
| ppu main running (dt − blocked) | 13.42 | 16.21 | 9.31 |
| rsx walker drain (CPU) | 21.38 | 28.65 | **17.25** |
| rsx walker put_wait | 12.23 | 15.16 | **11.77** |
| — slot wait before its covering PUT | 0.39 | 2.13 | 0.79 |
| — slot wait after the PUT (resume) | 4.57 | 11.02 | 4.62 |
| — park patched by the PPU (`u_park`) | 0.00 | 13.88 | 4.40 |
| — caught up with the PPU put (`u_caught`) | 0.92 | 4.76 | 1.96 |
| DisplayFlip ref wait | 1.06 | 31.73 | 9.31 |
| DisplayFlip flip-done wait | 16.88 | 33.90 | 19.63 |
| c690 cumulative, all lanes | 109.58 | 119.00 | 72.18 |

The before/after-covering-PUT split in that table is approximate; the authoritative a/d/e2/u split is the walker
attribution table below (`analyse_walker_wait.py`). While `ppu main` waits on the ref cond, per frame: **walker
draining 11.61 ms mean** (p50 12.53), **walker waiting for a
producer 8.08 ms mean** (p50 6.98). So about 60 % of the PPU's block is the walker's CPU work and 40 % is the walker
waiting on the producer.

Frame times are bimodal: p50 33.8 ms but mean 29.0 ms, and the 10 s counter is 34.58 fps (28.9 ms). The short frames
(8–24 ms, ~45 % of frames) are frames where the walker had less to drain; the common/p50 frame is ~34 ms. Both are
reported; the lever estimates use the mean 29.0 ms because that is what the fps counter sees.

Walker attribution over the same window (`analyse_walker_wait.py`):

| class | ms/frame | note |
|---|---:|---|
| a job not yet pushed | 1.81 | walker arrives before c690 was kicked |
| d job running | 0.90 | c690 production |
| e2 PUT visible, walker not resumed | 2.80 | park/wake cadence (`PUT→resume` p50 507 µs, p90 2.24 ms) |
| u caught up with put | 2.02 | walker outran the PPU append |
| u park patched by a non-SPU writer | 4.14 | PPU links the mesh ring (`Gfx_FrameEnd` 0x00066ae0) |
| queueing (idle lane / all busy) | 0.10 | no contention problem |
| **total** | **11.77** | |

Cross-checks: 412 c690 jobs/frame; c690 job p50 128 µs / p90 261 µs; 6 lanes configured, 57.3 % busy, c690 41.1 %;
lane 5 idle. This reproduces the previous attribution run within noise.

## 4. The serial chain

Per frame, with means from the steady window:

```
ppu main:  |<-- 9.3 ms game logic + Gfx_FrameEnd build/link -->|<-- 19.7 ms ref wait (FUN_000655cc) -->|
           (overlapped with the walker)                          |  11.6 ms walker draining
                                                                  |   8.1 ms walker waiting for a producer
rsx walker: |<----------------------------- 29.0 ms frame = 17.3 ms drain + 11.8 ms put_wait ------------->|
             during the PPU's ref wait it is either draining (11.6 ms) or starving (8.1 ms)

DisplayFlip: waits ref (9.3) -> cellGcmSetFlipImmediate -> waits flip done (19.6); ~29 ms, a pacer, not the limiter
c690/SPURS:  412 jobs/frame, 72 ms cumulative over 5 active lanes (~14 ms/lane), 41 % lane utilisation
```

The chain is not `main(run) + main(wait)`: the walker runs during the main thread's 9.3 ms, so the frame is
`walker drain + walker producer wait`, and main's block is the part of that the main cannot overlap. The
mutual wait is real and measured:

* main waits for the walker to reach the reused buffer's ref (`FUN_000655cc`),
* the walker waits 6.2 ms/frame for the PPU's own append/park (`u_park` 4.4 + `u_caught` 2.0),
* the walker waits 2.7 ms/frame for c690 production and 2.8 ms for its own resume cadence.

That is the same conclusion as the wake experiment, now with the PPU side quantified: **waking the walker earlier
does not help while the PPU producer is itself serialised behind the walker's progress.** The e2 residue is still
2.8 ms and the wake fix is still off by default.

## 5. What can overlap, and what the game requires

Can overlap (and partly does):

* **PPU game logic with the walker drain.** Main's 9.3 ms already overlaps. A deeper ring would let it overlap the
  *whole* walker frame instead of 9.3 ms, hiding the `u_park`/`u_caught` handoff.
* **c690 production with the walker drain.** c690 is 41 % of lane time; the lanes are not saturated. Jobs only need
  to be queued earlier (before the walker reaches their slot) to hide the 2.7 ms of admission+running.
* **The two flip-thread waits.** DisplayFlip's flip-done wait overlaps the next frame's main/walker work; it is not
  on the main path.

Required by the game's own ordering:

* the SPU mesh job list must be complete before `Gfx_FrameEnd` links the mesh ring
  (`Mesh_WaitPrevJobList`/`FUN_0006f998`). At the pitch this is already satisfied and costs ~0, so it is a
  correctness fence, not a measured cost.
* the PPU must not overwrite a ring buffer the RSX walker has not consumed. `FUN_000655cc` enforces this with the
  ref equality check. It is required for the current 2–3 buffer ring; a deeper ring (or an explicit ownership handoff)
  can relax it without breaking the rule. This is why the failed `MLB16_PIPELINE` `(id+1)&3` experiment corrupted
  geometry: it reordered the ring without adding buffers.
* the flip must be queued and the RSX must reach it before `Gfx_DisplayFlip` can flip. This orders display, not the
  next PPU frame (main does not wait on flip completion at the pitch).

## 6. Levers

Estimates start from the measured mean 29.0 ms (34.5 fps). They are upper bounds and not additive where they share
the same wait.

| # | lever | link removed | frame | fps | risk |
|---|---|---|---:|---:|---|
| 1 | **Cut walker drain CPU** (17.3 ms, 59 %). Vertex remap/texture checks/recording; the walker is ~40 µs/draw over ~440 draws | ~5 ms at −30 % walker CPU | 29.0 → 24.0 | **~42** | **low–medium**: `replay_rsx --verify` state hash catches command-stream changes; needs the pitch diff + a longer run for visual regressions |
| 2 | **Pipeline the PPU append with the walker** (deeper ring / explicit ownership; wake on the PPU put). Hides `u_park` 4.4 + `u_caught` 2.0 + most of `e2` 2.8 | up to ~8 ms | 29.0 → 21.0 | **~45–48** | **high**: the ref fence guards buffers the walker reads; the previous `(id+1)&3` reorder failed (185 flips/689 s). Needs more buffers, not reordering; verify geometry and long runs |
| 3 | **Get c690 off the walker's critical path** (earlier scheduling first; Design B/GPU offload if that is not enough) | a+d 2.7 ms (earlier schedule ~1.8; Design B ~2.8) | 29.0 → 26.3 | **~38** | **medium** for earlier scheduling (correctness unchanged, but may not help if input is gated by the PPU); **high** for Design B (2–4 months, GPU path) |

Combined 1 + 2 is the 60 fps path: 29.0 − 5 − 8 ≈ 16 ms (~62 fps) if both land fully; a conservative 18–20 ms is
**50–55 fps**. Lever 3 alone does not reach 55–60, matching [`c690-offload-decision.md`](c690-offload-decision.md).

Do **not** repeat the wake-on-slots experiment as a standalone fix: e2 is 2.8 ms and removing it relocates the wait
(measured in [`walker-wake.md`](walker-wake.md)). It is useful only as part of lever 2, together with a deeper ring.

## 7. Instrumentation

`ps3recomp-probe` on `mlb16-work`, trace events only, off by default (`PS3RECOMP_TRACE` unset ⇒ one load + branch):

* new `ppu.usleep` span in `runtime/syscalls/sys_timer.c`: every `sys_timer_usleep` ≤ 500 µs, tagged with the guest
  LR. This is what exposes the engine's 200 µs poll loops (Mesh/job-list/fence spins) as named waits. At the pitch
  the main thread has none; the 2,720 spans in the window are sound/kinematics pollers, which is itself the proof
  that the mesh fences are not on the critical path here.
* `ppu_thread_note_block` now closes an open `ppu.gcm_finish` poll run
  (`ps3_trace_poll_end(PS3_TR_PPU_GCM_FINISH)`), so a ref-read run is not left open across the next wait. The old
  unbounded run made the fence spin look like a 19.6 ms/frame block; after the fix `ppu.gcm_finish` on main is
  ~30 µs/frame of inter-poll code, and `ppu.lwcond` `0x1245090` is the real 19.7 ms block.

## Caveats

* One run, one window. The steady window is 361 frames / 10.47 s; the trace adds no measurable cost on the 10 s
  counter (34.58 fps vs the 34.63 no-trace baseline), but the frame-time distribution is bimodal (p50 33.8, mean
  29.0), so per-link means and p50s are both given.
* The lwcond waits return about every 1.06 ms because the lwcond implementation falls back and re-waits; the span
  counts are per wake-up, not per logical wait. Total blocked time is unaffected.
* "Running" is frame interval minus blocked; it includes host scheduling. The machine had 12 threads and the other
  PPU threads are blocked, so the error is small but not zero.
* The `u` writer is identified statically (`Gfx_FrameEnd` 0x00066ae0) and as "no SPU PUT covers the park" in the
  trace; no PPU write-side hook was added.
* The walker's before/after-covering-PUT split in `analyse_ppu_path.py` is approximate; the authoritative a/d/e2/u
  split is `analyse_walker_wait.py`.

## Reproduce

```
# one pitch run, trigger the window when the pitch reference matches (delete stale frame.ppm first)
EXTRA="PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/ppu_trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/ppu_go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=16 PS3RECOMP_TRACE_FLUSH_MB=8" \
  recomp-work/ab-tools/measure_fast.sh ppu_path.log

python3 tools/analyse_ppu_path.py /tmp/ppu_trace.json --skip-first 30 --skip-last 20 \
  --md ppu_path.md --json ppu_path.json
python3 tools/analyse_walker_wait.py /tmp/ppu_trace.json \
  --manifest recomp-work/spu_lifted/jobmods_manifest.json --md walker.md
```
