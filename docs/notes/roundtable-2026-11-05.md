# Roundtable 2026-11-05: path to 60 fps everywhere

Three reviewers each read the docs and code, wrote a position paper, then answered the other two:
- **A**, PS3 hardware/architecture.
- **B**, runtime and renderer performance.
- **C**, static recompilation and codegen.

Nothing was built or run. All ms and fps figures below are estimates from existing measurements, not new data. The full papers are in [roundtable-2026-11-05/](roundtable-2026-11-05):
- round 1: `A_architecture.md`, `B_renderer.md`, `C_recomp.md`;
- round 2: `*_round2.md`.

## Diagnosis (agreed)

Two separate floors stand between today's speeds and 60 fps, and both must drop below ~14 ms per frame.

**1. Walker (RSX emulation).** This is the bottleneck in crowded scenes: ~1,450 draws per frame, 82 ms per frame after the depth-as-texture fix ([depth-gpu.md](depth-gpu.md)).
- The live walker runs about twice as slow as the offline replay: 82 ms live against 42 ms offline.
- Parse is 17 ms live against 3 ms offline, and the live gap has never been profiled.
- `vk_eng_draw_from` issues all of these for every single draw:
  - an ALL_COMMANDS barrier;
  - a per-target image barrier;
  - a compute vertex unpack, with its own barrier;
  - its own render pass.

**2. Edge/c690 SPU burst.** This is the bottleneck at the pitch meter: ~29-35 ms of work across 5 lanes. It also sets a 24-31 ms floor in crowded scenes once the walker gets faster.
- Lifted SPU code runs ~3.3x slower per instruction than a real SPU.
- 26-37% of the emitted instructions are spills.
- The WWS job manager's own head and tail per job costs ~23 lane-ms, which is 84% of lane idle time.

**60 fps itself is a speed problem only.** The game's frame clock is time-correct, so no game-logic patch or frame interpolation is needed. Never set `system_info` bit 0: that makes the game cap itself at 30 fps.

## Agreed plan (in order)

| # | Work | Scenes | Est. gain | Risk |
|---|---|---|---|---|
| 1 | Profile the live walker and the producer wait in a crowded replay (perf plus a drain-scoped timer) | gates everything below | up to ~14 ms, no code | low, ~1 day |
| 2 | Native WWS job-manager head/tail. Do not start a native job runner unless lane gaps fall by more than 50% | meter, crowded floor | 2.5-4 ms | medium |
| 3 | Walker diet: per-draw barriers and render-pass churn, state dirty bits, and a constant-1.0 texture for the empty shadow map. The shadow change must give a byte-identical image | crowded | 6-12 ms | medium |
| 4 | SPU lifter register pressure, plus PGO/LTO over the lifted job modules | meter, crowded | 3-6 ms | medium |
| 5 | Only after 1-4 report back: a deeper PPU/walker ring or an N-2 gate (needs doubled pushbuffer and mesh rings), a single Vulkan record thread, a native job runner on more than 5 workers, and an event-driven flip tick once the producer is under ~20 ms | all | large, uncertain | high |

## Do not

- Resume the N-worker walker multithreading as it stands. The serial chain is the texture hand-off and the global command buffer.
- Cache recorded command streams across frames.
- Publish `ref` or complete the flip early. This measured 33.9 -> 25 fps.
- Pin threads to cores. This measured 35.3 -> 31.0 fps.
- Add SPU lanes.
- Use Edge on the GPU as a speed fix.
- Run c690 one frame ahead in crowded scenes: the burst is near constant per frame there.
- Let any new async stage read guest memory off the ordered walker thread. That widens the green-box class of bugs ([rsx-sync.md](rsx-sync.md)).

## Expectations (guesses)

| Scene | After items 1-4 | 60 fps needs |
|---|---|---|
| Menus | already 60 | — |
| Pitch meter | ~40-50 fps | item 5 |
| Crowded | ~25-45 fps | item 5 to succeed in full; unlikely on the 6-core Ryzen 5 2600X |

The panel advises keeping the 60 fps target but expecting crowded scenes to land below it unless the high-risk items all pay off.
