# What the NHL Legacy port teaches this one (performance audit)

Date: 2026-11-03. Analysis only: no game runs, no builds, no code changes in either tree.
Sources: the NHL Legacy native port at
`/tmp/claude-1000/-home-m/2e3f6ceb-7458-4679-bca1-76ef8ef8c2df/scratchpad/nhl/repo`
(MIT, `docs/notes/LOCAL_PATCHES.md`, `NEXT_STEPS.md`, `SPU_RECON.md`, `README.md`,
`nhl/port/nhl_edge_native.c`, `edge/edge_ref.{c,h}`), and this repo's
`docs/meter-pacing.md`, `meter-scene-baseline.md`, `frame-pacing.md`,
`readback-cost.md`, `tick-and-resync.md`, `ppu-critical-path.md`,
`walker-spu-wait.md`, `walker-wake.md`, `c690-offload-decision.md`,
`c690_function_map.md`, `targets-and-backlog.md`.

NHL's numbers were measured on Windows/D3D12 (i9-9700X and a Steam Deck via
Proton). They are evidence about mechanisms, not transferable milliseconds:
our renderer is Vulkan, our CPU here is a Ryzen 5 2600X (Zen+, AVX2/FMA, **no
AVX-512**), and our title is a different EA Sports build. Every "ms at the
meter" below is an estimate from our own profile until an A/B says otherwise.

## The shape of our meter, in one table

From `docs/meter-pacing.md`, `meter-scene-baseline.md`, `ppu-critical-path.md`,
`walker-spu-wait.md`, `readback-cost.md` (WCB=2 unless noted, 29-30 fps,
~34 ms frames, 12-60 ms spread):

| link | ms/frame | note |
|---|---:|---|
| RSX walker drain (its own CPU) | 25-27 | `engine 25.2`: tex 9.8, mesh 11.6, main ring 10.2, fetch 4.0, draw 4.75, pipe 1.9, parse 1.5, other 4.5 (overlapping blocks) |
| -- WCB readback inside tex/main | ~6.2 | guest-mandated NV3089 + mip page faults (`readback-cost.md`) |
| walker wait for producer (`put_wait`) | ~7.3 (WCB=2) / 11.8 (WCB=0 run) | PPU park patch 4.4, resume lag 4.6, c690 2.7, caught-up 2.0 |
| PPU main own work | 15-19 | p50 14.7 (WCB=0) / 18.8 (WCB=2); overlapped with the walker |
| PPU main block on ref cond | 19.7 | woken by the GCM user handler; the walker's work it cannot overlap |
| c690 cumulative, all lanes | 72-77 | 429-435 jobs/frame; 42 % of lane time; only 2.7 ms of walker wait |
| SPU lanes | 5 active, 58.8 % busy, lane 5 idle | no throughput wall; queueing 0.1 % |

So: **walker CPU first, producer/cadence second, PPU main third.** A patch
that only shortens the producer is worth ~0 at the meter unless it also
shrinks the walker drain or the PPU park-patch latency. This is the filter the
NHL list is passed through below.

## 1. Patch-by-patch

### 1.1 CPU / threads / sleeps / timers / scheduling

| NHL | What it does | Their measured gain | Ours | Applies to MLB? | Meter estimate |
|---|---|---|---|---|---|
| **#45** SPU idle cost | (a) per-host-thread `mfc_for` engine cache, avoiding a shared SRW lock on every MFC op; (b) GETLLAR back-off: an unchanged reservation-held line waits up to 1 ms on the channel CV (`SPU_LLAR_BACKOFF`, 32); (c) plain-128-byte-GET poll back-off with a per-thread content-hash table (`SPU_POLL_LSA/N/US`, 64/100); (d) `spu_overlay_note_get` rejects GETs outside the registered span; thread-local depth guard; (e) host-thread priority/affinity knobs. | Movie 6.4 -> 24.7 fps (82 %); process CPU 730 % -> 220 %; idle SPUs 100 % -> 12-29 %. Shared SRW was ~40 % of SPU samples; poll back-off alone 19.6 -> 24.7. | `mfc_for` TLS memo is PRESENT but default **off** (`SPU_MFC_LOOKUP_CACHE=1`, `runtime/spu/spu_channels.c:428-455`; hit path `:471-479`; locked path `:488-520`). **No LLAR back-off, no plain-GET poll back-off** (`:739-806` spins; `spu_coh_reserve_fast` avoids the global lock on a repeated line, `runtime/spu/spu_coherency.c:337-352`). Overlay EA filter PRESENT (`spu_channels.c:2088-2095`). | Mechanism yes: EA Job Manager kernels poll a queue; c690 lanes are 33-88 % idle. Lock-line contention and idle spins steal SMT/memory from the walker/main even when cores are not saturated. | **0-2 ms, confidence low.** Probe idle CPU first. Enabling the existing `SPU_MFC_LOOKUP_CACHE` is free and could be the larger half. |
| **#9** PPU affinity/priority | Pin guest PPU threads, map priorities; off by default after mask 3 was far slower. | None claimed (experiment). | Our `PS3RECOMP_PIN=1` already pins walker/main and leaves the rest to a pool (`docs/live-ab-and-ceilings.md:77-89`). | Same mechanism exists and is better tuned for our topology. | 0; skip. |
| **#154/#258/#259** SPU host-thread priority / one-core-each / auto by core count | Optional pinning of SPU threads and priority; auto only on >= 8 physical cores with SMT. | Smoke only, no in-match number. | Not present as SPU-specific pinning; we pin only named PPU/walker threads. | Plausible on 6C/12T: six SPU lanes + present + IMFE share cores 2-5,8-11. | 0-1 ms; cheap probe, but may starve the handler/present threads. |
| **#270** sub-ms `usleep` | Windows: below 1 ms use a high-resolution waitable timer instead of QPC + `SwitchToThread`. | Render thread 99 % -> 2 % of a logical CPU; +6-11 % at 1.5 GHz. | **Different base:** Linux/`sys_timer_usleep` uses plain `nanosleep` for every `usec > 0` (`runtime/syscalls/sys_timer.c:283-289`). The Windows path has the timer/spin split (`:251-282`). | The Windows problem (spin burns a core) is inverted on Linux (nanosleep oversleeps by the timer slack, and in the worst case adds latency to a 200 us poll). The title's `usleep(200)` sites are *not hit at the meter* (`ppu-critical-path.md:63`), so this is a load/hitch lever, not a meter lever. | **~0 at the meter;** worth doing for loading/hitches. |
| **#287** SPU context placement | 64 KB-aligned contexts plus `(slot % 7) * 9344`, so paired SPUs' same-LS lines do not share L1/L2 sets. | Not measured alone; part of #286-290 Deck work. | Absent: contexts are `calloc`-ed as a block. | Mechanism is generic x86 cache aliasing. | 0-1 ms; cheap to try (allocation change), but hard to attribute. |
| **#283** link order file | lld `/order` from hot samples. | **No gain on the Deck**; kept for PGO. | Absent. | — | 0; skip. |

