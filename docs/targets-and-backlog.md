# Targets and backlog (2026-11-02)

## Reference: RPCS3 on the same PC

Ryzen 5 2600X + RTX 3060 Ti, the MLB16 Online launcher's settings, 720p, Resolution Scale 100% (measured by the
user, 2026-11-02):

| Scene | RPCS3 | This port (build-play, WCB=2, CUBE_RT=1, PACE=1) |
|---|---|---|
| Menus, start screen | 60 (capped) | 42-46 |
| Pregame cinematics | 40-45 | ~30, with hangs |
| Pitch meter | 46-48, frame time ~20 ms (19-24) | 29-30, frame time 12-60 ms |
| Strikeout cinematics | ~45 | not measured |
| Zoom-ins / close-ups | 60 (capped) | not measured |
| Mound visit | ~25 | not measured |

RPCS3's SPU usage was 40-50% and its RSX thread 4-6%. On the meter we spend ~15-19 ms in the PPU main thread,
~17 ms in the guest SPU mesh producer and ~25 ms in the walker (see meter-pacing.md), so our overhead, not the game's
own work, is the gap. Targets: first match RPCS3 (46-48 at the meter with ~20 ms steady frames), then 60.

### The target is frame time, not fps (decided with the user 2026-11-02)

fps is an average and hides alternating short/long frames (ours swing between ~6 and 60-80 ms). Every A/B reports
the per-flip frame-time distribution at the meter, and the goal is stated on it:

| Metric (pitch meter, per game flip) | Now | Stage 1: match RPCS3 | Stage 2 |
|---|---|---|---|
| p50 frame time | ~34 ms | <= 21 ms | <= 16.7 ms |
| p99 frame time | ~60 ms | <= 28 ms | <= 20 ms |
| stddev | ~15 ms | <= 3 ms | <= 2 ms |
| frames outside 15-25 ms | most | <= 5% | n/a |

The Stage 1 numbers are provisional: the p99/stddev/outside-band values are estimates from the user's 19-24 ms
reading, not a measurement. Replace them with a MangoHud frame-time log of RPCS3 on the same meter scene. Pacing
alone can shrink the spread without speeding anything up (holding every frame to 33 ms gives stddev ~0 at 30 fps),
so the spread targets only count together with the p50 target. A change that raises fps but widens p99 or stddev is
not a win. The two halves have different levers: p50 comes down
by removing work (PPU, walker drain, c690); the spread comes down by fixing hand-offs and pacing (who waits for whom,
present timing).

## Backward frames (confirmed by the user, 2026-11-03)

User slow-motion video: some shown frames repeat an OLDER image, so motion steps back and forward (a batter's arm
swinging forward moves back a little for a frame or two, then forward again). Present everywhere (menus and game),
not in RPCS3, and has been there "since forever". This is the "garbled, jumpy" look, distinct from uneven frame
times. Two candidate classes: (a) whole-frame order: we present a stale display buffer (wrong flip buffer index,
PRESENT_NEW_ONLY/PACE queue, blit before the frame finished); (b) stale geometry: a draw uses last frame's c690 mesh
output (ring slot reuse/timing), so only animated meshes step back while camera/HUD stay correct. Check: log flip
buffer id requested vs blitted per present; dump consecutive presented frames and test monotonic motion; per-draw
c690 output slot/frame tag.

User follow-up from the video: the camera/zoom stays steady; the players AND the fans step back. So it is class
(b), stale animated geometry, not whole-frame order. Leads: the walker ignores GCM semaphore ACQUIRE by default
(`GCM_SEMA_ACQUIRE=0`, frame-timeline.md inventory), so a draw can read a mesh/crowd buffer before the SPU job that
fills it has written this frame's data; and any vertex/upload cache keyed by address without write tracking.

**Resolved 2026-11-03 (whole-frame order, not class (b)).** The user's newer
observation that the pitch-meter HUD and the camera also step back rules class
(b) out for the main symptom. `cellGcmSetPrepareFlip` returned `CELL_OK` (0)
instead of the display buffer id, so every flip named buffer 0 while the title
rendered alternately into buffers 0 and 1; a re-shown older buffer produced the
backward step. Fixed behind `PS3RECOMP_PREPARE_FLIP_RET` (on in
`play_windowed.sh`): 4.71 % -> 0.038 % stale-source presents and 219.9 -> 6.2
backward frames per 1000 moving frames. See
[`backward-frames.md`](backward-frames.md). The class-(b) leads are still worth
checking if any geometry-only regression remains.


**Review findings (2026-11-04, Claude):**
- FIXED 2026-11-05 (exp/capture-tools, docs/capture-tools-fix.md): 73deaefd frame-sampled walker profiler
  (DEFAULT ON) reported engine=0.000 on ~19 % of lines with real draws because it sampled whole frames from the flip
  counter while the timed work lives in a drain; an empty sampled drain could be paired with draws handled by
  unsampled ones. Sampling is now per drain, scaled by drains_total/drains_prof, and a report is deferred until a
  sampled drain carries draws (or the window is idle). Old log 322/1,698 bad lines; new live log 0/2,709.
  walker=/wait= semantics unchanged. New fixture test_walker_prof.
- FIXED 2026-11-05 (exp/capture-tools, docs/capture-tools-fix.md): crowded capture dropped ~440 of 1,702 draws
  (1,261 backend, 4,547 misses/frame). Three faults: M3's cold-start notes ran before the capture was armed and were
  cleared; the `.mem` writer extrapolated one EA across a range although the IO table maps main memory per MB; and
  the live frame can name a narrower span than a cold replay resolves. The capture now arms before noting, images
  every mapped main-memory IO page at begin, expands every main read to its whole 1 MB IO page, and resolves each
  page's EA on its own. Old capture: 1,261 draws / 4,547 misses. Same frame with the new page set: 1,694 backend
  draws (1,702 sink groups, the live count) / 0 misses. Live meter capture (one launch) replays 0 misses. New fixture
  test_walker_capture. USER still owes one crowded recapture with build-tools (command in the doc §5); gate
  draws/frame >= 1300, 0 misses, digest on. Nothing new is default-on.
