# Automated testing

How the game is driven and measured without a person at the controller: the pitch meter A/B (`ab_live.sh`), the
quick meter check (`measure_fast.sh`) and the mound visit. All of them boot the real game, navigate the menus from
frame dumps and measure presented frames over fixed windows.

## One set of settings

Every script runs the game with the same switches:

| File | What it holds |
|---|---|
| `tools/ab/common.sh` | Paths and build: `MLB16_BUILD` (default `build-soa`), `MLB16_PS3RECOMP` (`ps3recomp-soa`), the EBOOT, the ISO mount, and `REF_DIR` (screen references, found automatically). |
| `tools/ab/game_env.sh` | **Every game setting**, each with a one-line why and its measurement. Sourced by `play_windowed.sh`, `ab_live.sh` and `measure_fast.sh`. |
| the script itself | Only its own outputs: logs, timeline, frame dumps, the pad file or control file. |

So a number from `ab_live.sh` or `measure_fast.sh` is measured with what `play_windowed.sh` plays. To change a
default, edit `game_env.sh` (one place). To override for one run, set the variable in the environment: every line
keeps a value that is already set.

## Rules for every run

- **One game at a time.** All runs share `recomp-work/pad.txt` (input) and `recomp-work/frame.ppm` (frame dump).
  Never start a second run, or a full rebuild (~17 GB RAM), while a game is up.
- **Don't edit a launcher script while a game runs from it.** Bash reads scripts as it goes; an edit mid-run made
  it hit a syntax error and its cleanup closed the game.
- **ISO mounted.** After a reboot the boot hangs on the Sony screen and nav times out. Check
  `ls /run/media/m/PS3VOLUME/PS3_GAME`; if it's missing:
  `udisksctl loop-setup -r -f ".../MLB The Show 16 (USA)/MLB The Show 16 (USA).iso"`.
- **At the pitch, press X once.** The first press selects the pitch and parks on the pitch meter, which stays there
  indefinitely. A second X starts the throw. The idle pitch screen (no X) cuts to b-roll after ~30 s.
- **Compare inside one launch.** Absolute fps moves by a few fps from session to session (RustDesk open costs
  ~15-20 %). Use paired A/B arms in one run (`ab_live.sh`).
- **Prefer offline first.** Replay captures (`replay_rsx_engine`, `PS3RECOMP_REPLAY_VK=1`) and unit tests answer many
  questions without booting the game.

## Shared pieces

| Piece | Where | What |
|---|---|---|
| `nav3.py` | `tools/ab/` | Closed-loop menu driver: matches `frame.ppm` against screen references, writes `pad.txt`, skips cinematics with X. |
| Screen references | `recomp-work/ab-tools/ref/*.png` | Game screenshots, never committed. `common.sh` uses `tools/ab/ref` if it exists, else this folder. Without them nav fails with "missing screen references" and the script closes the game. |
| `meter_lib.sh` | `tools/ab/` | Confirms the pitch meter from the frame dump (`REF_DIR/pitch_meter.png`). |
| Pad codes | `pad.txt` = `<buttons hex> <frames>` | `0x4000` X, `0x0008` Start, `0x0020` Right, `0x0040` Down. |

## 1. Pitch meter A/B: `ab_live.sh` (the main benchmark)

