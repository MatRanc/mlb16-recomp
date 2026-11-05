# Panelist B (renderer/runtime): getting the RSX walker from 82 ms to a 60 fps budget

Sources: docs/ in mlb16-recomp (crowded-walker-profile.md = CWP, depth-gpu.md, walker-mt-*.md, walker-per-draw.md, rsx-sync.md, plan-meeting-2026-11-05.md) and libs/video/rsx_vulkan_backend.c. Read only; guesses are marked GUESS.

## 1. Diagnosis

Numbers (ms/frame, ~1,450-1,700 draws):

| | null backend | offline real Vulkan | offline VK + depth fix | live, crowded |
|---|---:|---:|---:|---:|
| walker | 34.0 | 62.2 | 42.2 (depth-gpu.md) | 82 (brief); pre-fix 92 median / 171 on the 1,702-draw subset (CWP s2.1) |
| parse | 2.7 | 2.8 | ~2.8 | 16.9 / 36.1 |
| fetch | 9.9 | 10.4 | ~10 | 8.7 |
| tex | 5.0 | 23.8 | 5.4 | 28 pre-fix, 10.5 post |
| pipe | 5.1 | 5.4 | ~5.4 | 7.8 |
| draw | 6.0 | 13.8 | ~13.8 | 14.5 |
| other | 5.2 | 5.4 | ~5.4 | 10.8 |

What the numbers say:

1. **The single-thread CPU floor is ~34 ms even with no GPU API at all.** The null backend (engine only) costs 20 us/draw. Hitting 10-12 ms at 1,450 draws means 7-8 us/draw. No micro-cut gets there; it needs both less work per draw and more than one thread. I think 10-12 ms is only reachable with a pipelined walker, and then as throughput, not latency.
2. **There is a ~40 ms live-vs-offline gap** (82 live vs 42 offline after the depth fix). `parse` is the known part: 16.9 live vs 2.8 offline, and not CPU contention (CWP s3.3, burner sweep). The rest is `other` (+5), `pipe` (+2.4), and the live 1,024-texture working set. Nobody has yet run a live `perf` on the walker (CWP s5 lever 3 says so). This is the biggest unknown, and a cheap one to remove.
3. **Per-draw Vulkan recording is wasteful in a way the docs have not named.** In `vk_eng_draw_from` (rsx_vulkan_backend.c:5639-5866) every draw does all of this:
   - an `ALL_COMMANDS -> ALL_COMMANDS` memory barrier (`vk_cmd_after_everything`, :845, called at :5748);
   - an image barrier per bound render target (:5822);
   - a GPU vertex-unpack compute bind, push constants, dispatch and buffer barrier (:5749-5799);
   - then `BeginRenderPass`, bind pipeline, set viewport/scissor/stencil/depth-bounds, bind descriptors, draw, `EndRenderPass` (:5830-5866).

   Render passes use `LOAD_OP_LOAD`/`STORE` (:1186-1192), so 1,450 passes means 1,450 attachment load/stores plus 1,450 full-pipeline drains on the GPU. The CPU `draw` phase (13.8 ms; 13.4% of perf samples inside the NVIDIA driver under this function, CWP s3.2) is the symptom. The real cost is probably GPU serialisation too (GUESS, not measured: GPU timestamps were never taken). Without batching, draw is 117 ms (CWP s3.2), so batching fixed submission but not these barriers.
4. **Why MT stalled.** Four attempts (walker-mt-m4, -live, -repair):
   - Offline W=3 was 8.8% slower (16.06 vs 14.76 ms).
   - Live, `tex` went 0.34 -> 7.85 ms (after repair) and fps 35.1 -> 33.0.
   - The visual result was wrong (A/B thumbnail diff 38.8 vs 3.4 baseline).
   - The resolver thread used the walker's global one-shot command buffer and fence, which hung.

   The cause is that the texture hand-off and the backend's global state (`s_vk.cmd`, `s_eb`, `s_e2`) are the serial chain, not the draw recording. The state-thread `draw` phase did fall (2.62 -> 1.04 ms live). Per-worker recording is correct in principle but was built on a backend that is not recording-safe. The N-worker design is the wrong first step.

## 2. Recommendations, ranked

