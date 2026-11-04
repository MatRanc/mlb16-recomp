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

## Priority: speed and frame-time stability first

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
- Game queue: PIN A/B + burst critical path (running) -> lever chosen from it (scheduling/gaps vs native unpack/pack) (meter + cinematics) -> team-logo FIFO dump -> tick marker-side A/B.

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

- Jerseys missing (not the cube probes; cause unknown).
- Tiled logo pattern over the screen during transitions / batter intro (recomp-work/ab_live_wcb_attr_20261102_191439/
  scene_1793665113991.png).
- Team-select 3D logo missing (not a missing texture; RESYNC_FIX doesn't restore it).
- Live-play lighting changes after plays, normal after replays (lighting-drift-notes.md).
- Crowd placement: port the RPCS3 crowd-fix patch.
- Loading hangs (~3 s) and start-menu hitches (these also hurt playability; may move up).
- 1080p output + 300% resolution scale (downscale on the GPU before readback).
