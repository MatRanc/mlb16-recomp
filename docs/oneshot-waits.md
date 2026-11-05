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
