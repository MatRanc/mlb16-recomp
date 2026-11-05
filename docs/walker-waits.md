# Walker waits: where the "rsx walker" thread blocks inside a drain

Date: 2026-11-05. Branch `exp/walker-waits` (off `exp/walker-diet`) in `ps3recomp-depth`, build `recomp-work/build-depth`
(incremental `replay_rsx_engine` and `MyGameRecomp` only). **Zero game launches.** Follows
[`walker-diet.md`](walker-diet.md), [`crowded-walker-profile.md`](crowded-walker-profile.md) and
[`walker-prof.md`](walker-prof.md). No RPCS3 or other GPL emulator source was read or cited.

## Why

`captures/walker_live.perf` (20 s, crowded, zoomed out) has the walker on-CPU about 52% of the window, while
`[walker-prof]` reports walker 60-400 ms and parse 20-73 ms per heavy frame with `wait=0`. `wait` is wall time between
reports minus drain time, so it only sees idle between drains. Anything the walker blocks on *inside* a drain lands in
`parse` (time outside the per-method dispatch timestamps: flip handling, `eng_present`, WCB service) or in an engine
phase (`tex`, `draw`, `other`). Nothing counted it. This adds the missing split.

## Blocking sites on the walker thread

The walker thread is the ticker that calls `cellGcm_rsx_process_fifo` (`cellGcmSys.c:2664`). Between drains it sits in
`cellGcm_fifo_kick_wait` (`cellGcmSys.c:2437`, `WaitForSingleObject` on the kick event, 4 ms); that is the `wait` term,
not counted here. Inside a pass:

| key in `[walker-waits]` | what | file:line (exp/walker-waits) |
|---|---|---|
| `fifo_lock` | `s_gcm_fifo_lock` acquire, taken before the drain timer starts (so not part of `walker`) | `cellGcmSys.c:2676` |
| `wcb_lock` | `wcb_lock` contention (spin + `sched_yield`/20-200 us `nanosleep`) against a faulting guest thread; used by every WCB/texture-dirty call | `rsx_wcb.c:190` (yield loop `rsx_wcb.c:60`) |
| `qsubmit` | `queue_mu` + `vkQueueSubmit`, every batch flush and one-shot | `rsx_vulkan_backend.c:818` |
| `oneshot` | `vk_submit_and_wait` fence wait: depth/colour readbacks to guest (`eng_zdepth_store`), pipeline/texture creates, snapshots | `rsx_vulkan_backend.c:961` |
| `ebs_swap` | pipelined batch: fence of the set about to be reused (GPU still on frame N-2) | `rsx_vulkan_backend.c:5471` |
| `ebs_other` | pipelined batch: fences of all other sets, after a one-shot | `rsx_vulkan_backend.c:5494` |
| `pres_done` | previous present's fence in `vk_present_complete_locked` (called from `vk_present_record_begin` on every flip) | `rsx_vulkan_backend.c:2930` |
| `pres_gpu` | `vk_present_wait_gpu`, when a texture/target is created while a present copy is in flight | `rsx_vulkan_backend.c:2863` |
| `pres_mu` | `present_mu` acquire (three sites: flip present, fixed-function present, present_finish) | `rsx_vulkan_backend.c:837` |
| `acquire` | `vkAcquireNextImageKHR` in `vk_present_window` (windowed only) | `rsx_vulkan_backend.c:2656` |
| `pipe_async` | `vk_pa_wait`: `pthread_cond_timedwait` for async shader translation, bounded by the frame budget | `rsx_vulkan_backend.c:4340` |
| `pipe_create` | `s_pipe_create_mu` + `vkCreateGraphicsPipelines` (synchronous compile on the walker) | `rsx_vulkan_backend.c:4944` |
| `tex_token` | `eng_tex_token_wait` spin/yield (only with `WALKER_MT` and the M4 resolver) | `rsx_draw_engine.c:7802` |
| `mt_turn` | `eng_mt_wait_turn` / `eng_mt_wait_upto` spin/yield (only with `WALKER_MT`) | `rsx_draw_engine.c:7457`, `:7479` |

Looked at and **not** timed, because they are not on the walker (or do not block it):

* GCM semaphore ACQUIRE (`cellGcmSys.c:3526`): a "blocked" acquire `break`s out of the pass; the walker returns to the
  idle wait, so it is `wait`, not `parse`.