One windowed launch: boot, navigate to the pitch, press X once, confirm the meter, settle, then alternate arms every
`SEG` seconds through the live control file (`PS3RECOMP_CTL`). Both arms run the same binary in the same session.

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp/tools/ab
TAG=mytest N=4 ARM_A="PS3RECOMP_FLIP_ON_DONE=0" ARM_B="PS3RECOMP_FLIP_ON_DONE=1" ./ab_live.sh
# three or more arms:
TAG=x N=4 REPORT=0 ARMS='A=KEY=0|B=KEY=1|C=KEY=2' ./ab_live.sh
```

- `ARM_A`/`ARM_B`: newline-separated `NAME=value` lines. Only live switches can change per arm; anything else goes
  in the environment for the whole run.
- Defaults: `N=6` segments per arm, `SEG=10` s, `DROP=2` s dropped after each switch, `WCB=2`, `SETTLE=10` s.
- Outputs in `recomp-work/`: `ab_live_<TAG>_<ts>.log`, `_segments.txt` (`start end arm fps` per segment), `_ab.json`,
  `_ft.png`, and a folder with the HUD frametime CSV, thumbnails and the meter frame. `REPORT=0` (needed for 3+
  arms) skips `_segments.txt`/`_ab.json`; compute per-arm numbers from `.marks` and the CSV.
- An A/A run (same keys in both arms) shows the noise: about ±1 fps.

## 2. Quick meter check: `measure_fast.sh`

```sh
mlb16-recomp/tools/ab/measure_fast.sh                       # prints "meter fps: N"
BUILD=build-depth mlb16-recomp/tools/ab/measure_fast.sh     # another build
EXTRA="PS3RECOMP_C690_SOA=0" mlb16-recomp/tools/ab/measure_fast.sh
```

`ab_live.sh` with one arm and `N=1`: boot, meter, 2 x 10 s, one number. Use it to check a build boots and to see its
meter fps; use `ab_live.sh` to compare two settings. `EXTRA` is passed as environment at boot.

(The older `recomp-work/ab-tools/measure_fast.sh` ran with a minimal environment of its own and read about 7 fps
lower than `ab_live.sh` on the same build. Don't compare its numbers with these.)

## 3. Mound visit (heavy scene)

The pitch meter is light; the mound visit is heavy and SPU-bound (c690 mesh production), so it shows SPU changes and
costs that the meter hides.

Route: `ab_live.sh` with `METER=0` parks on the first-pitch screen without pressing X, then
`recomp-work/captures/mound/mound_nav.sh` drives Start → Right (Bullpen panel) → X → Down (Mound Visit) → X during
the settle time. It waits for "pitch reached" in the ab_live log.

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp/tools/ab
W=/home/m/Documents/mlb16-online/recomp-work
METER=0 SETTLE=25 TAG=mound_x N=6 ARM_A="..." ARM_B="..." ./ab_live.sh > $W/captures/mound/ab_mound_x.out 2>&1 &
sleep 8; L=$(ls -t $W/ab_live_mound_x_*.log | head -1)
mkdir -p $W/captures/mound/run_x && $W/captures/mound/mound_nav.sh "$L" $W/captures/mound/run_x
```

- `mound_nav.sh` saves a shot after each step; check `n5_mound_visit.png` shows the visit before trusting the
  segments.
- The player count varies per launch (3 to ~7), so compare only paired arms inside one launch, and note the count.

## Reference numbers (2026-11-20, `build-soa`, shared settings)

| Scene | Harness | fps | Notes |
|---|---|---|---|
| Pitch meter | `measure_fast.sh` | 44.2 | 2 x 10 s |
| Pitch meter | `ab_live.sh`, flip A/B | 43.0 → 43.8 | `FLIP_ON_DONE` 0 → 1: frames ≥ 26 ms 32 % → 5 % |
| Mound visit | `ab_live.sh`, flip A/B | 28.7 → 27.3 | `FLIP_ON_DONE` 0 → 1: frametime sd 4.6 → 2.4 ms |

## Reading frame times

- The HUD CSV (`PS3RECOMP_HUD_CSV`) logs every present: `fps,frametime_ms,elapsed_ns`. With
  `PS3RECOMP_VK_PRESENT_NEW_ONLY=1` a present is a new game frame.
- The timeline (`PS3RECOMP_TIMELINE`, `..._HITCH_US`) keeps frames longer than the threshold with the work inside
  them. The scripts keep only > 250 ms; for frame-pacing work set `PS3RECOMP_TIMELINE_HITCH_US=15000` for one run
  (about 0.9 GB per 4 min). `tools/hitch_table.py <file> --bins --classify --csv out.csv` labels each frame;
  "DEADLINE SLIP" means the frame ended at the title's 2 x 16.7 ms flip deadline with its work done earlier.
