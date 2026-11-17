# Plan: road to 60 fps

**This is the main plan and todo list.** Work the checklist in order and update it alongside measurements. Detailed frame attribution lives in [`critical-path.md`](critical-path.md); older history is in [`targets-and-backlog.md`](targets-and-backlog.md).

## Current Status (2026-11-17, `build-soa`, Pitch Meter)

After finishing **Item 2** (SPU flat table + coherence holders) and **Item 3** (direct SSBO vertex pulling + per-batch descriptor cache):

| Metric | Nov 6 (`build-depth`) | Nov 17 post-Item 3 (`build-soa`) | Source |
|---|---:|---:|---|
| Pitch meter FPS | `~34.5-35.0` fps | **`38.86` (`SOA=0`) -> `40.26` (`SOA=1`)**, up to **`40.68-42.00`** (`item3_perf`, stats off) | `ab_live_item3_critprof` (N=3 paired), `ab_live_item3_perf` |
| Walker vertex `fetch` | `4.26-7.40` ms | **`1.16-1.53` ms** (Item 2 was `4.20-4.38` ms) | `item3_perf`, `item3_verify` Arm A |
| Walker drain / wait | `20.55` ms / `~3.2` ms | **`14.56-20.58` ms** drain, **`12.73-18.47` ms** wait (`12.98` ms traced) | `item3_perf`, `item3_critprof`, `item3_hud_trace.pstl` |
| CPU share (`cycles:u`) | SPU `69%`, main `10.5%`, walker `10.1%` | **SPU `73.30%`**, main `10.46%`, walker `9.25%` | `item3_perf.data` (`210K` samples) |
| SPU lanes 0-4 (`c690`) | `87-90%` busy | **`88.1-90.7%` busy (`67.3-68.1%` in `c690`)**, `609.1` jobs/frame (`p50 166 us`) | `item3_hud_trace.pstl` (`analyse_walker_wait.py`) |

**Takeaway:** Item 3 cut walker vertex fetch (`-3 ms`) and shrunk the RSX walker behind SPU+PPU production (`wait > walker`). Nearly three quarters of all CPU cycles (`73.30%`) are now in SPU worker threads (`spurs pm`), so accelerating `c690` + SPU dispatch/runtime is the primary unlock before multi-threaded walker split or main-thread optimization can translate into large FPS gains.

## Completed

- [x] **Item 2 — SPU flat dispatch, coherence holders & inline store cleanup** (`ps3recomp-soa` `1d3e7b91`, `exp/item2`):
  - Flat `(image, variant, lsa>>2)` SPU dispatch table (`spu_channels.c`), per-line coherence refcount bitmap (`spu_coherency.c`), compile-out of `PS3RECOMP_STORE_DIAG` globals (`ppu_memory.h`).
  - Verified live on pitch meter (`39.3-39.9` fps in `ab_live_item2_test`).
- [x] **Item 3 — Direct SSBO vertex pulling & descriptor cache** (`ps3recomp-soa` `b1dbf448`, `exp/item3-direct-vtx`):
  - Switched `rsx_vp_decompiler.{h,c}` to `rsx_raw_vb` SSBO vertex pulling (96B push constants, modes 0-4) and removed the `vtx_unpack.comp` compute prepass.
  - Added AVX2 index bswap + `eng_try_raw_pull_span` in `rsx_draw_engine.c` and per-batch `vk_desc_cache` + dynamic UBO offsets in `rsx_vulkan_backend.c`.
  - Offline `replay_rsx_engine`: `mcap` `30.32 -> 26.91` ms (`draw 13.92 -> 11.61` ms); `walker_replay_fixed` (`fetch 11.84 -> 9.76` ms, `draw 19.38 -> 15.59` ms). Live fetch `4.20 -> 1.16-1.53` ms; `34/34` video fixtures, `67/67` SPIR-V tests, `3/3` CTests.