### 1.2 SPU execution and code generation

| NHL | What it does | Their measured gain | Ours | Applies to MLB? | Meter estimate |
|---|---|---|---|---|---|
| **#69** AVX2 exact SPU float | 4 lanes widened to `f64x4`, op, 29 low bits cleared, narrowed; boundary lanes scalar-exact. | `fma`+`fm` pair 4.6 ns vs 18.8 ns scalar (~4x); in-match `spu__fma_any` was 30.7 % of an SPU thread. | **Code PRESENT but compiled out:** `spu_helpers.h:1070-1173` under `__AVX2__`. The build only passes `-msse4.1` (`CMakeLists.txt:318-343`; SPU TUs `templates/project/CMakeLists.txt:205-246`). | Yes: c690's unpack/transform/normalize/pack is float-heavy. AVX2 is the right target on Zen+ (no AVX-512). | **1-2 ms** from c690 wall (~73.5 -> ~50-60 ms cumulative), ~0.7-1.5 ms of walker c690 wait plus contention relief. Confidence medium. |
| **#70** AVX-512 SPU float | One `{rz-sae}` op + `vfpclass` fallback. | ~40 instructions/op -> ~1. | **Irrelevant on this CPU** (Zen+ has no AVX-512). Our helpers have no AVX-512 float path at all. | Only on a future Zen 4/5 machine. | 0 here. |
| **#142/#260/#280/#282/#286** lean / RZ / approximate float | Exact-ish fast path under MXCSR `RZ`, then a truly approximate "RPCS3 XFloat" mode that clamps operands and skips fix-ups; dual AVX2/AVX-512 copies. | Large (NHL went from per-lane exact to one op/op); exactness tests 96M lanes. | **Partial/different:** `SPU_FAST_FLOAT` is a native f32 RNE path with manual FTZ, opt-in, off (`spu_helpers.h:1050-1069`; `templates/project/CMakeLists.txt:195`). The RZ lean path exists only on AArch64 (`spu_channels.c:249-275`; x86 `spu_float_mode_enter` is a no-op). | Yes; this is the biggest SPU codegen lever on AVX2-only CPUs. Risk: float semantics (last-bit, overflow) can change physics/visuals. | **+1-3 ms** beyond AVX2 exact if accepted; confidence medium-low. Treat as a separate opt-in. |
| **#71** SPU LS probes compiled out | `SPU_LS_PROBES=0` for lifted code: removes ~14 x86 instructions + 5 branches per `lqd/stqd`. | Not quantified alone. | **Partial:** probes are gated on cached ints, not `getenv` (`runtime/spu/spu_context.h:569-637`, `:738-763`). Full compile-out + vector LS is opt-in `PS3RECOMP_SPU_FAST_LS` (needs `-mssse3`), default **off** (`templates/project/CMakeLists.txt:196-199`, `:232-237`). | Yes: c690 is LS-access dense. | **0.5-1.5 ms.** Cheap build option; no semantic change. |
| **#72** lean per-hop dispatch hooks | Skip lockstep/task-launch checks when nothing is armed. | 8.7 % + 2.6 % of an SPU thread. | **Present:** `g_spu_direct` disables per-step diagnostics; lean hooks in `runtime/spu/spu_drain.c:114-166`. | Yes. | Already captured. |
| **#138** call-bracket inlining (`SPU_CALL_DONE`) | Normal SPU `RET` clears `drain_ret_pc` inline. | `spu_drain_call` 7-10 % after #139. | **Absent** (only the out-of-line `spu_drain_call`, `spu_drain.c:541`). | Yes; c690/Job Manager call-heavy. | 0.5-1 ms, but needs a lifter change + relift. Medium. |
| **#139** direct tail calls | `musttail` for cross-function tails. | Removes a trampoline per hop. | **Present:** `runtime/spu/spu_dispatch_mt.c:24-69`, `spu_context.h:1029-1077`. | Yes. | Already captured. |
| **#137** x86 SIMD SPU integer ops | `shlqby/rotqby` = one `pshufb`; `cflts` etc. vectorized. | 97-177 instructions -> 9-47; `rotqmbybi` 4 % of an SPU thread. | **Partial:** `shufb` and byte permutes use SSE4.1; `cflts` and variable shifts use AVX2 blocks that are compiled out without `-mavx2` (`spu_helpers.h:476-494`, `:1660-1690`, `:2253-2320`). | Yes; enabling `-mavx2` activates the rest. | Folded into the AVX2 build item. |
| **#65/#49** resident code slots 4 -> 96 | More resident lifts. | Needed for 11+ concurrent jobs. | **4** resident code slots + overlay + task (`runtime/spu/spu_context.h:307-314`); job modules are AOT at fixed bases (`tools/lift_jobmods.py`). | Only if c690/neighbour modules evict each other. No eviction is reported in our traces. | 0 unless an eviction appears. |
| **#290** PPU VMX SIMD | `vperm/lvlx/lvrx/vadduwm/vsubuwm` as SSE, integer compares. | Equivalence only; no fps claim. | Not present; lifted VMX uses helpers. | Possibly; depends on how much VMX the PPU main runs (unknown). | 0-1 ms, confidence low. |
| **#160** SIMD `fi` | Exact fixed-point interpolate in 32-bit lanes. | No fps claim. | Unknown/absent. | — | Low. |

