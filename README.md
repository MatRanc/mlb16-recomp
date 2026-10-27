# MLB The Show 16: native PC port (work in progress)

An attempt to run *MLB The Show 16* (PS3, BCUS01089, update v01.10) natively on PC by static recompilation with
[ps3recomp](https://github.com/sp00nznet/ps3recomp): the game's PowerPC and SPU code is translated to C++ ahead of
time and compiled for your machine, instead of being emulated by RPCS3. The goal is better frame rates in the
CPU-bound scenes (dugout close-ups, mound visits) and, later, online play through the
[revival server](https://github.com/MatRanc/mlb16-revival-server).

**Status: early probing. It doesn't run yet.** See [docs/progress.md](docs/progress.md).

**This repo contains no game code or data.** Like other ps3recomp ports, everything game-derived (the decrypted
EBOOT, the lifted C++, the build) is produced on your own machine from your own copy of the game.

## Trying the probe

You need your own decrypted v01.10 EBOOT (RPCS3: **Utilities → Decrypt PS3 Binaries** on
`dev_hdd0/game/BCUS01089/USRDIR/EBOOT.BIN`) and a ps3recomp checkout with its Python requirements installed.

```sh
tools/probe.sh /path/to/ps3recomp /path/to/EBOOT.elf work
```

It loads the EBOOT, lifts the PPU code to C++ in `work/recompiled/`, generates the firmware import table and
extracts the embedded SPU programs. Keep `work/` out of git.

## Related

- [mlb16-revival-server](https://github.com/MatRanc/mlb16-revival-server): the online server and launcher for RPCS3
- [mlb-show-rpcs3-fixes](https://github.com/MatRanc/mlb-show-rpcs3-fixes): crowd fix and RPCS3 notes
- Other ps3recomp ports that got to playable: [inFAMOUS 2](https://github.com/ThiagoFraga761/infamous2-pc),
  [God of War II HD](https://github.com/andrebrumdev/gow2-recomp)

## License

MIT (see [LICENSE](LICENSE)), the same as ps3recomp, so fixes can go upstream. MLB The Show is © Sony Interactive
Entertainment.