- [x] **Native Linux Vulkan HUD & `IMMEDIATE` present** (`ps3recomp-soa` `a660cee2`, `exp/item4-c690` + `mlb16-recomp` `main`):
  - Added in-process `PS3RECOMP_HUD=1` (`275x130` 30 Hz RGBA overlay + `PS3RECOMP_HUD_CSV`) in `libs/video/rsx_hud.h` + `rsx_vulkan_backend.c`, plus `PS3RECOMP_VK_PRESENT_MODE=immediate`; switched `play_windowed.sh`, `ab_live.sh`, and `mlb16_launcher.py` off external MangoHud by default.
  - Verified on `item3_hudcap`: `185 s`, `0` frozen seconds, **`max vkQueuePresentKHR = 3 ms`** (prior `300-2800 ms` present spikes only occurred with external `mangohud=1` + `SCENE_SHOTS=1`). *Needs one quick visual screenshot verify of the on-screen HUD.*
- [x] **Item 4 (Stage 1 + output writers)** (`ps3recomp-soa` `791a26cd`, `c3457bf5`, `def62bbd`, `exp/item4-c690`):
  - Live-verified normals/tangents `0xFA20/FA58/FCB8/FDA0` (`2.4-16.3x` offline on approx twin, `266,240` live shadow calls `0` diffs) and output writers `0x7998/0x8160` (`2.44-4.68x` / `1.97-2.80x` offline vs hardware-`cflts` lifted baseline, `20,164,608` shadow calls `0` diffs).
  - Replaced 15-op software `cflts` bit-twiddling with 4-op hardware `_mm_cvttps_epi32` / `_mm256_cvttps_epi32` in `out_cflts256_142`, `native_cflts256` (`0x7C18`), and constant `127 <= i8 <= 173` `spu_cflts` (`spu_helpers.h`): `0` diffs across all `2^32` float32 inputs (IEEE + FTZ), `spu_float_exact` pass (`1/1`), `0x8160` native `0.31-0.41 us`.
  - Captured 4 real pitch-meter corpus samples each for all 17 hot `c690` functions (`recomp-work/captures/c690_fn_corpus/`) and added `--fn` dirty-page harness in `test_native_soa.c`.

## Next Up (Prioritized Todo)

- [ ] **1. Finish Item 4 — Native AVX2 kernels for remaining hot `c690` functions (`exp/item4-c690`):**
  Gate every kernel on offline corpus (`0` LS/GPR mismatches across base/ftz/approx, **`>=2x`** speedup or skip), live `PS3RECOMP_C690_SOA=2` (`0` mismatches), paired `ab_live.sh` (`MLB16_BUILD=build-soa`, `C690_STATS=0`), and clean visuals:
  - **`0xD080` / `0xD2B8` / `0xD4A8`** (`7.41%` total CPU: `5.28%` + `2.13%`; currently `8.8-12.1 us` for `0xD080/0xD2B8`, `4.5-6.0 us` for `0xD4A8` offline): screen-space cull/occlusion subtree — top single-call target.
  - **`0xAF98` / `0xB268` / `0xA798` / `0xB830`** (`4.63%` perf / `15.1%` exclusive `[c690-stats]`; `op 0x21` is `29.7%` of `c690`): test directly via `CMD21` (`n21/p*_in.bin`, currently `13.96-15.86 us` per `0x21` cmd) + `c690_fn_corpus`.
  - **`0x6D68`** (`3.37%` perf, `1.50-1.70 us` offline): benchmark de-pipelined unaligned 256-bit AVX2 unpack against the `>=2x` bar (skip if `<2x`).
  - **`0xF520` (`1.80%` perf) & `0xE750` (`1.47%` perf)**: wire runtime `<0x3000` helper support for `0xE750` in `test_native_soa`, test both against `>=2x` gate.
  - Visual-check one X11 HUD screenshot on `build-soa`, and promote `PS3RECOMP_C690_SOA=1` to default once Item 4 lands.
- [ ] **2. Item 5 — Re-evaluate / Repair Walker Split (`PS3RECOMP_WALKER_MT`, `exp/item5-walker-split`):**
  Re-run critical-path trace + `replay_rsx_engine` (`walker_replay_fixed`, `mcap`, `mound/m2_cap`) once Item 4 lowers SPU stalls. Prior MT attempts failed on sync barriers (`eng_zdepth_snapshot`, `eng_cube_collect`, immediate draws); now that Item 3 removed compute unpack/staging copies, check whether removing worker barriers lets MT cut walker drain (`14.6-18.8 -> <10 ms`) without worker-backlog stalls.
