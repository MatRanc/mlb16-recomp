# Panelist A: PS3 architecture view of the 60 fps problem

Sources are docs in /home/m/Documents/mlb16-online/mlb16-recomp/docs (cited by file). Guesses are marked GUESS. No code was read or run; no GPL source.

## 0. The machine model in one paragraph

On a PS3 the title runs three stages as a pipeline with one render frame in flight (cube-rt-and-frames-in-flight.md Part A: `FUN_000655cc` waits for the previous frame's SET_REFERENCE; the 3-flip gate is a separate display queue):
1. PPU builds frame N (game logic plus pushbuffer): 9.3 ms mean, 13.4 p50 (ppu-critical-path.md).
2. SPUs run the c690 Edge jobs (about 429 jobs/frame, 73.5 lane-ms/frame cumulative, 5 lanes max by the guest's maxContention) and write mesh slots into rings that the FIFO links in.
3. RSX, fixed-function hardware, consumes the FIFO for free as far as the CPUs are concerned.

In the port, stage 3 is a CPU thread, the walker. It parses the FIFO, resolves textures, fixes up vertices, translates shaders and records Vulkan. The walker is now the serial resource, and the PPU blocks on it for 19.7 ms/frame (ppu-critical-path.md). Frame time ≈ walker drain 17.3 + producer wait 11.8 ms = 29 ms at the meter. The PPU's own 9.3 ms is already hidden.

## 1. Diagnosis per scene

**Menus.** About 16.7 ms (60 fps) since the flip-tick fix (plan-meeting §1). 12% of frames are still 34 ms from ticks landing after the 17 ms bound, and the ~4.3 ms ref-publication tail stops a clean 60 (plan-meeting §3C). This is pacing, not work.

**Pitch meter (34.15 ms p50).** This scene is SPU-bound, not RSX-bound.
- frame-pacing-floor.md: a hard frame is a 32.8 ms c690 lane union inside a 34.28 ms frame, and the next fence lands 0.26 ms before the frame ends.
- The distribution is bimodal: 40% of frames are 34-36 ms, 22.7% are 8-16 ms, 0.5% are 20-32 ms. Frames either contain the c690 burst or they don't.
- Inside the burst a lane idles 5.5 of 29.2 ms. 84% of that is the guest's own PM head and tail (4.6 lane-ms), in lifted code (c690-lane-gaps.md).
- Lifted SPU code runs about 1.03 ns per SPU instruction against 0.31 ns on hardware, with 26-37% register-file spills (spu-codegen-audit.md).
- So the same 429 jobs that took roughly 10 ms of SPU time on a real PS3 (GUESS: 33 ms divided by about 3.3) take 33 ms here.
- Extra-work items the PS3 never did here: the 2 x 17 ms host deadline rounding is not the cause (it is where a late fence lands), and Edge-on-GPU hand-off cost more than it saved (+9.2 ms walker, overnight doc).

**Crowded play and replays (about 1,450-1,700 draws).** This scene is walker-bound. Walker was 149 ms, then 82 ms after depth-as-texture (depth-gpu.md).
- Added work the PS3 never did:
  - Per-draw Vulkan recording (draw 13.8 ms at 1,694 draws).
  - Texture binding and validation. `tex` was 23.8 ms against 5.0 ms on the null backend. Depth-GPU recovered about 18 ms of that.
  - Vertex fetch and remap (fetch 10.4 ms).
  - Pipeline lookup (5.4 ms).
  - Live-only `parse` of 16.9 ms against 2.8 ms offline (crowded-walker-profile.md).
- The RSX did all of this in its register and state machine at no CPU cost. Walker cost is about 37-47 us/draw live. 60 fps wants about 10 us/draw, a 4-5x cut (plan-meeting §3B).
- Walker-bound scenes also starve the SPU side. The ring slots are consumed at walker speed, so c690 is not the limit here.

## 2. Is 60 fps a speed problem or a design problem?

Speed only, on the evidence of fps30-investigation.md.
- The sim clock is the measured timebase delta. `Engine_FrameClock` produces dt = max(param_1, floor(...)) / 60, and the pitch meter is dt-scaled.
- 60 fps is the shipped mode (`g_engine_fps_setting_index` 6). The 30 fps self-cap triggers only if `system_info & 1`, so do not set it.
- The dt floor of 1/60 means that above about 63 fps the sim would run fast, so the useful ceiling is about 60.
- Frame interpolation is therefore neither needed nor useful. Game logic needs no patches.
- Caveat (GUESS): on real hardware some scenes (crowded fielding, cinematics) probably ran below 60 and relied on the dt floor at 2/60. They are "30 fps-designed" in the sense that the PS3 never hit 60 there. With a fast enough host they would simply run at 60.

One constraint matters. The flip-done path is tick-quantised at about 16-17 ms (live-ab-and-ceilings.md). A burst that finishes at 20 ms may still present at 33 ms. frame-pacing-floor.md reports no 20-32 ms population, but that is because nothing finished there. Once the producer gets near 17 ms, expect an alternating 17/34 cliff until the tick is made event-driven. The previous attempts failed (FLIP_ON_DONE and fence-done: 33.9 → 25 fps), because they let the display thread run ahead into the buffer-ref wait.

## 3. Recommendations (ranked)

**R1. Decouple the c690 burst from the frame: run it one frame ahead.** Meter, GUESS 10-15 ms.
- The evidence says the bursts are bursty: 40% of frames are hard at about 33 ms and 23% are fast at 14-18 ms. The mean c690 load (about 13-15 ms/frame) fits a 16.7 ms budget even though the hard frame does not.
- A real PS3 also had this load profile, but its SPUs finished the burst in about 10 ms.
- Mechanism: let SPURS jobs for frame N+1 start as soon as the PPU submits them, and keep the per-frame mesh-ring slot count at 2 so the walker consumes frame N while c690 fills frame N+1.
- This is the one-in-flight contract the guest already enforces, so it may need only a second ring generation. Check whether the guest waits on SPU completion before the fence (c690 admission is 2.7 ms in the walker wait, which suggests it is a data dependency, not a barrier).
- Effort 3-6 days. Risk: high (ring-reuse hazards, animation latency, stale-geometry look like the class-(b) backward frames in targets-and-backlog.md).
- Offline verification: reuse the 90 s binary timeline (frame-pacing-floor.md) to compute, per frame, when the c690 jobs for frame N were submitted relative to the frame N-1 fence. If submission waits on the fence, this lever is dead.

**R2. Shrink the SPU cost per job at its largest sources.** Meter, 5-8 ms.
- Register-file spills (26-37% of host instructions) and the PM head and tail (4.6 lane-ms) are the two measured buckets (spu-codegen-audit.md, c690-lane-gaps.md).
- Replacing the head and tail natively saves about 2-4 ms of burst wall. Codegen alone is estimated at 1.2-1.4x, about 5-8 ms. Together they are about 20-22 ms, not 16.7 ms alone.
- Effort 5-10 days. Risk: medium (exactness gates exist: 76/76 bit-exact style tests).
- Offline: the existing SPU replay and offline ns/call harness.
- Defer to panelist C for the codegen method.

**R3. Make the crowded draw path cheaper per draw by removing work, not by threading it.** Crowded, 20-40 ms.
- Merge or skip redundant state: consecutive draws with the same pipeline, textures and uniforms, and the 500-700 `dsnap` binds per frame. Avoid per-draw `rp_begin`/`rp_end` and descriptor writes (c690-offload-decision.md: 9.8 us/draw of mostly state).
- GUESS: the crowd is many near-identical sprite draws, so instancing or indirect draws would apply. Needs a capture check.
- Effort 5-8 days. Risk: medium.
- Offline: the crowded replay with real Vulkan, which already matches the live split for `tex` and `draw` (crowded-walker-profile.md §3), but is blind to `parse` and `other` (live-only).

**R4. Find the live-only `parse` (up to 14 ms) and `other` (5 ms).** Crowded, about 10-14 ms.
- This is not CPU contention (burner sweep). What a hardware RSX did that the port does at drain time: waiting for ring slots, park patching, FIFO resync.
- Effort 2-3 days to profile once, then a fix. Risk: low.
- Offline: not possible. It needs one live `perf` session on the walker thread, which the user must run.

**R5. Make the flip tick event-driven only after R1 or R2 move the producer below about 20 ms.** Menus and the meter, avoids a 17/34 cliff.
- Replace the 17 ms lwcond timeout with a wake on the walker's drain-complete event, and stop short of advancing the display thread into the ref wait (the failed variants in frame-pacing-floor.md).
- Effort 2 days. Risk: medium (regression of the 25 fps failure).
- Verify: per-flip frame-time histogram (p50, p99, stddev) on a short meter run.

## 4. What NOT to do

- Do not attempt full GPU or native Edge as a speed fix for the meter. Design A tops out at 37-40 fps and Design B is a 2-4 month project (c690-offload-decision.md). GPU 0x2A had a net +9.2 ms cost.
- Do not set `system_info` bit 0 (it makes the game cap at 30).
- Do not add frame interpolation or patch game logic for 60. The sim is already time-correct.
- Do not complete the flip earlier (FLIP_ON_DONE, fence-done, tick-in-drain): measured 33.9 → 25.5 fps.
- Do not add more SPU lanes (maxContention=5, more than 5 deadlocks) or `EXP_FAST_WAKE` (wedges).
- Do not judge renderer work offline alone (six offline-judged attempts all failed live) and do not resume walker multithreading (M3/M4) before R4 explains the live `parse`.
- Do not pace above 60 (dt floor makes the sim fast).

## 5. Open questions for the others

**B (runtime/renderer):**
- What fraction of the 82 ms is serialised between the FIFO parse and Vulkan recording? Could the walker be a two-stage pipeline (parse and fix-up in thread 1, Vulkan record in thread 2) with no change to ordering semantics?
- Is the live-only `parse` (16.9 ms) waiting on ring slots, park patches or a lock?
- Which draws are repeated state with identical bindings, and how many could be merged?

**C (codegen):**
- Can a register allocator at the lift level (keeping SPU regs in host YMM across a basic block) get the 26-37% spills to near zero? What does that buy per job?
- Are the PM head and tail (4.6 lane-ms) deterministic enough to replace natively, as with the Edge native 0x21 work?
- Is there any way to check offline whether the c690 jobs for frame N+1 can start before the frame N fence? (R1.)
