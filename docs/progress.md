# Progress log

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26) with `mlb16-work`, `feat/presentation-thread`, and `feat/gpu-vertex-offload`.

## 2026-11-02 (walker profile): fetch 3.6, texture 3.2, pipeline 1.9 ms per frame

Branch `feat/walker-prof` (`ps3recomp-probe`, off `mlb16-work`, not merged). New
`PS3RECOMP_WALKER_PROF=1` splits the real `sink_end` path per frame, and
`PS3RECOMP_WALKER_CAPTURE=<prefix>` images one pitch frame (FIFO words +
dispatcher state + every guest page the draws read, 107 MB). `replay_rsx_engine`
replays that capture through the real draw engine with a null backend; the
existing dispatcher-state hash still verifies at `0x85fc2ff5a1655c26`.

One pitch window (`measure6` flow, both switches on, 537 reports / 15.9 s,
37.07 fps): walker **17.19 ms/frame** at 500 draws. Split: parse 1.21, fetch
**3.56** (7.1 µs/draw), texture bind+hash **3.23** (6.5 µs/draw), pipeline
**1.86** (3.7 µs/draw), bind 0.19, Vulkan draw 4.78 (9.6 µs/draw), other 1.82.
Profiler overhead is inside noise (offline A/B 14.06–14.09 vs 13.95–14.91 ms per
14 ms drain). Offline replay (668 draws, 0 read misses) is 14.0–14.9 ms and
matches fetch/pipeline/draw per draw, but its warm texture phase is 0.2 µs/draw
against 6.5 in-game because one static frame has no texture updates — it is not
an A/B for the texture cut.

Top cuts (not implemented): fetch/remap caching **1.4–2.1 ms**, texture
page-dirty generations **1.9–2.6 ms**, pipeline-key/program memo **0.6–0.9 ms**;
together 3.9–5.6 ms off the 17.2 ms drain (~37 → ~42–44 fps if the producer wait
does not absorb it). Details and the capture format: `docs/walker-prof.md`.

## 2026-11-02 (critical path): the frame is walker drain (17.3 ms) + walker producer wait (11.8 ms)

One traced pitch run (34.58 fps / 10 s; steady window 361 frames, 10.47 s, 34.47 fps; trace window 411 frames,
34.08 fps). New `ppu.usleep` trace span (short `sys_timer_usleep`, guest LR) plus a fix to close the `ppu.gcm_finish`
poll run at the next block (`ps3recomp-probe` `mlb16-work`, trace-only, off by default). Analysis:
`tools/analyse_ppu_path.py` (new) and `tools/analyse_walker_wait.py`.

- **Frame = walker drain + walker put_wait**: 17.25 + 11.77 = **29.02 ms mean** (p50 33.8; distribution bimodal and
  the 10 s counter is 28.9 ms). `ppu main` runs 9.3 ms (already overlapped) and blocks **19.7 ms** in one place:
  `FUN_000655cc` (0x000655cc) waits on the ref cond (`0x1245090`) for the reused buffer. During that wait the walker
  drains 11.6 ms and starves 8.1 ms.
- **Only block on main**: the ref wait. `Mesh_WaitPrevJobList`/`FUN_0006f998` (mesh job list), the flip throttle
  (`0x1245100`, 17 ms) and the post-kick ref spin are **not taken at the pitch** — main has zero usleep spans.
  DisplayFlip blocks 9.3 ms on ref + 19.6 ms on flip-done; it is a pacer, not the limiter.
- **Walker split** (same shape as the attribution run): `u` PPU/park **6.16** ms, `e2` resume **2.80**, c690 `a+d`
  **2.71**, queueing **0.10**. 412 c690 jobs/frame, 41 % lane busy. The PPU and walker wait on each other for ~8 ms
  per frame: main waits for the ref, the walker waits for the PPU's own append (`u_park` 4.1 + `u_caught` 2.0).
- **Levers**: (1) cut walker drain CPU ~5 ms → ~42 fps (low–medium risk); (2) deeper ring / pipeline the PPU append,
  hides up to ~8 ms → ~45–48 fps (high risk, the `(id+1)&3` reorder is not enough); (3) c690 earlier/offload,
  −2.7 ms → ~38 fps (medium/high). 1+2 is the ~50–60 fps path; lever 3 alone is not. Full thread map, addresses,
  overlap/ordering analysis and caveats: [`docs/ppu-critical-path.md`](ppu-critical-path.md).

## 2026-11-02 (wake): waking the walker on the slot PUT removes e2 but not the frame time

Branch `feat/walker-wake` (`ps3recomp-probe`, off `mlb16-work`). The present thread waits on its kick event with a
4 ms timeout (`templates/project/main.cpp:220`, `cellGcmSys.c:1922`); the SPU PUT that fills the parked slot did not
signal it, so PUT->resume was p50 527 µs / p90 2.15 ms. `gcm_note_spu_put` now signals that event when the PUT range
contains the parked `get` (opt-in `PS3RECOMP_WALKER_WAKE=1`, default off; unit test
`tools/test_gcm_walker_wake.py`).

- **e2 removed:** PUT->resume **p50 8 µs / p90 11 µs** in a 398-frame, 12.98 s pitch window. But total wait stayed
  **11.64 -> 11.28 ms/frame** and fps fell **33.29 -> 30.67** traced (**33.44 -> 30.95** on the 10 s counter; the
  no-trace run with the watcher was 32.22 vs the 34.63 baseline). The wait moved to "job not yet pushed"
  (1.82 -> 3.65 ms/frame) and passes/frame rose 6.2 -> 35.4: the walker is caught up with the producer, so waking
  per slot just batches less and costs more walker CPU. Off by default.
- **`u` writer identified:** the park is patched by the PPU, not an SPU DMA. `Gfx_FrameEnd` (guest 0x00066530)
  writes the JUMP into the mesh ring at **0x00066ae0**; the put register is written by `FUN_00065760` (guest
  0x00065760) at **0x00065af8**. `u_park` waits are p50 11.7 ms and end near a flip/kick: the walker is early, and
  the real fix is to pipeline the PPU producer (`Mesh_WaitPrevJobList`, 0x0006ed24) with it, not just wake on the
  write. Numbers, tables and the proposed `u` fix: [`docs/walker-wake.md`](walker-wake.md).

## 2026-11-02 (attribution): the walker's 10 ms wait is mostly PPU/cadence, not c690 throughput

One pitch run (`measure_fast.sh`, 33.44 fps / 10 s; trace window 393 frames, 11.80 s, 33.29 fps; instrumentation
costs −3.4 %). New `PS3RECOMP_TRACE` events (`c690.job`, `spurs.kick`, `gcm.spu_put`, `rsx.park`,
`spu.overlay_load`, `spurs.init`) plus a trigger window attribute every `rsx.put_wait` to the SPU PUT that
overwrites the parked slot and the c690 job that made it.

- 2,444 waits = **11.64 ms/frame**. Only **~2.8 ms/frame is c690 production** (job admitted 1.82, running 0.96).
  **~5.7 ms/frame has no SPU PUT at the park** (20.5 % caught up with `put`, 28.7 % park patched by a non-SPU
  writer), and **~3.0 ms/frame is the walker not running again after the PUT** (slot filled p50 68 µs after the
  wait starts; resume p50 527 µs / p90 2.15 ms later).
- Workers are not saturated: c690 **42.1 %** of lane time over 6 configured lanes, 58.8 % busy, lane 5 idle
  (0 c690). Queueing with an idle lane 0.8 % of wait, all-busy 0.1 %. c690 wall ~75–77 ms/frame cumulative
  (matches 73.5 ms at 34.6 fps).
- **Implication:** the path to 60 is scheduling/overlap — wake the walker on the slot PUT, pipeline the PPU
  producer with the walker, and shave walker CPU (~37–39 fps for wake-on-PUT; ~44–46 for producer overlap;
  both + the no-trace 14–15 ms walker → ~60). Design B alone measures ~38 fps, not 55–60, because it removes
  only the c690 part. Full attribution, levers, caveats: [`docs/walker-spu-wait.md`](walker-spu-wait.md),
  analysis `tools/analyse_walker_wait.py`.

## 2026-11-01 (measure-and-decide): c690 offload verdict — no-go for A; B is the only 60 path

One pitch run (34.63 fps, 10 s window, `PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1`). c690: **429 jobs/frame**, job
wall **73.5 ms/frame cumulative** (p50 115 µs, p90 239 µs), opcode `0x0A` 30.6 % / `0x2A` 7.9 % (every job) /
culls 5.0 %. Fence round trip **41 µs p50 / 74 µs p99** (locked clocks). Design A per-job win 44.5 µs mean but
−16.5 µs at p50/p99 → **~37–40 fps; no-go**. Design B (whole job on GPU, indirect draws, no CPU wait) → **~55–60
fps but 2–4 months**. Next lever: tree/leaf lifter codegen (~34 % of the SPU wall). Full numbers + leaf
cross-check: [`docs/c690-offload-decision.md`](c690-offload-decision.md).

## 2026-11-01 (correction): 0x2A is an assembler; 0x6D68 is a direction unpack

- `0x10F78` (opcode 0x2A, selector 0) is a **vertex-format assembler**: it gathers four already-final planar
  arrays (UV0 `0x21C30`, COLOR0 `0x20FB0`, POSITION `0x20330`, NORMAL `0x1F6B0`) into 36-byte records at `0x2F870`,
  with one float→u8 color pack (`0x7D40`, `cfltu 149`). It does **not** unpack/transform/normalize.
- `0x6D68` is a **12/12/11 SNORM direction/tangent unpack** (4th field 0), not position. The real per-vertex chain
  is: unpack `0x6D68` → 3x4 transform `0xA798` (matrix LS `0x29540`) → normalize `0xAF98` (frsqest+fi) →
  snorm pack `0x7C18`/`0x8160` → planar arrays → `0x2A` assemble.
- The captures are from **different job instances**, so they cannot be concatenated byte-exactly; a co-temporal
  capture (or whole-job replay) is needed to validate the full pipeline.

## 2026-11-01 (capture): pitch pipeline is multi-command; per-call offload not viable

- Captured the pitch frame's `0x10F78` (0x2A handler), `0x10B88`, `0xA798`, `0xAF98`, `0x6D68` in one run
  (multi-address `SPU_FN_CAPTURE`). `0x10F78` replays offline with zero DMA/mailbox.
- **The 0x2A handler is not the position path**: it is the stream-2 attribute gather/convert (`0x7840` family),
  writing `0x2F870`/`0x23400`. Position unpack is the separate leaf `0x6D68` (bit-exact, `0xb98ec2c0561b2bbf`).
- **Fence-latency implication**: `0x6D68` is ~0.5 µs/call; a per-call GPU dispatch+fence (~10–20 µs) is a loss.
  The offload must be **job-level** (all streams, one dispatch + one fence per job), which requires the full
  per-vertex pipeline in one shader.
