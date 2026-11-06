# Panelist C, round 2

Docs under mlb16-recomp/docs. "Guess" marks anything unmeasured.

## 1. Answers to questions addressed to me

**A: can a lift-level allocator keep SPU registers in host YMM and get spills to near zero?**
- No. The SPU has 128 live-able 128-bit registers and Zen+ has 16 YMM and no AVX-512.
- `--local-regs` is already on. The remaining 26-37 % is intra-function pressure (100+ live vectors), not a storage-model problem (spu-inline-callees.md TL;DR; spu-codegen-audit.md §4).
- A smarter allocator buys 15-25 %. The audit says 1.2-1.4x, and I hold to the low end.

**A: are the PM head and tail deterministic enough to replace natively?**
- Partly. Head is the ticket claim (GETLLAR/PUTLLC), the descriptor DMA and a synchronous code re-DMA `memcpy`. Tail is output PUTs and bookkeeping (c690-lane-gaps.md §2; pin-and-burst-path.md TL;DR: no mailbox or tag waits, PUTLLC retry only 0.3 % of gap).
- Contention on the list line is the non-deterministic part. I would guess 50-70 % of the 4.6 lane-ms is replaceable. Gate it offline on the cursor/counter bytes and output rings.

**A: can frame N+1's c690 jobs start before frame N's fence?**
- No, not without a guest patch. The burst starts 0.25 ms after frame start (pin-and-burst-path.md TL;DR, "earlier kick 0 ms"). Jobs are submitted by the PPU during frame N's logic, and the PPU only starts frame N after the N-1 token (cube-rt-and-frames-in-flight.md Part A: "the host cannot relax the gate without corrupting the ring").
- Also, skinning inputs depend on frame N's game logic. Running ahead means a deeper ring plus one frame of animation latency.

**B: how many cores does the SPU/PPU fleet need? Can the walker get dedicated cores?**
- Five lanes at 70 % occupancy during a burst (pin-and-burst-path.md) is ~3.5 cores. Add the walker, main, present and DisplayFlip, and the 6C/12T box is full.
- Pinning already lost: 35.3 vs 31.0 fps at PIN=1. Do not dedicate cores.
- The burner sweep (walker 63 -> 99 ms with 5 burners) says the walker is SMT/cache-sensitive. Cutting c690 CPU also relieves the walker. That is an unquantified side benefit of my Edge items.

**B: cheaper FIFO writes from generated PPU code, or PPU-side wake latency inside `parse` (16.9 ms live)?**
- The FIFO is written by the game's own statically linked libgcm, so we cannot make it cheaper without patching.
- Part of the live-only `parse` is plausibly the PPU append/park wait (u_park 4.14 plus u_caught 2.02 ms/frame, ppu-critical-path.md §3). That is a guess. B's item 1 will settle it.

## 2. Where I disagree

- **A's R1, "mean c690 load fits 16.7 ms".** Per frame it does not. 621 of 895 frames carry a burst, and a burst is 146 lane-ms across 5 lanes (c690-lane-gaps.md §2). Averaged, that is ~100 lane-ms per 16.7 ms, about 6 lanes of work against 5 available (the maxContention ceiling). Pipelining bursts across frames is necessary but not sufficient. We need >=1.3x throughput regardless. It also needs a guest ring change.
- **A's "10 ms on a real PS3".** A guess. The audit cautions that frame comparisons are not controlled (spu-codegen-audit.md §2).
- **A's "never add lanes".** Correct for hidden helpers on the lifted PM. A native runner is different (c690-extra-lanes.md explains why the helpers failed). Keep it as a conditional stage.
- **B's crowded walker target of 30-55 ms.** Fine, but the Edge floor in crowded frames is 24-31 ms (c690-serialisation.md TL;DR). Walker work below ~30 ms is invisible until Edge improves. B's #4 (record thread) pays only after Edge does.
- **My own correction.** Burst-wall is the pitch floor, and the free-renderer ceiling of ~31-41 fps stands. B's data agrees: the meter is not walker-bound (B §4). Together, B's walker gains do not reach 60 at the meter and my Edge gains do not reach 60 in crowded scenes.

## 3. Merged top 5 (whole project)

| # | Item | ms | Effort | Unblocks |
|---|---|---|---|---|
| 1 | B1: live `perf` on the walker plus a drain-scoped timer for parse and put-wait | up to 14 ms | 1 day | Crowded. It also decides whether the walker is starved by the PPU or by Edge, so it gates everything below. |
| 2 | C R1 stage A: native PM head and tail, gated by the offline cursor/ring byte diff | 2.5-4 ms burst | 3-5 days | Meter, cinematics, and crowded once the walker is under 30 ms. |
| 3 | B2+B3: barrier and render-pass diet, then dirty-bit state | 4-8 ms CPU, plus 2-4 ms | 6-9 days | Crowded. |
| 4 | C R2 + R5: SPU register-pressure work (0xC0F4 back edge, split 0xFDA0) and PGO over the job modules | 3-6 ms | 7-10 days | Meter and crowded (shared Edge floor). |
| 5 | Ring depth/ownership handoff (A R1 + C R4), done last | 4-8 ms | 5-8 days, high risk | Meter. It converts the serial producer/walker chain into overlap. Only worth it once items 2 and 4 hold the burst at ~20 ms. |

Held back: C R1 stage B (native runner, >5 workers), B4 (single record thread), and A R5 (event-driven tick). The tick work matters only once the producer is under ~20 ms.

## 4. Realistic order to 60 fps everywhere

- **Settled:** the pitch meter, cinematics and crowded scenes each need both tracks. Walker gains are invisible at the meter, and Edge gains are invisible in crowded scenes until the walker is below ~30 ms.
- **Week 1:** item 1, then items 2 and 3 in parallel. Each is gated offline before a live A/B.
- **Weeks 2-3:** item 4 and the A/B of stage A.
- **Then decide:** item 5 and R1 stage B, based on whether the burst measures under ~20 ms.

Honest expectation, a guess: ~45-50 fps at the meter and ~25-30 fps in crowded scenes at the end of week 3. A clean 60 everywhere is not credible in one week without items 5 and the native runner. The path to it is those two plus a walker below 14 ms.