- Not yet reviewed (all default OFF): WCB prefetch 097e6c46, flip deadline 2dbca8a2, remap direct 73deaefd, walker MT
  M1/M2 cbcd3289 and M3 749fd6c8 (review with M4 before any user test).

## Priority: speed and frame-time stability first

**CURRENT FOCUS (user, 2026-11-04 evening): renderer first (walker multithread M4 running), then back to Edge.**
- DONE 2026-11-04: 30-fps-cap investigation (docs/fps30-investigation.md) — no active cap; the sim dt is measured elapsed time (1/60 quanta + residual carry) and the title's own 30 fps mode stays off (Gfx flag 1, `cellPadGetInfo2.system_info` 0), so 60 fps is purely a speed problem. Do not set that pad bit.
- DONE 2026-11-04: menu flip fix PS3RECOMP_FLIP_TICK_OWN_STATE=1 is the play default (ca855f72 + dcdd6728; medium
  review ok: FIFO fall-through keeps the old status gate, 17 ms bound never completes an abandoned frame, all under
  FLIP_LOCK). User: menus hit 60 fps (recording 42.1 -> 59.4 fps). Meter A/B: fps 34.97 -> 36.22, p50 34.15 both.
  Bound 2 (33.4 ms) measured worse (p50 36.05, -6 fps) — keep 1.
- E1 LIVE FAIL (2026-11-04 23:00, bba9787a merged, key default 0): HOLES_NATIVE=1 draws giant brown occluder
  polygons / flying skin-textured shards (user saw it toggle with the 10 s arms; thumbnails B/B 30 vs A/A 2.2);
  121,242 triangles added live vs 0 on the offline corpus; p90/p99 +2 ms. Needs a live capture of a bad pair and the
  vertex/index mapping fixed before any eye check. Planning meeting prep: docs/plan-meeting-2026-11-05.md.
**User rule (2026-11-04 21:35): keep the ball rolling.** Walker MT paused after 4 failed rounds (repair: exact,
no hang, black field fixed, but walker 9.5 -> 12.8 ms and thumbnails differ 38.8 vs A/A 3.4; code kept on exp/live-ab
230acd9f). Now: single-thread texture cost (walker-tex-cost.md). IF IT FAILS: park the renderer track and move straight
to Edge E1 (eye holes), then E2 — no further renderer rounds without the user.
**Order agreed with the user (2026-11-04):** (1) walker MT M4 (running); (2) DONE 2026-11-05: Claude review of
M2-M4 + crowded-capture fix (full guest imaging) + walker-prof sampling fix (docs/capture-tools-fix.md);
(3) user recapture + crowded replay test (next); (4) M5 hardening ->
default if it wins (renderer track done); (5) then Edge E1, E2. E1 only jumps ahead if the renderer is blocked waiting
on the user.
**Edge queue when it resumes, in order:**
- E1. EYE HOLES FIX (user: "aren't we gonna get to that?" -- yes, Edge work, not parked): fuse the holes rule into the
  native 0x25/0x26 screen cull (one pass, conservative: never drop a triangle the 2x1 AA grid keeps). Blocker from
  edge-native-step4.md: the final kept count is written by the 0x132E0 caller, so the fused pass must model it. Goal:
  eyes fixed at ~0 fps cost (BYPASS=3 is -7.6 fps, union -4.8). Can run right after M4 (both rebuild build-play);
  user checks the eyes visually. Until then BYPASS stays off (user, 2026-11-04).
- E2. Edge compute for the meter p50: 0x22 whole body, 0x21, 0x2A (CPU native or GPU), per the decision below.
**FOCUS history (user, 2026-11-04): Edge (c690) work only; everything else waits.** Order:
1. SPU codegen: inline acyclic direct callees (spill cut) FAILED its gate (spu-inline-callees.md).
2. GPU Edge stages via the plumbing (edge-gpu-plumbing.md): 0x2A 32-byte record class + walker GPU-buffer binding,
   then 0x21 sel7 (A3E0/BB98), then 0x25/0x26 cull. Exactness policy and per-stage status: edge-exactness.md.