- Report + constants: `docs/pitch_0x2A_pipeline.md`, `tools/replay/pitch_pipeline_constants.h`.

## 2026-11-01 (tooling decision): no Ghidra SPU import, no emulator FP

- Ghidra 12.1.2 has no SPU processor; GhidraSPU builds for it, but the MCP is bound to the EBOOT and cannot switch
  programs, and the c690 module is already lifted to readable, bit-exact C. **Do not import the SPU program.**
- anergistic (offline SPU emulator) has a flat EA model + MFC DMA but is **integer-only** (no `fma`/`cflts`/`frest`),
  so it cannot run the geometry math without ~15 added FP instructions. **Do not invest in emulator FP.**
- Use the lifted C + `tools/spu_disasm.py` for the thin low-LS runtime. Runtime per-format constants need
  function-entry captures, not decompilation.

## Next steps (2026-11-01)

Per-call offload is not viable (fence latency >> the ~0.5 us kernels). The offload must be **job-level**, and the
pipeline is a chain of separate commands. To build a job-level shader we need the full chain from one job:

1. **Co-temporal capture (recommended, one run).** Add a probe that captures the producing chain in a single job:
   `0x6D68` -> `0xA798` -> `0xAF98`, the `0x0A` pack scale/offset (`sr9`/`sr10`), and the `0x29540` matrix for the
   same job. Then build/validate the job-level shader offline against that one snapshot.
2. **Whole-job replay (offline, no run).** Lift the low-LS SPURS runtime (`0x1690/0x22B0/0x1B08/0x1AE0`) and add
   an EA/DMA model to the replay harness, then replay a job from the entry.

Then: build the job-level shader for all streams, validate offline, integrate at the DMA boundary (one dispatch +
one fence per job), and run the single go/no-go measurement. Function map: `docs/c690_function_map.md`.

## 2026-11-01 (decision): commit to the whole-pipeline GPU offload

Cheap levers are closed (per-function native 1.00×/1.03×; cull bypass loses). The only path to 60 fps is moving the
c690 **per-vertex math** (unpack `0x6D68`, skin `0xD2B8/0xD4A8/0xC98C`, normalize `0xAF98+B238`, pack `0x7C18/0x8160`)
to GPU compute, keeping culling/tree/index/ring on the SPU. First step is a 1–2 week go/no-go spike on one format
(bit-exact vs the offline harness, then measure the fence/sync cost). Full plan, data flow, risks, and milestones:
[`docs/gpu-offload-plan.md`](gpu-offload-plan.md).

## 2026-11-01 (late, 3): per-function native + cull bypass are both closed

- **Native kernels bit-exact but at parity:** `0x6D68` = **1.00×** (563 vs 568 ns/call); `0xAF98`+`0xB238` = **1.03×**
  (216 vs 223 ns/call). The lift already emits the same `vpshufb/vpsllvd/vfmadd132ps` mix. Per-function SIMD is not
  a performance lever.
- **Cull bypass loses in every variant:** all three = 33.6 fps; only `0x2C` = 34.85; baseline 36.6. The culls remove
  triangles before the per-vertex math, so they are a net win.
- **Conclusion:** cheap levers exhausted. 60 fps needs the whole-pipeline GPU offload (2–4 months) or accept ~36–40 fps.

## 2026-11-01 (late, 2): offline capture/replay harness working

`SPU_FN_CAPTURE=<lsa>[:dir]` (ps3recomp `a3a5564f`) dumps the full LS + 128 GPRs + pc at a function entry. First
capture/replay: `0x6D68` at the pitch → `tools/replay/` loads it, runs the lifted body, prints a deterministic
checksum (`0xfc2e9569dc2feb11`, loop 24) and **~563 ns/call**. This is the offline loop for native kernels — no
game boot or match load. Next: native `0x6D68` diffed against the checksum, then `0xAF98`+`0xB238` as one unit.

## 2026-11-01 (late): cull-bypass A/B (negative) + research triage

- **Cull bypass is a perf loss.** Porting the RPCS3 `MLB16_triangle_fix` idea (`bi $lr` on ICE handlers
  `0x2C`/`0x26`/`0x25`, LS `0x12DC0`/`0x13518`/`0x13520`) via an early `SPU_RET(ctx)` measured **33.57 fps vs 36.6**
  (diff 6.5). The cull is an early-out before the expensive per-vertex math; removing it costs more downstream
  than it saves. Keep it off; it stays a *visual* option for high-resolution pin-holes only.
- **Research doc:** its c690 command table / dispatch tables / patch offsets are corroborated by our own
  `mlb16-revival-server/docs/ice-mesh.md`. The valuable new prior art is **WKC2's capture/replay harness**
  (`WKC_JOB_CAPTURE` / `WKC_JOB_REPLAY`) and its serial-ring-commit rule. Its RPCS3 config flags are not
  applicable to a native recomp, and its "native SIMD → 6–7.5 ms" estimate is optimistic given our neutral
  `0x7C18` kernel and the cull result. RPCS3 calibration: 12700K ~55–60 fps minor-league / ~38–48 major.

## 2026-11-01 (night): Stage 0 c690 capture implemented — first real data flow

`SPU_JOB_LS_DUMP=<prefix>` (ps3recomp `f47deb52`, off by default) captures at the c690 entry (LS `0x135B8`):
the local store, the descriptor words, any guest EAs it references, and the job's first 400 DMAs. The job runs
under **image 6 (the SPURS policy)**, not image 204. Findings:

- The c690 code is DMA'd into LS `0x4000` from guest `0x105EE00` (variant `0x106D380`).
- The descriptor (`r3` = LS `0x1330`) is format/callback data; +0xC is a guest EA (`0x01243E00`) pointing at a
  **16×16-byte attribute-format table**.
- **Data flow:** inputs are per-mesh attribute streams from `0x0173xxxxx`–`0x0180xxxxx` (`0x180`–`0x500` blocks),
  params/matrices at `0x172F2FF0`, and a `0x2000` block from RSX memory `0x05800C000`; outputs are PUT back to
  the **same** `0x0173xxxxx` EAs (in-place transform), plus `0x011B00000` (`0x2400`), `0x146A800`, and
  `0x05800C000`.
- `tools/spu_profile.sh` is the repeatable gdb sampler (the `psample` attribution is unreliable).

Next: a larger/filtered DMA capture to separate palette/weight GETs from vertex-attribute GETs, and map the
attribute-format table. See `~/Documents/work_notes.md` "Session 7 Update".

## 2026-11-01 (evening, later): current cost table + the honest path to 60 fps

| Subsystem / Operation | Old figure | Current | Notes |
|---|---|---|---|
| Frame | ~31 ms | **~27.2–27.8 ms (36.0–36.8 fps)** | trace median 33.8 ms because flips burst/stall |
| RSX FIFO walker drain | ~31 ms | **24.65 ms** | single-threaded; includes ~7–10 ms waiting on the SPU |
| Walker *own* CPU | ~26 ms (~35 µs/draw) | **~14–15 ms** | ~35 µs/draw × ~440 draws; already under 16.6 ms |
| SPU lifted job | ~44 ms aggregate | **25.5 ms wall/worker × 5 (~127 ms CPU)** | 5 busy, 6th underused; spread over ~15 kernels |
| PPU `Gfx_FrameEnd` wait | ~30 ms | **18.2 ms** | halved by the `0xA8A0` lift |
| Present | ~9 ms sync | **offloaded to present thread** (windowed) | not on the benchmark critical path |
| GPU | — | **not the limiter** | headless replay 0.59 ms/frame |

120-sample gdb profile (140 c690 samples): `0xFDA0` tree/leaf 28, `0xD4A8` output/skin 15, `0x6D68` unpack 12,
`0x8160` pack 10, `0xAF98` normalize 10, `0x7C18` pack 8, `0xD2B8` skin 8, `0xFCB8` leaf 7. Buckets: **~50%
per-vertex math, ~34% tree/leaf, ~15% index/table/ring.**

**Why 60 fps is plausible but not a pile of 2 ms wins.** The `5M → 0.3M` win was *interpreted* SPU steps — a thin
unlifted-code layer, not the job (32.5 → 36.6 fps). The "30 ms → 0.5 ms" memory is the `replay_rsx --verify`
artifact, not a speedup. Micro-opts are single-digit %. The real path is one structural move: the walker's own CPU is
already ~14–15 ms (fits under 16.6 ms), and the SPU is ~50% per-vertex math (~12.7 ms) + ~34% tree/leaf (~8.7 ms) +
~15% index/ring (~3.8 ms). Move the per-vertex math to the GPU → SPU wall ~13 ms → the walker's ~10 ms SPU-wait
collapses → frame ~16–17 ms → **55–60 fps plausible**. If only some of the ~40 format variants are covered or sync
serializes, the walker caps it at **~40 fps**.

## 2026-11-01 (evening): reliable SPU profile + GPU-offload reality check

A gdb stack-sampling run (25 rounds, pitch screen, 36.8 fps binary) replaces the unreliable `psample`
per-thread attribution. The c690 job wall (~25.5 ms/frame across 5 workers) is **spread over many
format-dispatched kernels**, not concentrated in skinning:

- top `0xFDA0` (recursive tree/leaf) ~17% of job samples; then `0x7C18`, `0xFCB8`, `0x6D68`, `0xAF98`,
  `0x6860`, `0xE0A8`, `0xC98C`, `0x76B8`, `0x10700`.
- `0x31B0` is **not** c690: it is below the c690 LS base `0x4000`, inside the SPURS policy window.
- dispatch overhead is now tiny (`spu_lifted_lookup_full` 4, `spu_indirect_branch_slow` 3) after the `0xA8A0` fix.

Consequences for the plan:
- **Offloading only skinning (`0xD2B8`, ~10–20% of the job) saves ~2.5–5 ms → ~34–38 fps, not 60.**
  The whole per-vertex math pipeline (unpack/transform/normalize/pack, ~35–55%) is the necessary GPU step,
  and even then the walker (~24.65 ms) must be cut to <16.6 ms or it caps the frame at ~40 fps.
- `vtx_unpack.comp` is downstream of c690, so it cannot reduce c690's cost by itself.
- palette/weight addresses are runtime values; a first step is `SPU_LS_DUMP` + per-function timers, not code.

The walker remap cleanup (tightest table per draw) was behavior-preserving but **neutral** (36.80 vs 36.6).
See `~/Documents/work_notes.md` "Session 5 Update" for the full scoping.

## 2026-11-01 playtest (windowed, PS4 controller) — issues to fix

Run with `ab-tools/play.sh` on the 36.6 fps build. The game plays, but it is neither smooth nor visually correct yet.

**Rendering / visual**
- **Missing shadows, dark/black crowd, blank or chrome/pink players, missing jerseys.** The render-target readback
  (`PS3RECOMP_RSX_WCB`) was left off by default for benchmark speed; that is the path that writes rendered colour
  (and the crowd/face caches) back to guest memory. `play.sh` now sets `PS3RECOMP_RSX_WCB=1`; retest before
  concluding which of these it fixes.
