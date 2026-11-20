# MLB The Show 16: native PC port (work in progress)

*MLB The Show 16* (PS3, BCUS01089, update v01.10) running natively on PC through static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp). The game's PowerPC and SPU code is translated to C++ ahead of
time and compiled for your machine, instead of being emulated by RPCS3. The goal is better frame rates in the
CPU-bound scenes and, later, online play through the
[revival server](https://github.com/MatRanc/mlb16-revival-server).

**This repo contains no game code or data.** Everything game-derived (the decrypted EBOOT, the lifted C++, the
build, shader caches, screenshots) is produced on your own machine from your own copy of the game and never
committed.

## Status (2026-11)

| | |
|---|---|
| Platforms | Linux (Vulkan) and Windows (Vulkan, clang-cl; see [docs/WINDOWS.md](docs/WINDOWS.md)) |
| Playable | Boots, menus work, an exhibition game loads and plays |
| Speed (Ryzen 5 2600X + RTX 3060 Ti) | ~44 fps at the pitch meter with even frame pacing; ~27-29 fps in the heavy mound visit. RPCS3 on the same PC: 46-48 at the meter |
| Known visual issues | eye holes in close-up faces, a pants texture-filtering bug, a tiled pattern entering a mound visit, a rare black flash |
| First play of a scene | can stutter once while its shaders compile; they are cached on disk after that |

The day-by-day log is [docs/progress.md](docs/progress.md); where the frame time goes is
[docs/critical-path.md](docs/critical-path.md); the plan is [docs/plan.md](docs/plan.md).

## Play

Build it first from your own disc: [SETUP.md](SETUP.md) walks through prerequisites, the ISO mount, EBOOT
decryption, the PPU/SPU lift, the patches and the CMake/Ninja build. Windows players can use the launcher
(`tools/launcher/`).

```sh
# The ISO mount does not survive a reboot; repeat after restarting.
udisksctl loop-setup -r -f "/path/to/MLB The Show 16 (USA).iso"

tools/play_windowed.sh          # built-in HUD (fps, frame time, graph) + frametime CSV
tools/play_windowed.sh nohud    # no HUD
```

Close the window or press Ctrl-C to quit. If the intro video loops, the disc is not mounted.

## What's where

| Path | What |
|---|---|
| [SETUP.md](SETUP.md) | Build from your own disc, step by step |
| `tools/play_windowed.sh` | Play launcher (Linux) |
| `tools/ab/game_env.sh` | **Every game setting**, each with why it is on and what it measured. Used by play and all tests |
| `tools/ab/common.sh` | Paths and which build to run (`MLB16_BUILD`, default `build-soa`) |
| `tools/ab/` | Automated tests: `measure_fast.sh`, `ab_live.sh` (A/B), menu driver `nav3.py` |
| `tools/launcher/` | Cross-platform launcher with the same defaults |
| `tools/replay/`, `tools/hitch_table.py` | Offline frame replay and frame-time analysis |
| [docs/automated-testing.md](docs/automated-testing.md) | How to measure: pitch meter, quick check, mound visit |
| [docs/notes/](docs/notes/) | One note per investigation (what was tried, numbers, verdict) |

The runtime itself (graphics, SPU, system libraries) lives in the ps3recomp fork
[MatRanc/ps3recomp](https://github.com/MatRanc/ps3recomp):

| Branch | Role |
|---|---|
| `mlb16-windows-linux-stable` | Stable, the default branch: what the play build uses |
| `exp/item4-c690` | Development: new work lands here, then is promoted to stable |
| `archive/*` tags | Old experiments, kept for reference |

## Contributing

Fixes and measurements are welcome, from a one-line HLE fix to a renderer change.

1. **Runtime changes** go to [MatRanc/ps3recomp](https://github.com/MatRanc/ps3recomp) as a PR against
   `exp/item4-c690`. Keep each PR to one change, say which platform you built and ran it on, and include what you
   measured. Generic fixes can also go upstream to sp00nznet/ps3recomp.
2. **Settings** live only in `tools/ab/game_env.sh`. A new default needs a one-line reason and its measurement next
   to it, so the next person knows why it is on.
3. **Measure before and after**, in one launch: `tools/ab/ab_live.sh` alternates two settings while the game sits
   at the pitch meter. `tools/ab/measure_fast.sh` gives one number for a build. Both use the same settings as
   play, so the numbers mean the same thing. See [docs/automated-testing.md](docs/automated-testing.md).
4. **Check visuals by playing**, not just the numbers: the HUD's frametime graph shows stutter, and some changes
   (like skipping draws) only show up as wrong colours later in a match.
5. **Write it down:** a short note in `docs/notes/` (what, why, numbers, verdict) and a line in
   `docs/progress.md`. A failed experiment is worth a note too: it saves the next person the same week.
6. **Never commit game data**: no EBOOT, lifted code, screenshots or caches. `.gitignore` covers the usual ones.

## Related

- [mlb16-revival-server](https://github.com/MatRanc/mlb16-revival-server): the online server and launcher for RPCS3
- [mlb-show-rpcs3-fixes](https://github.com/MatRanc/mlb-show-rpcs3-fixes): crowd fix and RPCS3 notes
- Other ps3recomp ports that got to playable: [inFAMOUS 2](https://github.com/ThiagoFraga761/infamous2-pc),
  [God of War II HD](https://github.com/andrebrumdev/gow2-recomp)

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by Ned Heller ([@sp00nznet](https://github.com/sp00nznet))
  and its contributors: the lifters, runtime and system libraries this port runs on. The WWS job manager support
  MLB 16 needs came from [@sagemono](https://github.com/sagemono)'s LittleBigPlanet work.
- [RPCS3](https://rpcs3.net): ps3recomp's behavioral reference, and how this game was first understood.
- [inFAMOUS 2 PC](https://github.com/ThiagoFraga761/infamous2-pc) and
  [God of War II HD](https://github.com/andrebrumdev/gow2-recomp): ports whose write-ups showed what it takes.
- [Ghidra](https://ghidra-sre.org) with [Ps3GhidraScripts](https://github.com/clienthax/Ps3GhidraScripts) and
  [GhidraMCP](https://github.com/LaurieWired/GhidraMCP): reading the game's code.
- [SDL2](https://www.libsdl.org), [Vulkan](https://www.vulkan.org), [glslang](https://github.com/KhronosGroup/glslang)
  and [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) (input, windowing, rendering, shaders), and
  [Capstone](https://www.capstone-engine.org).

## License

MIT (see [LICENSE](LICENSE)), the same as ps3recomp, so fixes can go upstream. MLB The Show is © Sony Interactive
Entertainment.
