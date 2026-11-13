# MLB 16 on Windows: Complete Developer Setup Guide

**Status**: Tested and verified on Windows 11 with Vulkan backend, Clang-cl, native SDL2 controller support, and built-in MangoHud overlay.

This guide provides everything needed to lift, compile, and run **MLB The Show 16 (BCUS01089)** natively on Windows 10/11 starting from your own game disc or ISO.

> [!IMPORTANT]
> **Legal Notice**: The game assets and code are not distributed with this repository. You must own a legally acquired copy of MLB The Show 16 (BCUS01089) and decrypt the v01.10 EBOOT and Game Data from your own copy. Never distribute copyrighted game binaries or assets.

---

## Roadmap Overview

```
┌──────────────────┐     ┌──────────────────┐     ┌──────────────────┐
│  1. Prerequisites│ ──> │  2. Clone Repos  │ ──> │  3. Extract Assets│
│  VS, Vulkan, vcpkg│    │  mlb16 & ps3recomp│   │  RPCS3 VFS Setup │
└──────────────────┘     └──────────────────┘     └──────────────────┘
                                                           │
┌──────────────────┐     ┌──────────────────┐              ▼
│  6. Play & Test  │ <── │ 5. CMake & Ninja │ <── ┌──────────────────┐
│  Launcher & HUD  │     │ Build Executable │     │ 4. Lift Game Code│
└──────────────────┘     └──────────────────┘     │ Git Bash Scripts │
                                                  └──────────────────┘
```

---

## 1. Prerequisites & Toolchain

Install the following software before beginning:

### 1.1 Visual Studio 2022 (Community or Build Tools)
Download from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/). In the Visual Studio Installer:
* Select the **Desktop development with C++** workload.
* In the right-hand **Installation details** panel, verify the following are checked:
  * **MSVC v143 - VS 2022 C++ x64/x86 build tools**
  * **C++ Clang Compiler for Windows (`clang-cl`)** (CRITICAL — required for GCC atomics and AVX2 code generation)
  * **Windows 10/11 SDK** (e.g. 10.0.22621.0 or 10.0.26100.0)