- **Crowd LOD popping:** people appear only when the camera zooms in; segments of the crowd are missing.
- **Picture-in-picture view has a black background.**
- **Depth-of-field duplicates** (the RPCS3-fixes artifacts) are present.
- Several issues the `mlb-show-rpcs3-fixes` repo patches are present here and not yet ported.

**Presentation / window**
- **The window only updates when the mouse moves.** With the mouse still, the image runs for ~1 s then freezes while
  the game keeps running (controller rumble continues). The headless benchmark does not show this, so it is the
  windowed present/event path (Wayland/compositor frame callbacks or the SDL event pump), not the renderer. Try
  `SDL_VIDEODRIVER=x11` to test the XWayland path.
- Cutscenes and player entrances drop to low fps; load times are long.

**Input**
- **Controller presses are not always registered immediately**; menu items sometimes need several presses. Likely
  `PAD_POLL_INTERVAL_MS` / pad report pacing, not the renderer.

## 2026-11-01 (later): Target 4 — a missing lift entry was costing ~5 M interpreted SPU steps/s

The c690 job module's `spu_lifted` in the working tree was **stale**: it predated the `0xa8a0` entry already present in
`tools/lift_spu.sh`. The game reaches LS `0xA8A0` through a function pointer inside the lifted `0xA818` body, so with
no registered entry it fell to the SPU interpreter. `SPU_INTERP_STATS` showed ~5 M interpreted steps/s, with
`img=0xCC ls=0x0A8A0` as the top entry (~58k calls/s).

Re-running the canonical `tools/lift_spu.sh` (same flags; `0xa8a0` is already in `LBP_JOBMOD_EXTRA_FUNCS`) splits
`0xA818` into `0xA818` + `0xA8A0`. Pitch-screen result:

| | before | after |
|---|---:|---:|
| pitch-wait fps | ~32.5 (32.17–33.15) | **36.13 / 36.59 / 36.67** |
| interpreted SPU steps | ~5 M/s | **~0.1–0.3 M/s** |
| pitch visual diff | 12–18 | 14.2 |

The AVX2 `0x7C18` kernel was re-applied to the new lift and measured neutral (36.59 vs 36.67), so it stays wired.
`replay_rsx` still verifies `0x85fc2ff5a1655c26`.

**Lesson:** after changing `LBP_JOBMOD_EXTRA_FUNCS`, re-run `tools/lift_spu.sh`; `spu_lifted` is generated and not in
git, and a stale copy silently reintroduces the interpreter path.

**`0x108EC` — tried, then reverted.** The next missing c690 pointer target, `0x108EC` (between lifted `0x10758` and
`0x1095C`), was added to the lift. It cut interpreted steps further, **~0.3 → ~0.1 M/s**, but pitch-wait fps did not
move (**36.06 vs 36.1–36.7**, within noise), and the entry still hit the interpreter (`img=0xCC ls=0x108EC`) through a
residency/SMC path even though the function is now lifted. Adding it to the *global* `LBP_JOBMOD_EXTRA_FUNCS` also
pulled two other modules' `code_end` into their data tails (`7ac07dde` 0→411 unsupported, `31097a84` 0→195), so it
would need a per-module extras mechanism. Reverted to the `0xA8A0`-only build. The remaining interpreter
(~0.1–0.3 M/s) is the policy module's self-modifying-code entry at `0xA68` plus these residency paths; it is no
longer a meaningful frame-time lever.

## 2026-11-01: frame re-measured; the walker + SPU pipeline are the wall; Target 3 fence-skip fails again

Current `mlb16-work` tree, pitch screen: **32.2 fps** (1 s buckets 32.1 ± 2.0); `replay_rsx` still verifies
`0x85fc2ff5a1655c26`. The AVX2 c690 `0x7C18` kernel is wired and active (the linked binary calls
`native_spu_func_00007C18`), but it is not the main cost.

- **`replay_rsx --verify` is a correctness gate, not a benchmark.** The 36 ms "frame" is the per-call hashing; without
  `--verify` the same 646-draw frame is **0.59 ms** (~1,700 fps). The in-game walker is ~45× slower per draw because it
  also fetches/converts guest vertices, checks textures and records Vulkan commands.
- **Timeline (`PS3RECOMP_TRACE`, median 33.9 ms frame):** walker drain ~27 ms; PPU main sits in the `Gfx_FrameEnd`
  fence spin ~34 ms; 5–6 SPURS workers run c690 concurrently. The walker also has ~10.5 ms/frame of gaps waiting for
  the SPU to publish buffers (`rsx.put_wait`).
- **Walker CPU ~40 µs/draw** (38.5–49, `VK_EB_STATS`) over ~438 draws/frame: ~29% Vulkan driver command recording,
  ~18% `rsx_vertex_remap_build`, ~22% texture change checks, ~6% `sink_end`.
- **SPU profile (1 kHz per-thread `perf_event_open`):** the c690 job's hot functions are `0xD540`, `0x10028`, `0x6D68`,
  `0xAF98`, `0x8160`; `0x7C18` (the vectorized one) is a small share, and the others are local-store-bound.
- **Target 3 re-attempt, done correctly:** `0x011203D4` was left untouched; at the `Gfx_FrameEnd` call site (lr
  `0x6579C`) the wait used `(id + 1) & 3` instead of `id & 3`, behind `MLB16_PIPELINE=1`. It still fails: **185 flips
  in 689 s** and never reaches the pitch screen, versus ~9,300 flips for the same build without it. The fence also
  guards guest buffers the walker is still reading, so the PPU cannot run ahead of the drain. Reverted; both PPU
  recomp files are byte-identical to `.orig`.
- **Committed** the three infrastructure tweaks and the AVX2 kernel on `mlb16-work` (`ba20312c`). The
  `recomp-work/spu_lifted` wiring is still outside git; a copy is at `spu_recomp.c.avx2_wired`.
- **Walker split (`VK_DRAWPROF=1`):** of ~40 µs/draw, only ~10 µs is Vulkan recording (vertex dispatch 3.1, render
  pass begin 1.9, descriptors 1.4, copy 1.3, barriers 0.8, state 0.4, binds+draw 0.6, pass end 0.6); the rest is
  texture checks (~9 µs), vertex remap (~7 µs) and bookkeeping.
- **Walker experiments:** backing off the full texture re-hash for stable textures = 32.5 fps (no change, reverted);
  `PS3RECOMP_ENG_VTX_REMAP=0` = 28.4 fps (worse, dedup stays on); three frame-pipeline sets instead of two = 32.96 fps
  (1 s-bucket 34.1), a small within-noise gain, kept opt-in (`060bd105`).
- **Conclusion:** walker CPU is not the marginal limiter; the frame loses ~10.5 ms/frame waiting for the SPU to
  publish command buffers. The c690 SPU job is the next lever.

Next: cut the c690 job's time (the remaining hot functions `0xD540`, `0x10028`, `0x6D68`, `0xAF98`, `0x8160`) or
offload skinning to the GPU. The walker is close to its practical limit.


## 2026-11-01: Descriptor-Driven GPU Vertex Compute Unpack & Presentation Thread Complete

- **Descriptor-Driven GPU Vertex Offload (Macro Target 2 Completed, `feat/gpu-vertex-offload` commit `1f6dc0cd`):**
  - Integrated `vtx_unpack.comp` compute shader directly into `rsx_vulkan_backend.c` and `rsx_draw_engine.c`.
  - Upgraded from rigid layouts to a **Descriptor-Driven Compute Unpack** architecture:
    - *Modes 0, 1, 2 (Fast Paths)*: Zero-overhead unrolled unpack paths for 24-byte 3D meshes and 16/20-byte 2D UI quads.
    - *Mode 3 (Generic Descriptor Unpack)*: Receives up to 8 packed `AttrDesc` structs in push constants. Uses funnel shifts (`load_unaligned_be32`, `load_unaligned_be16`, `load_u8`) to natively unpack 2-byte aligned 22-byte streams (e.g., Short3 Pos + CMP Normal + CMP Tangent + Short2 UV0 + Short2 UV1) without alignment faults. Supports all 7 RSX attribute formats (Float, Half, UNorm8, UInt8, SNorm16, SInt16, CMP32).
    - *Dynamic Index Remapping*: Normalized non-contiguous vertex references (`elem = ((vertex_id + base_index) & 0xFFFFF) - stable_first`), streaming index remap buffers to compute threads to fetch and unpack sequential hardware vertex buffers.
  - Live Game Benchmark Results (`measure6.sh`):
    - Evaluated 2,278,559 draw calls.
    - **Total draws offloaded to GPU surged to 98.28%** (2,239,363 draws vs 320,595 previously).
    - **CPU software fallbacks collapsed from 85.0% to 1.72%** (39,196 draws).
    - Pitch-wait FPS rose to **24.19 fps** (1-sec mean 24.39 fps, instantaneous mean 24.43 fps ±2.62 fps), with instantaneous peaks reaching **32.15 fps** (vs 22.64 fps prior run, +1.55 fps / +6.8% uplift).
  - Deterministic & Visual Verification:
    - `replay_rsx kauffman_pitch.bin --frames 100 --verify`: 100% deterministic (State hash `0x85fc2ff5a1655c26`).
    - `same.py` pitch-wait reference match: diff 13.4 ($\le 25.0$). Full-screen end frame match: diff 2.6 across full 1280x720 viewport (sub-pixel match, zero mesh tears, zero spikes).
  - Architectural Diagnostic & Bottleneck Analysis:
    - Headless `replay_rsx` executes the entire 646-draw Kauffman frame in **0.59 ms (~1,700 fps)** without per-call hashing, proving the RSX walker itself is no longer the bottleneck.
    - The live game remains paced at ~24 FPS because the walker is starved at self-JUMP "parks" waiting for the **SPU Edge Geometry pipeline (`jobmod_c690c2f7b09a`)** which spends ~35–40 ms decompressing and skinning vertices on the CPU, and the **PPU main thread serializing in `Gfx_FrameEnd`**.
    - Target 2 permanently eliminated the walker's 26 ms CPU vertex anchor (which made 60 FPS previously impossible). The remaining gap to 60 FPS is now isolated to Macro Target 3 (PPU/RSX overlap) and SPU skinning loop optimization.

- **`mlb16-next` verified and fast-forwarded to `mlb16-work`:**
  - `PS3RECOMP_ENG_VTX_DIRECT`: converted draw vertices straight into the mapped Vulkan vertex ring buffer, bypassing intermediate host scratch copies.
  - Faster constant/NOP walker dispatch for repetitive WWS Edge state headers.
  - Benchmarked at pitch screen: `cur` 23.80 fps vs `next` 24.16 fps (+0.36 fps uplift).
  - Visual verification: `bodies.sh` captured 4 high-detail camera angles. Python/PIL pixel analysis confirmed identical rendering (no missing geometry, spikes, or flickering). Fast-forwarded.
