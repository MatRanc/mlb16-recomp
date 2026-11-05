# Walker diet: render-pass keep, pre-submitted vertex unpack, shadow constant

Date: 2026-11-05. Branch `exp/walker-diet` (off `exp/depth-gpu`) in worktree `ps3recomp-depth`, build
`recomp-work/build-depth` (incremental `ninja replay_rsx_engine` only). **Zero game launches**: the crowded capture
`captures/walker_replay_fixed` replayed through the real Vulkan backend (`PS3RECOMP_REPLAY_VK=1`, depth key on).
Follows [`depth-gpu.md`](depth-gpu.md) and [`crowded-walker-profile.md`](crowded-walker-profile.md). No RPCS3 or other
GPL emulator source was read or cited.

## TL;DR

* Four new live keys, all default 0, all in `PS3RECOMP_CTL` / env like `PS3RECOMP_ENG_DEPTH_GPU`:
  `PS3RECOMP_ENG_SHADOW_CONST`, `PS3RECOMP_VK_PASS_KEEP`, `PS3RECOMP_VK_VTX_PRE` (needs PASS_KEEP),
  `PS3RECOMP_VK_DRAW_STATE` (needs PASS_KEEP).
* Offline, 12 frames + 4 warm-up, 3 runs per arm, median of per-report values: **walker 43.3-46.2 ms (key off, two
  sets of 3) -> 38.3 ms** with PASS_KEEP + VTX_PRE + DRAW_STATE; `draw` 13.1-14.4 -> 8.9 ms. Tool median frame
  38.3-45.9 -> 34.4-35.5 ms. The final frame is **byte-identical** (md5 `e7b76dba...`, 2,764,816 bytes) for every
  arm and the all-on arm; off vs off-again equal.
* Shadow constant: correct and identical, but **no measurable gain** (walker 43.8 vs 43.3; `tex` 5.45 vs 5.29).
  After the depth-GPU fix the shadow bind is already one snapshot a frame, not 222 copies. Kept (default off) for the
  scenes where the empty map is re-snapshotted after each clear; do not expect fps from it.
* Existing keys `PS3RECOMP_REMAP_DIRECT=1 PS3RECOMP_REMAP_FAST=1` on top: `fetch` 10.9 -> 9.1 ms, walker 41.8 -> 37.6
  (3 runs), same image. Cheap to include in the live test (`rsx_vertex_remap_build` is the top single walker
  hotspot live, 12.6% of on-CPU).
* **Bound on CPU gains:** the live perf (`captures/walker_live.perf`) has the walker on-CPU only ~52% of the window;
  heavy-frame `parse` of 30-70 ms is mostly off-CPU waiting. These cuts shrink the on-CPU share (driver is 25% of it);
  they do not touch the wait.

## What changed (`libs/video/rsx_vulkan_backend.c`, `rsx_draw_engine.c`, `ps3_live_ctl.[ch]`)

| key | change |
|---|---|
| `ENG_SHADOW_CONST` (+ DEPTH_GPU) | the engine records a whole depth clear's value (`const_ok`, cleared by any depth write or non-whole clear); a snapshot of such a zeta binds a shared 1x1 `R32_SFLOAT` texture (`depth_constant` backend hook, one per clear value) instead of copying the 8 MB device depth |
| `VK_PASS_KEEP` | consecutive batched draws into the same render pass + framebuffer share one `vkCmdBeginRenderPass`; the ALL_COMMANDS barrier and the per-target image barriers are issued once at the pass start. Every other recorder into the batch's command buffer (`vk_cmd_after_everything`, `vk_barrier_image_on`, `vk_begin`, both flushes, `vk_submit_and_wait`) closes the open pass first, so copies/clears/uploads/readbacks never land inside it and still get their full barrier |
| `VK_VTX_PRE` | the GPU vertex-unpack dispatches (55% of draws) go to a second per-set command buffer submitted ahead of the batch's in the same `vkQueueSubmit`, ended by one compute->vertex-input memory barrier. They read host-written rings and write ring slices no earlier draw of the batch uses, so they never need to interleave with draws; before this they ended the pass on every such draw (reuse was 43% of draws; this raises it to all same-target runs) |
| `VK_DRAW_STATE` | inside an open pass, skip `vkCmdBindPipeline` / viewport / scissor / stencil ref / depth bounds when unchanged (cache dropped at every pass begin) |

