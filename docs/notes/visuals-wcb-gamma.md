# WCB ondemand/async vs the pitch visuals, and the remaining gamma gap

Date: 2026-11-02. Branch `play/multiscene` (ps3recomp worktree `ps3recomp-play`,
binary `recomp-work/build-play`). Windowed pitch runs, one game at a time.

## Question

The player's windowed pitch runs showed no jersey colour, a missing/dark crowd,
an oversaturated field and dark crowd areas, plus chrome/oil-slick rainbow on
faces in dugout close-ups. RPCS3 fixes the same set with **Write Color Buffers**
alone, so the first question is whether the new on-demand (`PS3RECOMP_RSX_WCB=2`)
or async (`=3`) readback does the same, and at what fps. The team-select
rotating 3D logo was also checked: WCB does not bring it back, and the cause is
the FIFO walker, not readback.

## Method

`measure_fast.sh` method, but running `build-play` and recording each arm:

- `PS3RECOMP_VK_WINDOW=1`, `VK_DUMMY_BLACK=1`, `PS3RECOMP_VK_PRESENT_MODE=fifo`
  (the `docs/notes/window-freeze-fix.md` fix), `VK_DRAW_BATCH=1`,
  `VK_FRAME_PIPELINE=1`, `GCM_FLIPCOUNT=1`, scripted pad to the pitch
  (NAV_HOME=1 nav), 10 s fps window from `[flip]`.
- MangoHud CSV per arm (`tools/frametime_plot.py` after each run).
- `PS3RECOMP_RSX_WCB_STATS=60` on the WCB arms.
- Pitch frame and the team-select frame saved per run; face crops in
  `recomp-work/visuals/compare.png`.
- Harness: `recomp-work/ab-tools/measure_wcb.sh`, `poll_pitch.py`,
  `watch_team.py`. The nav needed one extra cross press after the starting
  lineups; `poll_pitch.py` does that and also saves the last frame on failure.
- The first arm-a attempt stalled on the starting-lineups screen (no nudge
  yet) and was retried; it produced no pitch frame.

`HeadFaceCacheFile` was built in earlier WCB-off runs, so it was backed up to
`recomp-work/HeadFaceCacheFile.pre-wcb.bak` and removed before arm b. Arm b
regenerated it (314,584,064 bytes) with WCB ondemand.

## Results

Game fps is `[flip]` over the 10 s window, not the MangoHud window-present rate
(which is ~50-55/s and is not game fps).

| arm | config | game fps | flips / 10 s |
|---|---|---:|---:|
| a | WCB off | **35.94** | 360 |
| b | `PS3RECOMP_RSX_WCB=2` (ondemand) | 30.87 | 310 |
| c | `PS3RECOMP_RSX_WCB=3` (async) | 28.71 | 280 |
| b2 | ondemand repeat | 31.88 | 320 |
| b3 | ondemand repeat | 30.60 | 300 |

Ondemand over three runs: **31.12 fps mean, 30.60-31.88 range**, about
**4.8 fps below** WCB off in this session.

WCB counters, steady pitch windows (ondemand, b/b2/b3): about **2.0-2.5
copies/frame, 292-395 KB**, all demand copies; 0.6-0.7 faults and waits/frame,
wait avg **1.5-2.1 ms** (max 27.6-28.8 ms since start), copy **3.4-4.3 ms**
per frame, `timeouts 0`, `pending 49-55`.

Async (c): steady about **2.1-2.8 copies/frame, 309-395 KB**, async issued
2.1-2.7, fetched 1.8-2.4, discarded 0.3-0.4; wait avg **0.4-0.7 ms**, but one
**642 ms** spike (max since start), copy 2.9-3.3 ms, `timeouts 0`. During
loading it peaked at 5.5 copies/frame / 3.4 MB / 8.9 ms copy. Async is slower
here than ondemand, most likely the 642 ms wait and the larger loading copies.

MangoHud CSV / `frametime_plot.py` (window presents, not game fps): a 54.65,
b 51.40, c 50.42, b2 50.90, b3 51.40 avg.

## Visuals

Pitch frames: `recomp-work/visuals/arm_{a,b,c,b2,b3}_pitch.png`.
Team frames: `recomp-work/visuals/arm_{a,b,c,b2,b3}_team.png`.
Side-by-side with face/pitcher zooms: `recomp-work/visuals/compare.png`.

- **WCB off (a)**: field is a bright yellow-green, crowd is dark silhouettes,
  the pitcher is near-black with no uniform colour, and the catcher's
  mask/helmet shows rainbow/oil-slick speckles over dark skin. Team select:
  the large team logos are flat and the centre assignment panel is black.
- **WCB=2 / =3 (b, c, b2, b3)**: the crowd is visible and coloured (blue/white
  shirts, skin tones), the field is a natural green close to the RPCS3
  reference, the pitcher's cap/skin/blue uniform read correctly, and the
  catcher's face/mask is beige with no rainbow. This is the WCB fix, not a
  gamma change: the `[RSX fp] opcode 25` warnings are still present in these
  runs.
- **Still missing in every arm**: crowd density/placement (some fans misplaced
  or sparse) and the team-select 3D logo. These are not WCB artefacts.
