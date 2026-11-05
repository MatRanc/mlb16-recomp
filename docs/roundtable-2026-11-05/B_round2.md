# Panelist B, round 2

## 1. Answers to questions addressed to me

**A: how much of the 82 ms is serial between parse and recording; can it be a 2-stage pipeline?** Offline VK, 62 ms (crowded-walker-profile.md, CWP): parse 2.8, and the draw sink (`sink_end_work`) 28.7 ms CPU, of which vertex fill + driver recording inside `vk_eng_draw_from` is 4.3 + 8.2 = 12.5. So recording is ~20% of the walker, parse is small, and the engine (fetch, tex, pipe, other) is the bulk. A parse/engine -> record 2-stage split moves only ~12 ms off the thread, hence my rank-4 estimate. Ordering is safe if guest reads stay on the first stage. Live the 82 ms has an unexplained ~16 ms `parse` that is not serial work, I suspect (GUESS) a wait.

**A: is live `parse` a wait on ring slots/park/lock?** Probably the PPU handoff, not a lock. ppu-critical-path.md lines 15, 28: the walker waits 11.8 ms/frame for the producer, 6.16 of it the PPU's own append/park (`u_park` 4.14, `u_caught` 2.0) and 2.8 for "PUT visible, walker not resumed" (resume p50 507 us, p90 2.24 ms). That fits the 14-17 ms live-only gap, but those are meter numbers; no crowded-scene trace exists. Still needs the one live perf/timer (my #1).

**A: which draws repeat identical state?** Unknown, no doc has the histogram. It is a cheap offline script over `captures/walker_replay_fixed` (count runs of consecutive draws with equal pipeline key, texture set, constants). I agree merging is plausible for crowd quads (crowd-fix-findings.md), but instancing is a GUESS until the histogram exists.

**C (d): can 7 native workers coexist with the walker on 12 threads?** `nproc` says 12 (6C/12T). The walker is single-threaded and on the critical path in crowded scenes; 7 workers + walker + PPU main + present = 10 busy threads, tight but not impossible, with the memory-bandwidth caveat (CWP burner sweep: walker 63 -> 99 ms with 5 burners). Pinning hurt before (PIN=1), so don't pin.

**C (e): does crowded 60 fps need walker < 14 ms and Edge < 14 ms?** Yes, if frames are not overlapped. With the walker and Edge in different threads they overlap, so the requirement is throughput of each stage below 16.7 ms, not their sum. Today the PPU waits on the walker's ref gate (19.7 ms), which is why one-frame-in-flight matters.

**C (f): 6.2 ms PPU append/park without reordering.** I can't gate this offline; the `(id+1)&3` reorder corrupted geometry (ppu-critical-path.md s5). A deeper ring (add buffers) is the safe form. Offline gate: replay hash plus geometry diff, then the live meter. I rank it as a project item, not a renderer one.

## 2. Disagreements

- **C's "free renderer" caps (crowded ~31-33, pitch 35-41 fps) vs my 30-55 ms walker.** These are consistent, not in conflict: my 30-35 ms walker is itself ~30 fps. I agree no renderer-only path reaches 60. I also note C's crowded floor (+29-32 ms Edge) comes from c690-serialisation.md, measured when the walker stretched frames, so it may overstate the Edge floor once the walker is fast and the ring drains faster (GUESS). A's R1 (c690 one frame ahead) risks the same hazards as C's R4.
- **A's R3 (20-40 ms from removing per-draw work)** is too optimistic: the null-backend engine floor is 34 ms before any Vulkan, so draw-state work alone is ~14 ms (draw phase). 20-40 would need removing fetch/tex/pipe too.
- **C R3 (skip Edge jobs)** is fine, but cull-bypass-ab already shows moving cost across threads; I would drop it behind the others.
- **C's R2 vs R1B overlap** (C admits it). I would do R1A first and use its kill criterion.

## 3. Revised project top 5 (merged)

1. **Live walker/producer profile (perf + put_wait timers) in a crowded and a meter scene.** 1 day. Unblocks: crowded, and decides whether #4 is worth it. Zero risk.
2. **Native WWS PM head/tail (C R1A), kill criterion, then native job runner if it holds.** 3-5 d, then 5-8 d. Unblocks: pitch meter, cinematics, and the Edge floor in crowded.
3. **Renderer diet: batch vertex-unpack ahead, keep render passes open, targeted barriers, state dirty bits (my #2+#3).** 6-9 d, 6-12 ms CPU. Unblocks: crowded (82 -> ~55-60 ms) and cuts PPU stall in all walker-bound scenes.
4. **PPU append/park handoff: deeper ring, no reorder (C R4).** Hides 6.2 ms producer wait, up to ~8 ms (ppu-critical-path.md lever 2). High risk. Unblocks: meter and crowded.
5. **Single record thread behind #3, then event-driven flip tick (A R5) once the producer is under ~20 ms.** 7-10 d + 2 d. Unblocks: crowded (-> 30 ms), menus/meter 60 vs 17/34 cliff.

Dropped: SPU lifter regalloc (C R2) is parked behind #2 (partly redundant); A's R1 (c690 one frame ahead) hold until the offline timeline test shows submission does not wait on the fence.

## 4. Realistic order to 60 fps

Menus already 60. Order: #1 (cheap, informs everything) -> #2A and #3 in parallel (different threads, different files) -> measure -> #4 -> #5. Honest estimate (GUESS): after #1-#3 the meter is ~28-30 ms and crowded ~55 ms; after #4-#5 ~22-25 ms and ~30 ms. 60 fps in crowded scenes needs all of walker, Edge and handoff work, with 1-2 of them probably falling short. Report the shortfall to the user rather than promising 60 everywhere.
