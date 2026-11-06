# Panelist C: CPU side (PPU/SPU lift, job manager, codegen)

Docs are under mlb16-recomp/docs. "Guess" marks anything not measured.

## 1. Diagnosis with numbers

**The CPU side is not 60 fps even if the renderer were free.** The Edge/c690 SPU burst is the CPU-side floor.

- Pitch meter: the burst is ~29 ms mean (32.8 ms union in hard frames), against a 16.7 ms budget (c690-lane-gaps.md TL;DR and §2; frame-pacing-floor.md TL;DR). Across 500 big bursts the mean was 35.0 ms, with 3.47 of 5 lanes busy and lanes finishing within 0.17 ms of each other (pin-and-burst-path.md TL;DR).
- That makes it throughput-bound, with no serial tail. The 34.15 ms p50 is the guest's 2x16.7 ms flip deadline sitting over this burst (frame-pacing-floor.md).
- Crowded scenes: the Edge batch is nearly constant per frame. Lane-sum is 102-111 ms, 575-796 jobs, and the last job ends +29-32 ms after frame start in every frame band (c690-serialisation.md TL;DR). The walker is what stretches those frames to 80+ ms. If the renderer were free, crowded scenes would sit at ~30-32 ms, about 31-33 fps.
- Upper bound for "free renderer" at the pitch: the EXP_NO_DRAW run measured 41.2 fps (24 ms) against 30.3 baseline, with walker own CPU at 2.2 ms (live-ab-and-ceilings.md TL;DR). That run predates later c690 improvements. It sits oddly below the 29-33 ms burst, probably because the contention relief from NO_DRAW shortens the burst (guess).
- So today, with a free renderer: pitch ~35-41 fps, crowded ~31-33 fps, menus already 60. The renderer track and the Edge track are both required. Neither alone reaches 60.

**Where the burst's lane-ms go (pitch, c690-lane-gaps.md §2):** the burst has 5 lanes x 29.2 ms = 146 lane-ms.

| Component | Lane-ms | Source |
|---|---:|---|
| c690 job compute | 118 (23.6 ms per lane) | lane-gaps §2 |
| Job head, lifted WWS PM | 10.4 (2.075 per lane) | lane-gaps TL;DR |
| Job tail, lifted WWS PM | 12.6 (2.519 per lane) | lane-gaps TL;DR |
| Host dispatch | 3.0 (0.6 per lane) | lane-gaps TL;DR |

Head plus tail is 84 % of the idle gap, and that is guest PM code run through our lift, not our runtime. Neither the READY_WAKE fix nor FAST_RESCHED moved the p50.

Open discrepancy: other docs quote 72-77 ms cumulative job wall (ppu-critical-path.md §3, c690-offload-decision.md) and 48 ms after the cheap wins (cheap-wins.md TL;DR). The 118 ms lane-sum uses a different definition (PSTL spans, STATS on). I treat the ratios as reliable and the absolute ms as +/-30 %.

**Lifted-code quality (spu-codegen-audit.md §2, §4).** The lift costs 1.03 ns per SPU instruction against 0.31 ns on hardware (3.3x). It emits 8-13 x86 instructions per SPU instruction at ~3.1 IPC.
- 26-37 % of the emitted instructions are GPR-file or stack spill traffic.
- Byte permutes are 2.5-10.5 %. Address masking is 0.6-5.6 %. Branches are ~0.1-5 %.
- The cheap build switches are already on in build-play: AVX2/FMA, FAST_LS, FAST_FLOAT, inline WRCH, `--local-regs`. FAST_LS was the big one (c690 wall 65.5 -> 48.5 ms, cheap-wins.md).
- `--inline-direct` was built and failed its gate: instructions executed were unchanged at +0.02 % (spu-inline-callees.md TL;DR). The spills are intra-function register pressure from 100+ live 128-bit values, not call boundaries.
- Per-stage native rewrites land at 0.99-1.53x (spu-codegen-audit.md TL;DR). Native 0x21 reached offline parity at 31.42 vs 31.44 us/call (overnight-2026-11-05.md), and GPU 0x2A cost +9.2 ms walker (same file).

