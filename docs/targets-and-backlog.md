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

## Priority: speed and frame-time stability first

Decided with the user 2026-11-02: visual bugs are logged here and parked; work goes to frame rate and steady frame
times until we match RPCS3.

## Visual backlog (parked)

- Jerseys missing (not the cube probes; cause unknown).
- Tiled logo pattern over the screen during transitions / batter intro (recomp-work/ab_live_wcb_attr_20261102_191439/
  scene_1793665113991.png).
- Team-select 3D logo missing (not a missing texture; RESYNC_FIX doesn't restore it).
- Live-play lighting changes after plays, normal after replays (lighting-drift-notes.md).
- Crowd placement: port the RPCS3 crowd-fix patch.
- Loading hangs (~3 s) and start-menu hitches (these also hurt playability; may move up).
- 1080p output + 300% resolution scale (downscale on the GPU before readback).