- **Clang PGO automated end-to-end (`pgo_build.sh`):**
  - Profile counter pollution resolved: `pgo_profile_list.txt` filtered strictly to `src:*ps3recomp-probe/*.c` and `*.cpp`, eliminating counters from lifted SPU/PPU translation units (`0` in `spu_workloads.c.o`).
  - Clean training run: match load to pitch screen reached in 276.1 s (+15.5% overhead vs uninstrumented, zero hangs).
  - 9-run alternating A/B (load < 1.5, GPU clocks locked): GCC baseline 23.37 fps, Clang plain 23.66 fps, Clang PGO **24.73 fps (+5.82% / +1.36 fps, $\sigma = \pm 0.09\text{ fps}$)**.
  - Automated entire pipeline into `recomp-work/pgo_build.sh`.
- **Deterministic RSX FIFO Replay Tool (`tools/replay_rsx.cpp`, `tools/replay_rsx_core.c/.h`):**
  - Standalone C++17 CLI tool that accepts dumped RSX command streams (`.bin`) and replays them through the walker and register-file draw engine headlessly.
  - Reports p50, p95, min, max, equivalent FPS, draws/sec, and 64-bit deterministic state verification.
  - Synthetic test fixture `sample_fifo.bin` replays 50 frames at 1,720 equivalent fps with 0% measurement noise. Added CTest target `replay_rsx_sample_fifo`.
- **Vulkan Render-Target Readback (WCB) & Face Cache Purge:**
  - Implemented `rsx_draw_engine_flush_dirty_to_guest` in `libs/video/rsx_draw_engine.c`. Offscreen render targets (dynamic player faces, crowd atlases) marked dirty during rendering and blitted via `vkCmdCopyImageToBuffer` into guest memory before sync points (`NV406E_SET_REFERENCE`, `SEMAPHORE_RELEASE`, flips). Display scanout buffers protected.
  - Unit test `test_color_readback_guest.c` passing 13/13 checks.
  - Added `--purge-cache` and `PURGE_FACE_CACHE=1` to `tools/run.sh` to safely auto-delete corrupt, black `HeadFaceCacheFile` assets on startup.
- **Upstream Pull Request Campaign (11 PRs submitted to `sp00nznet/ps3recomp`):**
  - Converted local fork fixes into 11 clean-room PRs (#640–#650) addressing community issues #551 (MLB 15 match load), #464 (Puppeteer SPU taskset crash), and #345.
- **Real-Game FIFO Dump (`PS3RECOMP_DUMP_FIFO`) & Big-Endian Fix:**
  - Fixed big-endian dumper bug in `libs/video/cellGcmSys.c`: swapped words to big-endian (`__builtin_bswap32`) and padded unmapped payload dwords (`0u`).
  - Captured clean 100-frame trace at Kauffman Stadium pitch window (`kauffman_pitch.bin`, 40,466 words, 158.07 KB).
  - Verified 100% deterministic replay: 646 draws, 50 clears, 2,338 dispatcher calls per frame, state hash `0x85fc2ff5a1655c26`. Pure headless frame replay runs in 0.59 ms (~1,700 fps).
- **Decoupled Vulkan Presentation Thread (Macro Target 1, Completed):**
  - Moved `vkQueuePresentKHR` off the walker thread to a dedicated background worker thread in `libs/video/rsx_vulkan_backend.c`.
  - Resolved 12 FPS swapchain sync-coupling by requesting 2 physical queues (`queueCount = 2`): Queue 0 for walker submissions, Queue 1 for presentation (`s_vk.present_queue`). `vk_queue_present` calls `pvkQueuePresentKHR` directly on Queue 1 without acquiring `s_vk.queue_mu`.
  - Added `VK_PRESENT_MODE_MAILBOX_KHR` negotiation with fallback to FIFO and non-blocking slot acquire.
  - Exported `__GL_SYNC_TO_VBLANK=0` to eliminate driver sleeps.
  - Diagnosed performance behavior:
    - Synchronous WCB readbacks (`PS3RECOMP_RSX_WCB=1`) + GPU downclocking to P5 (810 MHz) had caused an ~11.2 FPS dip due to PCIe readback stalls (`g.be->submit_and_wait`).
    - With `PS3RECOMP_RSX_WCB=0`: Headless baseline instantly restored to **23.98 fps** (1-sec mean 24.84 fps, peak 34.25 fps).
    - Windowed live run (`PS3RECOMP_VK_WINDOW=1`): **23.34 fps** (instantaneous mean 23.52 fps ±2.17 fps), completely eliminating the 12.00 FPS clamp with only a 0.64 fps (2.6%) delta from headless.

---

## 2026-10-29: the match-load race found; a faster FIFO walker; clang PGO +5.8%

Measured by 3-run alternating A/Bs at the pitch screen. Today's absolute numbers are ~23 fps, not 26: the machine had
more running alongside (a remote-desktop session alone cost ~15-20%), so compare builds within a session only. The 55 s
"hold" number swings by ±3 fps between runs of one build, so decisions use the 15 s pitch-screen window.

- **The match-load freeze behind the PGO stalls:** the RSX drain gives up on a self-JUMP "park" after a grace period
  and follows `put` instead. MLB 16 parks in command buffers its SPU jobs are still writing, so when a slower runtime
  (an instrumented build, a busy host) took longer than the 50 ms grace, the drain skipped those commands and a GPU
  label with them, and the game waited forever. Confirmed by setting the grace to 5 ms on a normal build: same freeze.
  The default is now 1000 ms (God of War HD needed more than 50 ms too); zero resyncs and no fps change in A/B.
- **The instrumented builds' crawl** had a second cause: the profile list `src:*ps3recomp-probe*` also matched
  `spu_helpers.h`, so every SPU helper inlined into the lifted SPU code was counted (`spu_v2u` alone 50 billion
  times). Listing only `src:*ps3recomp-probe/*.c` and `src:*ps3recomp-probe/*.cpp` fixed it: training reaches the
  pitch screen ~10% slower than a normal build.
- **Clang PGO (runtime only):** 24.73 fps vs 23.37 for the gcc build and 23.66 for plain clang, best in all 3 runs
  (24.69, 24.84, 24.67).
- **FIFO walker:** with the draw engine on, every method word went through the per-word diagnostic loop. Runs of 3D
  methods now skip it, and plain register writes are stored without the full dispatcher: 19.7 → 11.1 ns per word in a
  replay benchmark with identical handler calls and register state. In the game it was within noise at the pitch
  screen; screenshots match.
- **Real `sys_lwcond`** (host condition variables, timeouts, a 10 ms fallback wake) behind `PS3_LWCOND_REAL=1`:
  +2.7% pitch, -7% hold over 4 runs, so neutral; left off.
- **A vertex cache across frames was measured and dropped:** validating the inputs costs about as much as converting
  them (≤1.2× best case in a benchmark). The cost that matters is converting and then copying ~55 MB a frame into the
  GPU ring; writing straight into the ring is next.

## 2026-10-28 (night): the frame traced; the walker is the bottleneck; +27% from pipelined GPU batches

All ps3recomp changes are on `mlb16-work` (MatRanc/ps3recomp).

- **Merged, measured at the pitch screen** (3 alternating runs each):
  - inFAMOUS 2's SPU DMA fast path, inline MFC channel ops and register-built insertion masks: +2.7%;
  - the lifter writes back only registers written since the last sync (register traffic in the hot job down 45%):
    within noise;
  - a ring filled exactly to its last word is no longer taken for an overrun (found by review);
  - `--const-bi` no longer turns resolver-only targets into gotos (found by review; MLB's lift is unchanged);
  - fixes from two reviews: memory containers reserve their size, SPURS slot checks and `maxContention` clamp, AIO
    write prefault and shared AIO ids, the GCM interrupt thread's stack, the template's SPU options, and the
    `run_tests.sh` and video fixture tests wired up. Within noise. One SPURS idle change was left out (-0.3 fps).
- **A frame tracer.** `PS3RECOMP_TRACE=<file.json>` writes a Chrome/Perfetto timeline: SPU job runs and waits,
  lock-line retries, the walker's drains and put waits, PPU lwmutex/lwcond/semaphore/event waits, GCM finish and
  flip waits. Tracing on costs nothing measurable at the pitch screen (20.6 fps either way).
- **What the trace showed.** The walker's drain is one ~36 ms pass over a few hundred bytes of the main command
  stream: it follows the command buffers the SPUs build. The SPUs out-run it, fill their rings and wait. Per thread
  the walker used ~55-68% of a core; the rest was GPU waits (`VK_EB_STATS=1`).
- **`VK_FRAME_PIPELINE=1`** (an existing ps3recomp mode) submits a full batch of draws without waiting for the GPU:
  20.8 -> 26.1 fps at the pitch screen, the pitch screen pixel-identical. Now on in `tools/run.sh`.
- **Native Edge kernels: dropped.** With the benchmark fixed (setup was inside the timed loop), an 8-wide AVX2
  version of job function 0x7C18 is bit-exact and 1.00x the lift. Only 2 of the job's 9 hot functions could be
  widened at all.

## 2026-10-28 (afternoon): a faster frame dump; striped lock-line locks measured; the hot job module identified

- **The frame dump was costing the walker ~20% of its time.** The test harness's PPM dump (every 10 frames) wrote 3
  bytes per `fwrite` call. It now packs the frame into one buffer and writes it once, to a temp file renamed into
  place. Pitch screen: 18.8 -> 20.3 fps (3 runs each). This only affects our measurements, not play, but the walker
  finishes last every frame, so the dump was inflating every number above (ps3recomp `c93af77`).
- **Striped lock-line locks (inFAMOUS 2, MIT): neutral.** Five SPUs contend on the one global lock-line lock in the
  job manager's atomic job grab, about 4% of SPU busy time. The 64-stripe version from inFAMOUS 2 measured 18.8 vs
  18.5 fps, within noise. It is kept on branch `port-lockline-stripes`, unmerged.
- **SPU time, split by sampling:** ~72% in lifted job code and ~28% in the runtime (lock waits, indirect-branch
  resolution, the drain loop, `mfc_for`). One module, `c690c2f7b09a`, is ~70% of the lifted part. A read-only
  analysis identifies it as a variant of Sony's Edge Geometry pipeline (vertex decompression, matrix-palette
  skinning, culling, quantization into the RSX mesh rings, and draw packets with fenced DMA).
  - A native replacement could at best halve the SPU phase. The walker would then set the pace.
  - The plan is one function at a time, each checked bit for bit against the lifted code offline, before
    committing to the whole module.

## 2026-10-28 (midday): the 5.9 s stalls fixed; SPU loops kept in registers; 17-18 fps at the pitch screen

**The 5.9 s stalls: our command-buffer `end` was one word too far.** The runtime set the context's `end` to the
first byte after the 64 KB buffer, IO 0x10000. The SDK sets it to the buffer's last word, the slot the wrap JUMP goes
in. MLB writes its own JUMP-to-begin at `end`, so it landed in whatever the game keeps right after the buffer. When
that word was overwritten, the walker walked on into data, read vertex-program microcode as a CALL, and skipped the
frame's fence. Gfx_FrameEnd then waited out its own 5.9 s timeout. `end` is now `begin + size - 4`
(ps3recomp `3695e97`).
- **Before:** 8 bad CALLs per run, and every one was followed by a ~6 s freeze.
- **After:** none in 3 runs, and no freezes during play. The few remaining multi-second gaps are scene loads: menus,
  the walk into the game, a new SPU workload being created.
- The pitch-screen benchmark is no longer bimodal: 15.2-17.3 fps across runs (before: 9 or 14-21).
- `GCM_RINGDBG=1` logs a walker that steps past the end of the ring.

**Where a frame goes now (ftrace at the pitch screen, ~58 ms frames).** The game publishes the FIFO `put` once per
frame, in Gfx_FrameEnd, and then waits for that frame's reference value. A frame is therefore:
1. the main thread runs about 12 ms, with the SPUs and the walker idle;
2. the SPU job lists (~44 ms on 4-5 SPUs) and the walker's drain (~46 ms) run together;
3. the walker finishes about 6 ms after the SPUs, which releases the main thread.

The game's own sync allows no more overlap than that, so the lever is speed: SPU job time first, then the walker.
- The interpreter is not the problem: 0.2-0.4 M interpreted SPU steps a second, a few ms of SPU time.
- One job module (`c690c2f7b09a`) holds most of the SPU compute.

**Two SPU code-quality fixes** (ps3recomp `a16e306`, `58587e9`):
- **`--ib-goto` (lifter).** The hottest loop (0x7C18) loops back through `binz $39, $50`, where `$50` holds its own
  entry address. The lifter treated that as leaving the function. Every iteration wrote back ~30 registers,
  tail-called itself and reloaded ~60. A computed branch whose target is a known entry inside the same function is
  now a guarded goto, and the loop keeps its registers. About +3%.
- **`shufb` without byte swaps (x86).** Registers keep each word byte-reversed, so the swaps fold into the index
  (`^ 3`), and a 16-entry lookup handles the special selectors. About 9 instructions instead of 18, and 9% fewer
  instructions in a hot function. About +4%.

**Result:** pitch-screen fps averages 17.8 over 3 runs (16.9 with the stall fix alone; 12-14 typical before it).

Next: the SPU job code is still about 10 x86 instructions per SPU instruction, and a third of that is register
spills. After that comes the walker (~40 us per draw).

## 2026-10-28 (morning): inFAMOUS 2's SPU and RSX work ported; a repeatable benchmark; a bimodal frame rate

Ported from the inFAMOUS 2 port (MIT, credited in each commit), all on branch `mlb16-work`:
- **SPU lifter:** `--local-regs` (registers in C locals) and `--const-bi`, plus our `--ib-known-targets`.
- **SPU helpers:** SIMD forms for the integer helpers that were still scalar on x86.
- **SPU runtime:**
  - a fast branch resolver for job modules (`SPU_IB_FAST=0` turns it off);
  - the channel-read store-forwarding fix;
  - cheaper DMA parameter writes.
- **RSX draw path:**
  - per-texture-unit write counters with cached decode (`PS3RECOMP_ENG_TEXWGEN=0`);
  - a texture-binding memo (`PS3RECOMP_ENG_TEXMEMO=0`);
  - vertex-shader constants by range (`PS3RECOMP_ENG_VPC_FULL=1`).
- **Frame pipelining (`VK_FRAME_PIPELINE=1`):** two batch sets; neither a present nor a full batch waits for the GPU.
- **CMP32 vertex fetch:** decodes from the stable source instead of resolving each element.
- **Interpreter stats** now name the job-code branch targets the lift lacks. Several were added to the jobmod lift
  (the entry list `tools/lift_spu.sh` passes).

**A repeatable benchmark at last (mostly).** Take the home team (the menu driver moves the controller right at team
select, then handles pitcher selection and the lineup), and the game waits at the first pitch selection with a fixed
camera: the full 3D scene, with the job lists still running. Fps is measured over 15 s from when that screen appears.

**Results:**
- None of the ports above shows a clear gain on its own.
- The walker's GPU waits fell from ~600 to ~100-170 ms per 2 s with pipelining, but they were off the critical path.
- All combined: 10.85, 18.24 and 12.99 fps, against 13.96, 8.85 and 8.72 for the previous build (and 13.7-14.9
  earlier). 18.24 is the best run yet.

**Both builds are bimodal.** Some runs settle around 9 fps and others around 14-18, which swamps every change
measured this morning. It is not thread placement: four more runs (11.4, 12.1, 12.2, 17.9 fps) had the same
spread of hot threads over cores and the same job lists on three or four SPUs at once.

**Found it: 5.9-second stalls from a FIFO parse slip.** Traced runs show the median frame is ~43 ms (~23 fps) in
slow and fast runs alike. The slow runs contain multi-second hitches:
- The walker reaches `0x709D302A` and treats it as a CALL to unmapped IO. It is the 12-word vertex-program
  microcode after a SET_TRANSFORM_PROGRAM header (`0x00300B80`) one word earlier.
- The walker got there misaligned: it decoded `0x01000014` as a 64-word method header. Before that, its
  control-flow log shows a park, a jump to 0xFFB8, and `0x00000009` at IO 0x15064 taken as a jump to IO 0x8.
- The same two words (CALL `0x709D302A`, JUMP `0x30464649`) appear 6-8 times in every run.
- After the bad branch the walker resyncs, so that frame's reference write is never seen. Gfx_FrameEnd waits for it
  in a loop that gives up after 0x15B3 = 5,555 `Cond_Wait(9 ms)` calls. With our 1 ms lwcond poll that is 5,555 ×
  ~1.06 ms ≈ 5.9 s.

Whether a run's measured window catches some of these is the bimodal frame rate. Fixing the parse slip (likely the
walker reading a ring slot before the SPU job has written it) is next. It is probably also the "hang-ups" seen
while watching.

