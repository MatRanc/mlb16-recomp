# MLB 16 on Windows: Complete Setup Guide

Status: **Tested and fully working on Windows 11 with Vulkan backend, Clang-cl, and SDL2 controller support.**

This guide details how to build and run MLB The Show 16 natively on Windows 10/11 starting from your own game disc or ISO.

> [!IMPORTANT]
> The game itself is not distributed with this project. You must own MLB The Show 16 (BCUS01089) and decrypt the v01.10 EBOOT and assets from your own copy. Never distribute decrypted game binaries or assets.

---

## 1. Prerequisites & Tools

Install the following on your Windows system:

1. **Visual Studio 2022** (Community or Build Tools):
   - In the Visual Studio Installer, select **Desktop development with C++**.
   - Under Installation details, ensure **C++ Clang Compiler for Windows** (`clang-cl`) and **Windows 10/11 SDK** (e.g. 10.0.26100.0 or 10.0.19041.0) are checked.
2. **CMake (3.25+) & Ninja**:
   - CMake from [cmake.org](https://cmake.org/download/) or winget (`winget install Kitware.CMake`).
   - Ninja is included with Visual Studio (at `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`) or via winget (`winget install Ninja-build.Ninja`).
3. **LunarG Vulkan SDK** (1.4.341+):
   - Download from [vulkan.lunarg.com](https://vulkan.lunarg.com/).
   - Provides Vulkan runtime headers, Vulkan loaders, and the SDL2 headers and import libraries (`Include\SDL2`, `Lib\SDL2.lib`).
4. **vcpkg** (for glslang / SPIRV guest shader compilation):
   ```cmd
   git clone https://github.com/microsoft/vcpkg.git %USERPROFILE%\vcpkg
   cd %USERPROFILE%\vcpkg
   bootstrap-vcpkg.bat
   vcpkg install glslang:x64-windows
   ```
5. **Python 3.10+**:
   - Ensure `pip` and `tkinter` are included (standard python.org installer has them).
   - Install required Python dependencies:
     ```cmd
     pip install capstone pyelftools
     ```
6. **Git for Windows**:
   - Includes **Git Bash**, which is used to run the lifting scripts (`probe.sh`, `lift_spu.sh`, `apply_patches.sh`).

---

## 2. Directory Layout & Repository Setup

Create a base folder (e.g., `C:\Users\<you>\mlb16-online\` or directly in your user profile):

```text
C:\Users\<you>\
├── mlb16-recomp/           (this repository)
├── ps3recomp/              (the ps3recomp fork with Windows Vulkan & SDL2 support)
├── recomp-work/            (generated: lifted code, vfs, builds, shader cache)
└── EBOOT.elf               (decrypted v01.10 EBOOT from your game)
```

Clone both repositories:

```cmd
git clone https://github.com/MatRanc/mlb16-recomp.git
git clone -b mlb16-windows-vulkan-rebased https://github.com/MatRanc/ps3recomp.git
mkdir recomp-work
```

---

## 3. Game Files & Virtual File System (VFS) Setup

The native port runs against the **v01.10 update** EBOOT and requires both the disc assets and the update Game Data.

### Step A: Decrypt the EBOOT (v01.10)
1. Install MLB The Show 16 and the v01.10 update package in RPCS3 (`File -> Install Packages/Raps/Edats`).
2. In RPCS3, open **Utilities -> Decrypt PS3 Binaries**.
3. Select `dev_hdd0\game\BCUS01089\USRDIR\EBOOT.BIN` from your RPCS3 install.
4. Save the decrypted file as `EBOOT.elf` at `C:\Users\<you>\EBOOT.elf` (or directly next to `mlb16-recomp`).

### Step B: Mount Disc ISO
In PowerShell (as normal user):
```powershell
Mount-DiskImage -ImagePath "C:\path\to\MLB The Show 16 (USA).iso" -Access ReadOnly
```
Note the assigned drive letter (e.g., `F:`).

### Step C: Build the VFS Directory Structure
Open `cmd.exe` and create the junction points and copy the game data:

```cmd
cd C:\Users\<you>\recomp-work
mkdir vfs\game

:: 1. Junction to the mounted disc image
mklink /J vfs\PS3_GAME F:\PS3_GAME

:: 2. Copy installed update from RPCS3
robocopy "C:\path\to\rpcs3\dev_hdd0\game\BCUS01089" vfs\game\BCUS01089 /E

:: 3. Copy installed Game Data (BCUS01089_R) from RPCS3 (CRITICAL)
robocopy "C:\path\to\rpcs3\dev_hdd0\game\BCUS01089_R" vfs\game\BCUS01089_R /E

:: 4. Junction to ps3recomp runtime
mklink /J runtime C:\Users\<you>\ps3recomp\runtime
```

> [!IMPORTANT]
> Copying `BCUS01089_R` from RPCS3 provides the pre-installed ~10 GB game data archives (`MLBPS3_EXT.WAD` and `MLBPS3_GD.WAD`).
> If `BCUS01089_R` is missing or incomplete, the game attempts to perform a ~10 GB background install on first boot; interrupting this will corrupt game data.

---

## 4. Lift the Game Code & Apply Patches

Run Git Bash (`C:\Program Files\Git\bin\bash.exe`) and execute the probe and lift scripts:

```bash
cd /c/Users/<you>/mlb16-recomp

# 1. Probe the decrypted EBOOT and generate PPU recompiled C++ sources
tools/probe.sh ../ps3recomp ../EBOOT.elf ../recomp-work

# 2. Lift SPU programs
tools/lift_spu.sh ../ps3recomp ../EBOOT.elf ../recomp-work

# 3. Apply game-specific SPU and runtime patches
tools/apply_patches.sh --work ../recomp-work --ps3recomp ../ps3recomp --wws-native --ftz --approx
```

---

## 5. Configure & Build with CMake & Ninja

Open the **x64 Native Tools Command Prompt for VS 2022** (or run `vcvars64.bat`):

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
  -DCMAKE_PREFIX_PATH="C:/Users/<you>/vcpkg/installed/x64-windows" ^
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON ^
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF ^
  -DPS3RECOMP_SPU_AVX2=ON -DPS3RECOMP_PPU_X86_64_V3=ON ^
  -DPS3RECOMP_VMX_FAST_ACCESSORS=ON ^
  -DPS3RECOMP_PAD_USE_SDL2=ON ^
  -DPS3RECOMP_BUILD_TESTS=OFF

:: Compile executable
ninja -C ..\recomp-work\build-depth MyGameRecomp
```

### Copy SDL2 Runtime DLL
Copy `SDL2.dll` from your Vulkan SDK into the output directory:
```cmd
copy "C:\VulkanSDK\1.4.341.1\Bin\SDL2.dll" ..\recomp-work\build-depth\
```

---

## 6. Launching the Game

### Method A: Launcher GUI (Recommended)
Run the launcher:
```cmd
python tools\launcher\mlb16_launcher.py
```
- Click **Check paths** to verify your EBOOT, VFS, disc junction, and executable.
- Click **Launch** to start the game.

### Method B: Headless Launch
```cmd
python tools\launcher\mlb16_launcher.py --headless
```

### Method C: Standalone Batch Script (`run_game.bat`)
A standalone batch script with all recommended Vulkan flags:
```cmd
C:\Users\<you>\recomp-work\run_game.bat
```

---

## 7. Controller Support

The game uses **SDL2 GameController** on Windows (`PS3RECOMP_PAD_USE_SDL2=ON`), which matches the Linux behavior:
- **PlayStation 4 (DualShock 4)**: Supported natively via USB and Bluetooth.
- **PlayStation 5 (DualSense)**: Supported natively via USB and Bluetooth.
- **Xbox 360 / One / Series X|S**: Supported natively.
- **Nintendo Switch Pro Controller & DirectInput pads**: Supported natively.
- No third-party wrappers (DS4Windows, Steam Input) are required.
- If no controller is attached, keyboard fallback is enabled on Port 0 (Arrow keys for D-Pad, Z/X/A/S for Cross/Circle/Square/Triangle, Enter for Start, Tab for Select).

---

## 8. Performance Overlay (Native MangoHud)

### Built-in In-Game Overlay (Windows & Linux)
- In the launcher GUI, go to the **Diagnostics** tab and check **MangoHud / In-Game Overlay (FPS, Frametime, GPU)** (or set `set PS3RECOMP_HUD=1` / `set MANGOHUD=1`).
- **No external software required!** The game itself renders a sleek, transparent, click-through overlay in the top-left corner:
  - Title & Title ID (`MLB The Show 16 (BCUS01089)`)
  - Live Framerate (`FPS 60.0`) in signature MangoHud green
  - Frametime (`TIME 16.6 ms`) in signature MangoHud cyan
  - GPU name and Graphics API (`Vulkan | NVIDIA GeForce RTX 3060 Ti`)
- Clicks pass completely through to the game window; it tracks window repositioning and automatically hides when minimized.
- On Linux, checking this box also integrates with the system `mangohud` command if installed and writes frametime CSV logs into `launcher_out\<timestamp>\`.

---

## 9. Troubleshooting & FAQ

| Symptom | Cause & Solution |
|---|---|
| **Window does not appear / invisible** | Ensure `run_game.bat` or the launcher is run in the active user desktop session (not from an isolated background service session). |
| **Boot hangs on Sony Computer Entertainment splash** | Corrupt or incomplete `BCUS01089_R` Game Data. Copy a verified ~10 GB `BCUS01089_R` from RPCS3 containing `MLBPS3_EXT.WAD` and `MLBPS3_GD.WAD`. |
| **Build error: `__atomic_*` undefined** | You are compiling with `cl.exe` instead of `clang-cl.exe`. Pass `-DCMAKE_C_COMPILER=clang-cl.exe -DCMAKE_CXX_COMPILER=clang-cl.exe`. |
| **CMake cannot find glslang or SPIRV** | Install via `vcpkg install glslang:x64-windows` and pass `-DCMAKE_PREFIX_PATH="C:/Users/<you>/vcpkg/installed/x64-windows"`. |
| **Game log says `no XInput pads`** | Make sure `PS3RECOMP_PAD_USE_SDL2=ON` was passed during CMake configure so SDL2 handles your controller. |
| **Black screen or missing textures** | Ensure `VK_DUMMY_BLACK=1` is set (enabled by default in launcher). |