| # | What | Expected | Effort | Risk |
|---|---|---|---|---|
| 1 | Live `perf` plus a drain-scoped timer on the walker (put-wait, ref wait, parse) | up to ~14 ms (CWP lever 3) | 1 session, 1 day | none |
| 2 | Remove the per-draw barriers and render-pass churn | 4-8 ms CPU, GPU time unknown | 4-6 d | medium |
| 3 | Dirty-bit state tracking for pipe/other | 2-4 ms | 2-3 d | low |
| 4 | One record thread (not N), after #2 | 8-12 ms off the walker thread | 7-10 d | high |
| 5 | Persistent or bindless texture descriptors | 1-2 ms | 3-4 d | medium |

**1. Live walker profile.** The brief's 82 ms live vs 42 offline is unexplained. Run `perf record -g` on the walker thread in the crowded replay (the CWP recipe), plus a timer around the `put_wait`/reference wait inside the drain. If `parse` is a wait on the PPU producer (park patch, c690), the fix is in the producer, not the renderer. This tells the other panelists whether the walker is really the bottleneck or is partly starved.

**2. Barrier and render-pass diet.** Three changes, each with a verifiable result:
- (a) Move the vertex-unpack dispatches out of the per-draw render path. They read only the CPU-written raw ring and write disjoint ring slices, so they are independent of render order. Queue them and emit them as one compute pass with one barrier at batch flush (or in a separate command buffer submitted first).
- (b) Keep the render pass open across consecutive draws with the same render pass, framebuffer and depth target. End it only on a target change, a clear, a draw that samples a target written in this batch, or a flush. Draws to the same attachment are already rasterisation-ordered, so same-target blending needs no barrier.
- (c) Replace the global memory barrier with a barrier only when a draw samples an image that an earlier draw in the batch wrote. Track a written-set bitmask per batch. The engine already tells the backend which units are targets (`dsnap`/`sv` counters).

Option for (b): `VK_KHR_dynamic_rendering` (not checked whether the code or device uses it; GUESS) would also remove per-draw framebuffer and render-pass lookups.

Offline check: `PS3RECOMP_REPLAY_VK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 replay_rsx_engine captures/walker_replay_fixed --frames 12 --warmup 4`. Gate on a byte-identical final PPM (depth-gpu.md did this: md5 equal) plus the `draw` phase. Add `VK_QUERY_TYPE_TIMESTAMP` per batch to see GPU time, which no doc has. Gate on the pitch-meter capture and the walker_frame hashes too.

**3. State dirty bits.** `eng_pipeline_get` is 5.1 ms (8.4% of samples) and `other` is 5.4-10.8 ms. Most consecutive draws change a few registers (texture, constants, vertex offset). Set a dirty mask at method decode and skip re-fingerprinting the pipeline key, VP analyse (`rsx_vp_analyze_inputs`, 0.6 ms) and viewport/scissor when clean. Note walker-per-draw showed that memos keyed on content (remap memo) failed live because index streams are rewritten. Dirty bits keyed on register writes do not depend on content, so that failure does not apply (GUESS: confirm with hit rate over the replay). Verify offline with the null-hash gates (they were bit-identical for all previous cuts) and the VK replay.

**4. One record thread, not N.** Rank 4 because it only pays after #2 makes recording cheap and side-effect-free. Design (walker-multithread-design.md s4 says a single record thread alone is 45-50 ms of the 59.5 crowded walker, so its own estimate is modest):
- The ordered state thread keeps parse, targets, clears, WCB, guest-memory reads, texture resolve/upload and the ref/flip publication.
- It emits immutable draw packets into a ring.
- One thread owns a private command buffer and fence, records the packets, and submits in order.
- Never let a second thread call `eng_texture_slot` or touch `s_vk.cmd` (the exact M4 hang, walker-mt-repair).

This moves roughly `draw` + `vertex fill` (4.3) + `bind` = ~19 ms of the 62 offline off the walker thread. The expected walker is ~42 -> ~25-30 ms. Verify offline first: the `test_mt_turns` harness and the walker replay with the Vulkan backend, and the PPM md5.

