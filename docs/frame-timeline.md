# Frame timeline at the pitch meter: the wait inventory, and where the 34 ms goes

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. One windowed meter run (`ab_live_timeline_20261102_233854`),
`METER=1`, `SKIP_CINE=1`, `WCB=2`, `PIN=1`, x11, `PRESENT_NEW_ONLY=1`,
`PACE=1`, `CUBE_RT=1`, the four cheap-win live switches on. Analysis and
instrumentation only: no optimisation in this mission.

This document answers the question the profile could not: *when* did the
event a wait wanted actually happen, and how long after it did the waiter
run. The result is that the meter's frame time is **work, not late
wake-ups**. At the meter the walker drains 26.4 ms/frame (p50 25.8, p90 42.4)
while the c690/mesh producer keeps its six lanes busy; the only measured
wake-up lag on the critical path is the PPU ref lwcond's **0.18 ms/frame**
(a 1 ms poll, mean 0.44 ms when it wakes), and the walker's own idle wait
wakes **10 µs** after its kick.

## TL;DR

* **Frame time, 300 consecutive meter frames:** p50 **33.85 ms**, p90 51.3,
  p99 59.6, mean 33.6 (full 45 s window: p50 33.9, p90 51.0, p99 58.6).
* **The frame is walker work.** `rsx.drain` is 26.4 ms/frame (78 % of the
  frame); the walker's idle wait is only 3.17 ms and its wake lag 0.00 ms.
  The PPU main's own cycle is `12.5 ms work + 20.5 ms ref wait`; the ref wait
  is the walker's work viewed from the other thread.
* **Short/long alternation is the c690/mesh hand-off.** On 256 of 300 frames
  the walker drain is the critical chain: 35.7 ms frames with 28.7 ms drain,
  24.7 ms of c690 lane time, 3.4 ms idle. On 37 frames the walker has nothing
  queued (11.4 ms drain, 0.44 ms idle, 2.3 ms c690): the frame collapses to
  main's ref-wait cycle, **19.5 ms**. The class changes 35 times each way in
  300 frames.
* **Wake-up lags are not the problem.** Ref lwcond lag 0.18 ms/frame overall
  (0.50 ms on the frames where it is the critical chain); walker idle lag
  0.00 ms/frame; flip-done lwcond (DisplayFlip) 0.40 ms/frame but off the
  guest critical path; present fence 3.30 ms/frame is GPU blit execution, not
  host wake. Making any of these event-driven changes p50 and p99 by **0**;
  the best case is −0.18 ms mean.
* **Overhead of the timeline** (same run, recorded window vs after it shut
  down): guest flip-to-flip p50 +0.03 ms, p90 +0.29, p99 +0.71, mean
  **+0.32 ms/frame**. 6,815,181 events, 218 MB, 0 dropped, 41 threads.
* **c690 is prompt, not late.** 578,384 jobs (435/frame) end-to-first-PUT
  p50 **3 µs**, p90 5 µs, p99 10 µs. The walker's kick-to-run lag is p50
  ~10 µs. The producer's *job work* is on the frame; its *wake-ups* are not.

## 1. Sleep / poll / timer inventory

Everything a meter frame can pass through, on `exp/live-ab` at the time of
the run. "Meter path" means the wait is on the chain that produces the next
guest flip; a wait that exists but does not block in this scene is marked no.

### 1.1 PPU guest waits