### 1.3 GCM / FIFO walker

| NHL | What it does | Their measured gain | Ours | Applies to MLB? | Meter estimate |
|---|---|---|---|---|---|
| **#61** `GCM_REF_IMMEDIATE` | Write `ctrl->ref` the moment the walker executes `SET_REFERENCE`, instead of one queued publish per tick. | Made a ~1 fps post-transition intro normal: EA's ring allocator reads `ref` before handing over the next 32 KB segment; one ref/tick capped dozens of segments/frame. | **Absent.** Refs are queued (`cellGcmSys.c:2023-2035`), published once at each drain tail (`:3548-3551`) and at most one per 200 us on the read-driven path (`:2197-2213`, `GCM_REFPOLL` default on). `gcm_ref_push_at` at `:2468-2473`. | Yes, but our cadence is already ~12 publishes/frame, not 1/tick. The meter's ref wait is ended by the GCM user handler (`.opd.FUN_00069138`), not by the ref write (`docs/overlap-analysis.md:96-100`), so immediate publication can only remove a sub-pass race, not the 19.7 ms block. | **0-1 ms, high variance.** Cheap A/B as a live switch; kill at <0.5 ms. |
| **#60** self-JUMP grace | A parked self-JUMP is not "stuck" for 10 s (`GCM_SELFJUMP_MS`). | Stopped a post-transition skip of ~600 KB and the label-64 freeze. | **Equivalent PRESENT:** self-JUMP park passes reset the stuck counter (`cellGcmSys.c:3425-3435`); the park grace is 10 s (`:1183-1189`). | Yes. | Already captured. |
| **#64** jump-park grace 10 s | Configurable grace before the jump-park resync. | Prevents dropped commands when EA patches the park >50 ms later. | **PRESENT:** `GCM_PARK_GRACE_MS` default 10000 (`cellGcmSys.c:1174-1189`), re-reads the park before giving up (`:2946-2968`). Plus the opt-in walk-through fix `PS3RECOMP_WALKER_RESYNC_FIX` (`:1830-1875`, default off; measured inert at the pitch in `docs/tick-and-resync.md`). | Yes. | Already captured. |
| **#46** flip marker on a title-owned ring | `_cellGcmSetFlipCommand` writes the flip marker when 8 bytes fit, else calls the buffer-full callback; 4 KB slack only for runtime recycle. | ~23 -> ~31 presents/s (every 4th flip used to fall back to an immediate flip and pair two flips on one present). | **Equivalent PRESENT:** `gcm_flip_into_fifo` with the 8-byte fit test and `GCM_RECYCLE_SLACK` (`cellGcmSys.c:3838-3845`, `:1193-1197`); our title's statically linked libgcm emits `0xFEAD` words instead (`req == fin`), and `PRESENT_NEW_ONLY` removes double presents (`docs/frame-pacing.md`). | Yes. | Already captured. |
| **#48** VSYNC flip mode | At most one FIFO flip per vblank; catch-up threshold. | Unheld flips ran at ~165 fps with zero frame time and froze on load. | Our `s_flip_mode` is stored but **never read** (`cellGcmSys.c:79`, `:711-715`, no other reads); the tick still presents at most one pending flip per tick (`main.cpp:272`). | **N/A for this title:** it never calls `cellGcmSetFlipMode` (`docs/meter-pacing.md`). Do not implement a mode gate on spec. | 0. |
| **#141** present each flip once | End-of-loop present consumes the pending-flip flag. | Stops double presentation. | **PRESENT:** `PRESENT_NEW_ONLY` edge (`docs/frame-pacing.md`) + `rsx_vulkan_backend.c` gate. | Yes. | Already captured. |
| **#230** NOP run skip | Skip the `NV4097_NO_OPERATION` (`0x0100`) words directly after the first of a run; the first still flushes a pending index/array draw. | NOPs were 41 % of all FIFO words; per-word loop leaf 8-9 % + `rsx_process_method` leaf 9-10 % of the RSX ticker. Title A/B identical (no NOP runs there). | **Partial:** runs of **zero** words are skipped (`cellGcmSys.c:2811-2833`), but `0x0100` is decoded as a normal method with count 0 (`:3005-3010`). | Very likely: same EA command builder; our `[words]` diagnostic exists for the same reason. | **0.5-2 ms** of walker CPU. Cheap and low risk; the replay gate must be identical. |
| **#63** draw coalescing | Contiguous `DRAW_INDEX_ARRAY` words with `next.first == first + count` become one backend draw; flush before any other method. | 5.2 M of 6.6 M vertex conversions per 20 frames were "partial" fallbacks; removed by this. | **PRESENT:** per-`BEGIN_END` accumulation (`rsx_draw_engine.c:4549-4594`), one draw at group end (`:5885-5912`); unique-vertex remap (`:4498-4504`). | Yes. | Already captured. |
| **#62** GPU-indexed VP draws | Convert each referenced vertex once, build a u32 index buffer, `DrawIndexed`. Other cases keep CPU expansion. | 3D intro ~1 game fps, 59 % in `read_vp_vertex` for ~2.2 M vertices/frame. | **PRESENT:** unique-vertex conversion + index buffer (`rsx_draw_engine.c:4276-4282`, `:5751-5762`); backend `draw_fill`/GPU vertex path (`:5885-5898`). | Yes. | Already captured. |
| **#5/#6** semaphore park/latch | Don't resync an ACQUIRE park; latch a PPU store of the awaited value. | Fixes a freeze, not fps. | Not verified; our park/stuck detector has the same shape. | Possibly. | Correctness; no meter estimate. |
| **#31** title recycle heuristic | `GCM_TITLE_RECYCLE=0` for titles that step their context through 32 KB segments. | Fixes a resync. | Need to check per-title; not a meter lever. | — | 0. |