Hazard argument for PASS_KEEP: a draw that reuses the pass has the same colour/depth targets as the previous one, so
rendering order within the pass covers blend and depth read-after-write; anything it samples was written before the
pass began (covered by the barrier at the pass start) or by a non-draw recorder (which closes the pass and barriers).
The one case not covered is a draw that samples its own bound target (a feedback loop); the replay (which would show it
in the image) does not exercise one. Not covered: the MT record path (`WALKER_MT`) is excluded (`vk_t_user` /
`vk_t_chunk` force keep off).

## Numbers (offline, 3 runs per arm, ms/frame, median of reports)

| arm | walker | draw | fetch | tex | tool median frame (3 runs) |
|---|---:|---:|---:|---:|---|
| key off (set A) | 43.28 | 13.07 | 10.70 | 5.29 | 38.3 / 42.6 / 39.1 |
| key off (set B) | 46.15 | 14.36 | 10.70 | 5.42 | 40.3 / 45.9 / 41.5 |
| SHADOW_CONST | 43.80 | 12.80 | 10.86 | 5.45 | 40.0 / 48.4 / 41.8 |
| PASS_KEEP only | 45.00 | 13.02 | 11.26 | 5.33 | 41.8 / 39.5 / 40.5 |
| PASS_KEEP + DRAW_STATE | 40.42 | 10.98 | 10.46 | 5.26 | 36.3 / 37.9 / 36.6 |
| + VTX_PRE | **38.29** | **8.87** | 10.28 | 5.60 | 35.5 / 35.0 / 34.4 |
| all four | 38.64 | 9.09 | 10.30 | 5.59 | 34.7 / 34.4 / 34.3 |

PASS_KEEP alone does nothing because the gpu-vertex draws broke the pass (TMPSTAT: 16,384 draws, 7,011 reused, 8,946
gpu-vertex); VTX_PRE is what lets it work. `VK_DRAWPROF` per draw: 7.4 us -> 6.8 us before VTX_PRE; the larger win
is the driver time the profile does not attribute to those buckets. Run-to-run noise on this box is ~3 ms (key-off
sets differ by 3 ms), so treat the gain as ~5 +- 3 ms.

Image: `PS3RECOMP_VK_DUMP`, 3 + 2 frames: off, off-again, SHADOW_CONST, PASS_KEEP, PASS_KEEP+DRAW_STATE,
+VTX_PRE, all four, and +REMAP_DIRECT/FAST: all md5 `e7b76dba8a66d87368fbebe4692a112c`.

Validation layers (`VK_LOADER_LAYERS_ENABLE='*validation*'`, installed on this host, 2 + 1 frames): key off and all
four on both report the same 10 errors, all `VUID-vkDestroyDevice-device-05137` (objects alive at teardown, pre-existing);
zero errors during recording, synchronization included.

Tests: `tools/test_video_fixtures.py` 34/34 twice; new `test_shadow_const` in `libs/video/tests/test_rsx_draw_engine.c`.

## Not done

* State dirty bits beyond the pass scope (pipeline/descriptor across passes): the cache is intentionally dropped at
  pass begin; the descriptor set is new per draw anyway.
* Full-game timing: none (offline only). `rsx_vertex_remap_build` rewrite: only the existing REMAP_DIRECT/FAST keys
  were measured.

## Live check (USER)

```sh
cd /home/m/Documents/mlb16-online/recomp-work
CTL=$PWD/walkerdiet.ctl
printf '%s\n' PS3RECOMP_ENG_DEPTH_GPU=1 PS3RECOMP_ENG_SHADOW_CONST=0 PS3RECOMP_VK_PASS_KEEP=0 \
  PS3RECOMP_VK_VTX_PRE=0 PS3RECOMP_VK_DRAW_STATE=0 > "$CTL"
BUILD=build-depth PS3RECOMP_CTL="$CTL" ./play_windowed.sh
# zoomed-out replay, ~60 s, then:
printf '%s\n' PS3RECOMP_ENG_DEPTH_GPU=1 PS3RECOMP_ENG_SHADOW_CONST=1 PS3RECOMP_VK_PASS_KEEP=1 \
  PS3RECOMP_VK_VTX_PRE=1 PS3RECOMP_VK_DRAW_STATE=1 > "$CTL"
# ~60 s later back to the all-0 line to confirm picture and fps return
```

(Use `PS3RECOMP_VK_PASS_KEEP=1 PS3RECOMP_VK_VTX_PRE=1 PS3RECOMP_VK_DRAW_STATE=1` first without SHADOW_CONST if you want
to bisect.) Watch `[walker-prof]` `draw` and walker, fps, and the picture at each flip: flicker, wrong shadows, or
missing/duplicated draws are regressions. The build must be rebuilt with the live game targets
(`build-depth` MyGameRecomp) after the user is done playing; this mission only built `replay_rsx_engine`.
