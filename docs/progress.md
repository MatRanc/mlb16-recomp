# Progress log

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26).

## 2026-10-27 (later): SPU job manager running, data install done

- **Policy module:** the WWS job manager's SPURS policy module is 12,032 raw bytes at `0x105bb80` (file offset
  `0x104bb80`) that SPURS loads at local store `0xA00`. Wrapped as an SPU ELF at that base
  (`wrap_spu_elf.py --base 0xA00`) and lifted with `build_spu_workloads.py`, its text segment registers under the
  fingerprint the runtime looks up (`0x75BFE68AE96D7577`).
  - The runtime's `SPU_DUMP_MISS=<dir>` setting wrote the missing programs out exactly as fingerprinted, which is
    how both were found.
  - The same job manager runs LittleBigPlanet, whose fixes are already upstream.
- **Job modules:** `lift_jobmods.py --engine-elf` finds and lifts the 7 job modules linked into the EBOOT. One has
  308 undecoded words, the same count as the MP3 decoder program, so they're probably its data tables.
- **Build:** with the 13 SPU programs (`-DSPU_DIR=`), the rebuild takes 38 s. All of this is in `tools/lift_spu.sh`.
- **Results:**
  - All three job workloads resolve and run.
  - The game's 9.5 GB first-boot data install completes in about 50 s.
  - The game then creates more workloads and its seven sound streaming threads. Job loads stall and resume through
    SPU DMA lists and interrupts, and the job manager's self-modifying code is handed to the SPU interpreter.
- **Open problems:**
  - The job manager sometimes spins at local store `0x2534`, repeating one DMA transfer until the runtime's
    watchdog halts that SPU (256 identical transfers). LBP's notes say this policy module gets code patched into it
    by SPURS at load time (holes at `0xAEE`–`0x11B0`, `executeStage` at `0x14f4`); `SPURS_PM_DUMP` checks for that.
  - One guest write to NULL+0x48 from `0x7b988`.
  - The headless null renderer shows nothing yet; the Vulkan backend is next.

## 2026-10-27: first boot

**Lifting the PPU code** (`tools/probe.sh`):
- `ppu_loader.py` finds 15,790 functions from the OPD table and 433 firmware imports across 32 libraries.
- `ppu_lifter.py` lifts 43,501 functions to about 6.5M lines of C++ in 11 chunks, in about 1 minute.
- **Pass `--code-end 0xf2c634`.** Without it the lifter treats the read-only data after the code (1.5 MB, up to
  `0x10a43e8`) as code: 1.93M undecodable words and 954 MB of output. Ghidra's section table shows the last
  executable section ends at `0xf2c634`.
- With it, 6,847 words stay undecoded. They are float constants (1000.0, 32767.0, 10000.0…) and zero padding
  between functions, not unsupported instructions.

**Building** (ps3recomp's `templates/project`, clang, `-j6`):
- No compile errors. 10 min 55 s wall time, about 1.5 GB of RAM per chunk.
- Result: a 205 MB native Linux executable.
- The runtime itself needs `SDL2-devel` (Fedora: `sudo dnf install SDL2-devel`) and builds in seconds.

**Running** (headless null renderer, no SPU programs lifted yet):
- The game's own code runs natively. It sets up memory, SPURS, GCM and sysutil, starts its threads ("Job Manager
  Event Handler", "IMFE PPU Thread"…), draws its first frames and flips buffers, creates its trophy context, finds
  a PS4 controller through SDL, and reads `MLBPS3_SYS.WAD` from the disc.
- First stop: the game's "not enough system storage (5,120 MB)" dialog. The runtime resolves `/dev_hdd0/game` under
  the same root as the disc, which was a read-only ISO mount. Fix: a writable root folder holding a `PS3_GAME`
  link to the disc and `game/BCUS01089` (the v01.10 update).
- With that, the game offers its optional data install, the runtime auto-answers yes, and the game starts
  installing ("Installing Game Data… File 1/6").

**Missing so far:**
- The WWS job manager's SPURS policy module (12,032 bytes at `0x105bb80`) isn't lifted, so the game's SPU job
  workloads (`Wws_Job`: `DXTJob`, `MeshJobs0`, `MeshJobs1`) never run. ps3recomp's `lift_jobmods.py` targets this
  job format (`c0dec0de` modules).
- The Bink SPU program (110,560 bytes) isn't lifted yet, so video won't play.
- Two unresolved firmware imports showed up early in the boot.

**SPU programs:**
- The EBOOT embeds 5 SPU ELF images: two WWS job checks, an MP3 decoder, surround audio and Bink.
- It also holds 8 `c0dec0de` job modules (4 inside those ELFs), including the ICE mesh program.
- RPCS3 showed 25+ SPU programs in use, so the rest come from the game's data files.
- All 5 ELF images lift with `spu_lifter.py --auto-functions` with no unsupported instructions.

**Firmware coverage** (approximate, from ps3recomp's handler table vs our 433 imports):
- 165 have a real implementation, 71 a thin one, 46 return a constant, 32 run inside the runtime itself, and 119
  are missing. Missing imports return CELL_OK without doing anything.
- Gaps for online play:
  - `cellHttp` is registered under made-up names, so its handlers never match the game's imports (37 of 48 missing).
  - `cellSsl`/`cellHttps` have no TLS.
  - NP auth, ID lookup and signaling are missing or stubbed.
- ps3recomp's own docs recommend Sony's `libsre` (lifted from your firmware) over its built-in SPURS, which has hung
  most SPURS-heavy titles.
