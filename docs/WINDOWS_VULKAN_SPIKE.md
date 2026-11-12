# Windows Vulkan spike: what has to change, and how to test it

Status: **code changes done, not yet built or run on Windows.** This is the checklist for getting our Vulkan
backend to build and run on Windows. The changes are committed on branch `mlb16-windows-vulkan` in ps3recomp-probe
(commit `e18c5ea4`, local only, not pushed). Line numbers are from the pre-change checkout at `356a579e`.

Goal: the same Vulkan backend we measure on Linux (about 35 FPS at the pitch meter there) running on
Windows, with the same switches. A Windows number is only comparable after we know the Windows hardware
and driver. Record them with it.

## 1. Blockers in the code

Items 1-7 are done in `e18c5ea4`. Item 8 is still open: it needs glslang for Windows at build time.


| # | where | what | fix |
|---|---|---|---|
| 1 | `ps3recomp-probe/CMakeLists.txt:52-55` | `if(WIN32 OR APPLE) FATAL_ERROR "Linux-only"` | remove the Windows check (keep Apple blocked) |
| 2 | `libs/video/rsx_vulkan_backend.c:41` | the whole file is inside `#ifndef _WIN32` ... `#endif` at line 4911 | remove the guard once the items below are done |
| 3 | `rsx_vulkan_backend.c:298-304` (9 `dl*` calls) | `dlopen("libvulkan.so.1")` / `dlsym` | `LoadLibrary("vulkan-1.dll")` / `GetProcAddress` on Windows |
| 4 | `rsx_vulkan_backend.c` (52 `pthread_` calls) | mutexes, condition variables, `pthread_create/join` | small Win32 wrappers (`CRITICAL_SECTION`, `CONDITION_VARIABLE`, `CreateThread`) inside the backend |
| 5 | `rsx_vulkan_backend.c:4146` (guarded) and `:4255` (**not guarded**) | `clock_gettime` | `QueryPerformanceCounter` on Windows; line 4255 must be fixed or it will not compile |
| 6 | `rsx_vulkan_backend.c:2258` | `rename` over an existing file | already has a Windows `remove()` first; check it on Windows |
| 7 | `templates/project/main.cpp` | `_WIN32` was tested before `PS3RECOMP_RSX_VULKAN`, so Windows always chose D3D12 | Vulkan branch moved first (done in `e18c5ea4`) |
| 8 | `rsx_shader_spirv.cpp` | guest shaders need glslang; without it the file is a stub | install glslang (in the Vulkan SDK) and link it, or the Vulkan path runs fixed-function and the picture will differ |

Reusable piece: `ps3recomp-probe/runtime/platform/win32_compat.h` already maps Win32 sync and timing onto
pthreads for POSIX. It goes the other way around, so it does not help item 4 directly. Item 4 needs a
Win32 version of the same small API used by the backend (about 52 call sites).

## 2. Things that are fine

- SDL2 has a Windows Vulkan window path (`SDL_Vulkan_CreateSurface`, `SDL_Vulkan_GetInstanceExtensions`).
- The precompiled fallback shaders (`vulkan/*_spv.h`) are byte arrays and portable.
- The NVIDIA driver installs `vulkan-1.dll`. The Vulkan SDK is only needed for headers and glslang.
- The rest of the RSX code (`rsx_draw_engine.c`, texture and vertex code) already builds on Windows for the
  D3D12 path, so it should not need changes. Confirm during the build.

## 3. Prerequisites on the Windows PC

- Visual Studio 2022 with C++ tools, clang-cl (LLVM component), Windows SDK 10.0.19041+
- CMake, Ninja, Python 3.9+
- An NVIDIA driver with Vulkan (check with `vulkaninfo` from the Vulkan SDK)
- LunarG Vulkan SDK (headers, glslang, SPIRV-Tools)
- SDL2 development libraries for MSVC

## 4. Build (once blockers 1-6 are done)

Same as `SETUP.md` section 7, Windows form (`WINDOWS.md` section 5), with the Vulkan switch added:

```
... -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_VULKAN_INCLUDE_DIR=<Vulkan SDK>\Include ...
```

Expected output: `Vulkan: RSX backend enabled` in the CMake summary, and `MyGameRecomp.exe` in the build dir.

## 5. Test plan (one launch per step, per the project rules)

1. **Configure:** CMake reaches the summary with Vulkan enabled. Save the CMake output.
2. **Build:** `ninja MyGameRecomp` finishes. Save the first error if it fails.
3. **Boot:** launch from `mlb16_launcher.py` with the default config. The window should open and show the
   title, not a black or pink screen. Look for `[RSX vulkan]` lines in `game.log`.
4. **Meter:** navigate to the pitch screen, press X once, and let it run. Save `game.log`, the MangoHud CSV
   if available, and a screenshot.
5. **Compare:** read the `[flip]` FPS from `game.log` and the frame time from the CSV. Report the GPU, driver
   version, and Windows build with the number.

Acceptance for the spike: step 3 renders the title, and step 4 reaches the meter and shows `[flip]` lines.
A number is not required to pass, but it is what we are after.

## 6. What to send back

- `game.log` and `config.json` from `recomp-work\launcher_out\<timestamp>\`
- The CMake output and the first build error, if any
- `vulkaninfo --summary` output
- GPU model, driver version, Windows build
- The meter FPS, if it reached the meter

## 7. Rules for this spike

- Do not change ps3recomp until the plan is approved. Blockers 1-6 are code changes in a shared checkout.
- Do not run the game on the Linux machine while the Windows test is running. Only one game instance at a time.
- The Linux build must keep working. Every Windows change is behind `_WIN32`, and the Linux numbers stay
  the baseline.