* A FIFO park (self-JUMP, `cellGcmSys.c:3351`): same, the pass ends.
* `vk_present_thread_barrier`, present-thread condvars, queue present, `window_fence`: present thread
  (`rsx_vulkan_backend.c:2427-2500`); only the no-thread fallback (`:2748`) blocks the walker.
* `vk_mt_oneshot_submit` and chunk fences (`rsx_vulkan_backend.c:6031-6071`): record workers, `WALKER_MT` only.
* `s_texr` condvars (`rsx_draw_engine.c:7640-7660`): resolver thread (the key is a no-op, `eng_tex_resolver_on`).
* `vk_pipe_queue_push`, `vk_dump_queue`: non-blocking pushes.
* `rsx_draw_engine_wcb_service` (`cellGcmSys.c:2668`, `:2480`): CPU copy work, not a wait; it runs before the drain timer.
* The PPU-side ring recycle `Sleep(1)` loop (`cellGcmSys.c:4056`) runs on the guest thread.

## The switch

`PS3RECOMP_WALKER_WAITS=1` (env, default 0). One extra line is printed per `[walker-prof]` report (so it needs
`PS3RECOMP_WALKER_PROF=1` too):

```
[walker-waits] report=N flips=F in_drain_total=X.XXXms | fifo_lock=ms(n) wcb_lock=ms(n) qsubmit=ms(n) oneshot=ms(n) ...
```

Every value is ms per frame (divided by the flips in the report, like `[walker-prof]`) with the per-frame site count in
brackets. Unlike the phase sums these cover **every** drain, not the 1-in-8 sampled ones, so they are not scaled.
`in_drain_total` is the sum of all sites except `fifo_lock`. Sites are timed with `CLOCK_MONOTONIC`
(`rsx_wprof_now`) and only on the thread between `rsx_ww_enter` and `rsx_ww_leave` (the walker pass); other threads
that reach the same code pay one thread-local load. Off, each site costs that one TLS load and a branch.

Sites do not nest, so they add up: `oneshot` times only the fence wait (submit is `qsubmit`), `ebs_other` is outside
the `oneshot` timer, `pres_gpu` and `pres_done` are separate waits. `pipe_create` includes only the create, not
`pipe_async`. Read the total against `walker - engine` (`parse`) and against the engine phases.

## Offline check

Crowded capture through the real Vulkan backend (`replay_rsx_engine captures/walker_replay_fixed --frames 12 --warmup 4`,
`PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 PS3RECOMP_ENG_DEPTH_GPU=1`, one run):

| report | walker (ms/frame) | waits in drain | what |
|---|---:|---:|---|
| 0 (cold, 1 flip) | 1972 | 110.3 | oneshot 90.1 (815 readback/create fences), pipe_create 10.1 (63), qsubmit 10.1 |
| 1 (steady, 8 flips) | 42.6 | 0.93 | ebs_swap 0.64 (3.0/frame), qsubmit 0.30 |

So offline the walker barely blocks (about 2% of a steady frame), which matches the offline `parse` of 2.9 ms. The live
gap (parse 17 ms vs 3 ms, on-CPU 52%) therefore needs the live run below. With the key off no `[walker-waits]` line is
printed. `tools/test_video_fixtures.py`: 34/34 (twice; `test_gcm_tick_in_drain` is the known flake). The two
fixtures that link `cellGcmSys.c` without the engine (`test_gcm_flip_wrap`, `test_gcm_fifo_user_command`) got stubs.

## Live run (USER, one windowed run, crowded scene)

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_WALKER_PROF=1 PS3RECOMP_WALKER_WAITS=1 PS3RECOMP_ENG_DEPTH_GPU=1 \
  BUILD=build-depth ./play_windowed.sh
# zoomed-out replay of the crowded play, 30-60 s, then quit.
grep -a '\[walker-waits\]\|\[walker-prof\]' play_*.log | tail -200
```

Compare, per heavy report: `in_drain_total` against `parse` and against `walker` minus the on-CPU share. If the sites
explain the off-CPU time, the largest one is the lever: `oneshot` (depth readback, so the depth-GPU work), `ebs_swap` or
`pres_done` (GPU is the limit, not the CPU), `wcb_lock` (guest faults), `pipe_create`/`pipe_async` (shader compile).
If `in_drain_total` stays small, the missing time is not blocking: look at involuntary preemption (`perf sched`) or the
`kick_wait` wake lag instead.

Not run: the optional automated pitch-meter run (the question is the crowded scene, and the offline numbers say the
meter walker is not blocked either).