**PPU side.**
- Main thread: 9.3-15 ms of own work, entirely overlapped. Its one block is 19.7 ms on the ref cond, which is waiting for the walker (ppu-critical-path.md TL;DR, §2).
- PPU codegen is not a lever until the walker and Edge fall well below ~15 ms. Dead-CA elimination is estimated 0.5-1.5 ms, and I expect most of it to be hidden (nhl-lessons.md §3 item C).
- A different PPU cost exists: the walker waits ~6.2 ms/frame on the PPU's append/park (u_park 4.14 plus u_caught 2.02, ppu-critical-path.md §3). That is a ring/handoff structure problem, not a codegen one (Panelist B's area).

**What the references did.**
- NHL Legacy (nhl-lessons.md §1.6):
  - Stage-split native Edge (decompress, skin, cull, compress), with per-stage layout checks and lifted fallbacks.
  - A corpus and differential harness: 22x on the kernel, about +14-18 % whole-game on a Steam Deck.
  - Their win came from a 17 % SPU share, with the whole job at 612 -> 55 kcyc. Ours is a different layout (nhl-lessons.md §1.6), and our native stages gave ~1.0x (above).
  - Most NHL items already apply here (FAST_LS, AVX2, MFC cache, NOP skip, PGO). I measured nothing new there.
- inFAMOUS 2: SIMD integer helpers took 112 -> 123 fps (spu-codegen-audit.md §5). That is already merged.
- I found no local GoW II notes in the docs and did not look further. Treat the "check reference ports" step as incomplete.

## 2. Top 5 recommendations

Estimates are burst-wall ms at the pitch unless stated. Frame ms equals burst ms only while the burst is the producer. The walker takes over in crowded scenes.

**R1. Replace the WWS PM head/tail with native code, then lift the 5-lane cap (best CPU lever).**
- Stage A, 3-5 days: hook the PM's hot sections (ticket claim, descriptor DMA, code re-DMA, post-job PUTs and cursor update) with native functions. Keep the lifted PM for everything else.
  - Gain: up to 23 lane-ms, ~4.6 ms of burst (head 2.1 + tail 2.5 per lane). Realistic 2.5-4 ms.
  - Risk: medium. The PM's observable protocol is the PPU's SpinUntilDone: six 16-bit cursors at +0x42..+0x4c, the list counter at +0x4e, and lock-line state.
- Stage B, +5-8 days: a native job runner on N host threads with the guest-visible `maxContention=5` and active/ready state preserved.
  - c690-extra-lanes showed that hidden helper lanes running the *same lifted PM* add nothing, because the guest list protocol needs a completed prefix and lanes 1-5 start at 0xffff.
  - Mitigation: complete the list natively, publish the cursors and counters the PPU reads, and never run the PM for mesh workloads.
  - Gain guess: lane-sum 118 / 7 workers is ~17 ms. Minus SMT and walker contention, a ~17-20 ms burst is plausible against 29. Risk: high.
  - Verify offline: capture one mesh WWS job list (descriptors and cursors) per scene; replay through the lifted PM and the native runner; byte-compare output rings plus final cursors. The `corpus_edge1` stage corpus and the `PS3RECOMP_C690_CORPUS` infrastructure exist.
- Kill criterion: after Stage A the offline lane-gap sum must fall by more than 50 %. Otherwise do not start Stage B.

**R2. Lifter register-allocation work for the SPU register file: 3-5 ms, 5-8 days, medium risk.**
- The audit estimates 1.2-1.4x on c690 compute (spu-codegen-audit.md §8). That is 118 -> ~85-98 lane-ms, so 4-6 ms of burst at 5 lanes.
- It also speeds the PM head/tail that R1 stage B retires, so R2 is partly redundant with R1B.
- Target the spill share (26-37 %) directly:
  - extend `--ib-goto` to the `bi $r102` back edge at 0xC0F4 so 0xBF00 and 0xC018 become one function. That removes ~30-40 GPR stores and loads per iteration (audit §8 step 3);
  - split the 0xFDA0 body (5,090 x86) so fewer than ~16 vectors are live;
  - give hot constants dedicated registers (audit item 2).
- Do not repeat inlining or `-O3`.
- Offline gates: x86 per SPU instruction down more than 15 % on 0xAF98, 0xA410 and 0xFDA0_body, and the four `0xFDA0` STAGE_OUT hashes still exact (audit §7).

