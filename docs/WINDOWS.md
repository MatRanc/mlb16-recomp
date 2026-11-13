# MLB 16 on Windows: setup checklist

Status: **untested on Windows.** Everything below comes from the Linux build and the ps3recomp docs
(`ps3recomp-probe/docs/BUILDING.md`, `GETTING_STARTED.md`). Record what fails and send it back so the
checklist can be corrected.

The game is not included. You need your own copy and must make the EBOOT and VFS on your machine.
Do not share the ISO, PKG, EBOOT, VFS, or generated C++.

## 0. Pick the renderer first

| path | what it is | status |
|---|---|---|
| D3D12 (ps3recomp's default on Windows) | ps3recomp's reference Windows renderer, `libs/video/rsx_d3d12_backend.c` | builds on Windows (no switch needed) |
| Vulkan (`-DPS3RECOMP_RSX_VULKAN=ON`) | our measured backend | **configures on Windows, not yet linked or run.** Branch `mlb16-windows-vulkan-rebased` in ps3recomp. Plan in [`WINDOWS_VULKAN_SPIKE.md`](WINDOWS_VULKAN_SPIKE.md) |

Use Vulkan (`-DPS3RECOMP_RSX_VULKAN=ON`) when you want numbers comparable to the Linux ~35 FPS, and D3D12 otherwise.
Vulkan without glslang runs the fixed-function fallback, so its picture will differ from Linux. See section 5.

## 1. Tools

- Visual Studio 2022 with the C++ workload, plus **clang-cl**. In the VS Installer, add
  `Microsoft.VisualStudio.Component.VC.Llvm.Clang` and `Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset`
  (verified on 17.x, Build Tools and Community). Admin rights needed.
- Windows SDK 10.0.19041 or newer (tested with 10.0.26100)
- CMake 3.25+ and Ninja (Ninja ships inside the VS CMake folder: `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`)
- Python 3.9+ with tkinter (python.org installer includes it)
- **LunarG Vulkan SDK** (tested 1.4.341.1). It gives the Vulkan headers, the SDL2 headers and import library
  (`Include\SDL2`, `Lib\SDL2.lib`). No separate SDL2 install is needed. glslang comes from vcpkg, see section 5.
- Git, and Git Bash for the bash helpers. Git for Windows includes it.
- Optional, only if you need to read a Linux partition: WSL2 (needs virtualization enabled in BIOS). Not needed
  for the EBOOT, see section 3.

Use **clang-cl**, not plain `cl`, for lifted game code. The runtime uses `__atomic_*` builtins.

## 2. Get the code

```
git clone <mlb16-recomp repo>
git clone -b port/stable https://github.com/MatRanc/ps3recomp.git ps3recomp   # known-good commit 1c2bfd8a
```

Put `recomp-work/` next to `mlb16-recomp/` as on Linux, or set the work folder in the launcher.

## 3. Make the EBOOT and VFS from your own game copy

1. Decrypt `EBOOT.BIN` to `EBOOT.elf` with RPCS3 (`SETUP.md` section 4). Do this on whichever OS has the
   installed v01.10 game. The output is the same file on any OS.
2. Put the decrypted file at `C:\Users\<you>\EBOOT.elf`. If it lives on a Linux partition, boot Linux and copy it
   to a shared NTFS or FAT32 drive. Do not mount the Linux partition from Windows. ext4 is not readable there
   without a driver, and WSL mounting needs virtualization enabled in BIOS.
3. Make the VFS in `recomp-work\vfs` (`SETUP.md` section 5), with these Windows equivalents:
   - Mount the game ISO read-only: `Mount-DiskImage -ImagePath "<iso>" -Access ReadOnly` (no admin needed).
   - Link the disc: `mklink /J recomp-work\vfs\PS3_GAME <drive>:\PS3_GAME` (a junction, no admin needed).
   - Copy the installed update from RPCS3: `robocopy <rpcs3>\dev_hdd0\game\BCUS01089 recomp-work\vfs\game\BCUS01089 /E`.
   - Link the runtime: `mklink /J recomp-work\runtime ps3recomp\runtime`.

The bash helpers (`probe.sh`, `lift_spu.sh`, `apply_patches.sh`) run from Git Bash:
`C:\Program Files\Git\bin\bash.exe -lc "cd /c/Users/<you>/mlb16-recomp && tools/probe.sh ../ps3recomp ../EBOOT.elf ../recomp-work"`.
Git Bash's `python3` resolves to Python 3.10 on the test PC. `apply_patches.sh` needs
`ps3recomp/tools/c690_ref/` and `runtime/spu/wws_native_pm.h`, both on the Vulkan branch.

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

This is the Windows version of the command in `SETUP.md` section 7. Run it from a Visual Studio developer
prompt (or `vcvars64.bat` first), from the `mlb16-recomp` folder. Add `-DPS3RECOMP_RSX_VULKAN=ON` for the Vulkan
renderer, which needs the two extra paths below.

```
cmake -B ..\recomp-work\build-depth -S ..\ps3recomp\templates\project -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_RC_COMPILER="C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" ^
  -DCMAKE_C_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DCMAKE_CXX_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DPS3RECOMP_DIR=%CD%\..\ps3recomp ^
  -DRECOMP_DIR=%CD%\..\recomp-work\recompiled ^
  -DSPU_DIR=%CD%\..\recomp-work\spu_lifted ^
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON ^
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF ^
  -DPS3RECOMP_SPU_AVX2=ON -DPS3RECOMP_PPU_X86_64_V3=ON ^
  -DPS3RECOMP_VMX_FAST_ACCESSORS=ON ^
  -DPS3RECOMP_BUILD_TESTS=OFF ^
  -DPS3RECOMP_RSX_VULKAN=ON ^
  -DPS3RECOMP_VULKAN_INCLUDE_DIR=C:/VulkanSDK/1.4.341.1/Include ^
  -DCMAKE_PREFIX_PATH=C:/Users/<you>/vcpkg/installed/x64-windows
ninja -C ..\recomp-work\build-depth MyGameRecomp
```

Why these extra flags (all tested on the Windows PC):
- **clang-cl and rc must be full paths, with forward slashes.** CMake reads backslashes in `-D` values as
  escapes, and fails with `Invalid character escape '\P'`. The `rc.exe` path must match the installed SDK version.
- **glslang comes from vcpkg.** The LunarG SDK's glslang package does not resolve (its SPIRV-Tools configs point at missing libraries), so install it with `vcpkg install glslang:x64-windows` (vcpkg bootstrapped in your user folder, no admin needed). Then pass `-DCMAKE_PREFIX_PATH` to the vcpkg `installed\x64-windows` folder, as above. Do not pass the SDK's SPIRV-Tools `_DIR` flags: they make configure fail.
- **`-DPS3RECOMP_SDL2_LIB`** defaults to `Lib/SDL2.lib` next to the Vulkan SDK. Set it only if SDL2 is elsewhere.

Notes on the Windows changes:
- The Vulkan backend is `libs/video/rsx_vulkan_backend.c` on branch `mlb16-windows-vulkan-rebased` in ps3recomp.
  Without `-DPS3RECOMP_RSX_VULKAN=ON` the build uses D3D12, which is the default on Windows.
- The shared Win32 shim for POSIX calls is `include/ps3emu/posix_win32.h`. Files that use `pthread_*`,
  `sched_yield`, `nanosleep`, `clock_gettime`, or `mkdir` include it.
- The SPU twins (`*_ftz`, `*_approx`) declare the variant registry as a plain extern on Windows. A weak
  reference breaks lld-link, see `tools/spu_ftz_twin.py`.
- Linux uses `-march=native`, and clang-cl does not take that flag, so `-mavx2 -mfma` is used instead. The AVX2
  flags are required (see `SETUP.md` section 7).
- The build log is very large. Lifted code produces about 90,000 warnings. Redirect ninja to a file, do not
  read it in full, and grep for `FAILED:` and `error:`.
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
| CMake cannot find SDL2 | Vulkan builds use the SDK's `Lib/SDL2.lib`; otherwise set `PS3RECOMP_SDL2_LIB` |
| `Invalid character escape '\P'` at configure | use forward slashes in the clang-cl and rc paths (section 5) |
| `glslang not found; guest programs unavailable` | install glslang with vcpkg and pass `-DCMAKE_PREFIX_PATH` (section 5) |
| `duplicate symbol: spu_register_function_var` at link | regenerate the twins with `apply_patches.sh` after the `spu_ftz_twin.py` fix |
| `'pthread.h' file not found` | the file needs `ps3emu/posix_win32.h` (see section 5) |
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
- Vulkan on Windows is not yet linked or run, so its toggles (WCB, cube RT, pins, pacer, present mode) are
  untested there. Thread pinning (`PS3RECOMP_PIN`) is a no-op on Windows.
- Timer behaviour differs: Windows uses 1 ms `Sleep` in the semaphore and GCM pump paths.
  Latency numbers from Linux may not carry over.
- The launcher GUI is untested on Windows as of this writing.
