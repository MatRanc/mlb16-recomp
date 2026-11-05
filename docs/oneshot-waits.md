# One-shot waits: who blocks the walker, and the create/snapshot cut

Date: 2026-11-05. Branch `exp/oneshot` (off `exp/walker-waits`) in `ps3recomp-depth`, build `recomp-work/build-depth`
(incremental only). **Zero game launches.** Follows [`walker-waits.md`](walker-waits.md). No RPCS3 or other GPL
emulator source was read or cited.

## Live finding

User run, crowded zoomed-out replay, 44 heavy reports: almost all in-drain blocking is `oneshot` (the fence wait in
`vk_submit_and_wait`), 15-40 ms per frame, 16-165 one-shots per frame. The offline replay only shows it in the cold
first report, so the per-frame ones are live-only.

## Attribution (`[oneshot-by]`)

With `PS3RECOMP_WALKER_WAITS=1` (and `WALKER_PROF=1`) each report now prints, next to `[walker-waits]`:

```
[oneshot-by] report=N <caller>=ms_per_frame(count_per_frame) ...
```

The tag is the calling function (`vk_submit_and_wait` is now a macro passing `__func__`, so no call-site edits; off it
costs one pointer argument). `vk_eng_flush` is the batch flush and is split by its why code (same codes as
`VK_EB_STATS`): `flush:other|ring|unbatched|max|engine|destroy|present|begin`. Note a one-shot's own `vk_begin()`
flushes the open batch with a wait first, so `flush:begin` is the cost of someone else's one-shot, and every
one-shot also waits for all other pipelined frame sets (`vk_ebs_wait_other`): each one drains frames in flight.

Offline cold report (815 one-shots, 87.8 ms): `flush:begin` 77.7 (396), `vk_eng_texture_create` 12.5 (360),
`vk_eng_color_target_create` 1.2 (31), `vk_eng_depth_target_create` 1.1 (25), clear_color, depth_snapshot ~0.

## Ranking by code (callers that can fire every frame live but not in the replay)

1. `vk_eng_texture_create` (360 cold): UNDEFINED -> GENERAL barrier submitted alone. Live, a crowd streams new
   textures (cache misses, size changes), each paying a full submit + fence + wait on all frame sets. The barrier has no
   CPU read, so the wait was never needed.
2. `flush:begin` caused by 1, colour/depth target creates, colour snapshot, depth constant (one-shot begin flushes the
   batch synchronously). Gone with 1 and 3.
3. `vk_eng_color_snapshot` (always a one-shot; copy for a colour target sampled as a texture), `vk_eng_depth_target_create`,
   `vk_eng_color_target_create`, `vk_eng_depth_constant`, the depth-snapshot first-use barrier: same shape, no CPU read.
4. Genuinely synchronous (CPU needs the bytes now), kept: `vk_eng_readback` (guest reads / WCB when
   `PS3RECOMP_RSX_WCB_STREAM` is off), `vk_eng_depth_readback` (`eng_zdepth_store`, depth guest fallback), `vk_eng_present`
   fixed-function path, `vk_cube_rt_copy` (kept conservative), `flush:engine` (guest sync points). These need a live
   run to see if they matter; the guest-fault ones do not exist in the replay (no CPU thread), which is the likely
   remaining live-only cost. `vk_eng_depth_readback` has no stream/ticket form unlike colour.

## The fix: `PS3RECOMP_VK_BATCH_CREATES`

Default 0, live via `PS3RECOMP_CTL` (`PS3_LIVE_VK_BATCH_CREATES`). Only with draw batching on and not on a record
worker. Items 1 and 3 record their barrier/clear/copy into the open draw batch (`vk_begin_j`/`vk_end_j`, the way
`vk_eng_clear_color` already did) instead of submit + wait. Correctness: the images are new or queue-ordered, every later
use is in the same batch or a later submit on the one queue, destroy already flushes and waits (`vk_destroy_image`), and
nothing reads the results on the CPU. Colour snapshot adds a full memory barrier first (the source may be drawn earlier
in the batch).