- [ ] **3. Major Next Levers Beyond Item 4 (SPU runtime & PPU main):**
  - **SPURS Policy-Module / Indirect Dispatch (`~10.9%` CPU in `item3_perf.data`):** `spu_lifted_lookup` + `spu_indirect_branch_slow` + `spu_lifted_lookup_full` + `spu_drain_call` (`7.53%`) plus `spu_lockline_lock` (`1.90%`) and per-job PM DMAs (`__memmove` `3.14%`). Caching PM entrypoints / fast MFC list transfers shrinks non-`c690` SPU worker overhead (`20-23%` of worker wall).
  - **PPU Main & Synchronization (`ppu main` `10.46%` + `Kinematics`/`IMFE`/`MainMultiTask` `~5.9%` CPU; `4.74 ms/frame` PPU/park walker waits):** cut pre-PUT PPU critical path (`spu_coh_notify_write`, atomic overhead) and better overlap PPU job pushing (`a_not_pushed` = `68.9%` of slot waits in `item3_hud_trace.pstl`).
  - **Scene Coverage & Hardware Check:** re-measure crowd and mound scenes post-Item 3/4 (all Nov 17 measurements are pitch-meter only); check host RAM speed (`sudo dmidecode -t memory | grep -i speed`) vs L2 miss signature.

*(Note: Roadmap Item 1 — half-rate shadows — is intentionally skipped/deferred per user request on 2026-11-17.)*

## Historical Baseline (2026-11-06, `build-depth`, Pre-Items 2-4)

Meter scene, build-depth `f8bff427`, play settings, real fps. Effort and gain figures marked "est." are estimates, not measurements. Scripts: `../tools/exp60/` (one-off, with hard-coded local paths). Run logs: `recomp-work/ab_live_{th,ct,px,py,pz}*`.

| measurement | result |
|---|---|
| CPU share of all cycles (`proc_meter.data`) | SPU lanes 69 %, main 10.5 %, walker 10.1 %, other PPU ~6 % |
| of that | c690 job code 41.7 %; lane runtime + memcpy + PM + kernel 22.4 %; SPU indirect dispatch/lookup 6.8 % |
| whole process | 24.9 Gcycles/s = 722 Mcycles/frame, ~6.5 busy threads at the 3.82 GHz measured all-core clock |
| 60 fps capacity of this CPU | 382 Mcycles/frame on 6 cores (~480 with SMT, est.): total work must fall ~35-45 % |
| frame by target (`meter_cap`, 820 draws) | main view 364 draws, 28.5 % refs, 35 % mesh refs; shadows 295, 39.6 %, 47 %; 512 target 146, 31.9 %, 18 % |
| Vulkan | instance apiVersion 1.0; per draw: descriptor set alloc + write, remap, raw copy, compute dispatch |
| hardware counters at the meter | IPC main 0.94, walker 1.15, lanes 1.38; ~13 L2 misses per 1000 cycles on all three |

### Live A/Bs (2026-11-06)

| arm | real fps | note |
|---|---|---|
| huge pages (guest RAM, heap, .text; ~640 MB) | 34.71 -> 35.06 over 4+4 launches | page walks -45 %; inside launch noise. Not a lever |
| shadow casters skipped (`0x853C6C`, camera +0x464 bit 7) | 34.55 -> **38.55** | -233 draws, -43 % mesh packets; shipping form would be half rate |
| shadow casters every other frame | 34.55 -> **36.67** | same with depth keys off (36.55). Pacing and image quality not checked |
| `ENG_DEPTH_GPU` + `ENG_SHADOW_CONST` | 34.55 -> 34.69 | neutral; needed for the full skip (without them `tex` 65 ms, 18 fps) |
| players removed from main view | 34.5 -> 45.2 | calibration only, -6.9 ms |
| `FUN_009e6d98` masks 1/8/0x10/0x20/0x400 | no change | not the meter's heavy passes |
| skip `0x8541C8` + `0x2AC53C` | 24.8 | invalid as a shadow test: view setup, culling got worse (+245 draws) |

### Compile tests on one PPU TU (static, sample-weighted, 2026-11-06)

| variant | instructions | ctx accesses |
|---|---|---|
| `__restrict ctx` | +0.2 % | -4 % (`ctx` is captured; `noescape` would be false, `g_active_ctx` exists) |
| all registers local (`ppu_locals.py`) | +12.7 % | -35 % (32 GPRs onto 16, spills) |
| cr/xer/ctr local only (`ppu_flags.py`) | -0.8 % | -13 % |
