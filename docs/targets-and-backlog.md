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
