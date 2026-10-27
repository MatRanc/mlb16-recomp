# Progress log

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26).

## 2026-10-27 (evening, playtest): menus playable, match load crawls

Played with a PS4 controller through SDL, in a window:
- **Menus work.** Experience level, main menu and moving between screens, at about 20 fps (109 new frames in about
  5 s, read from the frame counter at `0x011203d8`).
- **Button presses sometimes need two taps.** `PAD_ALWAYS_CHANGE=1` (every read returns a full report) didn't fix it.
  The controller is sampled every 4 ms, so short taps aren't the cause.
- **Menu boxes flicker** (only the boxes, not the whole screen): to capture with the renderer's pass dumps.
- **SELECT (Quick Menu) froze it.** This is online code, which also needed our server in RPCS3. Skipped for now;
  offline first.
- **Loading an exhibition match crawled.** The crowd job workload (`CrowdJobList`) started and stadium data loaded,
  then reads dropped to about 17 KB/s with no new frames. The main thread was busy in game code (`0x5499a0`,
  `0x643200`…) while the job module `255e2961` kept dropping into the SPU interpreter.
  - A gdb breakpoint on `spu_interp_run_until` caught 7,293 interpreter entries in 25 s, mostly at `0xd7a0`,
    `0xd9d0`, `0xd810` and `0xd878`: functions reached only through function pointers, so the lift never had them as
    entry points.
  - The 28 addresses are now passed to `lift_jobmods.py` (`LBP_JOBMOD_EXTRA_FUNCS`, in `tools/lift_spu.sh`). The
    job modules gained functions, e.g. `7ac07dde` went from 241 to 630. Test pending.
- **CPU use:** the Bink SPU program spins at 99% of a core in its wait loop even with no movie, and a job manager
  thread runs at about 84%.
- The build now lives in `recomp-work/build` (lifted source in `recomp-work/recompiled`), not the session's scratch
  folder.

## 2026-10-27 (later still): first menu

- **The autosave-screen stall** was the game's file copy into its `/dev_hdd1` cache (`0x776d8`, a "Duplicate"
  routine called from the file open at `0x78ca4`). It writes 64 KB chunks with `cellFsAioWrite` and polls a status
  word set by the completion callback (`0x79c2c`). `cellFsAioWrite` (`0x4CEF342E`) wasn't implemented in ps3recomp,
  so nothing was written and the callback never ran. Implemented on our branch `cellfs-aio-write` (upstream PR
  candidate 3), modeled on its `cellFsAioRead`.
- **Result:** about 500 async writes during boot, then the game's first interactive screen, **"Choose your
  experience level" (Veteran / Novice)**, drawn correctly through Vulkan with the team background behind it. This
  is the first-time setup a new save sees before the main menu.
- The Bink thread still spins at 99% of a core; whether the intro movie plays isn't known yet.
- **New unresolved imports, reached for the first time:** `0xB20F87B3`, `0xCE4374F6`, `0xDBF22BD1`, plus
  `cellHttpInitCookie` and `cellSpursAttributeEnableSystemWorkload` from before.

## 2026-10-27 (late night): flip loop fixed; stuck on the autosave notice

- **The 100,000 flips a second** came from the flip handler running twice per flip. `cellGcmSetFlipImmediate` calls
  it at request time (a compatibility "early edge" in ps3recomp) and the pump calls it again at completion. MLB's
  handler advances the displayed-frame counter (`0x011203dc`) each time, so it overtook the rendered-frame counter
  (`0x011203d8`). `DisplayFlip` (`0x692dc`) then saw a frame ready forever and flipped non-stop. Upstream's
  `GCM_FLIPCB_ONTICK=1` drops the early call. With it, about 35 flips a second, and the two counters stay a few
  frames apart.
- **Where it stops now:** the screen stays on the autosave notice (cover art) from about 20 s on, flipping buffer 0
  only, while the game keeps loading in the background. It opens `MLB_INTRO.BIK` early, probably to preload.
  - The main thread polls an event flag with an ~11.6 ms timeout (`0x4c7ae0` → `0x78ca4` → `0x785d8` → `0x776d8`).
  - The Bink SPU program (`BinkSPU.spu.elf`, task created with a SPURS event flag, `direction=2`, SPU to PPU) spins
    in its wait-for-work loop (`0x3870` → `0x3438` → `0x3448`) at 99% of a core.
  - Next: check ps3recomp's SPURS task event flag and queue path between the two (`docs/spurs-task-signal-wake.md`
    upstream).
