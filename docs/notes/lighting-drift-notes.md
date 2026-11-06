# Lighting drift in live play: first look

Session: user-driven windowed play, 2026-11-02 21:43-21:48, `build-play` (`exp/live-ab` 952795bb), WCB=2,
CUBE_RT=1, PACE=1, `PS3RECOMP_LIGHT_PROBE=1`. Log: `recomp-work/play_20261102_214343.log` (game data dir).

## What the user saw

Pregame and the first two pitches looked right. Around 21:45:52, after the batter hit the ball, the mound and grass
lighting changed. Entering and leaving a replay (~21:46:18) brought it back to normal. Around 21:46:30 the pitcher's
and batter's pants changed lighting again.

## What the probe shows

- Every guest read was fresh (`fresh=0` count: 0). The readback is not handing the game stale data.
- **Exposure (`ToneMapSample`, 32x32 -> 1x1 chain at 0x019D7900/0x019D9000/0x019D9080)** is read about every
  2 frames. In 5 s bins its 1x1 luma sits at 78-80 with short swings at camera cuts (21:45:50-55, 21:46:10-20,
  21:46:40-45), which is normal auto-exposure. From 21:47:45 it settles lower (72-74) for the rest of the session.
- **Scene-setup lighting bursts** (a 32x32 at 0x019E4C00, six 32x32 reads of 0x018C9200, then one at
  0x075D9400/0x07484000) happen exactly at 21:45:53-57 and 21:46:18-20, i.e. at the two moments the user reported.
  These match the sky-light measurements (`QuickAmbientSky` / `HarmonicsSample`) that the game re-runs when a scene
  is set up. The values are **identical in all three bursts** (lumas 85.6, 86.7, 145.2, 54.2, 77.8, 94.2 for the six
  0x018C9200 reads), and one of the six is pure black (RGB 0,0,0) every time.
- A pair of 32x32 targets (0x0B4BFC00, 0x0B714C00) is read every few seconds with low, varying values.

## Reading

The game re-measures its scene lighting after each play and on leaving a replay, which is when the look changes.
Since that measurement returns the same values every time, the visible change is probably not in these readbacks.
Candidates, in order:

1. The **one black reading out of six** in the sky-light burst: if one direction of the sky light renders black
   (missing draw, or our cube/face handling), the ambient light is lopsided. It's deterministic, so it would
   differ only if the game uses it differently per camera.
2. The **cube-probe fallback** (`CUBE_RT`): after a scene change, an incomplete 8x8 probe binds the *last complete*
   image for that address, which may come from the previous camera/scene until the probe is complete again.
3. Not readback at all: per-camera lighting the game applies itself (live vs replay camera).

## Next checks (not done)

- Same session with `PS3RECOMP_TEX_MISS_LOG=1` and cube logging, to see which probes are incomplete after 21:45:52.
- Decode which sky-light reading is black and whether it should be (compare with an RPCS3 capture of the same
  burst).
- A/B in live play: `PS3RECOMP_RSX_CUBE_RT=1` vs 0 across a play + replay.