### 1.4 Renderer caches, textures, pipelines, shaders

| NHL | What it does | Their measured gain | Ours | Applies to MLB? | Meter estimate |
|---|---|---|---|---|---|
| **#128** persistent PSO library | `ID3D12PipelineLibrary` in `pso_lib.bin`, pipelines named by a 64-bit desc hash; save after 3 s idle. | ~900 PSO builds x 2-16 ms = ~6 s of the puck-drop stall. | **Absent:** every Vulkan pipeline is created with `VK_NULL_HANDLE` (`rsx_vulkan_backend.c:74-77`, `:1175`, `:1518`); only in-memory tables (`rsx_draw_engine.c:60-61`, `:258`). A whole run makes 340-430 pipelines / 30-41 ms total (`docs/live-ab-and-ceilings.md`). | Mechanically yes (`VkPipelineCache` persisted to disk is the 1:1 analogue), but our pipeline creation is not a meter cost. | **~0 at the meter** (pipecrt=0, pipe 1.9 ms is memo/setup not creation). Worth it for load/first-use stutter only. |
| **#136** texture victim choice | Empty slot first, else least-recently-bound, **never one bound this frame**; stale re-upload reuses its hash. | Puck drop 842 ms / 22k binds; a 4 s ~10 fps burst from re-creation cascades. | **Partial:** empty-first + global LRU is present (`rsx_draw_engine.c:2610-2625`), but there is **no "bound this frame" pin**; the content hash is full-span (`:2195-2210`) and revalidated sparsely each frame / fully every 8 (`:2212-2255`). | Yes; the backend already has `last_use_serial`, so the pin is a small change. | **0-1 ms** at the meter (tex is 9.8 ms but mostly steady); more during loading/hitches. |
| **#239** hash every mip level | Texture change hash covers all uploaded mips, not level 0. | Fixes stale lower mips (Flyers kit). | **PRESENT:** full-span hash including the whole mip chain (`rsx_draw_engine.c:2156-2168`, `:2195-2210`, `:2536-2539`). | Yes. | Already captured (correctness). |
| **#225/#227** async shader compile | Compile non-essential pipelines on a pool; skip draws until ready. | Removes first-use stalls. | Absent. A run makes 340-430 pipelines / 30-41 ms total. | Yes, for hitch scenes only. | ~0 at meter; consider for the loading/menu hitches on the backlog. |
| **#162/#81/#82** FP key memo / narrower VS hash / FP copy at record | Avoid re-walking shader code and constants per draw. | Not quantified separately. | Our engine has a 512-entry pipeline memo and keys pipelines by the draw state (`rsx_draw_engine.c:258`, `:3826-3838`); no per-draw FP code walk is visible in the profile (pipe 1.9 ms). | Partly. | 0-0.5 ms; audit only if `pipe` grows. |
| **#224** one big-endian load per vertex component | Replace a separate `vf_be32` load per component. | Part of the vertex-fetch work. | Unknown; our fetch is 4.0 ms/frame. | Possibly. | 0-0.5 ms. |
| **#233/#243** per-frame surface history, one-off RT alias, LRU eviction | Stop readback/re-decode of transient targets. | Hitches. | Our WCB readback is a different design; `docs/readback-cost.md` already attributes the meter's copies. | Different mechanism. | See readback cuts, not NHL. |
| **#228** hot-path `getenv` caching | Cache every gate in a static. | Not quantified. | Largely present in hot paths (e.g. `spu_channels.c:480-487`); an audit may find stragglers. | Yes. | 0-0.5 ms. |

