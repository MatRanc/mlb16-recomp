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
6. Persistent VkPipelineCache: built (PS3RECOMP_VK_PIPELINE_CACHE, off). Removes ~0.29 s driver time only; not kept.
   Next: an on-disk cache of our translated shaders (SPIR-V) targets the ~3.8 s of translation (load + cinematics).
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
