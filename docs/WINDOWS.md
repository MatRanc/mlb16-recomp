# MLB 16 on Windows: setup checklist

Status: **untested on Windows.** Everything below comes from the Linux build and the ps3recomp docs
(`ps3recomp-probe/docs/BUILDING.md`, `GETTING_STARTED.md`). Record what fails and send it back so the
checklist can be corrected.

The game is not included. You need your own copy and must make the EBOOT and VFS on your machine.
Do not share the ISO, PKG, EBOOT, VFS, or generated C++.

## 0. Pick the renderer first

| path | what it is | status |
|---|---|---|
| D3D12 (ps3recomp's default on Windows) | ps3recomp's reference Windows renderer, `libs/video/rsx_d3d12_backend.c` | **the only renderer that builds on Windows** |
| Vulkan (`-DPS3RECOMP_RSX_VULKAN=ON`) | our measured backend | **Linux only.** `ps3recomp-probe/CMakeLists.txt:54` stops configure on Windows with "PS3RECOMP_RSX_VULKAN is Linux-only for now" |

Use D3D12 on Windows. Do not add `-DPS3RECOMP_RSX_VULKAN=ON`; CMake will stop. A D3D12 build does not have
our MLB16 tuning (the `PS3RECOMP_VK_*` and Vulkan-side switches only apply to the Vulkan backend), so its FPS
is not comparable to our ~35 FPS on Linux. Measure it on its own terms and report the number.

## 1. Tools

- Visual Studio 2022 with the C++ workload, plus **clang-cl** (LLVM component)
- Windows SDK 10.0.19041 or newer
- CMake 3.25+ and Ninja
- Python 3.9+ with tkinter (python.org installer includes it)
- SDL2 development libraries (set `SDL2_DIR` in CMake if it is not found)
- Git, and optionally Git Bash for the bash helpers

Use **clang-cl**, not plain `cl`, for lifted game code. The runtime uses `__atomic_*` builtins.

## 2. Get the code

```
git clone <mlb16-recomp repo>
git clone -b port/stable https://github.com/MatRanc/ps3recomp.git ps3recomp   # known-good commit 1c2bfd8a
```

Put `recomp-work/` next to `mlb16-recomp/` as on Linux, or set the work folder in the launcher.

## 3. Make the EBOOT and VFS from your own game copy

1. Mount or extract your copy of `MLB The Show 16 (USA)` (BCUS01089).
2. Decrypt `EBOOT.BIN` to `EBOOT.elf` (`SETUP.md` section 4).
3. Build the writable VFS into `recomp-work\vfs` (`SETUP.md` section 5).

The bash-only helpers (`apply_patches.sh`, `lift_spu.sh`) need Git Bash or WSL. A native PowerShell
version is not written yet.

## 4. Apply patches and lift

From Git Bash in `mlb16-recomp` (same as `SETUP.md` section 6):

```
tools/probe.sh ../ps3recomp ../EBOOT.elf ../recomp-work
tools/lift_spu.sh ../ps3recomp ../EBOOT.elf ../recomp-work
tools/apply_patches.sh --work ../recomp-work --ps3recomp ../ps3recomp --wws-native --ftz --approx
```

These are bash scripts written for Linux. If one fails on Windows, copy the console output and send it
back. Do not edit the scripts to make them run.

## 5. Build

This is the Windows version of the command in `SETUP.md` section 7. It is **not tested yet**. Run it from a
"x64 Native Tools" prompt or Git Bash with the VS environment loaded, from the `mlb16-recomp` folder.

```
cmake -B ..\recomp-work\build-depth -S ..\ps3recomp\templates\project -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_C_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DCMAKE_CXX_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DPS3RECOMP_DIR=%CD%\..\ps3recomp ^
  -DRECOMP_DIR=%CD%\..\recomp-work\recompiled ^
  -DSPU_DIR=%CD%\..\recomp-work\spu_lifted ^
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON ^
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF ^
  -DPS3RECOMP_SPU_AVX2=ON -DPS3RECOMP_PPU_X86_64_V3=ON ^
  -DPS3RECOMP_VMX_FAST_ACCESSORS=ON ^
  -DPS3RECOMP_BUILD_TESTS=OFF
ninja -C ..\recomp-work\build-depth MyGameRecomp
```

Notes on the Windows changes (all unverified):
- The renderer is D3D12 by default on Windows; there is no switch. Vulkan is Linux-only (see section 0).
- Linux uses `-march=native`. clang-cl does not take that flag, so `-mavx2 -mfma` is used instead. The AVX2
  flags are required (see `SETUP.md` section 7).
- `-DPS3RECOMP_BUILD_TESTS=OFF` skips the Linux-only test targets. Turn it back on only if the build needs them.
- The output should be `recomp-work\build-depth\MyGameRecomp.exe`. If the name differs, set `exe_name` in the
  launcher.

## 6. Launch

```
python mlb16-recomp\tools\launcher\mlb16_launcher.py
```

Check **Paths**: work folder, EBOOT, build dir, and executable name (`MyGameRecomp.exe`). Click
**Check paths**, then **Launch**.

Toggles are grouped (Window, Performance, SPU, Library, Visual, Diagnostics). Defaults match our measured
configuration. Turn things off one at a time to narrow down a problem.

## 7. Share your config

- **Copy config** puts the full config on the clipboard as JSON (`mlb16-launcher-1`).
- **Paste config** loads one from the clipboard.
- **Save / Load** writes or reads a `.json` file.

Send the config with any bug report, plus the log folder in `recomp-work\launcher_out\<timestamp>\`.

## 8. First-run test

1. Boot to the title or main menu. Note the screen and time.
2. Load an exhibition game. At the pitch screen press **X once** (selects the pitch, the meter appears).
   Never press X a second time (that starts the throw).
3. Note the FPS from the console or the `[flip]` lines in the log.

Report: GPU, driver version, Windows build, config JSON, and whether the window showed the game.

## Troubleshooting

Find your symptom below. Then send the log and config (step 7).

| symptom | check first |
|---|---|
| build fails: `__atomic_*` undefined | you used `cl` instead of `clang-cl` |
| CMake cannot find SDL2 | set `SDL2_DIR` to the SDL2 CMake folder |
| CMake stops with "PS3RECOMP_RSX_VULKAN is Linux-only" | remove `-DPS3RECOMP_RSX_VULKAN=ON`; Windows uses D3D12 |
| "executable not found" in the launcher | build step not done, or wrong build dir / exe name |
| "EBOOT not found" | EBOOT path points to the encrypted or wrong file |
| boot hangs on "Sony Computer Entertainment presents" | the disc or VFS is not mounted or built correctly (see `SETUP.md`) |
| black or pink screen | toggle **Black dummy for unbound textures** on |
| window freezes or present stalls | switch **Present mode** to `fifo` (our default); try `mailbox` only to compare |
| audio crackles or stutters | toggle **Audio pacing** off to compare, then report |
| very low FPS (below ~10) | toggle **Readback WCB** to 0 to check it is not the readback path |
| timeouts or crashes after a few minutes | toggle **SPURS teardown** and **Real lwcond waits** off to check |
| visuals differ from our screenshots | toggle **Swizzled render targets** and **Guest polygon offset** to compare |

If the symptom is not listed, turn off the Performance and SPU groups first. Then turn them back on one at a
time. Stop at the first toggle that changes the result and report it.

## Known gaps (Windows)

- No native PowerShell helpers yet. The launcher is Python.
- Vulkan is Linux-only in ps3recomp. Our Vulkan-side toggles (WCB, cube RT, pins, pacer, present mode) do
  nothing on Windows.
- Timer behaviour differs: Windows uses 1 ms `Sleep` in the semaphore and GCM pump paths.
  Latency numbers from Linux may not carry over.
- The launcher GUI is untested on Windows as of this writing.
