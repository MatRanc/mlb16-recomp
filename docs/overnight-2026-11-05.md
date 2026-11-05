# Overnight 2026-11-05: results and the decision they force

Missions run while the user slept (Claude orchestrating, DeepSeek missions). Every number below is from a live
pitch-meter A/B or an offline gate named in the linked doc.

## Shipped into the play build (default on, reviewed)

| Fix | Effect | Doc |
|---|---|---|
| Jersey torsos (`PS3RECOMP_FP_TXD`) | user-confirmed; ~-1.5 fps (real work that was skipped) | jersey-transparent.md |
| Better depth of field (`PS3RECOMP_DOF_PATCH`) | user-confirmed better blur | dof-patch.md |
| Bink video: shared fd space + Bink SPU task descriptor | intro now decodes (live boot check); picture/logo = user check | bink-video.md |
| Async frame dump (`PS3RECOMP_VK_DUMP_ASYNC`) | 0.5-0.7 s freezes from the thumbnail dump gone (2 -> 0 per 133 s) | present-stalls.md |
| Walker profiler + full crowded captures | profiler engine=0 lines 19 % -> 0; crowded replay 1,261 -> 1,694 draws, 0 misses | capture-tools-fix.md |

## Edge speed: three routes measured, none moves the meter p50 (34.15 ms)

| Route | Result | Doc |
|---|---|---|
| GPU 0x2A (exact kernels, 68 % class coverage offline) | overlay costs +9.2 ms walker/frame (export 1.4, resolver 1.9, GPU hand-off 5.9); max saving 1-3.4 ms | edge-gpu-binding.md, edge-gpu-2a-classes.md |
| Native 0x2A field pack (exact) | live-neutral (-1.5 % per call) | edge-native-2a.md |
| Native whole 0x21 (bit-exact 76/76) | offline parity 31.42 vs 31.44 us/call; live arm wedged on a since-fixed bug | edge-native-21.md |
| Extra host lanes (0/2/4 helpers) | p50 34.15 in every arm; the guest's maxContention=5 + intra-lane gaps limit it; >5 deadlocks | c690-extra-lanes.md |

Reading: our lifted SPU code is already as fast as hand-written native C for the same work (the compiler emits the
same AVX2), the GPU hand-off costs more than the work it moves, and the guest will not use more than 5 lanes. The
remaining Edge levers are (a) SPU codegen quality for ALL SPU code (spu-codegen-audit.md: ~1.03 ns per SPU
instruction vs 0.31 ns on hardware, 26-37 % spills, estimated 1.2-1.4x), (b) the ~20 % intra-lane idle time inside
the burst (what the lanes wait on), (c) the frame's 2 x vblank flip deadline that pins frames at 34.15 ms once the
burst passes ~16.7 ms. Decision for the user: which of these to pursue, or whether the renderer (crowded scenes,
47.5 us/draw) is now the better use of the week.

## Not ready / pending the user
- Async pipeline compile (`PS3RECOMP_PIPE_ASYNC`): first live run skipped every draw; redesign under verification.
- Uniform moire: not fixed (mipmaps/sampler/LOD/2x1 derivative ruled out); next: anisotropy + texture dump.
- User checks: intro video + team-select logo (Bink), lighting drift (note times), crowded-replay capture.