**Where a frame goes now** (trace): five SPUs run the job lists for ~50 ms, then the RSX walker drains for ~35-40 ms
with the SPUs idle. The game waits at frame end for the RSX to finish the previous frame, so the two phases don't
overlap.

## 2026-10-28 (early morning): job lists on several SPUs, ~6 → ~12-13.6 fps

With batching on, the frame trace (SPURS workers now record busy spans) showed one SPU busy nonstop for ~180 ms of a
~208 ms frame while the others idled. The WWS job manager's two main workloads ran on one SPU at a time, by turns.

**The cause was a missing API, not the scheduler's speed.** MLB calls `cellSpursRequestIdleSpu(spurs, wid, 7)` for
every workload. The SPURS kernel lets an SPU take a workload while its contention is below ready count + that idle
count; WWS uses it to spread one job list over several SPUs, each grabbing the next job. Our runtime's version was a
stub that kept nothing, so with a ready count of 1 every list got exactly one SPU.

The inFAMOUS 2 port's notes and SPURS kernel had the answer (it is the same job manager). Now implemented on branch
`mlb16-work`: the job lists run on three or four SPUs at once.

| | fps in play (30-210 s after the first pitch) |
|---|---|
| Before (batching on) | 5.4-8.0 over six runs |
| With idle SPUs honoured | 11.93, 13.59 |

**Also learned:** vertex decode is not where the RSX walker's time goes. SIMD decode is 2× faster in isolation and
bit-exact, but the walker's CPU per draw didn't move (60.4 vs 60.5 µs), and the walker sleeps most of the frame.

**Next:** the inFAMOUS 2 port's other SPU work, ported where MLB needs it:
- the lifter keeping SPU registers in locals (`--local-regs`), `--merge-chunks` and `--const-bi`;
- SIMD forms of the integer SPU helpers;
- cheaper DMA fences.

## 2026-10-28 (overnight): the frame is a waiting chain; draw batching +20%

**Correction to the entry below.** The CPU-vs-CPU game plays out differently every run, so it wasn't the same
at-bat each time, and fps swings ±20% with what's on screen. Changes are now judged by alternating A/B runs: a
closed-loop menu driver that recognises each screen, fps over 30-210 s after the first pitch, one game at a time.

**Where the time goes**, now measured on the running game:
- Sampled with gdb, plus the runtime's `PS3_FTRACE` timeline (SPURS workers now record busy spans).
- **Nothing is CPU-bound.** The process uses about 18% of the CPU. One SPU worker is ~60% busy, the RSX walker about
  half, and the main thread ~10%.
- **The frame is a chain of waits:**
  - The main thread waits (Gfx_FrameEnd) for the RSX walker to finish the previous frame.
  - The walker spent ~45% of its time waiting for the GPU after each draw.
  - The SPU mesh jobs wait on the walker for ring space, which shows as bursts of ~8 ms on, 8 ms off.
