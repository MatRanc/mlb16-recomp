# MLB The Show 16: native PC port (work in progress)

An attempt to run *MLB The Show 16* (PS3, BCUS01089, update v01.10) natively on PC by static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp): the game's PowerPC and SPU code is translated to C++ ahead of
time and compiled for your machine, instead of being emulated by RPCS3. The goal is better frame rates in the
CPU-bound scenes (dugout close-ups, mound visits) and, later, online play through the
[revival server](https://github.com/MatRanc/mlb16-revival-server).

**Status: early. It boots natively through Vulkan, the menus work, and an exhibition game loads and plays, with
many surfaces still drawn wrong (solid magenta).** See [docs/progress.md](docs/progress.md).

**This repo contains no game code or data.** Like other ps3recomp ports, everything game-derived (the decrypted
EBOOT, the lifted C++, the build) is produced on your own machine from your own copy of the game.

## Trying the probe

You need your own decrypted v01.10 EBOOT (RPCS3: **Utilities → Decrypt PS3 Binaries** on
`dev_hdd0/game/BCUS01089/USRDIR/EBOOT.BIN`) and a ps3recomp checkout with its Python requirements installed.

```sh
tools/probe.sh /path/to/ps3recomp /path/to/EBOOT.elf work
```

It loads the EBOOT, lifts the PPU code to C++ in `work/recompiled/`, generates the firmware import table and
extracts the embedded SPU programs. Keep `work/` out of git. `tools/lift_spu.sh` lifts the SPU code (including the
job manager), and `tools/run.sh` runs a build; see [docs/progress.md](docs/progress.md) for the build steps.

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
