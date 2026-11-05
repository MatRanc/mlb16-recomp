# Planning meeting: the fastest way to 60 fps with correct visuals (prepared 2026-11-04)

Target: 60 fps means 16.7 ms per frame in every scene, and visuals that look right. The aim is to get there in about
one week of agent time. This page is the prep for a meeting between the user and Claude; nothing on it is decided.

## 1. Where we stand

| Scene | Frame time now | What limits it | Source |
|---|---|---|---|
| Menus / start / team select | **~16.7 ms (60 fps)** since the flip-tick fix | was a bug, not workload | menu-34ms.md, menu-flip-fix.md |
| Pitch meter | p50 34.15 ms (~36 fps), p90 ~38, p99 ~41 | Edge (c690) burst ~33 ms on 5 SPU lanes | meter-p50-pin.md |
| Crowded play (fielding, zoomed-out replay) | 80-140 ms (7-12 fps) | renderer thread: ~1,300-1,700 draws at ~37-47 us/draw live | c690-serialisation.md |
| Batter cinematics | up to ~200 ms frames (user) | unknown; not yet broken down | user play test |
| Match load | ~2 s | first-use shader translation (cache helps on 2nd run) | hitch-census.md |

Live in the play build today:
- native Edge scheduling (fewer 40-50 ms meter frames);
- the menu flip fix;
- PIN=0;
- the shader disk cache;
- the native DXT texture compressor.

## 2. What we learned the hard way (process)

- **Offline replays misjudge the renderer.**
  - Live per-draw cost is about twice the offline cost.
  - Live texture time is 13-31 ms against about 3 ms offline.
  - Six renderer attempts were judged offline and none paid off live: per-draw caches, WCB prefetch, remap direct,
    multithreaded walker M3/M4, texture cuts.
  - Rule from now on: renderer work starts from a LIVE profile of the slow scene.
- **The biggest wins came from noticing a wrong number and asking why**: the 34 ms menu frames, the Edge job gaps, PIN.
  None of them needed new speed; each was a bug or a bad interaction.
- **Edge add-paths need captures that exercise them.** E1 passed offline with 0 triangles added, then drew giant
  occluders live (121k added triangles).

## 3. Paths to 60 fps, with rough cost (agent missions)

### A. Edge (pitch meter, cinematics; and crowded scenes once the renderer is fast)
- **E1 eye holes, repair:** the fused pass adds wrong geometry live. Next: capture a bad live pair, fix the
  vertex/index mapping, re-gate. 1-2 missions. Affects visuals, not fps.
- **E2 Edge compute, the meter p50 lever.** 0x21 transform is 25 %, 0x22 tree/cull whole body 21 %, 0x2A assembly
  13.5 %, cull 11 %. Two routes:
  - CPU-native whole stages (the partial ports were neutral);
  - GPU stages. The plumbing and exactness ledger are ready; 0x2A has 4+ record layouts. About 15-30 missions, roughly
    1-2 weeks.
  - Expected: meter 34 ms -> ~28 ms from a 2x cut of the top three; reaching 16.7 ms needs most of Edge on the GPU.
- Extra Edge lanes: parked (meter Edge is not lane-bound once the gaps are gone).

### B. Renderer (crowded scenes)
- **Step 0, live profile of a crowded frame.**
  - The user runs the zoomed-out replay with a sampling profiler (perf) on the walker thread and `PS3RECOMP_TEX_STATS=1`.
  - Answers: where do the live-only costs go (texture binding at ~7 us/bind x 2,300-3,400 binds, Vulkan recording,
    uploads)?
  - Cost: 1 user session plus 1 analysis mission.
- **Then fix the live costs.** Likely candidates:
  - per-bind descriptor/sampler work;
  - large 8 MB texture uploads (20-33 ms each);
  - 2-8 MB WCB readbacks (26-30 ms outliers).
- **Multithreaded walker** (paused at repair 230acd9f): exact, no hang, but slower and visually off. Resume only after
  the live costs are known.
- Needed to reach 16.7 ms crowded: walker 55-80 ms -> under ~14 ms, i.e. 4-5x. That is the hardest target on this page.

### C. "Menu-style" suspects: things that look wrong and may be cheap bugs
Each needs one recording (user-driven if not the meter) plus a per-frame anatomy, the same method as the menu fix.

| # | Symptom | Why it might be a bug and not workload |
|---|---|---|
| 1 | Live texture time 4-10x offline | Same work, very different cost. Something live-only (re-validation, descriptor churn, sync). |
| 2 | Live per-draw ~2x offline | As above, for the rest of the draw path. |
| 3 | Batter-cinematic ~200 ms frames | Never broken down. Could be shader compile, uploads, WCB, or a pacing bug. |
| 4 | 0.7-1 s present/pump stalls seen in several meter A/B segments | Host-side stall, not guest work. |
| 5 | ~4.3 ms ref-publication tail on menu frames (stops a clean 60) | Pacing / handoff, not work. |
| 6 | Boot SPU gaps of 237-654 ms | SPU asset loading on our runtime. Possibly a slow path. |
| 7 | 12 % of menu frames still 34 ms | Ticks landing after the 17 ms bound. |

### D. Visual bugs and whether they could also cost fps

| Visual bug | Known cause | Possible fps link |
|---|---|---|
| Missing jersey textures | not found | A texture that fails to resolve or decode every frame (miss path, retries) costs time every frame. Check `[tex-stats]` misses. |
| Missing team logos | the game never submits the draw. Guest-side; diagnostic run ready (CONFIG_MAIN.TXT StressTestUIElem). | probably none |
| Colour-buffer (WCB) issues | readback ordering / sync | Readbacks are already ~2 ms typical, 26-30 ms outliers in crowded frames. A correctness fix may remove waits. |
| White uniforms / lighting | not found | unknown; may be the same colour-buffer/readback path (exposure chain readback) |
| Eye holes | Edge cull grid | E1 fixes it at ~0 fps once repaired |
| Tiled logo pattern in transitions, crowd, 1080p | not found | unknown |

## 4. Proposed week (for discussion)
1. **Day 1:**
   - live crowded profile (user session, about 5 min) plus analysis;
   - E1 repair (offline, its own worktree);
   - jersey-texture triage using the same session's `[tex-stats]`.
2. **Days 1-3:** fix the top live renderer costs from the profile, each judged by the user's crowded fps. Run the
   menu-style suspect hunts (C1-C4) in parallel where they don't need the game.
3. **Days 2-6:** Edge E2 at the meter, starting with the biggest stage by clean ms (CPU-native or GPU, chosen by the
   first spike's numbers).
4. **Visuals alongside, prioritised by any fps link:** jerseys, WCB/lighting, then the logo diagnostic run.

Open questions for the user:
- Is 60 fps in every scene the bar, or 60 fps at the meter and menus plus "playable" crowded scenes first?
- How much user time per day is available for driven tests (crowded replay, cinematics, eye check)?
- Visual priority order.