### 1.5 Build flags and PGO

| NHL | What it does | Their measured gain | Ours | Applies to MLB? | Meter estimate |
|---|---|---|---|---|---|
| **#156** Clang PGO | instrumented build, `-fprofile-instr-use=<profdata>`; profile dumped every 30 s. | main busy -22 %; 0/124 s below 20/s vs 12 s. Deck: +3.5 % after a refresh (inside noise). | **Absent:** no `-fprofile-*`, no LTO, no `-march` (`CMakeLists.txt:318-343`, `templates/project/CMakeLists.txt:205-246`). | Yes. PGO is the highest-leverage generic build change, but it needs profiling runs. | **1-3 ms** at the meter via shorter producer/park latency; larger at the pitch. Medium effort, one-time per code change. |
| **#261** PPU codegen | `-march=x86-64-v3` (movbe folds bswaps), `-Zc:tlsGuards-`, force-inline the VM helpers even when PGO thinks the function is cold. | ~0.5 % from movbe; the big find was PGO outlining hot render helpers (6,682 out-of-line calls in one TU). | **Partial:** inline stores/loads with a span gate are PRESENT (`tools/ppu_lifter.py:180-211`, `:262-306`; `runtime/ppu/ppu_loader.cpp:1677-1692`). No `-march`, no TLS-guard flag. | Yes, but check the PGO interaction *if* PGO is added: verify hot functions did not get outlined. | `-march` alone **<0.5 ms**; the inlining check matters after PGO. |
| **#272** dead-flag elimination | Drop XER.CA writes nothing reads; fuse compare+branch where the CR field is dead. | 816,699 CA writes + 323,970 fusions; source 15 % smaller; runtime not separately measured. | **Partial:** compare/branch fusion PRESENT (`tools/ppu_lifter.py:777-952`), but deliberately refuses XER[SO]; **no liveness/dead-CA pass**. | Yes, but it needs a relift and a post-pass (like #272) and changes every lifted function. | **0.5-1.5 ms** (PPU main -10-15 % instructions), overlapped so the visible gain is smaller. |
| **#149** VMX accessor rewrite | `memcpy` + `__builtin_bswap` instead of four byte loads/shifts; clang then vectorizes. | `vmx_vmaddfp` 202 -> 71 instructions; 12 % render / 4 % main before. | **Absent:** hand-written byte-shift loops (`runtime/ppu/ppu_vmx.h:31-37`, `:86`); FMA in `vmaddfp` is present (`:278-286`). | Yes, if our PPU main uses VMX element access; unknown. | **0-1 ms**, cheap helper-only change. |
| **#146** PPU FMA | `/clang:-mfma -ffp-contract=off` for lifted PPU. | Part of the same audit. | Present: `fma()` in scalar lifted FP (`tools/ppu_lifter.py:425-434`, `:2431-2443`). | Yes. | Already captured. |
| **#283** order file | linked order from samples. | No gain. | — | — | 0. |
| **#287** context placement | see 1.1. | — | — | — | 0-1 ms. |

### 1.6 Native Edge geometry (the big one)

NHL: **#268** recon, **#291** corpus capture, **#292** native AVX2 stages,
**#293/#295** settings. Their Edge job is SPU program `0x01DBF980`; the native
path replaces decompress `0xA048`, skin `0xA0F8`, cull `0x5C08` and compress
`0x4C70` (~95 % of the job) through hooks, leaving index decode, ring alloc
and the command hole lifted. Any other layout falls back and logs.

- **Result:** whole job ~612 -> ~55 kcyc/segment; `#292` measured **429 -> 19
  kcyc/segment (22x)** on PC, ~30 cycles/vertex. Deck in-match **+14-18 %**
  (whole-match sim steps 41.7-43.0 -> 49.2-49.3), and the Deck exe-cycle share
  of that program was 17.1 %.