3. 0x22 whole-body native (0x10190 command body, 21.2 % of c690 inclusive time; partial 0xFDA0 port is neutral).
- 2026-11-05: 0x2A position-first 32-byte class exact on the GPU: 51/51 clean
  meter pairs, 20,312 records, 0 position bytes, <= 1 LSB packed fields, SPU
  attribute word exact, ring position/attr exact. Coverage 60/192 (31.2 %) of
  meter 0x2A pairs -> fallback 68.8 %, so no live A/B; there is also no
  `PS3RECOMP_C690_GPU_2A` key/walker binding on any branch. Next: walker
  binding + live key, then the 20/48/other 0x2A classes, 0x21 sel7 (4.74 ms),
  culls (3.03 ms); 0x22 (5.80 ms) is already CPU-native.
  Details: [`edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md).
- 2026-11-05: **0x2A record classes decoded and kerneled (offline).** Ten
  layouts; seven class kernels gate **131/131 classified meter pairs**
  (45,440 records, positions 0, packed <= 1 LSB, SPU bytes untouched, no
  outside writes) = **131/192 = 68.2 % coverage**. The 9 packed-offset 32-byte
  pairs are included. Fallback 31.8 %: 31 first-in-job pairs enter the low-LS
  SPURS runtime (16.1 %), 19 outputs write no positions (9.9 %), 11 rare
  64/40/16/4/112-byte layouts (5.7 %). `0x00A00004` is 26 %, not 51 %.
  Details: [`edge-gpu-2a-classes.md`](edge-gpu-2a-classes.md).
- 2026-11-05 (later): **0x2A binding + live key delivered, default off.**
  `PS3RECOMP_C690_GPU_2A` exports the A/B class, runs the 32-byte Vulkan kernel
  on the renderer's device and serves the range through the draw-engine
  resolver. Offline gates pass (resolver transparency hash MATCH, compact
  packing PASS, 51/51 kernel, unit/fixtures/ABI). **Live A/B blocked:** 2/2
  launches aborted with a heap corruption at the key-on switch; no numbers.
  The SPU pack is not skipped yet (the handler reads back its records). Next:
  isolate the live crash, then replace `0x10C70`/`0x109C0`, then 0x21 sel7
  (4.74 ms) and the culls (3.03 ms).
  Details: [`edge-gpu-binding.md`](edge-gpu-binding.md).
- 2026-11-05 (later): **native 0x2A field pack delivered, default off**
  (`PS3RECOMP_C690_NATIVE_2A`, `exp/native-2a`). It replaces the four
  lifted field passes (`0x7840`/`0x7FC0`/`0x7AC0`/`0x7540`), not
  `0x10C70`/`0x109C0` (those parse the descriptor). Offline
  **112/131 classified pairs (85.5 %) policy-exact**, 19 attribute-first
  fall back byte-identical. Live A/B neutral: 0x2A 21.92 -> 21.59 us/call
  (-1.5 %), p50 34.15 both, fps paired -0.59 +/- 2.75. Offline the
  20-byte/position/44-byte classes speed up 22-43 % but the largest
  32-byte class is 11 % slower. Conclusion: a GPU pack replacement can
  save at most ~0.2-0.5 ms/frame while its hand-off costs 5.96 ms/frame;
  0x21 whole-command is the next lever. Details:
  [`edge-native-2a.md`](edge-native-2a.md).
- 2026-11-05 (later, crash fix): **the live heap corruption is fixed.** The
  lane key was `ctx->spu_id % 16`, but the c690 jobs run through
  `spu_run_spurs_job`, which builds every lane's context with `spu_id 0`; all
  five lanes shared one export slot (run-1 log: `worker=0..4 spu=0`). The core
  shows the abort in `vk_c690_prepare` freeing an already-freed job buffer.
  Lane state is now `SPU_THREAD_LOCAL`; the live key uses relaxed `__atomic` loads/stores; the
  seed-fallback and requeue lifetimes are fixed. A new 5-lane/1-drain corpus
  harness (`tests/test_c690_gpu_2a_race.c`) reproduces the old bug under ASan
  and is clean under ASan/UBSan and TSan. **One live pitch-meter A/B, no
  crash:** A 30.38 fps / p50 34.23 / p90 43.90 / p99 48.24; B 22.93 / 34.16 /
  68.60 / 72.98. The overlay costs **5.96 ms/frame** (submit+fence, 2.41 ms
  per drain) and cannot gain while the SPU packs. Next: replace
  `0x10C70`/`0x109C0` **and** pipeline the fence (timeline semaphore); the
  0x2A saving is ~1.07 ms at 31.2 % coverage, ~2.3 ms at the 68.2 % now
  classified.
  Details: [`edge-gpu-binding.md`](edge-gpu-binding.md) §7.
- 2026-11-05 (later, async hand-off): **the c690 compute runs on a dedicated
  compute queue with a timeline semaphore; the walker never waits.** Submit is
  **0.061 ms/frame, wait 0** (target <= 0.5 ms), 4,525 batches over 1,104
  key-on frames; the resolver serves the exact SPU seed until the timeline
  value completes and never blocks. But p90/p99 still rise (46.4/50.9 vs
  38.8/42.6 ms) because each compute-queue submit costs ~1.9 ms of graphics
  scheduling; p50 34.65 vs 34.15, fps 27.6 vs 37.1. Next: record the dispatches
  into the frame's command buffer (no separate submit) and have the draw
  consume the GPU range after the pack replacement; then the other six classes
  (68.2 % offline), 0x21 sel7 (4.74 ms) and the culls (3.03 ms). Default stays
  off. Details: [`edge-gpu-binding.md`](edge-gpu-binding.md) §8.
- 2026-11-05 (later, diagnosis): **the key-on loss is the GPU hand-off, not the
  submit call.** Four arms in one clean launch (A off / B export / C resolve /
  D full): walker +1.39 / +1.94 / +5.88 ms; D-A p50 +6.23 ms, fps -9.75.
  Submit is 16.3 us, 0.076 ms/frame, wait 0; staging 0.29 ms, export
  0.10 ms/lane, resolver probe ~0.02-0.1 ms after a new 4 KiB page filter
  (offline entries/probe 50-100 -> 0.02-1.7). The 0x2A slice is still
  1.07/2.3/3.43 ms (31.2/68.2/100 %), so the binding cannot win until the
  compute is recorded in-stream and the pack is replaced. Default off.
  Details: [`edge-gpu-binding.md`](edge-gpu-binding.md) §9.
- DONE 2026-11-04: native c690 dispatch SPURS_PM_NATIVE_DISPATCH=6 + FAST_RESCHED=64, play default (medium review ok:
  lock order si->lock -> registry, fairness yield to other workloads, 256/residency cap). Meter: fps 34.00 -> 35.33,
  p90 43.2 -> 39.2, p99 46.7 -> 42.7, 40-50 ms share 24.3 % -> 7.1 %, p50 34.15 unchanged. Needs user play test
  (audio, start screen, cinematics, in-play).
- MEASURED 2026-11-04 (docs/meter-p50-pin.md): the 34.15 ms p50 is the **c690 producer, not flip/vblank**. With
  native on, a 25 s meter timeline gives frame p50 34.15, c690 union p50 32.01, last c690 job p50 32.97; the extra
  ~1.2 ms is the walker drain / ref-publication tail. Capping the flip-done wait to the guest's exact 2 x 16683 =
  33.366 ms (new live key PS3RECOMP_FLIP_DEADLINE_US, default 0) is **neutral**: A/B p50 34.15 vs 34.15, p90
  37.93 vs 38.45 (B hit three 0.7-0.9 s present stalls). Fast frames (<20 ms) are 31 % of the meter, so there is no
  fixed 30 fps cap. Native dispatch bought the 40-50 tail, not the median; Edge compute is still the p50 lever.
- DECISION: if the meter p50 is the target, continue Edge compute (0x22 21.2 %, 0x21 emit 25.0 %, 0x2A 13.5 %; a 2x
  cut of those is ~5-7 ms of burst, p50 ~28-29 ms). If the real-play 40-80 ms frames are the target, the renderer/
  walker is the lever and the meter median stays 34 ms. The remaining Edge GPU/native items are parked below.
Every mission ends with a meter A/B number; no mapping-only missions unless an A/B result makes no sense.
Long frames (40-80+ ms): the replay/real-play long frames are walker-bound (docs/c690-serialisation.md); meter
long frames are c690 (docs/long-frames.md).
Parked until the user says otherwise: team logo (offline follow-up done, user diagnostic run ready in
team-logo.md §8.3), visuals, HLE batch 2, review nits.

Decided with the user 2026-11-02: visual bugs are logged here and parked; work goes to frame rate and steady frame
times until we match RPCS3.

## Work queue (kept current; nothing gets dropped)

Updated 2026-11-03 afternoon. One game-running mission at a time.

**Running**
- None. HLE fix batch 1 landed (audit #9 lwcond, #6 audio pacing, #11 cellFsWrite, #2+#3 SPURS shutdown/error
  base, #7 GCM offset tables), switches on in the play scripts; the flip-deadline investigation is done.

**Speed: steady frames (meter)**
1. ~~The 33 ms deadline: per frame, did `Gfx_DisplayFlip` (0x000692dc) leave its wait by the flip handler or by the
   2x16683 us timeout, and what was still running then (walker, c690, present)?~~
   **Done 2026-11-03** ([`flip-deadline.md`](flip-deadline.md)): 28.6 % of frames leave by the deadline, present
   queued p50 0.35 ms, completion beat delayed by the walker drain (tick and drain share a thread). Earlier
   completion (`FLIP_ON_DONE`) and the status-preserving candidate both lose; both default off. Follow-up: decouple
   the display tick from the walker drain (separate tick thread or a tick checkpoint) without exposing the
   buffer-ref wait.
2. Native Edge rewrite of the c690 stages (decision with the user; edge-native-step2.md has the algorithm).
3. Walker plan leftovers: readback cuts (#2), Vulkan recording (#3), GPU vertex conversion (#7), parse/record split.
4. Untested: PIN=0 vs PIN=1; SPU_CODE_DMA_MEMO measured neutral (off).

**Hitches (100-160 ms frames, startup/cinematics/loading)**
5. Hitch census: DONE (docs/hitch-census.md). Cold run: 824 frames > 50 ms. Cinematics 798 (c690, same lever as
   the meter), boot 18 (353-589 ms, SPU asset jobs), load 1 (5.1 s: 2.9 s our shader translation, 0.25 s driver
   pipelines; file I/O is async and negligible), meter 7. Menus 0.
6. Shader disk cache: DONE, default on (docs/shader-disk-cache.md). Load freeze 5.1 s -> 2.06 s warm. Review
   fix: hash seeded with the translator build stamp (stale entries after translator changes). Open review nits:
   orphaned .tmp files and no pruning (dir grows across versions), key includes render state (duplicate files),
   static s_eng_pipe_key handoff, mutex held across file I/O. Remaining ~2 s load = walker drain (3,484 draws) +
   texture uploads. Hitch recorder on in play tests (hitches.pstl + tools/hitch_table.py).
6b. Boot 0.35-0.6 s stalls: SPU asset jobs on the spurs pm lanes while the main thread usleeps; not yet explained.
6c. Warm census run "stuck in cinematics": user watched it; it was the idle b-roll after a nav miss (X landed on
   the pitch view, not the meter). Harness nav issue, not a game loop.
6d. Visual: "cheese-grated" holes in some meshes (user, 2026-11-03; long-standing). Check whether Edge's small/no-pixel
   triangle culling uses a viewport size different from what we render at (would also explain seams when upscaled).
7. Menu lag / CPUs at 70-90 % on menus: census saw no menu frames > 50 ms; recheck in the user's windowed play test.

**Play test 2026-11-03 16:37 (user, play_20261103_163747)**
- Fixed per user: main menus responsive, clicks register; frame jitter (backward frames) gone. ~35 fps avg in game.
- Start menu laggy on the Start press.
- Team select: d-pad up switches team, then ~1 s with no input accepted while frame times stay steady (<=38 ms).
  Possibly the same asset path as the missing floating team logo.
- 3.0 s freeze mid-game right after an out at first (MangoHud, ~187 s after launch); 5.06 s at match load;
  0.33-0.39 s frames in game. Running: shader disk cache mission + hitch recorder for play tests.
- No audio heard on headphones; log has no [cellAudio] lines. Running: static check (edge/holes/audio mission).
- White pants on pitcher, catcher, batter, umpire (lighting/colour, known). Mesh holes around the pitcher's eyes,
  same as RPCS3 when upscaled (see 6d).

**Next user play test: prepared probes (docs/edge-holes-native-plan-audio.md)**
- Audio: `PS3RECOMP_AUDIO_DIAG=1 ./play_windowed.sh` (SDL device/spec + per-second mix line). The game does open
  cellAudio and our SDL backend opens a device, so the silence is downstream (sink routing or PCM/read index).
- Mesh holes: `MLB16_CULL_BYPASS=1` (all Edge screen culls off; 2 = 0x2C only, 3 = 0x25/0x26). Holes gone => Edge
  sub-pixel cull vs the 2x1 AA sample grid. (Our size/pixel centres match the game's 1280x720 / 2560x720.)
- Native Edge: the step-2 wedge is a coverage/switch bug (fixable), not a dead end. Kernel bit-exact but 1.30x.
  Plan: prove the live class, then hand-write to ~5x or start with unpack/pack (23.5 %). Non-native levers: 1-3 ms
  each (AVX2/FAST_LS, PGO), more lanes 0-1 ms. Decision with the user.

**Team select / logos / Start (docs/team-select-and-logos.md, static)**
- Team-switch ~1 s input lock: no stall in the run (max frame 41 ms, no I/O); likely the game's scripted wheel
  transition with the missing logo hiding the cue. Pad probe if it persists: `PAD_ALWAYS_CHANGE=1`.
- Missing floating 3D logo: not a failed load; the FIFO jump-park resync drops its draw range. Probe:
  `GCM_PARK_GRACE_MS=600000` (or `PS3RECOMP_WALKER_RESYNC_FIX=1`). Fix: never abandon a park whose skipped range
  has draw/state commands.
- Start press lag: frontend construction (up to 887 ms frames, walker idle) = guest PPU work + ~27 pipeline
  creations. Recheck with the shader disk cache warm; if unchanged, profile the frontend PPU path.

**Play test 2026-11-03 20:26 (user, play_20261103_202606; CULL_BYPASS + AUDIO_DIAG + PARK_GRACE)**
- Audio: works (SDL/pipewire 48 kHz, ~188 blocks/s). Earlier silence not reproduced.
- Mesh holes: GONE with MLB16_CULL_BYPASS=1 -> Edge triangle culls confirmed. Fix inside native Edge (step 3).
- Team logo: still missing with GCM_PARK_GRACE_MS=600000 -> the park/resync theory is wrong or incomplete.
- Start-screen lag: 14 hitches 0.28-0.95 s at 21-34 s, all spu.run (boot SPU work). Static mission running.
- Match load 5.55 s (cache was emptied by the review fix; expect ~2 s next time). 2.05 s in-game hitch at 217 s
  (spu.run). In game p50 35.5 / p90 61 ms with bypass vs 32.9 / 44.5 at 16:37 (different scenes; not clean).
- Low priority (user): throw-power build-up (hold a face button) fills much slower than on PS3; per-frame or
  per-poll? Note only.
- Native Edge: APPROVED by the user (20:30). Step 3 mission running (wedge fix, holes fix, speed).
- Process: walker-cuts-2 mission hung at OpenCode startup 17:24-20:23 (no log); relaunch after native Edge.
  Check each launch is alive a few minutes in.

**Boot SPU / replay / logo (docs/boot-spu-hitches.md, static)**
- Start-screen and instant-replay freezes = the game's DXT texture-compression jobs (Wws_Job/DXTJob), pure lifted SPU
  compute on 4 lanes (title sets maxContention 4; 2 of 6 lanes idle; host CPU 26-36 %). Next: probe
  PS3RECOMP_SPURS_MAXC=6 (cheap, needs an HLE switch + one start-screen run); bigger: native DXT encoder (pure
  function, ideal native candidate; check output matches the game's).
- 2.05 s in-game freeze at 217 s = instant replay / depth-of-field camera: DXT + synth + MP3 + 192 cold shader
  translations (the cache was cold that session).
- Team logo: park/resync theory DISPROVED (0 resyncs in either run). Next: FIFO dump of team select +
  tools/replay_rsx, then state bisect (ENG_TEXMEMO=0, ENG_FSBIND_ALL=1, ENG_DEPTH_VIA_GUEST=1, GCM_SEMA_ACQUIRE=1).
- Team logo capture (2026-11-04, docs/team-logo.md §5): the logo is NEVER SUBMITTED to the RSX (657 draws, all quads, 0
  triangles, no offscreen RT). Cause is guest-side: FUN_009d7b30 target-texture lookup FUN_009d4c68 returning 0, or
  the script never creating the ScriptUIElem. Next: USER diagnostic run (BOOT/CONFIG_MAIN.TXT with StressTestUIElem,
  grep 'Did not find target texture'; else PPU_WWATCH=0x013560d8). Reviewed 2b16133a (tool fixes): ok.
- Throw/pitch meter fill is per guest frame (no wall-clock pacing): at ~30 fps it fills at about half the 60 fps
  rate. Fixed by raising fps; no separate fix.

**Native Edge step 3 (docs/edge-native-step3.md)**
- Wedge FIXED (native stage didn't publish the return PC): 130,304 live native calls, 0 mismatches, no wedge.
- Speed: native 0xFDA0 stage NEUTRAL at the meter (-0.32 +/- 1.97 fps; p50 34.15 both). Off by default. Next speed
  stage per plan: unpack/pack (23.5 %), or hand-tune the cull to ~5x.
- Holes: the 0x25/0x26 screen cull. Cull transform {595, 640, -335, 360} for 720p; scaling to the 2x1 sample grid
  keeps some triangles but drops others (a separate fixed bound). Open: find that bound with the exact 400-pair
  replay gate; accept only changes that never reduce the kept triangles. USER CONFIRMED 2026-11-03: MLB16_CULL_BYPASS=3 fixes the holes, =2 does not (0x25/0x26 cull). Was: (should fix)
  vs =2 (control).
- Review of 8a082ddb (all default off): with CULL_FIX and the spool both on, the spool records the patched
  transform (enter/leave order in c690_cull_fix.c:162 / patch_cull_hooks.py:38) -> fix before measuring the fix;
  cull_live_get ignores the env fallback when live-ctl is linked but unseeded (c690_cull_fix.c:38); cull pair cap
  can overshoot by the lane count, cull_f never closed (c690_corpus.c:464).
- Holes fix (docs/edge-holes-fix.md, branch exp/edge-holes 1d3633aa, offline): the 0x25/0x26 cull tests in 1280x720
  logical space with a fixed w/720p edge, the target is 2560x720 2x1 AA. Fix = UNION (original list untouched + the
  triangles a 2x run keeps). 400-pair gate: 0 removed, 7 added (meter corpus has no eye close-up). NOT in build-play
  yet: needs merge into exp/live-ab + re-patching the generated module wrapper + rebuild, after the DXT mission.
  Until then PS3RECOMP_C690_CULL_FIX=1 runs the OLD unsafe single-grid fix: do not use. Fallback if the union
  doesn't remove the holes: full candidate superset (= BYPASS=3 without editing the original list).
  Review fixes from 8a082ddb: done in 1d3633aa.
  Review of 1d3633aa: append cap = entry count at +0x114, not a proven list capacity. Before merging, confirm what
  +0x114 holds and the real index-buffer size (Ghidra/LS layout), so the append can neither overflow LS nor
  silently drop the added triangles. Also count skipped unions (range/realloc fail) instead of silent skip.
- 2.2 GB of stage pairs under recomp-work/ from run 2 (delete once no longer needed).

**Native DXT (approved by the user 2026-11-03; docs/dxt-native-step1.md)**
- Phase 1 (offline) found the PPU side (GpDxtCompressor FUN_00a566e8, 0x30-byte params, DXT1/3/5 formats) but could
  NOT tie the SPU kernel to a module offline: DXT spans load no registered jobmod; candidates are jobmod_96de, c566
  or an unlifted base-0 module at 0x010A1700. Next (needs the game, queued right after native Edge): one capture run
  (SPURS_PM_TRACE + a one-shot wid==0 hook dumping the job's code EA and input/output blocks), then the bit-exact
  native encoder + harness in worktree ps3recomp-dxt (branch exp/dxt-native).
- Phase 2 capture (docs/dxt-native-step2.md, b96b2122): the DXT kernel is EBOOT 0x010A1700, UNLIFTED, running
  through the SPU interpreter rejoin -> likely why boot/replay freezes are so long. 93 MiB corpus in
  recomp-work/dxt_cap1/ (841,340 blocks; DXT1 + DXT5). Review of b96b2122: inert when off; capture-only nits
  (first 0x4000 bytes scanned, soft run cap). Running: lift mission (lift 0x010A1700, bit-exact gate, start-screen
  A/B), then the native encoder.
- DXT LIFTED (docs/dxt-native-step3.md, 750cb5e9, default on): start-screen freezes >250 ms 11 / 7.6 s -> 1 / 0.31 s;
  DXT lane time 11.7 s -> 0.87 s; offline gate 890,688 blocks, 0 mismatches. Native encoder parked (lift covers it).
  Review (medium): fixed in ddbce4f3 (sig overlays had enabled = 0; a disabled gated region fell through to stale
  code). Open nits: lift_jobmods.py now accepts every base-0 module (a full regen would double-register DXT ungated
  and shift image ids: pin it); PIC call-stub seeds hard-coded from capture; g_spu_interp_steps not thread-local;
  capture hook on the central lookup path (weak call when off).
  User check: start screen + instant replay.
- Holes union MERGED (283c2d86, default off): cap proven (+0x114 = candidate count, buffer end at +0x1E0); gate 0
  removed. Live meter: p50 same, but -3.7 fps (heavier tail; double cull + LS snapshot). User check of the eyes:
  PS3RECOMP_C690_CULL_FIX=1. If it fixes them, the cheaper version belongs in native Edge (cull once at 2x-aware
  rules) rather than running the lifted cull twice. Dispatch fix ddbce4f3 verified live (audio 187.5 blocks/s, no
  sig-overlay interpretation, start screen 0 frames >= 250 ms). SPU unit tests 11/11 again (stubs for DXT hooks).
- walker-cuts-2 (docs/walker-cuts-2.md, ec4c75cf): REMAP_FAST memo on by default (offline -0.51 ms, live walker
  -0.27 ms) but the frame does NOT move (p50 34.16 both, deadline frames 456 vs 455). The walker is not the p50 lever:
  frames end on the c690 producer burst (frame-timeline-2.md). Tick-in-drain checkpoint built, default off; the
  marker-side design (complete at the next frame's flip marker) needs one A/B. ab_live.sh PITCH_THRESH 25 misses the
  meter (use 35). => Speed priority is now c690 / native Edge.
  Review of ec4c75cf (medium): REMAP_FAST memo stays correct (key + generation checked on every hit); play scripts
  do set it (the "not in commit" note is because the scripts live outside that repo). Must fix before any
  TICK_IN_DRAIN use: presented_seq is stamped at take time, not with the queued image's seq (cellGcmSys.c:204,
  :1789) -> can complete a flip that was never presented; completion logic copied 3x (share a helper); test misses
  the marker path. Minor: memo hash collides on swapped base/id; 16 KB memo in every remap struct.
- Cull bypass A/B (docs/cull-bypass-ab.md): BYPASS=3 -7.56 fps (2.07x vertices, walker drain +6.1 ms), union
  -4.83 fps. Neither kept. Holes fix moves into the native cull (one pass, holes rule built in): Edge step 4 running.
  Note: baseline A ran at 35.35 fps with PIN=0 (ab_live default) vs ~32-33 in PIN=1 A/Bs -> PIN=0 vs 1 A/B is worth
  doing (Speed item 4).
- Edge step 4 (docs/edge-native-step4.md, b14617a5, default off): native screen cull 0xD080, exact on 400/400,
  884k shadow calls 0 mismatches, 1.53x per call; live neutral (B-A -0.04 / +0.21 fps). Union mode still ~-5 fps (the
  final count is written by the 0x132E0 caller, so a fused pass needs that modelled). c690 lane busy ~0.54 while the
  burst wall ~36 ms ~ frame: the burst looks latency-bound, so faster stages alone don't move the frame. Running:
  PIN A/B + burst critical-path analysis before more native stages.
  Review of b14617a5 (all default off; fix before any default-on): run_mode0 skips the host_depth/spu_img_restore
  bracket (c690_native_screen.c:219); shadow PC check compares r0 with ctx->pc after spu_drain_call (false
  mismatches possible) and never compares the published pc (:286); non-atomic env/dump/progress statics (:84);
  native mode 2 enables the union for 0x25/0x26 but not 0x2C (c690_cull_fix.c:106).
- PIN + burst path (docs/pin-and-burst-path.md): PIN=0 35.31 vs PIN=1 30.98 fps (+14 %); play_windowed was already
  effectively PIN=0, now explicit (Speed item 4 DONE). c690 burst (35 ms): ~70 % job compute, ~30 % per-job
  dispatch turnover (host 5.0, head 2.6, tail 3.0 ms), 5 lanes balanced, no serial chain. Levers: gaps -5..-10.6 ms,
  sixth lane -5.8 ms, native unpack/pack+skin+normalize at 1.4x -3.2 ms, native cull alone -0.3 ms. Running:
  dispatch cuts + sixth lane.
- c690 dispatch cuts (docs/c690-dispatch-cuts.md, 84e5dd06, default off): host gap -61 %, burst wall 34.25 ->
  30.99 ms, occupancy +6 pts, but fps -2.0 (other workloads delayed) and p50 still 34.15. Sixth lane: guest
  nSpus=6, MeshJobs maxContention=5; SPURS_LANE6 wedges in ~10 s. Not kept.
  Review of 84e5dd06 (only matters if a key is ever turned on): FAST_GEN epoch race with cellSpursShutdownWorkload
  (epoch bumped after shutting_down = 1); FAST_ACTIVE uses `active > maxcont` with a workload-wide active count;
  ONE_LOCK inlines spurs_pm_fast_consume (may drift).
- KEY FINDING: p50 is pinned at 34.15 ms (two 16.68 ms periods) no matter how much work we cut. The next lever is
  the flip/present pacing floor, not more work cuts. Running: frame-pacing-floor mission (present mode, tick
  marker-side completion with the presented_seq review bug fixed first).
- Frame pacing (docs/frame-pacing-floor.md, d686e621): the 34.15 ms p50 is NOT a present/tick rounding grid. Hard
  frames = the c690 burst (lane union p50 32.8 ms) + ~3 ms, ending at the game's own flip deadline (~34.1 ms).
  Present runs on its own thread and never blocks the guest (present mode not a lever). TICK_IN_DRAIN -8.4 fps,
  FLIP_FENCE_DONE -9.4 fps (guest then waits on the buffer ref). Flip completion now one helper; presented_seq
  identity fixed. Title has no in-FIFO flip marker. Review: default tick path unchanged; fence-gate nits (marker seq
  slot order, req_ref 0 when toggled live, baseline mismatch with 2 requests) only matter if FENCE_DONE is used.
  => To move p50 the c690 burst must drop well below ~30 ms (cut compute), without delaying other workloads.
- Edge step 5 (docs/edge-native-step5.md, 2638108b measurement only): at the meter the c690 wall is 42.9 % command
  0x21 (handler 0x13080 "emit/ring", 102 us/call), 0x2A assembler 15.7 %, 0x13 copy 9.7 %, cull 12.7 %, 0x0A 7.5 %.
  The old stage shares were from another scene. Per-stage native SIMD tops out ~1.0-1.5x (compiler already emits the
  same AVX2). Running: is 0x21 compute or a WAIT (ring space / label / reservation / DMA)?
- 0x21 emit/ring (docs/c690-emit-ring.md, 8043db5e): pure COMPUTE (0 DMA, 0 reservations, 0 waits). Two float
  transform/pack kernels (sel 1: 390 us/call, sel 7: 52 us/call). Native at 1.3-2x would save ~3-5 ms of burst.
  8043db5e NOT pushed: GitHub returned HTTP 500 on `git push fork exp/live-ab` (3 tries 04:4x); retry later.
  Running: static SPU codegen audit (is lifted code slower than a real SPU, and why?).
- SPU codegen audit (docs/spu-codegen-audit.md, static): lifted c690 runs ~1.03 ns per SPU instruction vs a real
  SPU's 0.31 ns (~3.3x slower); 8.4-12.7 x86 per SPU instruction, 26-37 % of it GPR/stack spills. Realistic codegen
  gain 1.2-1.4x (spills 15-25 %, call dispatch 5-10 %, byteperm/masks 3-6 %). port-local-regs, port-spu-int-simd,
  port-spu-runtime-cuts are already merged; port-lockline-stripes is neutral.
- SPEED DECISION FOR THE USER (2026-11-04 morning): the meter frame = the c690 burst (~33 ms), pure compute, on the
  5 lanes the game allows. Neither per-stage native (<= 1.5x) nor codegen (1.2-1.4x) alone gets near 2x. Options:
  (a) codegen work (spills) for all SPU code, 1.2-1.4x; (b) native emit kernels 0x21 sel 1/7 (~3-5 ms);
  (c) GPU offload of Edge geometry (c690-offload-decision.md); (d) run more c690 jobs in parallel than the guest's
  5 lanes without the guest knowing (SPURS_LANE6 wedged; needs a different design); (e) combine a+b.
- Review-fix batch 1 (docs/review-fixes-batch1.md, 16fc2d28): sticky ferror, live fs switch, lwcond tombstone,
  shader cache tmp sweep / trailing bytes / I/O outside the lock. Medium review: tmp sweep deleted ANY old *.tmp in a
  possibly user-supplied dir -> fixed (own names only, test added). Open nits: clearerr hides an earlier failed
  write's lost bytes (game sees CELL_OK on the next write); lwcond tombstone still ambiguous on Windows (32-bit long);
  fs_write_check_on per-TU cache and live key now overrides env in cellFsUtilWriteFile; shader cache: duplicate
  translation on concurrent misses, rejected load leaves junk in the caller buffer, lock taken 2-3x per load,
  Windows remove+rename vs unlocked readers.
- Game queue: speed paused for the user decision above; offline review-fix batch running. Team logo: static analysis + a capture command for the user to drive (no automated menu runs). (meter + cinematics) -> team-logo FIFO dump -> tick marker-side A/B.

**Review follow-ups (low code review of 7a3b38e4, batch 1; none urgent)**
- ppu_fs.cpp:572 / cellFs.c:586: ferror() is sticky; after one failed write every later write on that fd returns
  EIO. clearerr() before the write.
- cellFsUtility.c:92: reads PS3RECOMP_FS_WRITE_CHECK directly, ignores the live key; use fs_write_check_on().
- lwcond_real.h:99: tombstone (-1) matches guest address 0xFFFFFFFF; exclude it.
- (Checked, not a bug: the SDL audio room check paces on the device queue; no device falls back to clock pacing.)
- Not reviewed: ppu_sysprx.cpp past line 163 (output limit).

**Correctness**
8. HLE fix batch 2: audit #1 Bink task attribute, #4 cellAudioOutGetState layout, #5 cellAdecOpenEx, #8 fs
   rename/truncate root, #10 cellSearch, #12 save delete, + the remaining audit findings.
9. Controller presses ignored under load (re-check after batch 1 and the flip fix).

**Also tracked (from earlier plans; lower priority or needs a decision)**
- Codegen/build: dead XER.CA elimination (NHL #272), SPU_FAST_FLOAT trial (risky, exactness), PGO on the current
  build (+5.8 % measured on an older build), call-bracket inlining (NHL #138).
- Stale geometry: GCM_SEMA_ACQUIRE is off by default; check whether any draws still read a mesh/crowd buffer before
  its SPU writer finished (residual after the PREPARE_FLIP fix).
- Bink SPU program spinning ~99 % of a core with no movie (progress.md 10-27); job manager thread ~84 %.
- Harness parity: ab_live.sh should take its defaults from play_windowed.sh (today two were missing).
- Menu fps vs 11-02 (42-46 on the start screen then; user saw ~20-30 today): check whether it is the honest count
  after PRESENT_NEW_ONLY/PREPARE_FLIP or a regression.
- Lighting: lighting-drift-notes.md "next checks" (black sky-light reading, cube fallback after scene change).
- Long term: native renderer (engine-level hooks, walker as reference), upstreaming fixes to ps3recomp.

**Visual backlog (parked)** - see below: jerseys, team-select 3D logos, lighting changes, crowd patch, tiled wipe,
1080p/300 % scale.

**User checks**
- Play test after each batch that lands in play_windowed.sh (what to watch is in each mission's report).
- RPCS3 MangoHud frame-time log on the meter (to replace the provisional spread targets above).

## Visual backlog (parked)

- **Bink video / team logo ROOT CAUSE FOUND 2026-11-05 (offline-tested, live
  verification pending).** The intro `.BIK` opened on fd 12 with `cellFsOpen`,
  but the Bink SDK's default PS3 IO reads it with raw `sys_fs_read/lseek`; the
  runtime had two fd tables, the read returned `EBADF`, and BinkOpen reported
  "Not a Bink file." with no read. Fixed by bridging the fd spaces plus the
  Bink SPU task-attribute descriptor. Details and the ~2-min user check:
  [`bink-video.md`](bink-video.md). Same path as the team-select `BINK_LOGO`.

- **Depth of field (close-up/replay blur) PATCH IMPLEMENTED 2026-11-05
  (key off by default).** `PS3RECOMP_DOF_PATCH=1` applies the user's RPCS3
  `MLB16_dof_fix.yml` bytes at ELF load: the 0xA90-byte 17-tap depth-aware
  program to the unused gap at 0x10A4400, both program headers repointed.
  EBOOT fingerprints are checked and a mismatch refuses. Offline: 148
  instructions, no unhandled opcode, SPIR-V compiles; no capture contains the
  DOF pass. Key-on meter launch applied cleanly (first A/A segments
  33.4/36.2/34.3/36.7 fps; later segments contaminated by another mission's
  build). Next: user visual check in a replay/close-up, then decide on a play
  default. Details: [`dof-patch.md`](dof-patch.md).
- **Jerseys FIXED 2026-11-05 (key off by default).** The torso was not a
  texture: character fragment programs were dropped because they use
  `DDX`/`DDY` + gamma `TXD`, which the decompiler did not translate. Implemented
  `TXD`/`DDX`/`DDY` behind the live key `PS3RECOMP_FP_TXD` (default off);
  key-off replay digest unchanged, key-on restores **23 draws/frame** and the
  jersey. Meter A/B: fps 37.88 -> 36.38, p50 34.15 both, walker +2 ms -- the
  cost of the draws that were missing. Details and the user command:
  [`jersey-transparent.md`](jersey-transparent.md). Next: user visual check,
  then decide on a play default.
- **Uniform moire / ripple (OPEN, 2026-11-05).** At the meter the guest asks for
  54 sampler states: LOD bias 0 on all of them, full mip chains, 2x-8x anisotropy
  requested but ignored by the Vulkan sampler; the 64-entry sampler table never
  fills, so the inFAMOUS-style overflow is not it. Candidate
  `PS3RECOMP_FP_AA` (scale DDX/DDY and implicit 2D samples by the 2x1 sample
  grid) is default off: key-off digest exact, 51/51 shaders compile, but the
  meter A/B showed no confirmed visual change and cost ~5 fps (35.56 ->
  30.44). Next: implement guest MaxAnisotropy behind a key and dump the pants
  texture at a close-up. Details: [`uniform-moire.md`](uniform-moire.md).
- Jerseys: previous note (not the cube probes; cause was unknown) superseded by
  the entry above.
- Tiled logo pattern over the screen during transitions / batter intro (recomp-work/ab_live_wcb_attr_20261102_191439/
  scene_1793665113991.png).
- Team-select 3D logo: **not a mesh path.** 2026-11-05 offline follow-up ([`team-logo.md`](team-logo.md) §10): the resolver's
  asset kind is `BINK_LOGO`; the in-game twin is a `MovieSprite`; the settled capture's 107 quads and the two 1024x512
  targets at team entry point at a Bink/UI-movie or texture-card sprite, not a `ScriptModel`. No HLE gap found; no fix
  implemented. Next user run: `PPU_RWATCH=0x01177574,0x228` + armed FIFO dump at a **team switch** (team-logo.md §10.6).
- Team-select 3D logo missing (not a missing texture; RESYNC_FIX doesn't restore it). **NEXT after the jersey + exposure missions (user, 2026-11-05).** Start from team-logo.md: the capture shows the logo is never submitted
  (upstream of RSX). Offline follow-up (team-logo.md §8, 2026-11-05): the 0x900 lookup is a world **model/material**
  match, and all plain-text disc ScriptUIElem targetnames found are stadium screens, so "the logo is a ScriptUIElem" is unproven. The
  title's miss text goes to `BADJUMBOS_PS3.TXT`, not the log, and the ISO is read-only, so the old CONFIG_MAIN recipe
  fails. User command: §8.3.1 (no config, `PPU_WWATCH=0x013560D8 PPU_WWATCH_LEN=0x120`) or §8.3.2 (writable overlay +
  `StressTestUIElem`, with create and remove steps). No HLE cause found; no runtime fix yet.
- Live-play lighting changes after plays, normal after replays (lighting-drift-notes.md).
- Crowd placement: port the RPCS3 crowd-fix patch.
- Loading hangs (~3 s) and start-menu hitches (these also hurt playability; may move up).
- 1080p output + 300% resolution scale (downscale on the GPU before readback).