Offline (`replay_rsx_engine walker_replay_fixed --frames 12 --warmup 4`, VK + BATCH + PIPELINE + DEPTH_GPU, one run per
arm): cold report one-shots 815 -> 5 and 87.8 -> 0.84 ms oneshot; final image md5 `e7b76dba8a66d87368fbebe4692a112c`
for both arms (equal to the earlier reference). Validation layers, 2 + 1 frames, key on: only the pre-existing
`VUID-vkDestroyDevice-device-05137`. `tools/test_video_fixtures.py` 34/34 twice. There is no GPU-less unit test for the
Vulkan backend; the replay md5 equality is the test.

## Not done

Readbacks (4) are untouched; the live `[oneshot-by]` decides whether they are next (a depth stream ticket like colour's).
Steady-state replay shows 0 one-shots with or without the key, so the live win is unmeasured.

## Live result and correction (user run 17:11, key flipped 17:13:44)

The create ranking was wrong. Heavy reports, per frame, key off: oneshot 22.4 ms (13.8 one-shots): `flush:engine` 16.1,
`flush:begin` 5.5, `vk_eng_readback` 0.6, `vk_cube_rt_copy` 0.13, all creates 0.0. Key on: unchanged. The create key
stays (harmless, default 0) but is not the lever.

## flush:engine by call site (branch `exp/oneshot2`)

`flush:engine` is now split by name (a pointer in `g_rsx_ww_ctx` set at each site, consumed by the flush):
`eng:guest_read` (NV3089 source read, `rsx_draw_engine.c` ~2540), `eng:zdepth_store` (~3809, depth to guest),
`eng:wcb_copy_surface` (~4044, WCB demand copy, non-stream), `eng:pd_write_surface` / `eng:pd_write_depth` (dump
tools), `eng:flush`, `eng:shutdown`, plus the backend's own `be:readback_fetch` (stream/prefetch ticket collect) and
`be:submit_batch` (no frame pipeline). Replay md5 unchanged, fixtures 34/34 (one run had a flake, rerun clean).

## What forces each wait

* `eng:guest_read`: the walker itself consumes the bytes next (the NV3089 transfer is a CPU copy from guest memory), so
  it cannot be deferred without moving the transfer to the GPU.
* `eng:wcb_copy_surface`, `be:readback_fetch`: a guest CPU thread has faulted on a protected render-target page and is
  blocked until the bytes exist. The game's own fences (`SET_REFERENCE`, `Gfx_WaitPrevFrameRef`, see
  [rsx-sync.md](rsx-sync.md)) do not apply: the read is not "after a fence", it is a demand read now. The time is not
  the copy (4 KiB) but the accumulated draw batch completing on the GPU (wcb-async.md: p50 10 copies/frame in the
  crowd, 13.5 ms/frame). Fits 13.8 one-shots/frame at 16 ms.
* `eng:zdepth_store`: same, for a depth target the guest or a texture bind needs in guest memory; absent from the live
  `[oneshot-by]` (no `vk_eng_depth_readback` entry), so not a factor.
* `flush:begin`: the `vk_begin()` of the sync `vk_eng_readback` flushing the batch it is about to follow.

## Result: no new key

The safe deferred form already exists and was measured at the meter: `PS3RECOMP_RSX_WCB_PREFETCH` (queue the copy at
the sync point, submit without waiting, collect the in-flight ticket at the fault; -86% readback engine cost, wcb-async.md)
with `PS3RECOMP_RSX_WCB_STREAM` (one wait instead of two). Both are live keys, off by default, and have not been tried on the
crowded zoomed-out scene. Writing another form would duplicate them. Prefetch changes the read to "the target as of the
sync point it just passed" (same contract as WCB=3); that is the correctness question the next run tests visually.
