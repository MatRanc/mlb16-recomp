# Plan: road to 60 fps

**This is the main plan and todo list.** Work the checklist in order; update it in the same commit as the work. Where frame
time goes: [`critical-path.md`](critical-path.md). History and older backlog: [`targets-and-backlog.md`](targets-and-backlog.md).

Verdict (2026-11-06): a locked 60 fps at the meter is not realistic on the Ryzen 5 2600X with today's design; ~45-50 fps
is (est.). A locked 60 also needs the walker split to work, or a faster CPU.

Meter scene, build-depth f8bff427, play settings, real fps. Effort and gain figures marked "est." are estimates, not
measurements. Scripts: `../tools/exp60/` (one-off, with hard-coded local paths). Run logs: `recomp-work/ab_live_{th,ct,px,py,pz}*`.

## New measurements

| measurement | result |
|---|---|
| CPU share of all cycles (`proc_meter.data`) | SPU lanes 69 %, main 10.5 %, walker 10.1 %, other PPU ~6 % |
| of that | c690 job code 41.7 %; lane runtime + memcpy + PM + kernel 22.4 %; SPU indirect dispatch/lookup 6.8 % |
| whole process | 24.9 Gcycles/s = 722 Mcycles/frame, ~6.5 busy threads at the 3.82 GHz measured all-core clock |
| 60 fps capacity of this CPU | 382 Mcycles/frame on 6 cores (~480 with SMT, est.): total work must fall ~35-45 % |
| frame by target (`meter_cap`, 820 draws) | main view 364 draws, 28.5 % refs, 35 % mesh refs; shadows 295, 39.6 %, 47 %; 512 target 146, 31.9 %, 18 % |
| Vulkan | instance apiVersion 1.0; per draw: descriptor set alloc + write, remap, raw copy, compute dispatch |
| hardware counters at the meter | IPC main 0.94, walker 1.15, lanes 1.38; ~13 L2 misses per 1000 cycles on all three |

## Live A/Bs

| arm | real fps | note |
|---|---|---|
| huge pages (guest RAM, heap, .text; ~640 MB) | 34.71 -> 35.06 over 4+4 launches | page walks -45 %; inside launch noise. Not a lever |
| shadow casters skipped (`0x853C6C`, camera +0x464 bit 7) | 34.55 -> **38.55** | -233 draws, -43 % mesh packets; shipping form would be half rate |
| shadow casters every other frame | 34.55 -> **36.67** | same with depth keys off (36.55). Pacing and image quality not checked |
| `ENG_DEPTH_GPU` + `ENG_SHADOW_CONST` | 34.55 -> 34.69 | neutral; needed for the full skip (without them `tex` 65 ms, 18 fps) |
| players removed from main view | 34.5 -> 45.2 | calibration only, -6.9 ms |
| `FUN_009e6d98` masks 1/8/0x10/0x20/0x400 | no change | not the meter's heavy passes |
| skip `0x8541C8` + `0x2AC53C` | 24.8 | invalid as a shadow test: view setup, culling got worse (+245 draws) |

Reflection (512 target) was not measured. All of this is the meter only.

## Compile tests on one PPU TU (static, sample-weighted)

| variant | instructions | ctx accesses |
|---|---|---|
| `__restrict ctx` | +0.2 % | -4 % (`ctx` is captured; `noescape` would be false, `g_active_ctx` exists) |
| all registers local (`ppu_locals.py`) | +12.7 % | -35 % (32 GPRs onto 16, spills) |
| cr/xer/ctr local only (`ppu_flags.py`) | -0.8 % | -13 % |

## The four items

1. **SPU: feasible, not as codegen.** Spills cap at 1.23x and `0x21` flattening gave parity ([`edge-native-21.md`](notes/edge-native-21.md)): those
   were the same instructions rearranged. A true SoA rewrite of the hot stages has not been tried; NHL got ~11x on a whole
   job of the same family. Keep 4-wide: Zen+ has 128-bit FP units, so 8-wide gains nothing here (est.).
   Hot list, 65 % of c690: FDA0 D4A8 FA58 6D68 7C18 8160 AF98 D2B8 F520 E750. Separately replace lookup with a flat
   per-image table. Target: lanes 122 -> ~65 lane-ms/frame. 4-6 weeks (est.). Stop if the first stage is under 2x.
2. **Main runtime: feasible, small.** Coherence marks are never cleared (`spu_coherency.c`), so stores to any once-reserved
   line pay a seq_cst fence plus a list walk: use per-line holders. Compile the two diagnostic globals out of the inline
   store path. Expect 1-2 ms busy, ~40 % of it on the chain (est.). PPU register promotion is ruled out above.
3. **Renderer: feasible, the biggest chain lever, and not a full redesign.** Replace remap + raw copy + compute unpack with
   vertex pulling from a per-frame raw ring, bswap indices with SIMD, use dynamic offsets or push descriptors. No
   cross-frame reuse, so the 60 % SPU-rewritten share does not matter. Target drain x0.5, -9 ms by the replay. 3-4 weeks (est.).
4. **Frame ahead: not as a guest N-2.** Three ring sets, six compares and unknown hazards ([`frames-in-flight-2.md`](notes/frames-in-flight-2.md)). The
   workable form is a walker split: front parses, snapshots and publishes; back owns Vulkan. It depends on item 3, is the
   only way past main's ~8.7 ms pre-put chain, and is high risk.

## Todo

- [ ] 1. Half-rate shadows behind a key; user checks image and pacing at full resolution. Measure the 512 target the same way.
- [ ] 2. SPU flat dispatch table, coherence holders, inline store cleanup.
- [ ] 3. Direct vertex path (vertex pulling, SIMD index swap, cheaper descriptors).
- [ ] 4. Native c690 stages, one at a time on the corpus. Stop if the first stage is under 2x.
- [ ] 5. Walker split, only if the chain is still main-bound.
- [ ] Check RAM speed (`sudo dmidecode -t memory | grep -i speed`).
- [ ] Re-measure crowd and mound scenes: everything here is the meter only.

Expected on the 2600X: ~45-50 fps at the meter after 1-4 (est.). A locked 60 needs step 5 to succeed as well.
A newer CPU is untested but the L2-miss signature suggests it would help all three producers at once.