| wait | file:line | mechanism | default / override | thread | meter path |
|---|---|---|---|---|---|
| **FUN_000655cc ref cond** (`lwcond` EA `0x1245090`) | `runtime/ppu/ppu_sysprx.cpp:497`, poll `:518-525` | guest lwcond modelled as owner/recur stamps; waiter releases the lwmutex, `Sleep(1)`, re-takes it; `PS3_LWCOND_REAL=1` swaps in a host CV (`runtime/ppu/lwcond_real.h`, fallback `PS3_LWCOND_FALLBACK_MS=10`) | `Sleep(1)`; no env needed | `ppu main` (boot thread) | **yes, 20.5 ms/frame** |
| DisplayFlip flip-done cond (`0x1245100`) | `ppu_sysprx.cpp:497` | same | `Sleep(1)` | `ppu DisplayFlip` | display pacer, ~30 ms blocked; not the guest frame |
| DisplayFlip flip-queue/ref conds (`0x12450c8`, `0x1245090`) | `ppu_sysprx.cpp:497` | same | `Sleep(1)` | `ppu DisplayFlip` | no (queue is usually non-empty) |
| lwmutex lock | `ppu_sysprx.cpp:235` | CAS on the state word; contended wait is a host semaphore with the guest's timeout | r4 timeout, 0 = infinite | main / DisplayFlip | lock only (<0.01 ms) |
| `sys_cond_wait` | `runtime/syscalls/sys_cond.c:153`, `:175`, `:191` | host CV, timed wait | guest timeout | guest PPU | no |
| `sys_semaphore_wait` | `runtime/syscalls/sys_semaphore.c:396`, `:441`, `:455` | host CV (Linux), 1 ms slices (Windows) | guest timeout | guest PPU | no on the meter |
| `sys_event_queue_receive` | `runtime/syscalls/sys_event.c:479-580` | host CV; sub-ms timeouts are a poll/yield | guest timeout | guest PPU | no |
| `sys_event_flag_wait` | `runtime/syscalls/sys_event.c:1256-1336` | host CV / timed | guest timeout | guest PPU | no |
| `sys_mutex_lock` | `runtime/syscalls/sys_mutex.c:278-291` | pthread mutex; `SwitchToThread` for <1 ms, `Sleep(1)` otherwise | guest timeout | guest PPU | no |
| `sys_timer_usleep` | `runtime/syscalls/sys_timer.c:137`, `nanosleep` `:288` | Linux `nanosleep` for every `usec > 0`; Windows busy-yields below 1 ms | guest arg | guest PPU | not hit at the meter (`docs/ppu-critical-path.md:63`); loading/hitches |
| `sys_timer_sleep` | `runtime/syscalls/sys_timer.c:301-309` | `sleep(sec)` | guest arg | guest PPU | no |
| `sys_spinlock` | `ppu_sysprx.cpp:927` | CAS spin in a guest word | — | guest PPU | no |

### 1.2 GCM / RSX walker

| wait | file:line | mechanism | default / override | thread | meter path |
|---|---|---|---|---|---|
| **Walker idle between passes** | `templates/project/main.cpp:246-258` -> `libs/video/cellGcmSys.c:2184`, wait `:2196/:2202` | auto-reset kick event, `WaitForSingleObject(ev, ms)`; woken by an SPU PUT that covers the park, a dry ref poll, or a WCB fault | timeout `PS3_FRAME_CLOCK_MS=4` ms; `GCM_FIFO_KICK` on by default; `PS3RECOMP_WALKER_WAKE` (off) kicks on the slot PUT; `EXP_FAST_WAKE` yields instead | `rsx walker` | **yes, 3.17 ms/frame, wake lag 10 µs** |
| Display tick | `main.cpp:280` | `next_tick += tick_period_qpc()`; flips complete on this beat | `EXP_TICK_US=16000` (live), `PS3_FRAME_CLOCK_MS` is only the idle wait | `rsx walker` | quantises flips, not a wait |
| GCM semaphore acquire | `cellGcmSys.c:3135-3210` | a pass stops on `ACQUIRE`; bounded wait bookkeeping | `GCM_SEMA_ACQUIRE=0` (off), `GCM_SEMA_ACQUIRE_MS` | `rsx walker` | no (off by default) |
| `cellGcmSetWaitFlip` | `cellGcmSys.c:718-731` | `Sleep(1)` up to 64 times | — | guest PPU | only when a title uses it; DisplayFlip uses flip-immediate |
| FIFO recycle wait | `cellGcmSys.c:3700-3714` | `Sleep(1)` until the walker takes the jump, bounded 2 s idle / 60 s total | — | guest writer | loading only |
| Ref publication pacing | `cellGcmSys.c:2244-2252` | one publish per ~200 µs on the poll path (`GCM_REFPOLL` on) | ~200 µs | any guest poller -> kicк | rate limit, not a sleep |
| GCM handler thread | `cellGcmSys.c:904-906` | `Sleep(1)` (Windows) / `usleep(250)` (POSIX) pump loop | `GCM_HANDLER_THREAD=1` in `play_windowed.sh` | `gcm intr` | delivers the user handler that wakes the ref wait |