- **Upstream PR branches** are in the fork (`MatRanc/ps3recomp`: `template-vulkan-backend`, `gcm-handler-thread`),
  with the draft descriptions in `PR_DRAFTS.md` on its `pr-drafts` branch. Nothing is opened yet.

## 2026-10-27 (night): boot freeze fixed, display stops updating

**The freeze was a leaked lock, not Bink.** The game usually hung on its first screens, at random, before ever
opening the intro movie; one early run got through by luck. Stacks of the stuck process (`eu-stack`, function names
are PS3 addresses) and its guest memory (`gdb`, reading `vm_base + address`) showed:
- `DisplayFlip` (thread entry `0x692dc`) flips each frame, then waits on a condition for the flip handler
  (`0x691b0`, registered with `cellGcmSetFlipHandler`) to clear a "flip pending" flag at `0x011203e0`.
- MLB's lock wrappers (`0x9a2da8` lock, `0x9a2e78` unlock) record the owner's thread ID at `struct+8` and only
  unlock when the caller's ID matches.
- ps3recomp delivers vblank/flip handlers at whatever firmware call a guest thread happens to make, and the callback
  inherits that thread's ID (`ppu_guest_call`). The flip handler ran nested inside `DisplayFlip`'s own call, while
  `DisplayFlip` held the lock at `0x012450d8`. It re-entered the lock (same ID), and its unlock cleared the owner
  record. `DisplayFlip`'s own unlock then saw an owner of 0 and was skipped, so the lock leaked (owner 4,
  recursion 1). The display engine and a sound thread blocked on it forever, and with the handler stuck inside the
  pump, no more vblank/flip callbacks were delivered.
- On a PS3 these handlers run on GCM's own interrupt thread.

**Fix** (our ps3recomp branch `gcm-handler-thread`, upstream PR candidate): `GCM_HANDLER_THREAD=1` delivers the
handlers only on a dedicated host thread with its own identity. With it, 3 of 3 runs get past the boot screens,
open `MLB_INTRO.BIK`, and go on loading user data, crowd sound banks, profiles and the logo cache.

**Next problem:** the presented picture stays on the autosave notice (cover art) from about 20 s on, while the game
keeps running. `DisplayFlip` now calls `cellGcmSetFlipImmediate(0)` about 100,000 times a second, so its flip loop no
longer paces itself. The movie may be playing unseen.

Also unresolved, not blocking yet: `0x4cef342e`, `cellSpursAttributeEnableSystemWorkload` and `cellHttpInitCookie`.

## 2026-10-27 (evening): first picture through Vulkan

- **Packages:** Vulkan needs `vulkan-headers`, `glslang-devel` and `spirv-tools-devel` (Fedora). Build with
  `-DPS3RECOMP_RSX_VULKAN=ON`. With glslang found, CMake reports "guest programs available": the game's own RSX
  shaders are translated (HLSL, then SPIR-V).
- **Template gap (upstream fix candidate):** `templates/project/main.cpp` picks D3D12, Metal or the headless
  renderer and never Vulkan, even in a Vulkan build. The runtime's own host tool (`runtime/host/host_posix.c`) does
  pick it. Adding a `PS3RECOMP_RSX_VULKAN` case with `rsx_vulkan_backend_init/present/pump_messages` fixes it.
- **Run:** `PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1`. The renderer picks the RTX 3060 Ti, opens a
  1280×720 Wayland window, and the game draws its **legal and licensing screen correctly**: text, logos and the
  animated red and blue background. `PS3RECOMP_VK_DUMP=<file>.ppm PS3RECOMP_VK_DUMP_EVERY=60` saves frames.
- **Next blocker:** it freezes right after opening the intro movie, `PS3/UI/Movies/intro/MLB_INTRO.BIK`. Bink's
  SPU program is lifted, but playback hangs.
- `PATCH_GD.WAD` fails to open from the data install folder (`BCUS01089_R`); it may be found in the update folder
  instead, not checked yet.

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
    - Checked with `SPURS_PM_DUMP=1`: the same holes as LBP's (1,728 of 1,730 bytes zero, `executeStage` empty).
      No game code writes the main-memory copy in 60 s. Upstream has no fix for LBP either.
    - That may be expected: the job manager writes code into its own local store (the runtime logs these as
      `spu-smc` and runs them through the SPU interpreter). That would fill the holes on the SPU side, where a probe
      of main memory can't see them.
    - The spin re-reads the same 128 bytes of the workload's data (data + `0x80`) 256 times, which looks like
      polling an empty job queue rather than a crash. The game keeps re-running those workloads afterwards. Still to
      confirm.
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