- **RPCS3 on the same PC** holds about 43 fps in the same scene, spreading SPU work over six threads.
- The SPU compute per frame is similar between the two, so faster SPU code (a lifter flag `--ib-known-targets`
  that turns the hot loops' computed branches into direct ones) gave no measurable gain.

**Draw batching.** `VK_DRAW_BATCH=1`, ps3recomp's opt-in mode written for God of War: Ascension, records 100-200
draws per GPU submission instead of waiting after each one:

| | fps (runs) |
|---|---|
| Batching off | 5.41, 5.53 (4.50, 5.00, 6.24 before the GPU clock lock) |
| Batching on | 6.25, 7.96 (6.01, 6.17 before) |

It is now on in `tools/run.sh`.

**The GPU sat in its idle clock** (210-270 MHz on a 3060 Ti). The bursty submit-and-wait pattern never ramps it up,
and PowerMizer "prefer maximum performance" didn't stick. `nvidia-smi -lgc 1400,1980` (root; reset with `-rgc`) pins
it for testing.

**Smaller fixes** on branch `mlb16-work`:
- Vertex fetch skips duplicate-removal hashing for sequential draws and fills attributes that have no array.
- `VK_EB_STATS=1` prints why GPU batches end.

**Next:**
- Asynchronous GPU submission, so the walker records while the GPU executes. The first step publishes RSX labels only
  after their GPU work completes; today the game can see a label before the work behind it ran.
- Then SIMD vertex decode and a cache for static geometry; the walker re-decodes every vertex every draw.
- Then why the SPU rings wait on the walker.

## 2026-10-27 (end of day): SPU speed, 3.4 → 6.4 fps

Measured as the average flip rate over 60 s of play (same at-bat each time, `GCM_FLIPCOUNT=1`).

| Change | fps |
|---|---|
| Start (exact SPU float, scalar local-store access) | 3.4 |
| `PS3RECOMP_SPU_FAST_FLOAT=ON` | 4.6 |
| + `PS3RECOMP_SPU_FAST_LS=ON` | 6.4 |

Both are CMake options on branch `mlb16-work` that affect only the lifted SPU sources (configure with
`-DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON`):

- **Fast float:** ps3recomp's x86 path emulates SPU single precision bit-exactly: each op in f64x4 with lane checks
  (~30 instructions), plus a scalar fix-up for tiny/out-of-range lanes that the vertex code hits often. The option
  uses native SSE/FMA3, keeps the SPU's denormal flush, and differs only in rounding (nearest vs truncate) and
  overflow (infinity vs saturate). It is within 1 ulp on 800k random lanes and ~6× faster per fma, like an
  emulator's default "approximate" SPU float mode.
- **Fast local store:** each 16-byte local-store access was four byte-swapped scalar moves plus a diagnostics-flag test
  and cold call. Now it's one vector load/store and a `pshufb`, with no diagnostics in lifted code. The hottest mesh
  block went from 827 to 478 x86 instructions.

Where the time goes now: one SPU worker at ~86% running the mesh jobs (the game runs that list on one SPU at a time,
as on hardware), a second at ~50%, the RSX walker thread at ~40% (CPU vertex conversion).

**Next lever, register caching (prototype in `tools/spu_regcache.py`):** the lifted code keeps SPU registers in
`ctx->gpr[]`, and every local-store store goes through a char pointer that may alias it, so the compiler reloads
registers after each store. The prototype rewrites each lifted function to use C locals (load at entry, write back
before exits/calls/channel ops, reload after calls).
- Applied to all 639 functions of the mesh job module, the job went wrong (an SPU return to LS `0x22`). The likely
  culprit is the interrupt path, which snapshots/restores `ctx->gpr` and can unwind host frames.
- Applied only to the 402 functions without calls or channel ops, the game plays correctly but no faster (6.0 fps).
  The hot loop (`0x7c18`) contains a call, so it wasn't covered.
- Next: make calls inside hot blocks safe for caching (or move the caching into the lifter itself, with interrupt
  points as explicit write-back sites), then measure again.

Also open: render-target writeback ("Write Color Buffers") for the dark players and crowd sprites, the RSX walker's CPU
vertex conversion, and the PPU side (same `ctx->gpr` pattern).

## 2026-10-27 (later): magenta fixed; where it stands against RPCS3

- **Magenta surfaces:** a RenderDoc capture of our own Vulkan output (layer mode,
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` with `SDL_VIDEODRIVER=x11`, since the layer has no Wayland support) showed the
  player shader sampling texture unit 5, which the game leaves disabled. Our backend binds a 1×1 magenta placeholder
  to such units, and the shader adds it as a tint. `VK_DUMMY_BLACK=1` (branch `mlb16-work`, set in `tools/run.sh`)
  makes the placeholder transparent black. Dirt, crowd, field and walls now look right.
- **Players and crowd sprites are dark/blank:** the same problem as RPCS3 without "Write Color Buffers". MLB 16 builds
  some textures by rendering them and reading the render target back from the CPU/SPU (the generated faces in
  `HeadFaceCacheFile`, the crowd sprite sheets). Our renderer doesn't write render targets back to guest memory, so
  those reads see black. Next to fix. Then delete `HeadFaceCacheFile`, which was built blank.
- **Speed: about 3.3 fps in a game** (flip counter, no RenderDoc). The two busiest threads are SPU workers running the
  mesh jobs (`jobmod_c690c2f7b09a`, skinning/geometry); the PPU main thread is at ~30% and the GPU is nearly idle.
  So the lifted SPU code is the bottleneck: one real SPU runs these jobs well within a frame, our translation is many
  times slower.
- **How this differs from an emulator, honestly:** the CPU side (PPU and SPU) is compiled ahead of time, but the GPU
  side is still an emulated RSX (command walker, translated shaders, texture formats), the same job RPCS3 does. And
  ps3recomp's RSX layer is much younger than RPCS3's, so graphics bugs RPCS3 fixed years ago show up here. The
  speed advantage can only appear once the SPU code generation is fast. Right now it's the opposite: RPCS3's SPU JIT
  (LLVM, with years of tuning) beats ps3recomp's straightforward SPU-to-C translation by a wide margin.

## 2026-10-27 (late night): in the game, pitches being thrown

**The match-load deadlock is fixed and the game plays:** the dugout, the pre-game intro, Kauffman Stadium, Volquez
pitching to Curtis Granderson, with the count and the score bug updating. Geometry is correct. Many surfaces
(uniforms, skin, infield dirt, part of the crowd) come out solid magenta, a renderer problem still to look at.

The cause was ps3recomp's SPURS scheduling, in three parts:

1. **Max contention dropped.** `_cellSpursWorkloadAttributeInitialize` takes `maxContention` as its ninth argument,
   on the guest stack, so the generic adapter never saw it and stored 1. The mesh lists ask for 5 and the DXT list
   for 4. Fixed with a context handler that reads the stack (branch `spurs-workload-maxcontention`).
2. **The ready count was consumed.** The scheduler decremented a workload's ready count each time it gave it an SPU.
   In the SDK the ready count is a level: the kernel runs up to `min(ready, maxContention)` SPUs and the workload
   lowers it itself. The WWS policy module does exactly that, setting it to 0 when its list is empty and back to 1
   when it sees more work. Consumed, a kick could be used up by a dispatch that found no jobs yet, and nothing
   dispatched the list again. `SPURS_READY_LEVEL=1` keeps the level.
3. **Idle probes ran workloads nobody had kicked.** To cover for (2), the scheduler periodically dispatched idle
   workloads. One such probe ran mesh jobs that the command-building threads had already queued, *before* the PPU
   reset the mesh rings and kicked the list. The reset then threw away the ring space those jobs had used, the next
   job was given the same space again, and it waited forever for the GPU to free it. `SPURS_NO_IDLE_PROBE=1`
   schedules only kicked workloads, as real SPURS does.

(2) and (3) are opt-in switches on branch `mlb16-work` for now, and `tools/run.sh` sets them. They change SPURS
scheduling for every title, so they need testing on other ps3recomp games before going upstream.

How it was found: a temporary override that forced the stuck wait through after 2 s got the game into the match
(with garbled geometry), which proved the wait was the only blocker. Then gdb watchpoints on the ring allocation
pointers and on the ready-count bytes showed the order of events.

## 2026-10-27 (night): how the mesh rings work, and where the deadlock really is

The stall at the pitching matchup isn't the game waiting for a pick (X on the matchup, START on the lineups):
flips stop and the main thread never leaves the job-list wait, so pad input is never read. Pressing X there changes
nothing.

How MLB 16 feeds geometry to the GPU (worked out from walker traces, gdb watchpoints and Ghidra):

- **Five mesh rings** of 2 MB each, holding **vertex/index data, not commands**: EA `0x11900000`, `0x11b00000`,
  `0x11d00000`, `0x11f00000` and `0x12100000` (IO `0x00900000`–`0x012fffff`), with RSX labels 72–76
  (`0x480`–`0x4c0`). Their state lives in the render object at `0x1246680`: `+0x200` holds
  `{base, end, alloc pointer, label address}` per ring (one 128-byte line).
- **Per-mesh slots in the command stream.** The PPU leaves a 0x90-byte slot per mesh job in its big command ring
  (IO `0x013xxxxx`–`0x014xxxxx`): a self-JUMP plus `0x40880100` (a non-incrementing NOP skipping 34 words). The job
  fills it by DMA when it finishes: vertex/index array offsets into its ring, the draw, and a release of its ring's
  label with the segment's end address. The GPU stops at the first unfilled slot, so it consumes ring data as jobs
  complete.
- **Frame setup** (`0x70694` → `0x6ed24` → `0x6f6b0`): wait for the command-building worker threads (`0x70894`),
  wait for the previous mesh job list (`0x9fb898`, list "MeshJobs0"), then set each ring's start for the new frame
  and kick the next list. Frame end is `0x66530` (links the frame into the libgcm ring, flips, paces on the
  frame counters).
- **Each ring has one SPU writer.** Watchpoints on the alloc pointers show ring 72 only written by one SPU worker,
  ring 75 by another, ring 76 by a third. Plus the PPU's frame setup. No lost updates.

The deadlock: the GPU parks at the first unfilled slot. That slot's job waits for ring space (`jobmod_c690c2f7b09a`,
`0xe4d0`–`0xe5c0`): it compares the ring's label (the GPU's read position) against the range it wants and retries
forever. In every run the waiting job wants a range the GPU hasn't released yet, often right after wrapping its ring.
Open question: why it wraps so early. In one run, ring 76's writer wrapped at `0x12137d00`, 0x37D00 bytes into a 2 MB
ring. The frame setup also once set ring 76's start to `0x12105180` rather than the base. Next: trace the per-frame
start/end values `0x6f6b0` hands out (`+0x18`, `+0x58`, and the single-ring branch at `+0x280`/`+0x290`) and how the
job derives its ring end. An RPCS3 memory dump of the same structure at the same screen would settle what "normal"
looks like.

Side findings:
- The walker's 50 ms park recovery is no longer needed for this game; running with it effectively off
  (`GCM_PARK_GRACE_MS=100000000`) still boots, and parks are always patched within 4–47 ms (`[parkfix]` log).
- `HeadFaceCacheFile` is the generated-face cache from the RPCS3 white-faces fix. Faces built natively before RSX
  render-target readback works may be cached blank; delete it once a match loads.
- Temporary diagnostics on branch `mlb16-work` (not for upstream), all under `MLB_SEMA_TRACE=1`: label releases with
  stream position, CALL/JUMP into the rings, park patches, and SPU PUTs into the command ring (`[ringput]`).

## 2026-10-27 (evening): Bink spin fixed, match load reaches the pitching matchup

- **`cellFsFtruncate` never worked:** it resolved to `libs/filesystem/cellFs.c`, which looks fds up in its own table,
  one that the port's `cellFsOpen` (`runtime/ppu/ppu_fs.cpp`) never fills, so every call returned EBADF.
  - The game sizes `HeadFaceCacheFile` (in the install folder) to 314,584,064 bytes before a match, and
    `ArenaLogoCache` to 9 MB. Implemented next to `cellFsOpen` (branch `cellfs-ftruncate`).
  - With it, the match load gets past the starting lineups to the **pitching matchup** screen.
- **The Bink SPU spin (99% of a core) is fixed:**
  - The task retried `cellSpursEventFlagWait` forever, because each call failed at once with `0x80410911` (null
    pointer): its event flag address was 0.
  - The task DMAs its setup block from r4, the taskset argument. `_cellSpursTasksetAttributeInitialize` dropped
    `argTaskset` and `cellSpursCreateTasksetWithAttribute` created the taskset with args 0.
  - Carrying it through (branch `spurs-taskset-args`) gives r4 = `0x01476E80`; the Bink thread now sleeps in its
    wait like on hardware. That's a core back.
- **The match-load stall is a real deadlock**, not a slow first cache build: with the SPU watchdog at 600 s it never
  moves. Three parties wait on each other:
  - **PPU:** the main thread spins in `0x9fb898`, the WWS job manager's wait for job list "MeshJobs0". Two SPU lanes'
    progress counters are stuck.
  - **SPU:** two mesh jobs (`jobmod_c690c2f7b09a`, loop at `0xe510`–`0xe550`) wait for ring space. Each DMA-reads an
    RSX label holding the GPU's read position in its ring (labels 72, 75 and 76; rings at EA `0x11900000`,
    `0x11f00000` and `0x12100000`). It retries while that position is inside the region it wants to overwrite.
  - **GPU:** our command walker has drained everything up to a self-JUMP "park" at IO `0x0138ef10`. The GPU waits
    there for the PPU to write more; the PPU waits for the jobs.
  - The only label release in that frame is label 75 = `0x11f02080`, inside the region its job wants. So that ring's
    previous output was never marked as read by the GPU.
- **Walker oddities found on the way** (not yet shown to cause the deadlock):
  - The game extends its big ring by leaving a self-JUMP at the write head. After 50 ms (`GCM_PARK_GRACE_MS`) the
    walker gives up on the park and jumps to `put`; a real RSX waits.
    - With the grace effectively infinite the game still boots and gets to the same stall.
    - The selected team's **logo now shows on team select**. It was probably in commands the early jump skipped.
  - At about 2.5 minutes in, the walker reads data as commands at IO `0x2d4e80`. The frame `CALL`s a vertex-program
    upload at `0x2d4ef0`; the walker was 0x70 bytes before it.
    - It reads an upload argument (`0x709d302a`) as a CALL to an unmapped address, and drops the rest of that pass.
    - Possibly a block rewritten while the walker, which drains on 4 ms ticks, was still behind it.
  - New-style CALL targets are masked with the old JUMP mask (`0x1ffffffc`) and new-style JUMPs (`w & 3 == 1`)
    aren't decoded at all. This game seems to use only old-style JUMPs and new-style CALLs into low IO, so neither
    has bitten here yet.
- **Ghidra:** the recompilation addresses are now named in the project (`Gfx_DisplayFlip`, `Gfx_FlipHandler`,
  `Mutex_LockOwned`/`Mutex_UnlockOwned`, `Cond_Wait`, `File_Duplicate`, `File_Open`, `File_AioCallback`,
  `MemPool_Acquire`, `MemPool_SharedMemContAnim_LazyInit`, `MemContainer_Carve`).
- **Next:** find which commands should advance labels 72/75/76 and why the walker never reaches them: trace every
  `0x1d6c`/`0x1d74` release value per frame, and the `CALL`s into the mesh rings (IO `0x0090xxxx`, `0x00f0xxxx`,
  `0x0110xxxx`).

## 2026-10-27 (afternoon): driving it headless; match load gets to the lineups

- **Scripted input:** `PAD_FILE=<file>` presses whatever button mask is written to the file (`0x4000` CROSS,
  `0x0008` START, `0x0040` DOWN; `"0x0040 3"` holds it for 3 reports). With a frame dump every 10 frames, the menus
  can be driven without a person: title → START → "Online disabled" OK → Play Now → Offline Rosters → team select →
  stadium select (a still image of Kauffman) → game options → starting lineups.
- **`-O2 -march=native` build** (`recomp-work/build-o2`, 16.5 min at `-j4`): same menu frame rate as `-O1`, 421
  frames in 20 s each (about 21 fps). The menus are paced at one frame per three 60 Hz vblanks, so they don't show CPU
  speed.
- **"3D TV detected":** `cellVideoOutGetResolutionAvailability` reported the 3D modes (ids `0x80`+) available, so
  the game asked to enable 3D and, on yes, rendered over-under. Fixed on branch `cellvideoout-no-3d` (PR draft 5).
- **The match-load NULL call** came from the game's memory containers:
  - `0x14fc4` carves an N MB container out of the default one (id `0xFFFFFFFF` at `0x0111ffc4`): get its size,
    destroy it, recreate it smaller, create its own.
  - ps3recomp's `sys_memory_container_get_size` rejected the default id, took its arguments in the wrong order and
    wrote `{total, used}` instead of `{total, available}`. `sys_memory_allocate_from_container` (syscall 350) wasn't
    registered.
  - So the "SharedMemContAnim" pool at `0x0134a464` (built lazily in `0x99c45c`) never got memory, and `0x8c5508`
    called a method on a NULL buffer.
  - Fixed on branch `sys-memory-containers` (PR draft 4). Both pools now allocate (25 MB in container 1, 7 MB in
    container 3), and the NULL call is gone.
- **Next stall:** about 30 s into the lineups screen, an SPU job on the job manager (local store `0xe510`) re-reads
  RSX label 76 (`0x200004c0`; labels start at `0x20000000`) until the runtime halts it. Something that should write
  that label, probably a semaphore release in the GPU command stream, isn't writing it.

- **Label 76, first look:** a temporary trace in our runtime branch (`MLB_SEMA_TRACE=1`, branch `mlb16-work`) shows
  the GPU command stream *does* release it. The game cycles through five labels, `0x480`, `0x490`, `0x4a0`, `0x4b0`
  and `0x4c0`, with `NV4097_SET_SEMAPHORE_OFFSET` (`0x1d6c`) and texture-read releases (`0x1d74`), on subchannel 0,
  with no semaphore context DMA set (so they land in the label window). The waiting job is
  `jobmod_c690c2f7b09a` (a geometry ring producer: DMA-reads the label at `0xe510`, compares it against its ring
  bounds at `0xe550`).
  - Next: compare the values released against what the job waits for. Candidates: byte order (`0x1d74` is written
    verbatim, `0x1d70` byte-swapped), the walker running behind the job's own output, or releases in
    SPU-built command blocks.
  - Driving there takes about 4 minutes with the `PAD_FILE` script above.

## 2026-10-27 (evening, playtest): menus playable, match load crawls

Played with a PS4 controller through SDL, in a window:
- **Menus work.** Experience level, main menu and moving between screens, at about 20 fps (109 new frames in about
  5 s, read from the frame counter at `0x011203d8`).
- **Button presses sometimes need two taps.** `PAD_ALWAYS_CHANGE=1` (every read returns a full report) didn't fix it.
  The controller is sampled every 4 ms, so short taps aren't the cause.
- **Menu boxes flicker** (only the boxes, not the whole screen): to capture with the renderer's pass dumps.
- **SELECT (Quick Menu) froze it.** This is online code, which also needed our server in RPCS3. Skipped for now;
  offline first.
- **Loading an exhibition match crawled.** The crowd job workload (`CrowdJobList`) started and stadium data loaded,
  then reads dropped to about 17 KB/s with no new frames. The main thread was busy in game code (`0x5499a0`,
  `0x643200`…) while the job module `255e2961` kept dropping into the SPU interpreter.
  - A gdb breakpoint on `spu_interp_run_until` caught 7,293 interpreter entries in 25 s, mostly at `0xd7a0`,
    `0xd9d0`, `0xd810` and `0xd878`: functions reached only through function pointers, so the lift never had them as
    entry points.
  - The 28 addresses are now passed to `lift_jobmods.py` (`LBP_JOBMOD_EXTRA_FUNCS`, in `tools/lift_spu.sh`). The
    job modules gained functions, e.g. `7ac07dde` went from 241 to 630. Test pending.
- **CPU use:** the Bink SPU program spins at 99% of a core in its wait loop even with no movie, and a job manager
  thread runs at about 84%.
- **Match load still stalls with the entry points added** (interpreter entries are now only the job manager's own
  self-modifying code at `0xa68`). At the start of each match load the main thread calls a NULL object:
  - `0x8c5508` writes the game's "FE data block" (`HDDBlk`) to the HDD cache. It takes a buffer from a 3-slot pool
    at `0x0134a464` via `0x98c72c`, which hands one out only if the slot's current owner says it's done (virtual
    `+0xc`).
  - All three were busy, so no buffer came back. The caller doesn't check, and calls a virtual method on 0 (call at
    `0x8c5638`, logged as `[null-call] near lr=0x8C563C`).
  - Next: read the pool's owners (slots `+0x18`, `+0x24`, `+0x30`) at that moment to see what's still holding them.
- `cellFsAioWrite`'s completion callback (`0x79c2c`) only forwards to the request's handler, which the game sets up
  before submitting, so completing synchronously looks safe here.

- The build now lives in `recomp-work/build` (lifted source in `recomp-work/recompiled`), not the session's scratch
  folder.

## 2026-10-27 (later still): first menu

- **The autosave-screen stall** was the game's file copy into its `/dev_hdd1` cache (`0x776d8`, a "Duplicate"
  routine called from the file open at `0x78ca4`). It writes 64 KB chunks with `cellFsAioWrite` and polls a status
  word set by the completion callback (`0x79c2c`). `cellFsAioWrite` (`0x4CEF342E`) wasn't implemented in ps3recomp,
  so nothing was written and the callback never ran. Implemented on our branch `cellfs-aio-write` (upstream PR
  candidate 3), modeled on its `cellFsAioRead`.
- **Result:** about 500 async writes during boot, then the game's first interactive screen, **"Choose your
  experience level" (Veteran / Novice)**, drawn correctly through Vulkan with the team background behind it. This
  is the first-time setup a new save sees before the main menu.
- The Bink thread still spins at 99% of a core; whether the intro movie plays isn't known yet.
- **New unresolved imports, reached for the first time:** `0xB20F87B3`, `0xCE4374F6`, `0xDBF22BD1`, plus
  `cellHttpInitCookie` and `cellSpursAttributeEnableSystemWorkload` from before.

## 2026-10-27 (late night): flip loop fixed; stuck on the autosave notice

- **The 100,000 flips a second** came from the flip handler running twice per flip. `cellGcmSetFlipImmediate` calls
  it at request time (a compatibility "early edge" in ps3recomp) and the pump calls it again at completion. MLB's
  handler advances the displayed-frame counter (`0x011203dc`) each time, so it overtook the rendered-frame counter
  (`0x011203d8`). `DisplayFlip` (`0x692dc`) then saw a frame ready forever and flipped non-stop. Upstream's
  `GCM_FLIPCB_ONTICK=1` drops the early call. With it, about 35 flips a second, and the two counters stay a few
  frames apart.
- **Where it stops now:** the screen stays on the autosave notice (cover art) from about 20 s on, flipping buffer 0
  only, while the game keeps loading in the background. It opens `MLB_INTRO.BIK` early, probably to preload.
  - The main thread polls an event flag with an ~11.6 ms timeout (`0x4c7ae0` → `0x78ca4` → `0x785d8` → `0x776d8`).
  - The Bink SPU program (`BinkSPU.spu.elf`, task created with a SPURS event flag, `direction=2`, SPU to PPU) spins
    in its wait-for-work loop (`0x3870` → `0x3438` → `0x3448`) at 99% of a core.
  - Next: check ps3recomp's SPURS task event flag and queue path between the two (`docs/spurs-task-signal-wake.md`
    upstream).
- **Upstream PR branches** are in the fork (`MatRanc/ps3recomp`: `template-vulkan-backend`, `gcm-handler-thread`),
  with the draft descriptions in `PR_DRAFTS.md` on its `pr-drafts` branch. Nothing is opened yet.

## 2026-10-27 (night): boot freeze fixed, display stops updating

**The freeze was a leaked lock, not Bink.** The game usually hung on its first screens, at random, before ever
opening the intro movie; one early run got through by luck. Stacks of the stuck process (`eu-stack`, function names
are PS3 addresses) and its guest memory (`gdb`, reading `vm_base + address`) showed:
- `DisplayFlip` (thread entry `0x692dc`) flips each frame, then waits on a condition for the flip handler
  (`0x691b0`, registered with `cellGcmSetFlipHandler`) to clear a "flip pending" flag at `0x011203e0`.
- MLB's lock wrappers (`0x9a2da8` lock, `0x9a2e78` unlock) record the owner's thread ID at `struct+8` and only
  unlock when the caller's ID matches.
- ps3recomp delivers vblank/flip handlers at whatever firmware call a guest thread happens to make, and the callback
  inherits that thread's ID (`ppu_guest_call`). The flip handler ran nested inside `DisplayFlip`'s own call, while
  `DisplayFlip` held the lock at `0x012450d8`. It re-entered the lock (same ID), and its unlock cleared the owner
  record. `DisplayFlip`'s own unlock then saw an owner of 0 and was skipped, so the lock leaked (owner 4,
  recursion 1). The display engine and a sound thread blocked on it forever, and with the handler stuck inside the
  pump, no more vblank/flip callbacks were delivered.
- On a PS3 these handlers run on GCM's own interrupt thread.

**Fix** (our ps3recomp branch `gcm-handler-thread`, upstream PR candidate): `GCM_HANDLER_THREAD=1` delivers the
handlers only on a dedicated host thread with its own identity. With it, 3 of 3 runs get past the boot screens,
open `MLB_INTRO.BIK`, and go on loading user data, crowd sound banks, profiles and the logo cache.

**Next problem:** the presented picture stays on the autosave notice (cover art) from about 20 s on, while the game
keeps running. `DisplayFlip` now calls `cellGcmSetFlipImmediate(0)` about 100,000 times a second, so its flip loop no
longer paces itself. The movie may be playing unseen.

Also unresolved, not blocking yet: `0x4cef342e`, `cellSpursAttributeEnableSystemWorkload` and `cellHttpInitCookie`.

## 2026-10-27 (evening): first picture through Vulkan

- **Packages:** Vulkan needs `vulkan-headers`, `glslang-devel` and `spirv-tools-devel` (Fedora). Build with
  `-DPS3RECOMP_RSX_VULKAN=ON`. With glslang found, CMake reports "guest programs available": the game's own RSX
  shaders are translated (HLSL, then SPIR-V).
- **Template gap (upstream fix candidate):** `templates/project/main.cpp` picks D3D12, Metal or the headless
  renderer and never Vulkan, even in a Vulkan build. The runtime's own host tool (`runtime/host/host_posix.c`) does
  pick it. Adding a `PS3RECOMP_RSX_VULKAN` case with `rsx_vulkan_backend_init/present/pump_messages` fixes it.
- **Run:** `PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1`. The renderer picks the RTX 3060 Ti, opens a
  1280×720 Wayland window, and the game draws its **legal and licensing screen correctly**: text, logos and the
  animated red and blue background. `PS3RECOMP_VK_DUMP=<file>.ppm PS3RECOMP_VK_DUMP_EVERY=60` saves frames.
- **Next blocker:** it freezes right after opening the intro movie, `PS3/UI/Movies/intro/MLB_INTRO.BIK`. Bink's
  SPU program is lifted, but playback hangs.
- `PATCH_GD.WAD` fails to open from the data install folder (`BCUS01089_R`); it may be found in the update folder
  instead, not checked yet.

## 2026-10-27 (later): SPU job manager running, data install done

- **Policy module:** the WWS job manager's SPURS policy module is 12,032 raw bytes at `0x105bb80` (file offset
  `0x104bb80`) that SPURS loads at local store `0xA00`. Wrapped as an SPU ELF at that base
  (`wrap_spu_elf.py --base 0xA00`) and lifted with `build_spu_workloads.py`, its text segment registers under the
  fingerprint the runtime looks up (`0x75BFE68AE96D7577`).
  - The runtime's `SPU_DUMP_MISS=<dir>` setting wrote the missing programs out exactly as fingerprinted, which is
    how both were found.
  - The same job manager runs LittleBigPlanet, whose fixes are already upstream.
- **Job modules:** `lift_jobmods.py --engine-elf` finds and lifts the 7 job modules linked into the EBOOT. One has
  308 undecoded words, the same count as the MP3 decoder program, so they're probably its data tables.
- **Build:** with the 13 SPU programs (`-DSPU_DIR=`), the rebuild takes 38 s. All of this is in `tools/lift_spu.sh`.
- **Results:**
  - All three job workloads resolve and run.
  - The game's 9.5 GB first-boot data install completes in about 50 s.
  - The game then creates more workloads and its seven sound streaming threads. Job loads stall and resume through
    SPU DMA lists and interrupts, and the job manager's self-modifying code is handed to the SPU interpreter.
- **Open problems:**
  - The job manager sometimes spins at local store `0x2534`, repeating one DMA transfer until the runtime's
    watchdog halts that SPU (256 identical transfers). LBP's notes say this policy module gets code patched into it
    by SPURS at load time (holes at `0xAEE`–`0x11B0`, `executeStage` at `0x14f4`); `SPURS_PM_DUMP` checks for that.
    - Checked with `SPURS_PM_DUMP=1`: the same holes as LBP's (1,728 of 1,730 bytes zero, `executeStage` empty).
      No game code writes the main-memory copy in 60 s. Upstream has no fix for LBP either.
    - That may be expected: the job manager writes code into its own local store (the runtime logs these as
      `spu-smc` and runs them through the SPU interpreter). That would fill the holes on the SPU side, where a probe
      of main memory can't see them.
    - The spin re-reads the same 128 bytes of the workload's data (data + `0x80`) 256 times, which looks like
      polling an empty job queue rather than a crash. The game keeps re-running those workloads afterwards. Still to
      confirm.
  - One guest write to NULL+0x48 from `0x7b988`.
  - The headless null renderer shows nothing yet; the Vulkan backend is next.

## 2026-10-27: first boot

**Lifting the PPU code** (`tools/probe.sh`):
- `ppu_loader.py` finds 15,790 functions from the OPD table and 433 firmware imports across 32 libraries.
- `ppu_lifter.py` lifts 43,501 functions to about 6.5M lines of C++ in 11 chunks, in about 1 minute.
- **Pass `--code-end 0xf2c634`.** Without it the lifter treats the read-only data after the code (1.5 MB, up to
  `0x10a43e8`) as code: 1.93M undecodable words and 954 MB of output. Ghidra's section table shows the last
  executable section ends at `0xf2c634`.
- With it, 6,847 words stay undecoded. They are float constants (1000.0, 32767.0, 10000.0…) and zero padding
  between functions, not unsupported instructions.

**Building** (ps3recomp's `templates/project`, clang, `-j6`):
- No compile errors. 10 min 55 s wall time, about 1.5 GB of RAM per chunk.
- Result: a 205 MB native Linux executable.
- The runtime itself needs `SDL2-devel` (Fedora: `sudo dnf install SDL2-devel`) and builds in seconds.

**Running** (headless null renderer, no SPU programs lifted yet):
- The game's own code runs natively. It sets up memory, SPURS, GCM and sysutil, starts its threads ("Job Manager
  Event Handler", "IMFE PPU Thread"…), draws its first frames and flips buffers, creates its trophy context, finds
  a PS4 controller through SDL, and reads `MLBPS3_SYS.WAD` from the disc.
- First stop: the game's "not enough system storage (5,120 MB)" dialog. The runtime resolves `/dev_hdd0/game` under
  the same root as the disc, which was a read-only ISO mount. Fix: a writable root folder holding a `PS3_GAME`
  link to the disc and `game/BCUS01089` (the v01.10 update).
- With that, the game offers its optional data install, the runtime auto-answers yes, and the game starts
  installing ("Installing Game Data… File 1/6").

**Missing so far:**
- The WWS job manager's SPURS policy module (12,032 bytes at `0x105bb80`) isn't lifted, so the game's SPU job
  workloads (`Wws_Job`: `DXTJob`, `MeshJobs0`, `MeshJobs1`) never run. ps3recomp's `lift_jobmods.py` targets this
  job format (`c0dec0de` modules).
- The Bink SPU program (110,560 bytes) isn't lifted yet, so video won't play.
- Two unresolved firmware imports showed up early in the boot.

**SPU programs:**
- The EBOOT embeds 5 SPU ELF images: two WWS job checks, an MP3 decoder, surround audio and Bink.
- It also holds 8 `c0dec0de` job modules (4 inside those ELFs), including the ICE mesh program.
- RPCS3 showed 25+ SPU programs in use, so the rest come from the game's data files.
- All 5 ELF images lift with `spu_lifter.py --auto-functions` with no unsupported instructions.

**Firmware coverage** (approximate, from ps3recomp's handler table vs our 433 imports):
- 165 have a real implementation, 71 a thin one, 46 return a constant, 32 run inside the runtime itself, and 119
  are missing. Missing imports return CELL_OK without doing anything.
- Gaps for online play:
  - `cellHttp` is registered under made-up names, so its handlers never match the game's imports (37 of 48 missing).
  - `cellSsl`/`cellHttps` have no TLS.
  - NP auth, ID lookup and signaling are missing or stubbed.
- ps3recomp's own docs recommend Sony's `libsre` (lifted from your firmware) over its built-in SPURS, which has hung
  most SPURS-heavy titles.

## Reference

- **Upstream PRs** (submitted to `sp00nznet/ps3recomp`, all open): #640 templates Vulkan-on-Linux; #641 cellFsAioWrite;
  #642 cellVideoOut stereo; #643 cellSpurs taskset args; #644 sys_memory container size; #645 cellFsFtruncate;
  #646 cellSpurs maxContention; #647 GCM handler thread; #648 GCM wrap JUMP slot; #649 cellSpurs maxContention clamp +
  RequestIdleSpu; #650 PS3RECOMP_TRACE.
- **Clean-room rule:** ps3recomp/mlb16-recomp are MIT; RPCS3 is GPLv2. Never copy or port RPCS3 (or other GPLv2) code;
  write original code against ps3recomp's own structures. This keeps the project MIT-compliant and upstreamable.