### 1.3 SPU / SPURS

| wait | file:line | mechanism | default / override | thread | meter path |
|---|---|---|---|---|---|
| SPU channel wait | `runtime/spu/spu_channels.c:1275-1325` | SRW condition variable with a **10 ms** timeout, re-checking the channel predicate | 10 ms | `spurs pm` | no channel waits on the meter |
| SPU GETLLAR / PUTLLC | `spu_channels.c:754-822`; lock line `runtime/spu/spu_coherency.c:94-120` | guest atomic op; the host lock line retries 20 µs with backoff then parks on a pthread mutex | — | `spurs pm` | c690 job body; idle polls during loading |
| SPURS physical-lane worker | `libs/spurs/cellSpurs.c:1761-1770` | CV `INFINITE`, woken by the kernel | — | `spurs pm` | yes (not the critical one; c690 is) |
| SPURS kernel | `cellSpurs.c:1801-1816` | timed CV, **1 ms** | — | `spurs kernel` | admits c690 435 times/frame |
| CRI/taskset wait | `runtime/spu/spu_workload.c:1493` | CV with **1000 ms** timeout | — | CRI tasks | audio, not the meter |

### 1.4 Present, audio, I/O

| wait | file:line | mechanism | default / override | thread | meter path |
|---|---|---|---|---|---|
| Present queue | `libs/video/rsx_vulkan_backend.c:2200` | pthread CV, waits for a queued present | — | `present` | parallel |
| Present fence | `rsx_vulkan_backend.c:2217` | `vkWaitForFences(..., UINT64_MAX)` on the swapchain blit | — | `present` | parallel, 3.3 ms (GPU) |
| Present pacer | `rsx_vulkan_backend.c:2225` | `nanosleep` in ≤2 ms slices until the arrival-cadence due time | `PS3RECOMP_VK_PACE=1` | `present` | no (does not hold guest work) |
| Swapchain acquire | `rsx_vulkan_backend.c:2370` | `vkAcquireNextImageKHR` timeout | `PS3RECOMP_VK_ACQUIRE_MS` | `present` | no |
| Audio mix loop | `libs/audio/cellAudio.c:664-666` | `usleep(2000)` / `usleep(5000)` on POSIX | 2/5 ms | `audio mix` | no; shares cores |
| File open retry | `runtime/syscalls/sys_fs.c:428`, `:435` | `Sleep(1)` / 1 ms `nanosleep` | — | guest FS | loading |
| Synchronous file reads | `libs/filesystem`, `sys_fs.c` (`pread`) | blocking host syscall | — | guest FS | loading |

### 1.5 What the user saw during loading: two cores at 100 %