**5. Descriptors.** Descriptor writes are only ~1% of samples (CWP s3.2), so bindless is a small win here. Do it only if #2 and #3 leave `bind`+`tex_slot` visible. The 8.5-10 ms `tex_slot` (plain cache slot) at 1,669 binds/frame is the real texture cost; the existing `TEX_DIRTY`+texcache cuts target it.

Honest total: 82 -> ~55-60 ms if #1 pans out and #2-#3 land, ~30-35 ms with #4. That is not 10-12 ms. I would not promise a 60 fps crowded frame from the walker alone (see section 4).

## 3. What NOT to do

- **Do not resume N-worker MT in its current form** (4 launches, 4 failures, all above).
- **Do not cache recorded command streams across frames.** Constants, vertex offsets and textures change every frame, and the guest rewrites index streams (walker-per-draw: remap memo hit 497/502 offline, worse live; FETCH_MEMO hit ~5%). Frame-to-frame replay validation would cost as much as recording.
- **Do not trust an offline-only win** (plan-meeting s2: six renderer attempts, none paid off live). Every item above needs the live perf from #1 first, and a pitch-meter regression check (walker 12-19 ms there; it must not slow).
- **Do not cut the N-1 ref gate or publish `ref` early** to hide walker time. `Gfx_WaitPrevFrameRef` is the guest's one-frame-in-flight gate (rsx-sync.md s1.1) and the walker reads guest texture bytes synchronously at drain time (`eng_texture_upload_work`, rsx_draw_engine.c:2886-2956).
- **Do not move texture decode to GPU as a first move.** DXT decode is already native, tex is 5.4 ms offline after the depth fix, and 8 MB uploads were mostly the depth path.
- **Do not add frame-level or render-target-level parallelism** (draws sample earlier targets, backend caches are global; design doc s3.5).

## 4. Sync and pacing: how walker lag interacts with the game

- The green-box bug (rsx-sync.md) was a guest write landing after the guest's GPU-done wait covered the frame but before the walker bound the draw. The walker may lag the PPU by up to one render frame by design (N-1 gate). Any asynchronous stage I add (#4) widens that window unless guest-memory reads stay on the ordered thread (snapshots at packet creation, as M4's `TEX_SNAP=1` did) and `ref` is published only after the packet's guest reads are done. Rule: ref publication must follow guest-visible reads, never recording or GPU completion.
- A slow walker directly stalls the PPU: the PPU waits 19-21 ms per frame on the ref cond (cube-rt-and-frames-in-flight.md, NHL table "block on ref cond 19.7"). So walker speed gains show up as PPU frame time only when the walker is the critical path (crowded scenes yes; the pitch meter no, c690-bound at 34.15 ms).
- The pitch meter is c690/flip-deadline bound at 34.15 ms (2 vblanks). Even a 0 ms walker does not give 60 fps there. Walker work is for crowded scenes only; it does not help the 60 fps goal for the meter.
- Frame-pacing-floor.md: drop frames rather than wait on present. Keep that.

## 5. Open questions

**To the PS3 architecture expert:**
- Does the game really need ~1,450 draws? Crowd quads use one BEGIN/END with per-batch vertex-array offsets (crowd-fix-findings.md). Could consecutive draws with identical state (same pipeline, textures, constants) be merged without breaking order? What dependence does the guest have on the 222 shadow-map lookups (shadow map is cleared and never drawn, depth-gpu.md)? Can a draw-skip rule exist for them?
- Is the walker's `parse` stall a producer-side wait (put pointer, ring allocator, park patch, SPU `c690` output)? Where does the guest cross from "writes FIFO" to "walker may read it"?
- Which game structures are the frame-in-flight limit: is N-1 a hard requirement or a game choice we can patch (e.g. N-2) given the 3 batch sets?

**To the recompilation/codegen expert:**
- The PPU/SPU threads compete with the walker for cores (burner sweep: walker 63 -> 99 ms with 5 burners). How many host cores does the SPU/PPU fleet need? Can the walker plus record thread get dedicated cores?
- Could generated PPU code emit cheaper FIFO writes (e.g. method-count headers), so `parse` costs less? Is the 16.9 ms live `parse` partly PPU-side wake/park latency (NHL: park patch 4.4 + resume 4.6 ms)?
- What does the generated code do around `cellGcm` calls that forces the walker to wait (reference/semaphore polling)?