- No jersey name/number is visible in the tested pose (the pitcher is bent
  over, back not facing the camera), so this run does not prove or disprove
  the composite number textures.

## Team-select rotating logo: not WCB, the walker's park resync drops the draw

The team-select frames are **pixel-identical** with WCB off, ondemand and
async (mean diff 0.0, max 1 LSB; the centre panel is black in all of them), so
it is not a guest readback and WCB cannot fix it.

The cause is the FIFO walker's jump-park resync. A park that stands with the
same `put` for `GCM_PARK_GRACE_MS` is abandoned by jumping straight to `put`
(`libs/video/cellGcmSys.c:2701-2712`). That keeps the FIFO alive but drops
everything between the park and `put`; the resync replays fences and label
releases, and its own comment says draws in the range are still lost
(`cellGcmSys.c:1651-1745`). `docs/progress.md` already records the logo
showing when the park grace was made effectively infinite
(2026-10-27 evening, "the selected team's logo now shows on team select. It
was probably in commands the early jump skipped").

Proposed fixes, not implemented:

1. Quick test/config: run with a much larger `GCM_PARK_GRACE_MS` (the env
   knob already exists) so the walker waits for the park to be patched.
2. Correct fix: replay the skipped range's method headers and draws through
   `rsx_process_method`/the sink instead of only fences and labels, or refuse
   the resync when the range contains method commands, so a frame is not lost.
3. Do not treat it as render-to-texture: the frame does not change with WCB.

## Gamma / sign-expand fetch modes (static; not implemented)

The two log lines in the player session are both opcode `0x19`, `TXD`
(texture fetch with derivatives), not TEX:

```
[RSX fp] gamma fetch opcode 25 unsupported on unit 2/4/8
[RSX fp] sign-expand fetch opcode 25 unsupported on unit 6 (read unexpanded)
```

The decompiler already implements both operations for `TEX/TXP/TXB/TXL`:

- **Gamma**: `TEXTURE_ADDRESS` bits 20-23 are a per-channel mask. The sampled
  (filtered/remapped) texel is converted sRGB-to-linear on the named lanes
  before instruction result scale and saturation
  (`libs/video/rsx_fp_decompiler.c:551` helper, applied at `:1050-1056`).
- **Sign expansion**: `UNSIGNED_REMAP_BIASED` (`TEXTURE_ADDRESS` bit 12)
  re-centres the named lanes, `(round(x*255)-128)/127` (`/32767` for 16-bit
  channels), and the `_bx2`/`EXP_TEX` fetch (`OPDEST` bit 21) returns
  `x*2-1` on every lane and bypasses gamma (`:611` helper, applied at
  `:1060-1076`).
- **TXD gap**: with gamma state, `TXD/TEXBEM/TXPBEM` make the decompiler
  return `-1` (`:876-883`), which fails the whole fragment program and drops
  the draw (`rsx_draw_engine.c:3196`, "FAILED (draw dropped)"). With sign
  expand, the same opcodes log "read unexpanded" and continue (`:1078-1083`).
  So the current behaviour is *worse* than reading raw for gamma TXD: the
  draw is lost.

Why this mission did not add `PS3RECOMP_FP_GAMMA=1`: the WCB arms already
remove the observed chrome faces and the oversaturated field, so the
condition for doing the secondary work was not met. The pitch frame does not
show a missing object obviously tied to the 13 dropped TXD programs; the
remaining differences are crowd placement and the team logo, both explained
above.

Proposed follow-up (needs the user's approval before default-on):

- Implement `TXD/TEXBEM/TXPBEM` gamma/sign-expand behind
  `PS3RECOMP_FP_GAMMA=1`, default off. For TXD use `SampleGrad` with the same
  `_rsx_gamma_input` / `_rsx_expand_input` wrappers; do not fail the program
  when the flag is off (keep "read unexpanded" for sign expand; for gamma,
  reading raw is better than dropping the draw, or keep the current `-1` only
  as the old behaviour).
- Add a unit test of the generated shader math (sRGB piecewise curve and
  `x*2-1` / biased re-centre on selected lanes only) and a generated TXD
  program with gamma state.
- Note the existing `TEX` path is on unconditionally (upstream merge
  `c8b5fc8e`/`4e8f3ca8`); if the user wants gamma "off until approved", it
  needs gating too, which is a behaviour change for every title.

## Crowd fix: not applied

The recomp has no RPCS3 `.yml` game-patch mechanism and does not apply the
crowd patch. `mlb-show-rpcs3-fixes/patches/MLB16_crowd_fix.yml` is a single
branch at `0x009bd544` forcing every crowd batch onto the single forward
`DRAW_ARRAYS` path. All four crowd draw paths are still taken in the recomp
(`0x9bd7d8`, `0x9bd54c`, `0x9bd600`, `0x9bd6f4` in the write-up). Proposal:
port that one branch into the recompiled PPU code (or force the same
single-draw path in the crowd setup) before judging crowd placement. Not done
here.

## Recommendation

Use **`PS3RECOMP_RSX_WCB=2` (ondemand)** for correct visuals at ~31 fps; async
is slower in these runs and had a 642 ms read wait. WCB is the primary fix.
Keep the gamma work (TXD opcode 25) for a separate, approved change; next
visual levers are the crowd patch and the park-resync team logo.