**R3. Skip Edge work the frame never sees: 2-5 ms, 4-6 days, medium risk. Needs measurement first.**
- 412-796 jobs per frame, with only a few percent culled by the game (guess).
- The documented cull bypass showed that removing the cull moves cost to the walker (+16.4 -> +22.5 ms drain, fps 35.4 -> 27.8; cull-bypass-ab.md TL;DR). So skip only whole jobs for meshes proven off-screen, and only by agreeing with the guest's own cull flags. The converse has the same risk.
- Offline: count, over recorded frames, jobs whose entire output is culled or degenerate.
- Gate: if that fraction is under 10 %, drop it.

**R4. Free PPU-side serialisation with the walker: owned by Panelist B, but I want the CPU view in the plan. Est. 4-8 ms, high risk.**
- ppu-critical-path.md lever 2 estimates ~8 ms; the `(id+1)&3` reorder broke geometry (§5).
- From the recomp side the safe version is to add buffers (a deeper ring), not reorder them.
- Offline: replay hash plus a geometry diff.

**R5. Build-time polish: 1-3 ms CPU, 2-3 days, low risk.**
- PGO: +5.8 % was measured on the runtime only (progress.md, 2026-10-29: 24.73 vs 23.37 fps).
- The lifted C is explicitly excluded from the PGO profile (progress.md ~3715).
- Next: a PGO profile that covers the lifted SPU job modules, restricted to hot c690 symbols, and LTO on the runtime hot path.
- Offline: x86/SPU, `perf stat` on the corpus, and the exactness gate.
- Also: the SPU TUs use `-ffp-contract=off`, so SPU_FAST_FLOAT must be validated against the hash corpus first (nhl-lessons.md §3 item 6).

Summed realistic range for R1+R2+R5: burst 29 -> ~15-20 ms (guess, not additive). R3 and R4 are extras. 60 fps requires R1B, a faster walker, and probably all of the above.

## 3. What NOT to do

1. Per-stage native 0x21/0x2A/cull rewrites. They measured ~1.0x live, and the compiler already emits the same AVX2.
2. GPU offload of Edge, Design A or B. The fence is 41 us, and the hand-off cost +9.2 ms (c690-offload-decision.md, edge-gpu-binding.md).
3. `--inline-direct`, `-O3`, or more SPU "micro-opts". All measured zero.
4. More helper lanes while the lifted PM governs completion (c690-extra-lanes.md). More lanes is only valid with a native runner.
5. READY_WAKE, FAST_WAKE, lock-line stripes, or wake-on-PUT as standalone fixes. All neutral or wedged.
6. Cull bypass as a speed fix: it trades Edge ms for walker ms and loses.
7. Judging CPU work offline alone. Six offline renderer wins failed live (plan-meeting §2). My offline gates only screen; a live A/B on the meter settles it.

## 4. Open questions

**For the PS3 architecture expert:**
- (a) Is the WWS job-list completion protocol documented anywhere beyond what we reverse engineered: the `+0x42..+0x4c` cursors, `+0x4e`, and the meaning of 0xffff and chain lengths? Can a native runner satisfy every PPU observer (SpinUntilDone, cellSpursGetWorkloadInfo)?
- (b) The guest's `maxContention=5` and `priority[5]=0` for MeshJobs0/1: is that a hard SPURS contract, or is the game's own list the limiter? Could the PM tolerate 6+ native workers?
- (c) Do jobs in a list have dependencies (barriers, chain length) that bound parallelism below 5x?

**For the runtime/renderer engineer:**
- (d) The 8-thread budget: 6C/12T already hosts the walker, PPU main, present, DisplayFlip, and 5 SPU workers. Can R1B's 7 workers fit without costing the walker, e.g. a dedicated core split? The pin A/B showed PIN=1 hurt (35.31 vs 30.98 fps).
- (e) After the depth fix the crowded walker is 82 ms against an Edge floor of 24-31 ms. Does the plan accept that crowded 60 fps needs walker < 14 ms *and* Edge < 14 ms? That is a 6x walker cut and a 2x Edge cut.
- (f) The 6.2 ms/frame the walker waits on the PPU append/park: a structural fix (deeper ring, R4) has the largest single frame-ms upside. Can your side do it without reordering, and what offline gate would you trust?