- **Exactness:** positions max error 7.6e-6, packed normals 94.5 % exact /
  rest 1 LSB, cull differs on 5 of 216 captured segments (one borderline
  triangle each). Their square roots are table estimates and `1/w` is an
  estimate, so bit-exactness is not achievable without emulating those too.
- **Ours:** the c690 job (`docs/c690_function_map.md`) is the same Edge
  geometry family but a **different per-game layout**: 12/12/11 SNORM unpack
  `0x6D68`, 3x4 affine `0xA798`, `frsqest`+`fi` normalize `0xAF98`, snorm16/8
  pack `0x7C18`/`0x8160`, 36-byte assembler `0x10F78`, culls `0x132E0`/
  `0x12C08`. There is no native path. `docs/c690-offload-decision.md` already
  rejected a GPU fenced design and priced a full native/GPU job at 2-4 months.

**Can their C reference be reused?** Partly:

- `edge/edge_ref.{c,h}` is **layout-specific**: 3x u16 pos in 2.14 + s32
  offsets, 3-byte 10:10+axis unit vectors, 4-weight skin, 12/20-byte output.
  That is not c690's format, so the per-vertex kernels cannot be copied.
- What *is* reusable, and is the real value of the port:
  1. the **stage split** (`decompress -> skin -> cull -> compress`) with a
     layout check and a lifted fallback per stage;
  2. the **cull algorithm** (screen flags from view-projection x world, sample
     grid 2.0 for AA 3/4, degenerate-column and backface tests). Edge's cull is
     almost certainly shared middleware; this is the most likely kernel to
     port nearly as-is, subject to c690's flag encoding;
  3. the **corpus + differential harness** method: dump N segments per format
     key, replay offline, compare bytes and final mesh visually. That is how
     they got 22x without visual regressions.
- **Meter estimate:** the walker's c690 wait is only **2.7 ms/frame**
  (`walker-spu-wait.md`), split admission + running. Native Edge removes most
  of the running part; the admission/PPU append remains. Upper bound ~2 ms at
  the meter, **realistic 1-2 ms**. At the pitch / in play the producer is
  ~17 ms (`targets-and-backlog.md`), so the same work is worth much more
  there (their Deck +14-18 % is the analogue).
- **Kill criterion:** native c690 must cut the c690 job wall by >= 5x and the
  walker's c690 wait by >= 1 ms at the meter; if the corpus fallback rate is
  > 10 % of segments, stop and fix the format coverage first.

## 2. Direct answers to the named questions

**Native Edge / C reference** — see 1.6. Reuse the architecture, cull and
corpus method; rewrite the c690 kernels from our own disassembly. Do not
expect their `edge_ref.c` to run on MLB data.