The only threads that burn a full core with **no** OS wait in the loop are
the SPU host threads running lifted guest code that polls (`GETLLAR` /
128-byte GET) and the walker when the FIFO is continuously fed. The runtime
side of the spin is `spu_channels.c:754-822` (the guest GETLLAR loop is a
tight host loop with no GET/LLAR back-off; NHL item #45) and
`spu_coherency.c:94-120` (the lock line itself backs off after 20 µs, so it
is the guest loop, not the lock, that keeps the core). The idle SPURS policy
module is the classic case: on this title it polls its job queue, so an idle
lane is a 100 % lane until the scheduler runs something else.

`PS3RECOMP_PIN=1` puts `rsx walker` on cpu 0 and `ppu main` on cpu 1 and
leaves every other named thread on cpus 2-5 and 8-11
(`docs/live-ab-and-ceilings.md`, pin banner). **CPU 10/11 are therefore
unpinned SPU threads: two `spurs pm` workers** (policy/idle poll loops, or
back-to-back c690/mesh jobs during loading), not the walker or main. This is
a static/deduction answer: the planned `top -H` sample at boot+60 s did not
run (see Deviations), so the next run should confirm with
`top -H -b -n 1 -p $(pgrep -x MyGameRecomp)` and look for the `spurs pm`
tasks.

## 2. The event timeline (`PS3RECOMP_TIMELINE=1`)

### 2.1 Sink and file format

The existing `PS3RECOMP_TRACE` machinery (`include/ps3emu/ps3_trace.h`) is a
per-thread single-producer ring of 32-byte `ps3_tr_rec` records
`{t0, t1, a, b, ev, pad}`; the flusher formats them as Chrome JSON. The new
binary mode writes the same rings **without formatting**:

```
PS3RECOMP_TIMELINE=<path|1>       (1 -> ./ps3_timeline.bin)
PS3RECOMP_TIMELINE_RING_MB=32     per-thread ring (records are 32 B)
PS3RECOMP_TIMELINE_FLUSH_MB=8     pending records that trigger a drain
PS3RECOMP_TIMELINE_TRIGGER=<path> start only when this file appears
PS3RECOMP_TIMELINE_SECS=45        window length after the trigger
```

File: `"PSTL"`, `u32 version`, `u32 record size`, `u64 base`, then chunks:

* `u32 kind=1, u32 tid, u32 len, name bytes` (padded to 4) — thread name;
* `u32 kind=2, u32 tid, u32 count, count * 32-byte record` — events.

A truncated final chunk (SIGKILL) is tolerated: the analyser stops at the
short read. Records keep absolute `CLOCK_MONOTONIC` ns, the same clock as
the `[fliplog]` timestamps, so the two files can be joined without a
conversion. `PS3RECOMP_TRACE` still writes JSON; the timeline only changes
the sink. With the variable unset, every hook is one load and a
not-taken branch, and no state is touched.

### 2.2 Events

Existing trace events (already used by `analyse_walker_wait.py`) plus the
timeline ones, all on the walker / main / present / SPU threads:

| event | where | meaning |
|---|---|---|
| `gfx.frame_end` | `cellGcmSys.c:3879` (`gcm_flip_request`) | the flip word the walker decoded: the **frame boundary** |
| `ppu.frame_begin` | `ppu_sysprx.cpp` ref-wait wrapper | main resumed from the ref wait (with the ref value it saw) |
| `ppu.lwcond` (0x1245090 etc.) | `ppu_sysprx.cpp:542` | the ref / flip-queue / flip-done waits |
| `wait.wake_lag` | emitted at each instrumented wake | **lag = wake − newest causal event**; `b` = cause \| (wait id << 8) |
| `rsx.drain` | `cellGcmSys.c:2429` (`gcm_tr_pass`) | one FIFO walker pass |
| `rsx.idle_wait` | `cellGcmSys.c:2206` | the 4 ms kick wait (with its own lag) |
| `rsx.fifo_put_seen` | `cellGcmSys.c:2433` | put the pass acted on and the get it reached |
| `gcm.ref_set` / `gcm.ref_pub` | `cellGcmSys.c:2049`, `:2085` | `SET_REFERENCE` executed / ctrl->ref made visible |
| `gcm.spu_put` | `gcm_note_spu_put`, `cellGcmSys.c:1796` | the producer's DMA signal into RSX memory |
| `c690.job` | `runtime/spu/c690_stats.c:234` | one c690 job (needs `PS3RECOMP_C690_STATS=1`) |
| `rsx.wcb_done` | `rsx_draw_engine.c:1003` (`eng_wcb_attr_note`) | one readback copy finished (KiB, duration) |
| `rsx.draw_submit` | `rsx_vulkan_backend.c:4590`, `:4697` | a draw batch was submitted |
| `present.queued` / `.submit` | `rsx_vulkan_backend.c:2485-2488` | a new guest frame reached the window / its blit was submitted |
| `present.fence_wait` | `rsx_vulkan_backend.c:2217` | the present thread waiting the blit fence |
| `present.vkqueue` / `.shown` | `rsx_vulkan_backend.c:2258`, `:2261` | `vkQueuePresentKHR` and the display seeing the image |
| `gcm.user_handler` | `cellGcmSys.c:953` | the guest user handler that pumps the ref/flip conditions |

The causal notes (`ps3_trace_note`) are process-wide and racy by design:
the newest ref publish / flip completion / flip request / walker kick / SPU
PUT / present submit / user handler is timestamped, and the waiter subtracts
it. A stale note only makes the measured lag larger, never invents one.

### 2.3 Overhead

Same launch, same meter, two adjacent windows: the 45 s recorded window and
the 30-80 s after the window closed (the flusher shut the sink down, so every
hook is back to one load/branch). Guest flip-to-flip from `[fliplog]`:

| window | n | p50 | p90 | p99 | mean | std |
|---|---:|---:|---:|---:|---:|---:|
| timeline on | 1320 | 33.90 | 51.28 | 59.60 | 34.21 | 13.59 |
| timeline off, after | 2164 | 33.87 | 50.99 | 58.89 | 33.89 | 13.54 |
| difference | | **+0.03** | **+0.29** | **+0.71** | **+0.32** | +0.05 |

The mean and p50/p90 are inside the 0.5 ms budget; the +0.71 ms p99 is a
~20-sample tail estimate and is not separable from the meter's own drift
(the window and the post-window are 5 s apart, and the scene is stable but
not frozen). The recorded window held 6,815,181 events (218 MB) with 0
dropped.

## 3. The meter run and the per-frame critical path

Run: one launch, 179 s to the pitch, X once, meter confirmed 3 s later;
timeline armed 20 s later for 45 s. 1,329 guest frames in the window; the
300 consecutive frames below are frames 200-499 (seconds ~20-30 of the
window) to drop the first frames after the trigger. Full per-frame CSV:
`recomp-work/ab_live_timeline_20261102_233854/timeline_per_frame.csv`.

### 3.1 Frame distribution (300 frames)

| metric | value |
|---|---:|
| p50 / p90 / p99 | **33.85 / 51.3 / 59.6 ms** |
| mean / std | 33.6 / 13.8 ms |
| < 25 ms / 25-35 / 35-45 / > 45 | 83 / 138 / 12 / 67 frames |

Full 45 s window (1,328 intervals): p50 33.90, p90 50.96, p99 58.57.

### 3.2 Per-frame chain

The walker is a serial thread: frame = drain + idle + its non-drain work
(present submit, WCB service, lock). For the 300 frames:

| term (ms/frame) | mean | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| frame (flip to flip) | 33.64 | 33.85 | 51.32 | 59.60 |
| walker drain (own CPU) | **26.37** | 25.81 | 42.40 | 53.06 |
| walker idle wait (kick event) | 3.17 | 2.19 | 7.69 | 12.55 |
| — of which wake lag | **0.00** | 0.00 | 0.00 | 0.00 |
| walker other (present submit, WCB, lock) | 4.10 | 5.00 | 8.57 | |
| main blocked (all waits) | 21.10 | 18.95 | 32.90 | 40.49 |
| main work (frame − blocked) | 12.54 | 17.73 | 22.24 | 25.96 |
| main ref wait (`0x1245090`) | **20.54** | 18.76 | 32.07 | 39.74 |
| — of which wake lag | **0.18** | 0.00 | 0.75 | 1.04 |
| c690 lane time (union, 6 lanes) | 22.05 | 32.13 | 35.66 | 38.92 |
| WCB copies (4.5/frame) | 0.64 MiB, 8.5 ms summed | | | |
| present fence wait (present thread) | 3.30 | 4.68 | 5.58 | 11.86 |

`drain + idle + other` is 33.6 ms, i.e. the whole frame; the main's ref
wait is inside it (20.5 of 33.6), which is the mutual wait the earlier docs
describe from the PPU side.

Classification by the dominant chain (`>65 %` of the frame):

| class | frames | frame | drain | idle | other | ref | ref lag | c690 union |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **walker drain** | 256 | 35.68 | 28.68 | 3.44 | 3.56 | 20.84 | 0.14 | **24.65** |
| **main ref wait** | 37 | **19.45** | 11.38 | 0.44 | 7.63 | 19.24 | 0.50 | **2.30** |
| mixed | 7 | 34.04 | 20.93 | 7.81 | 5.31 | 16.22 | 0.00 | 31.60 |

Short (`≤30 ms`, 104 frames): frame 20.6, drain 14.5, idle 0.6, ref 20.4,
ref lag 0.48, c690 2.6. Long (`>40 ms`, 79 frames): frame 50.5, drain 41.2,
idle 5.5, c690 35.2, ref lag 0.02.

### 3.3 Why frames alternate short/long

The hand-off that flips is **the c690/mesh producer feeding the walker**.

* A **main-ref frame** is a frame with an empty FIFO: c690 lane time 2.3 ms,
  walker drain 11.4 ms, idle 0.44 ms. Main runs its own 19.5 ms cycle and
  blocks on the ref cond; the walker has nothing to execute, so the frame is
  short (19.5 ms).
* A **walker-drain frame** is a frame with a full batch ready: c690 lane time
  24.7 ms (all six lanes nearly solid), walker drain 28.7 ms. Main's ref wait
  is 20.8 ms inside a 35.7 ms frame, so the walker, not main, sets the
  length. The longest frames (>45 ms) are this class with a 40-44 ms drain.

The transition counts in 300 frames are 35 walker→main-ref, 35
main-ref→walker, 215 walker→walker; the lag-1 autocorrelation of frame
length is 0.02, i.e. the two regimes interleave rather than run in long
blocks. That is the same producer burst the earlier docs saw as "two guest
flips handed to the window back to back": the guest catches up a c690/mesh
batch, drains it (long frame), then runs a main-only frame while the next
batch is being built (short frame). The frame-time plot is a dense sawtooth
between ~15-22 ms and ~45-60 ms (below).

The present side is not the cause: with `PACE=1` the shown intervals are p50
37.6 ms, p90 50.8, p99 59.3, and only **0.15 %** are below 8 ms (the pacer
removed the pairs from `docs/frame-pacing.md`). The guest cadence itself
alternates; the pacer only redistributes the display times.

### 3.4 Ranked list: wake-up lags and waits

Ranked by the lag they can actually remove from the guest frame, not by raw
span length (a 20 ms ref wait that is 20 ms of walker work is not a wake
problem). "At p50/p99" is the frame-time change if the wait became
event-driven and the measured lag disappeared on the frames where that wait
is the critical chain.

| # | wait (file:line) | ms/frame as a wait | measured wake lag | event-driven buys at p50 / p99 | kill criterion |
|---:|---|---:|---|---|---|
| 1 | ref lwcond `0x1245090` (`ppu_sysprx.cpp:497`, poll `:525`) | 20.54 (overlapped; 19.24 of the 19.45 ms main-ref frame) | **0.18** mean/frame, 0.50 on main-ref frames, p90 0.75, p99 1.04 | **0 / 0**; best case −0.18 ms mean. `PS3_LWCOND_REAL=1` already exists | p50 gain < 0.5 ms → do not do it for p50; tail-only, and only if a later run shows the lag growing |
| 2 | walker idle (`cellGcmSys.c:2184`, wait `:2202`) | 3.17 | **0.00** (10 µs mean per wake) | 0 / 0; already an auto-reset event. `PS3RECOMP_WALKER_WAKE` adds wakes and was −7.5 % fps (`walker-wake.md`) | any change that increases passes/frame; keep as is |
| 3 | present fence (`rsx_vulkan_backend.c:2217`) | 3.30 | 3.25 ms submit→fence (this is **GPU blit execution**, not scheduler lag) | 0 / 0 to guest frames; it is already a fence wait, and the pacer is downstream | do not touch for frame time; it is off the guest chain |
| 4 | DisplayFlip flip-done `0x1245100` (`ppu_sysprx.cpp:497`) | ~30 ms blocked on the display thread | 0.40 mean/frame (584 µs/wake) | 0 / 0; DisplayFlip is not the guest frame producer at the meter | no guest fps change |
| 5 | DisplayFlip flip-queue `0x12450c8` (`ppu_sysprx.cpp:497`) | rarely blocks | 0.45 mean/frame | 0 / 0 | same |
| 6 | SPU channel waits (`spu_channels.c:1275`) | 0 on the meter | — | 0 / 0; c690 lanes run jobs, they do not block on channels here | — |
| 7 | `sys_cond` / `sys_semaphore` / `sys_event` (inventory 1.1) | 0 on main at the meter | — | 0 / 0 | — |
| 8 | `sys_timer_usleep(200)` in `Gfx_FrameEnd` | not hit (`docs/ppu-critical-path.md:63`) | — | 0 / 0 | — |

The real levers are therefore **work**: the 26.4 ms walker drain (which is
78 % of the frame and 28.7 ms on the long frames), the c690 producer behind
it (435 jobs/frame, 578,384 in 45 s, p50 129 µs each), and the WCB copies
(4.5 per frame, 0.64 MiB, ~8.5 ms of copy time summed, some of it inside the
drain). The timeline does not support an event-driven fix for p50.

### 3.5 Images

* `recomp-work/ab_live_timeline_20261102_233854/frame_time_timeline.png` —
  guest flip-to-flip for all 1,328 intervals of the 45 s window, with p50
  33.9, p90 51.0, p99 58.6 and the 16.7/33.3 ms guides. It is a dense
  sawtooth: low points around 15-22 ms alternate with 45-60 ms spikes; there
  are no long flat stretches. The blue box marks the 300 analysed frames.
* `recomp-work/ab_live_timeline_20261102_233854/timeline_5frames.png` —
  five consecutive frames F300-F304 (22.4, 54.4, 33.9, 24.3, 33.9 ms),
  threads as rows. Reading it: `ppu main` is green only in the few gaps
  between red ref waits; `rsx walker` is solid blue (drain) on F301/F302/
  F304 and short on F300/F303; the six `spurs pm` rows are solid purple
  (c690) exactly on the long frames and nearly empty on F300/F303;
  `present` shows one fence wait and one shown dot per frame; `ppu
  DisplayFlip` is blocked red across the whole strip (display pacer). The
  picture is the hand-off in 3.3.
* The MangoHud plot of the same run is
  `recomp-work/ab_live_timeline_20261102_233854_ft.png`; it includes the
  menus and loading, so use the band at ~180 s onward for the meter.

## 4. Reproduce

```sh
# build
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp

# one meter run with a 45 s binary window armed 20 s after the meter is up
export PS3RECOMP_TIMELINE=/tmp/tl.bin
export PS3RECOMP_TIMELINE_TRIGGER=/tmp/tl.go
export PS3RECOMP_TIMELINE_SECS=45
export PS3RECOMP_TIMELINE_RING_MB=32 PS3RECOMP_TIMELINE_FLUSH_MB=8
export PS3RECOMP_C690_STATS=1 PS3RECOMP_FLIP_LOG=1 PS3RECOMP_WALKER_PROF=1
export PS3RECOMP_VK_PACE=1 PS3RECOMP_RSX_CUBE_RT=1
export SPU_MFC_LOOKUP_CACHE=1 GCM_NOP_SKIP=1 GCM_REF_IMMEDIATE=1 PS3RECOMP_RSX_TEX_PIN=1
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=timeline METER=1 SKIP_CINE=1 WCB=2 PIN=1 VIDEODRIVER=x11 \
  N=1 SEG=90 DROP=0 SETTLE=10 ARM_A="" ARM_B="" ./ab_live.sh &
# when the log says "pitch meter confirmed", wait 20 s, then:
#   date +%s%3N > /tmp/tl.origin; touch /tmp/tl.go
```

The run folder keeps `timeline_per_frame.csv`, `analysis_timeline.py`
(the per-frame attribution) and `make_timeline_plots.py` (both PNGs). The
raw `/tmp/tl.bin` is 218 MB and was deleted after the analysis.

## Caveats

* One launch, one timeline window. The 300-frame table is frames 200-499 of
  1,329; the full-window percentiles differ by <0.1 ms.
* `ppu main` appears as `host thread 7` in the binary: the boot thread was
  named before the trace ring existed. It is identified by its only lwcond,
  `0x1245090`, and by the flip/ref correlation; the docs' `ppu main` names
  are the same thread.
* The ref wait is instrumented at the HLE `sys_lwcond_wait`, so it includes
  non-blocking calls; the tables above use only spans >20 µs when counting
  "blocked", which is why 25,755 calls reduce to ~1,313 blocking waits.
* c690 spans need `PS3RECOMP_C690_STATS=1` (the run had it). Without it the
  timeline still has the walker/present/ref events but no c690 rows.
* The c690 "lane union" is the union of all six lanes, not the sum; it is a
  lower bound on lane occupancy and an upper bound on wall time.
* The loading CPU sample did not run (the `top -H` helper produced no file);
  section 1.5 is static/deduction, not a measured per-thread CPU table.
* `PS3RECOMP_TIMELINE_RING_MB=32` was chosen to make drops impossible at
  ~2,200 SPU PUTs/frame; the run reports 0 dropped. A smaller ring is fine
  with a higher flush rate.