### 1.2 CMake & Ninja
* **CMake 3.25+**: From [cmake.org](https://cmake.org/download/) or via winget:
  ```powershell
  winget install Kitware.CMake
  ```
* **Ninja**: Included with Visual Studio (`Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`) or via winget:
  ```powershell
  winget install Ninja-build.Ninja
  ```

### 1.3 LunarG Vulkan SDK (1.3 or 1.4)
Download from [vulkan.lunarg.com](https://vulkan.lunarg.com/).
* The installer automatically configures the `%VULKAN_SDK%` environment variable.
* The SDK provides Vulkan runtime headers, Vulkan loaders, and the SDL2 headers and import libraries (`Include\SDL2`, `Lib\SDL2.lib`).

### 1.4 vcpkg (Guest Shader Compiler)
Open a regular Command Prompt or PowerShell and install `glslang`:
```cmd
git clone https://github.com/microsoft/vcpkg.git %USERPROFILE%\vcpkg
cd %USERPROFILE%\vcpkg
bootstrap-vcpkg.bat
vcpkg install glslang:x64-windows
```

### 1.5 Python 3.10+
Install Python from [python.org](https://www.python.org/) (ensure **Add Python to PATH** and **tcl/tk and IDLE** are checked). Then install required lifting packages:
```cmd
pip install capstone pyelftools
```

### 1.6 Git for Windows
Download from [git-scm.com](https://git-scm.com/).
* This installs **Git Bash** (`C:\Program Files\Git\bin\bash.exe`), which is required for running the Linux-compatible lifting shell scripts in Step 4.

---

## 2. Workspace Directory Layout & Cloning

Choose a workspace root (for example, `C:\Users\<you>\mlb16-dev\` or directly in your user folder `%USERPROFILE%`):

```text
C:\Users\<you>\
├── mlb16-recomp/           (branch: main)
├── ps3recomp/              (branch: mlb16-windows-vulkan-rebased)
├── recomp-work/            (generated: lifted code, vfs, builds, cache)
└── EBOOT.elf               (decrypted v01.10 EBOOT from your game)
```

Open a Command Prompt in your chosen root and clone both repositories:

```cmd
:: 1. Clone the game recomp repository (main branch)
git clone https://github.com/MatRanc/mlb16-recomp.git

:: 2. Clone the ps3recomp runtime (MUST use mlb16-windows-vulkan-rebased branch)
git clone -b mlb16-windows-vulkan-rebased https://github.com/MatRanc/ps3recomp.git

:: 3. Create the working build directory
mkdir recomp-work
```

> [!IMPORTANT]
> **Branch Requirement for `ps3recomp`**:
> You **must** be on the branch **`mlb16-windows-vulkan-rebased`**. This branch contains:
> * Windows Vulkan presentation backend (`rsx_vulkan_backend.c`)
> * Native SDL2 gamepad input for PlayStation & Xbox controllers (`cellPad.c`)
> * Built-in MangoHud overlay with frametime histogram graph and live min/max tracking
> 
> If you already cloned `ps3recomp` on a different branch, switch to it with:
> ```cmd
> cd ps3recomp
> git checkout mlb16-windows-vulkan-rebased
> git pull origin mlb16-windows-vulkan-rebased
> ```

---

## 3. Game Files & Virtual File System (VFS) Setup

MLB The Show 16 runs against the **v01.10 update** EBOOT and requires both the disc assets and the pre-installed Game Data.

### Step 3.1: Decrypt the v01.10 EBOOT
1. In RPCS3, install the game disc and the official v01.10 pkg update (`File -> Install Packages/Raps/Edats`).
2. Go to **Utilities -> Decrypt PS3 Binaries**.
3. Select `dev_hdd0\game\BCUS01089\USRDIR\EBOOT.BIN` inside your RPCS3 directory.
4. Save the decrypted ELF file as `EBOOT.elf` directly at `C:\Users\<you>\EBOOT.elf` (as a sibling to `mlb16-recomp` and `ps3recomp`).

### Step 3.2: Mount the Game Disc ISO
In PowerShell (as normal user):
```powershell
Mount-DiskImage -ImagePath "C:\path\to\MLB The Show 16 (USA).iso" -Access ReadOnly
```
Take note of the virtual drive letter assigned by Windows (e.g., `F:`).

### Step 3.3: Set Up the VFS Junctions and Data
Open a standard **Command Prompt (`cmd.exe`)** and run:

```cmd
cd C:\Users\<you>\recomp-work
mkdir vfs\game

:: 1. Junction to the mounted disc image (replace F: with your drive letter)
mklink /J vfs\PS3_GAME F:\PS3_GAME

:: 2. Copy installed update v01.10 from RPCS3 (~1.5 GB)
robocopy "C:\path\to\rpcs3\dev_hdd0\game\BCUS01089" vfs\game\BCUS01089 /E

:: 3. Copy installed Game Data from RPCS3 (CRITICAL ~10 GB)
robocopy "C:\path\to\rpcs3\dev_hdd0\game\BCUS01089_R" vfs\game\BCUS01089_R /E

:: 4. Junction to ps3recomp runtime shaders and data
mklink /J runtime C:\Users\<you>\ps3recomp\runtime
```

> [!WARNING]
> **Why `BCUS01089_R` is Critical**:
> The `BCUS01089_R` folder contains the pre-installed game archives (`MLBPS3_EXT.WAD` and `MLBPS3_GD.WAD`), totaling ~10 GB. 
> If this folder is missing or incomplete, the game tries to perform a background disc installation on first boot, causing an infinite splash screen hang.

---

## 4. Lift the Game Code & Apply Patches

> [!NOTE]
> Run this step inside **Git Bash** (`C:\Program Files\Git\bin\bash.exe`), as these scripts use Unix tools (`awk`, `sed`, `sh`).

Open Git Bash and run:

```bash
cd /c/Users/<you>/mlb16-recomp

# 1. Probe the decrypted EBOOT and disassemble PPU instructions to C++
tools/probe.sh ../ps3recomp ../EBOOT.elf ../recomp-work

# 2. Lift SPU isolated programs
tools/lift_spu.sh ../ps3recomp ../EBOOT.elf ../recomp-work

# 3. Apply game-specific SPU and runtime optimizations
tools/apply_patches.sh --work ../recomp-work --ps3recomp ../ps3recomp --wws-native --ftz --approx
```

When finished, `recomp-work/recompiled/` and `recomp-work/spu_lifted/` will contain the generated C++ sources ready for compilation.

---

## 5. Configure & Build with CMake & Ninja

> [!NOTE]
> Run this step in the **x64 Native Tools Command Prompt for VS 2022** (accessible from your Windows Start Menu).

### Step 5.1: Run CMake Configure
In the x64 Native Tools prompt:

```cmd
cd C:\Users\<you>\mlb16-recomp

cmake -B ..\recomp-work\build-depth -S ..\ps3recomp\templates\project -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="clang-cl.exe" ^
  -DCMAKE_C_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DCMAKE_CXX_FLAGS_RELEASE="/O2 /DNDEBUG -mavx2 -mfma" ^
  -DPS3RECOMP_DIR="%CD%\..\ps3recomp" ^
  -DRECOMP_DIR="%CD%\..\recomp-work\recompiled" ^
  -DSPU_DIR="%CD%\..\recomp-work\spu_lifted" ^
  -DPS3RECOMP_RSX_VULKAN=ON ^
  -DCMAKE_PREFIX_PATH="%USERPROFILE%/vcpkg/installed/x64-windows" ^
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON ^
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF ^
  -DPS3RECOMP_SPU_AVX2=ON -DPS3RECOMP_PPU_X86_64_V3=ON ^
  -DPS3RECOMP_VMX_FAST_ACCESSORS=ON ^
  -DPS3RECOMP_PAD_USE_SDL2=ON ^
  -DPS3RECOMP_BUILD_TESTS=OFF
```

### Step 5.2: Build the Game Executable
```cmd
ninja -C ..\recomp-work\build-depth MyGameRecomp
```
Ninja will compile the lifted code and link `MyGameRecomp.exe` in `recomp-work\build-depth\`.

### Step 5.3: Copy the SDL2 Runtime DLL
Copy `SDL2.dll` from your Vulkan SDK into the build directory:
```cmd
copy "%VULKAN_SDK%\Bin\SDL2.dll" ..\recomp-work\build-depth\
```
*(If `%VULKAN_SDK%` is not set in your prompt, copy from `C:\VulkanSDK\<your-version>\Bin\SDL2.dll`)*.

---

## 6. Launching the Game

### Method A: Launcher GUI (Recommended)
Double-click `run_launcher.bat` inside the root of `mlb16-recomp\`, or run:
```cmd
python tools\launcher\mlb16_launcher.py
```
* **Verify paths**: Click **Check paths** to confirm your EBOOT, VFS junctions, and executable are found.
* **Launch**: Click **Launch** to start the game!
* The launcher automatically manages all Vulkan presentation flags, frame pacing, controller initialization, and MangoHud overlay state.

### Method B: Headless Launch
To launch directly from a script or terminal without opening the GUI:
```cmd
python tools\launcher\mlb16_launcher.py --headless
```

---

## 7. Controller Support

Controller input is handled natively by **SDL2 GameController** (`PS3RECOMP_PAD_USE_SDL2=ON`), providing plug-and-play controller support:

* **DualShock 4 (PS4)**: Supported natively over USB and Bluetooth.
* **DualSense (PS5)**: Supported natively over USB and Bluetooth.
* **Xbox Controllers**: Xbox 360, Xbox One, Xbox Series X|S supported natively.
* **Nintendo Switch Pro Controller & DirectInput pads**: Supported natively.
* **No third-party wrappers**: You do **not** need DS4Windows, Steam Input, or third-party drivers.
* **Keyboard Fallback**: If no controller is connected, port 0 maps Arrow Keys (D-Pad), Z/X/A/S (Cross/Circle/Square/Triangle), Enter (Start), and Tab (Select).

---

## 8. Performance Overlay (Native MangoHud)

The Windows build includes a **native, built-in MangoHud overlay** integrated directly into `ps3recomp`'s Vulkan engine.

```text
┌──────────────────────────────────────────────────┐
│ MLB The Show 16 (BCUS01089)                      │
│ FPS 60.0    TIME 16.6ms   min 15.8ms             │
│                           max 18.2ms             │
│ Vulkan | NVIDIA GeForce RTX 3060 Ti              │
│ ┌──────────────────────────────────────────────┐ │
│ │. . . . . . . . . . . . . . . . . . . . 16.6ms│ │
│ │██████████████████████████████████████████████│ │
│ └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

* **Zero External Software**: No RTSS, no MSI Afterburner, no UAC prompts, and no background services.
* **Enabled by Default**: Checked by default in the launcher under **Diagnostics -> MangoHud / In-Game Overlay** (or via `set PS3RECOMP_HUD=1`).
* **Features**:
  * **FPS & Live Frametime**: Real-time FPS with millisecond frametime.
  * **Dynamic Min/Max**: Accurately tracks the lowest and highest frametime within the current graph period (unclamped so true hitch durations are visible).
  * **Frametime Histogram Graph**: 120-sample live scrolling graph with a dotted `16.6ms` 60 FPS reference baseline and color-coded bars (Green = 60 FPS, Lime = minor hitch, Orange = drop, Red = spike).
  * **Click-Through & Pinned**: The overlay is completely click-through (`WS_EX_TRANSPARENT`), stays locked to the game window when moved, and automatically hides when the game is minimized.

---

## 9. Troubleshooting & FAQ

| Symptom | Root Cause | Solution |
|---|---|---|
| **Boot hangs on Sony Computer Entertainment splash screen** | Missing or incomplete `BCUS01089_R` Game Data archive. | Copy a verified ~10 GB `BCUS01089_R` folder from RPCS3's `dev_hdd0\game\` containing `MLBPS3_EXT.WAD` and `MLBPS3_GD.WAD`. |
| **Build error: `__atomic_*` built-in function missing** | Visual Studio's `cl.exe` was used instead of Clang. | Ensure you pass `-DCMAKE_C_COMPILER="clang-cl.exe" -DCMAKE_CXX_COMPILER="clang-cl.exe"` to CMake. |
| **Missing `SDL2.dll` at game launch** | The SDL2 runtime library was not copied to the build folder. | Run `copy "%VULKAN_SDK%\Bin\SDL2.dll" ..\recomp-work\build-depth\`. |
| **Game log says `no XInput pads found`** | Built without SDL2 controller support. | Ensure `-DPS3RECOMP_PAD_USE_SDL2=ON` was included in your CMake configure command. |
| **Black screen or missing 3D models** | Dummy texture layer missing. | Ensure `VK_DUMMY_BLACK=1` is set in the environment (enabled by default in the launcher). |
| **`glslangValidator` or SPIRV not found** | vcpkg dependencies not installed or prefix path missing. | Run `vcpkg install glslang:x64-windows` and ensure `-DCMAKE_PREFIX_PATH` points to your vcpkg installed folder. |
| **Permission denied writing `MyGameRecomp.exe` during build** | A previous instance of the game is still running in the background. | Open Task Manager or PowerShell and stop `MyGameRecomp.exe` before rebuilding (`Stop-Process -Name MyGameRecomp -Force`). |

---

## 10. Summary Verification Checklist

Before running your first game, confirm your workspace matches this checklist:

- [ ] `EBOOT.elf` exists at `C:\Users\<you>\EBOOT.elf`
- [ ] `ps3recomp` is on branch `mlb16-windows-vulkan-rebased`
- [ ] `vfs\PS3_GAME` links to your mounted disc ISO
- [ ] `vfs\game\BCUS01089_R` contains `MLBPS3_EXT.WAD` and `MLBPS3_GD.WAD` (~10 GB)
- [ ] `vfs\game\BCUS01089` contains the v01.10 update files
- [ ] `runtime` junction points to `ps3recomp\runtime`
- [ ] `build-depth\SDL2.dll` is present
- [ ] Launch via `mlb16-recomp\run_launcher.bat` and enjoy!