**SPU idle cost (#45)** — mechanism applies; our tree has neither GETLLAR nor
plain-GET back-off, but it does have the `mfc_for` memo (off) and a lock-free
repeated-GETLLAR path. First measure per-SPU-thread CPU; if idle lanes are
> 30 % of a core each, add the back-off. The cheap half is
`SPU_MFC_LOOKUP_CACHE=1`.

**Lifted-SPU build flags / float paths** — the AVX2 exact float and integer
blocks exist in `spu_helpers.h` but are compiled out because the SPU TUs only
get `-msse4.1`. Adding `-mavx2 -mfma -ffp-contract=off` (and `-mssse3` for
`PS3RECOMP_SPU_FAST_LS`) is the single highest-value cheap change. AVX-512
(#70) does nothing on this CPU. The lean RZ path (#142/#260) does not exist on
x86 here; `SPU_FAST_FLOAT` is the available approximate mode.

**SPU LS probes** — our probes are cached-int gated, not per-access `getenv`;
the remaining cost is the branch and the un-vectorized LS read/write. Enable
`PS3RECOMP_SPU_FAST_LS` to compile them out and use one vector access.

**`GCM_REF_IMMEDIATE` (#61)** — absent; our refs publish once per drain (about
12/frame) plus a 200 us poll path, not one per tick, so the NHL cliff does not
apply. A/B it as a live key; expect 0-1 ms.

**Park grace (#64) and self-jump (#60) vs our resync bug** — both behaviours
are already in our tree: 10 s grace, re-read the park before resyncing,
self-JUMP park passes reset the stuck counter, and the opt-in
`PS3RECOMP_WALKER_RESYNC_FIX` walks the skipped range instead of dropping it.
The pitch-measured failure is different: `FAST_WAKE` lets the walker re-read an
unpatched park and wedges (`docs/tick-and-resync.md`). Don't import #60/#64;
they are already here.

**Draw coalescing (#63)** — present (per-`BEGIN_END`, with unique-vertex
remap and GPU vertex fill). No action.

**NOP skip (#230)** — zero-word runs are skipped; `0x0100` runs are not. This
is a small, safe walker cut and the only NHL FIFO change that is not already
here.

**Persistent pipeline cache (#128)** — absent (`VK_NULL_HANDLE` everywhere).
It is cheap to add (`VkPipelineCache` + a disk blob), but our measured cost is
340-430 pipelines / 30-41 ms per *run*, not per frame. Do it for load stutter,
not for the meter.

**Texture cache (#136, #239)** — #239 (all-mip hash) is present; #136's
never-evict-this-frame pin is not. Add the pin behind a switch; expect 0-1 ms
at the meter.

**Inline memory ops (#147, #261)** — inline aligned BE stores/loads with a
span gate and the SPU-reservation bitmap are present; the line-level
`-march=x86-64-v3`/TLS-guard items are not. The important #261 lesson is the
PGO interaction: after adding PGO, check that hot render helpers were not
outlined as cold.

**Sub-ms `usleep` (#270)** — our Linux path is `nanosleep` for every sleep, so
the Windows spin problem is inverted. The title's `usleep(200)` sites are not
hit at the meter. Treat as a load/hitch item.

**Frame pacing / vsync (#46, #48, #141)** — #46 (flip marker fit / callback)
and #141 (one present per flip) are present; #48 (mode-gated flips) is N/A
because the title never calls `cellGcmSetFlipMode`, and our `s_flip_mode` is
stored but unread. Our pacer is arrival-cadence-based (`PS3RECOMP_VK_PACE`),
which is a better fit than copying a vblank gate. Two structural facts bound
any pacing patch: `Gfx_FrameEnd` lets the main run three flips ahead
(`DAT_01120410 = 2`), and the `FUN_000655cc` ref cond is a depth-1 submission
throttle, not a display wait. NHL's #46/#48 fix *marker placement* and
*flip mode*, neither of which is our missing tick.

**PGO** — absent and worth doing. It is the only item here that improves every
CPU-side path at once (PPU main, walker runtime, SPU glue). Budget one
instrumented build plus 2-3 scripted runs, then a rebuild; merge the profiles.
The #261 caveat applies: after PGO, verify the hot render/VM helpers are still
inlined.

## 3. Ranked plan

Ordering is expected **ms saved at the meter per unit of effort**, not total
possible gain. "Cheap" = env/flag/small patch, no relift, testable in the
existing A/B harness; "big" = relift/codegen/renderer work. All estimates are
pre-measurement. Use `ab_live.sh` on the meter (X once, PIN=1, WCB=2, 6x10 s,
2 s dropped), and read `PS3RECOMP_C690_STATS` job wall plus the walker phases
as the mechanism check.

### Cheap, try first

| # | Change | Expected at meter | Effort | On/off test | Kill criterion |
|---|---|---|---|---|---|
| 1 | `SPU_MFC_LOOKUP_CACHE=1` (exists, default off; `spu_channels.c:428-455`) | 0-2 ms; more if MFC lock contention is real | XS (env only) | One launch, A/B live key if made live; else two runs, same scene | c690 job wall -< 5 % and fps within A/A noise. |
| 2 | `PS3RECOMP_SPU_FAST_LS=ON` (+ `-mssse3`) for SPU TUs | 0.5-1.5 ms | S (build option) | Two builds, paired meter run; spot-check LS-heavy scene | c690 job wall -< 5 %, or any LS-fidelity test failure. |
| 3 | Add `-mavx2 -mfma -ffp-contract=off` to the SPU TUs (new option), enabling exact AVX2 float + int blocks | 1-2 ms at meter, more at pitch | S/M (CMake + CPUID guard; verify `spu_helpers` tests) | Two builds; A/B meter + c690 wall; rerun `test_spu_float_lean`/int tests | c690 wall -< 10 % or any exactness test failure. |
| 4 | NOP `0x0100` run skip in the FIFO loop (#230) | 0.5-2 ms of walker CPU | S | Env switch; offline `replay_rsx_engine` frame must be identical, then A/B | Walker parse/drain -< 0.3 ms and fps within noise; any replay diff. |
| 5 | `GCM_REF_IMMEDIATE` as a live key (#61) | 0-1 ms (high variance) | S | Live A/B at the meter, read main ref-wait span | Main ref wait -< 0.5 ms and fps within noise. |
| 6 | `PS3RECOMP_SPU_FAST_FLOAT=ON` trial (approximate RNE, RPCS3-style) | +1-3 ms beyond exact AVX2 | S (flag) but risk | One build, meter + one scripted play scene, frame-diff + physics spot checks | Any visible/physics divergence, or < 0.5 ms. Keep exact AVX2 as default. |
| 7 | Texture victim "never evict bound this frame" (#136) + reuse a stale upload's hash | 0-1 ms | S | Live/env switch; count texture creates/evictions per frame | Texture create/re-upload count unchanged or fps within noise. |
| 8 | `-march=x86-64-v3` for PPU (movbe), TLS-guard flag (#261) | < 0.5 ms | XS | Rebuild + paired run | No measurable change; keep only if free. |
| 9 | VMX accessor `memcpy`+bswap (#149), helper-only | 0-1 ms | S | Build + A/B; VMX equivalence test | PPU main time unchanged. |
| 10 | Sub-ms sleep precision on Linux (hybrid sleep/spin under 1 ms) | ~0 at meter; load/hitch only | S | Load-time and hitch-count A/B | No load/hitch change. |
| 11 | SPU context cache-set placement (#287) | 0-1 ms | S | Build + A/B; check c690 wall | No change outside noise. |
| 12 | Persist `VkPipelineCache` (#128 analogue) | ~0 at meter | S/M | Load-time and first-use hitch measurement | No load/hitch change; do not keep on the meter's account. |

### Big

| # | Change | Expected | Effort | On/off test | Kill criterion |
|---|---|---|---|---|---|
| A | PGO (instrumented build, 2-3 scripted runs, merged profile) (#156) | 1-3 ms at meter; 10-20 % CPU-side elsewhere | M (needs runs) | Two builds, paired meter + pitch runs; compare PPU main busy and walker drain | < 3 % frame-time change, or hot helpers outlined as cold (#261). |
| B | SPU idle back-off: GETLLAR CV sleep + plain-GET poll hash sleep (#45) | 0-2 ms via contention | M (runtime + test) | First measure per-SPU-thread CPU; then live/env A/B; `test_spu_lockline_wait` | Idle lanes < 30 % of a core, or no fps change. |
| C | Dead XER.CA elimination + dead-CR fusion (#272) | 0.5-1.5 ms | L (post-pass + relift) | Rebuild, equivalence tests, paired meter/pitch; source-size and instruction-count check | < 0.5 ms at meter and pitch, or any lifted-equivalence failure. |
| D | Native c690 Edge stages (#292 pattern) | 1-2 ms at meter; large at pitch | XL (weeks) | Offline corpus first (bit/1-LSB), then on/off meter + pitch | c690 wall < 5x faster, or walker c690 wait < 1 ms, or fallback > 10 %. |
| E | Lean RZ SPU float (port #142/#260/#280 to x86) | between exact AVX2 and `SPU_FAST_FLOAT` | L | Only if #6 is rejected for accuracy but a fast path is wanted | No gain over #6 or any exactness failure. |
| F | Call-bracket inlining (#138) | 0.5-1 ms | M (lifter + relift) | Rebuild + A/B; c690 wall | < 0.3 ms. |

The first four cheap items can all be tested in **two builds** (one with
`FAST_LS`+AVX2, one baseline) plus one env-only run. If the AVX2/`FAST_LS`
pair does not move the c690 wall by at least 10 %, stop the SPU codegen line
there and move the effort to PGO and the walker.

## 4. What does not transfer

- **D3D12-specific renderer pieces:** PSO libraries, HLSL decompiler caches,
  root signatures, offscreen-RT/alias heuristics, D3D12 present/VRR. The
  Vulkan equivalents are a `VkPipelineCache` (cheap, low meter value) and our
  existing engine/backend split.
- **NHL's game-speed assumptions:** their 2-vblank swap interval, `GCM_FLIP_EVERY`,
  and the 30 fps cap are title policies. MLB never sets a flip mode and is not
  vblank-capped in the same way (`docs/meter-pacing.md`).
- **Their PGO profile and lifted/game-derived code:** not reusable (different
  game; and the profile is tied to their binary).
- **Their thread-affinity/priority experiments:** ours already pins the walker
  and PPU main to separate cores; theirs was exploratory and mostly off.

## 5. Licence and attribution

The NHL port's own code is **MIT**, `Copyright (c) 2026 HeckYouMan`
(`LICENSE` in that repo). MIT permits reuse, modification and redistribution
provided the copyright notice and permission notice are included in all copies
or substantial portions. ps3recomp itself is MIT, © Ned Heller / sp00nznet,
and any changes we make to the runtime stay under that licence.

Files we would take or translate if the native-Edge item is approved:

| File | Use | Attribution needed |
|---|---|---|
| `edge/edge_ref.c`, `edge/edge_ref.h` | C reference for the Edge stage split and the cull math; starting point for a c690-specific reference | Keep the MIT notice; add "Portions derived from NHL Legacy Native (c) 2026 HeckYouMan, MIT" in the file header and in the repo's third-party notices/credits. |
| `nhl/port/nhl_edge_native.c` | AVX2 implementation pattern: per-stage layout check, `[EDGE-NATIVE]` fallback, cull flags/backface/sample grid, per-thread scratch | Same notice; it is a derivative even after translation to C++/Vulkan. |
| `edge/cullsim.py`, `edge/cullcheck.py`, `edge/recdump.py` | Corpus/differential test method | Same notice if copied; a reimplementation of the method needs no licence but credit is courteous. |
| `docs/notes/LOCAL_PATCHES.md`, `NEXT_STEPS.md` | Design notes (facts, not code) | Cite in the doc; no licence obligation for facts, but name the source. |

Do not take: their `nhl/pgo/*`, lifted `recompiled/` or `spu/` output, game
data, or anything derived from RPCS3 (GPLv2). Our repo's clean-room rule
(never copy RPCS3) is unaffected: the NHL patches are independent MIT code.

## 6. One-line summary

The NHL list is a catalogue of the same EA-engine problems, but our meter is
walker-CPU and producer-cadence bound, not SPU-throughput bound. The
transferable wins are (1) compile the existing AVX2/vector SPU paths and turn
on the existing MFC cache, (2) skip `0x0100` NOP runs, (3) add PGO, and
(4) only then consider a native c690 Edge path, whose meter ceiling is ~2 ms
even though it is worth far more at the pitch.
