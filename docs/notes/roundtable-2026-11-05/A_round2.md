# Panelist A, round 2

Docs are in mlb16-recomp/docs. GUESS marks anything unmeasured.

## 1. Answers to questions addressed to me

**B: is N-1 a hard requirement?**
- It is a guest code choice, not a hardware rule. cube-rt-and-frames-in-flight.md Part A shows `FUN_000655cc` reading `DAT_01245058[DAT_011203d4 & 3]`, i.e. the previous frame's token. The token array is already 4 deep, and the display queue allows 3 flips.
- N-2 would be a one-site PPU patch (`DAT_011203d4` bookkeeping).
- It is only safe if whatever the gate protects survives a longer lifetime. ppu-critical-path.md says the wait is for "the buffer it is about to reuse", so the pushbuffer and mesh-ring slots are the hazard (GUESS: double their count).
- The green-box bug (rsx-sync.md) shows what happens when the guest reuses memory early. This is a late lever, after the cheap ones.

**B: can the 1,450 draws be merged or skipped?**
- I cannot confirm from docs. `crowd-fix-findings.md` does not exist in the docs folder, so B's per-batch-offset claim is unchecked here.
- The 222 shadow-map binds are better than a skip. depth-gpu.md says the zeta is cleared to 1.0 and never drawn, so the sampled content is a constant. Binding a tiny constant-1.0 depth texture should be pixel-identical (GUESS: it depends on the sampler compare mode). This also removes the 2048x1024 snapshot cost. Cheap, and byte-testable with the existing PPM md5 gate.

**B: is `parse` a producer-side wait?**
- Probably partly. ppu-critical-path.md §3 already puts 6.2 ms/frame of the walker's wait on PPU append/park (`u_park` 4.14 + `u_caught` 2.02) at the meter.
- Crossing point: the PPU writes put in `FUN_00065760` after the ref wait. The walker may read only up to put, plus the jump-park patches.

**C: WWS protocol and the 5-lane cap.**
- c690-native-dispatch.md line 43 and c690-extra-lanes.md line 46 already say the PPU's SpinUntilDone observes the cursors at +0x42..+0x4c and +0x4e.
- boot-spu-hitches.md:181 warns that `maxContention` is the guest's workload parameter, not an SPU hardware limit. The DXT workload (maxC=4) was proposed for an HLE override, not shipped.
- I found no dependency doc for job lists. Treat C's question (c) as open (GUESS: lists are mostly independent, with barrier commands).

## 2. Disagreements

- **My own R1 is wrong for crowded scenes.** C's data (c690-serialisation.md) says the Edge batch is nearly constant per frame, with a 102-111 lane-ms sum. Running it a frame ahead cannot average it down. It stays valid only for the meter's bimodal bursts. I demote it, since it carries ring-reuse risk and is a latency trade.
- **B's "meter is not a walker problem" is right.** C's "free renderer gives 31-33 fps crowded" and B's "renderer alone gives 30-35 ms" are the same statement. They agree on a ~30 fps plateau, and neither alone reaches 60.
- **C's 41 fps NO_DRAW figure is below the 29-33 ms burst.** Those are different eras of c690 code (live-ab-and-ceilings.md predates the PM fixes). I would not use it as a ceiling.
- **C's R3 (skip culled jobs) and R4 as ranked.** cull-bypass-ab.md shows Edge-to-walker cost shifting loses. I would drop R3 unless the offline count shows over 10% fully culled jobs.
- **Host budget is the unmodelled constraint.** The 2600X has 6C/12T. The burner sweep (walker 63 -> 99 ms) shows the walker is memory/core sensitive. A 7-worker Edge runner plus walker, record thread, PPU main and present is not obviously compatible. Crowded 60 fps (walker under 14 ms and Edge under 14 ms) is a 6x and a 2x cut at once. My honest guess is that it is out of reach on this CPU without a GPU-driven draw path. 40-45 fps is realistic, and I would tell the user so.

## 3. Revised top 5 (whole project)

1. **One live `perf` plus drain timers on the walker (all crowded scenes).** 1 day, risk none. It splits `parse` 16.9 ms into starvation or real work, and the 40 ms live-vs-offline gap. Everything else is sequenced by this result. Unblocks: crowded.
2. **Native WWS PM head/tail (meter and crowded).** 3-5 days, 2.5-4 ms of burst. Kill criterion: lane-gap sum must fall by more than 50% offline before starting a native job runner (7 workers, 17-20 ms burst, 5-8 more days, high risk). Unblocks: meter, and the crowded Edge floor.
3. **Walker diet: per-draw barrier and render-pass removal, dirty-bit state, constant shadow texture (crowded).** B estimates 4-8 plus 2-4 ms. About 6-10 days, medium risk. Gate on PPM md5 and a GPU timestamp query. Pitch meter must not regress.
4. **Lifter register allocation for the SPU register file (meter and crowded).** 5-8 days, 4-6 ms burst. It partly overlaps the native runner in item 2.
5. **Deeper ring or N-2 gate to remove the PPU/walker 6.2 ms mutual wait, followed by an event-driven flip tick (all scenes).** Do it only after items 2-4 push the producer under about 20 ms, or the 17/34 ms tick cliff (live-ab-and-ceilings.md) hides the gain. High risk: the earlier fence-done variants dropped 33.9 -> 25 fps.

**Realistic order and ceiling.**
- Menus are already 60.
- Meter: items 2 then 4 (then the native runner) could plausibly reach ~17-20 ms, and 60 only with the runner plus item 5.
- Crowded: item 1 first, then 3, then a record thread; ~35-45 fps is the likely honest end state.
- Do not start the record thread or native runner until their prerequisites (items 1 and 2) report.
