# Setting up the MLB The Show 16 native port

This is the start-to-finish path from your own disc or ISO to a running native
build. Everything game-derived (the decrypted EBOOT, the lifted C++, the build,
the vfs, the shader cache) is produced on your machine and stays out of git.

The known-good source is branch `port/stable` in the ps3recomp fork
(`https://github.com/MatRanc/ps3recomp`). Do not mix lifted trees between
branches: the generated SPU modules are tied to the runtime headers.

## 1. Hardware and software

- Linux x86-64, a Vulkan 1.1 GPU with a working driver, X11 or XWayland.
- **clang** (the lifted PPU/SPU code needs clang; GCC is not enough), CMake
  3.20+, Ninja, Python 3.10+.
- Fedora package names (adapt for your distribution):
  `clang cmake ninja-build python3 python3-pip SDL2-devel vulkan-headers
  vulkan-loader-devel glslang-devel spirv-tools-devel mesa-vulkan-drivers git`.
  `mangohud` and ImageMagick (`convert`) are optional: they add the fps overlay
  and the measurement thumbnails.
- Python packages for ps3recomp: `python3 -m venv .venv && .venv/bin/pip
  install -r requirements.txt`. The lift scripts use `.venv/bin/python` when it
  exists.
- Disk: about **25-30 GB free** (the game's first-boot data install is about
  11 GB, the lifted trees and build about 1.5 GB, plus your ISO). RAM: the full
  build reaches about **17 GB at `-j8`** (about 2 GB per clang job); **use
  `-j4` on a 32 GB machine**, `-j8` only when you have the headroom.
- CPU governor `performance` for honest measurements.

## 2. Layout

The helpers default to this layout. Any other layout works through environment
variables (section 8).

```
~/Documents/mlb16-online/
  mlb16-recomp/       this repo
  ps3recomp/          clone of the fork, branch port/stable
  recomp-work/        generated: recompiled/, spu_lifted/, build-play/, vfs/
  EBOOT.elf           decrypted v01.10 EBOOT
```

```sh
git clone https://github.com/MatRanc/mlb16-recomp.git
git clone -b port/stable https://github.com/MatRanc/ps3recomp.git ps3recomp
```

## 3. Mount the disc

Insert the disc, or mount your own ISO read-only:

```sh
udisksctl loop-setup -r -f "/path/to/MLB The Show 16 (USA).iso"
ls /run/media/$USER/PS3VOLUME/PS3_GAME
```

If it mounts somewhere else, set `MLB16_ISO_MOUNT` to the `PS3_GAME` folder
(section 8).

## 4. Decrypt the EBOOT (v01.10)

The port runs the **v01.10 update** EBOOT, not the 1.00 disc EBOOT.

1. Install the game and update 1.10 in RPCS3 (`File -> Install .pkg` for the
   update; the disc works too), or install them on a PS3.
2. In RPCS3, use **Utilities -> Decrypt PS3 Binaries** on
   `dev_hdd0/game/BCUS01089/USRDIR/EBOOT.BIN`.
3. Put the decrypted output at the repo root as `EBOOT.elf`.

Using RPCS3 only as the decryption tool is fine; this project contains no RPCS3
code.

## 5. Build the writable vfs

The game needs a writable `/dev_hdd0` beside the read-only disc. `PS3_GAME`
points at the mounted disc; `game/BCUS01089` is the installed v01.10 update; the
game writes its data install to `game/BCUS01089_R` on first boot.

From the `mlb16-recomp` checkout (`~/Documents/mlb16-online/mlb16-recomp`):

```sh
WORK=$(cd .. && pwd)/recomp-work
mkdir -p "$WORK/vfs/game"
ln -s "$MLB16_ISO_MOUNT" "$WORK/vfs/PS3_GAME"
cp -a "/path/to/rpcs3/dev_hdd0/game/BCUS01089" "$WORK/vfs/game/"
# The lifted SPU sources include ../../runtime/... relative to their module,
# so recomp-work/runtime must exist and point at the checkout you build with.
ln -sfn "$PWD/../ps3recomp/runtime" "$WORK/runtime"
```

Do not point the vfs at RPCS3's own `dev_hdd0`; the port writes its install and
caches there. Keep `recomp-work/runtime` on the same ps3recomp branch as the
build; `check_spu_context_abi.sh` guards the one header both trees share.

## 6. Lift the game code and apply the patches

From the `mlb16-recomp` checkout:

```sh
tools/probe.sh ../ps3recomp ../EBOOT.elf ../recomp-work
tools/lift_spu.sh ../ps3recomp ../EBOOT.elf ../recomp-work
tools/apply_patches.sh --work ../recomp-work --ps3recomp ../ps3recomp
```

- `probe.sh` lifts the PPU code and extracts the embedded SPU images.
- `lift_spu.sh` lifts the SPU programs, the WWS job manager and the job
  modules into `recomp-work/spu_lifted/`.
- `apply_patches.sh` re-applies the generated-code patches in order: the c690
  cull hooks, the native screen hook, and the Bink readiness/keep-plane patch.
  It is idempotent; run it again after any relift.

A fresh lift does **not** need the `wws_native_pm.h` header. The optional
`--wws-native` patch (branch `exp/wws-native` or later) and `--ftz` twin
(`exp/spu-v4` or later) are off by default; `apply_patches.sh --help` lists
them.

## 7. Configure and build

```sh
cmake -B ../recomp-work/build-play -S ../ps3recomp/templates/project -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=native" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=native" \
  -DPS3RECOMP_DIR=$PWD/../ps3recomp \
  -DRECOMP_DIR=$PWD/../recomp-work/recompiled \
  -DSPU_DIR=$PWD/../recomp-work/spu_lifted \
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON \
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF \
  -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_BUILD_TESTS=ON
ninja -C ../recomp-work/build-play -j4 MyGameRecomp
```

The result is `recomp-work/build-play/MyGameRecomp` (about 260 MB).

## 8. Play

With the default siblings layout, nothing else is needed:

```sh
tools/play_windowed.sh          # MangoHud overlay + CSV
tools/play_windowed.sh nohud    # no overlay
```

The script uses `recomp-work/build-play`, `../EBOOT.elf` and
`recomp-work/vfs`. For another layout, set the variables once:

| variable | default | meaning |
|---|---|---|
| `MLB16_WORK` | sibling `recomp-work` | generated trees, build, logs, vfs |
| `MLB16_ROOT` | `$(dirname $MLB16_WORK)` | where `EBOOT.elf` lives |
| `MLB16_EBOOT` | `$MLB16_ROOT/EBOOT.elf` | decrypted EBOOT |
| `MLB16_ISO_MOUNT` | `/run/media/$USER/PS3VOLUME/PS3_GAME` | mounted disc |
| `MLB16_PS3RECOMP` (`PS3RECOMP_DIR`) | sibling `ps3recomp-play` | ps3recomp checkout |
| `MLB16_BUILD` (`BUILD`) | `build-play` | build directory name |
| `REF_DIR` | `tools/ab/ref` | measurement reference screenshots |

A simpler controller-only session without the instrumentation is
`tools/ab/play.sh`.

## 9. Measure

The automated harness parks on the pitch meter: at the pitch screen it presses
X exactly once, which selects the pitch and leaves the meter in its first state
indefinitely. Never press X a second time (that starts the throw).

```sh
cd tools/ab
MLB16_WORK=/path/to/recomp-work ./measure_fast.sh baseline
```

`measure_fast.sh` boots, navigates the menus, presses X once and reports a 10 s
flip-rate window. `ab_live.sh` does an in-run A/B by switching keys through the
`PS3RECOMP_CTL` control file:

```sh
TAG=demo N=6 SEG=10 ARM_A="" ARM_B="PS3RECOMP_RSX_WCB=0" ./ab_live.sh
```

Both scripts need the small reference screenshots under `REF_DIR`. They are
screenshots of the game and are deliberately **not** committed. Copy them from
an existing working setup's `ab-tools/ref/` (same 640x360 names), or record your
own on first play: save a 640x360 PNG of each menu screen as the name `nav3.py`
lists in its `SCREENS` table, plus `pitch_wait.png` and `pitch_meter.png`. With
no `pitch_meter.png`, `METER_CAL=1` saves calibration frames after the X press.

Measurements need the governor at `performance`, the ISO mounted, and no other
game instance running. The scripts wait for a running `MyGameRecomp` to exit
and refuse to start if the governor or ISO is wrong.

## 10. Troubleshooting

- **A build fails on `wws_native_pm.h`**: `spu_lifted/` was patched by the
  optional WWS-native script, but the checked-out ps3recomp branch has no
  header. Use a fresh lift with `port/stable`, or check out a branch that has
  `runtime/spu/wws_native_pm.h`.
- **The game refuses to start with `spu_context ABI mismatch`**: the lifted SPU
  modules were generated against a different `spu_context.h`. Re-run
  `lift_spu.sh` against the same ps3recomp checkout, or set
  `SPU_ABI_CHECK=0` only to investigate.
- **Window present stalls on Wayland**: the scripts default to
  `SDL_VIDEODRIVER=x11` (XWayland) and FIFO present mode; keep those defaults.
- **No thumbnails**: install ImageMagick or ignore the helper; the game and the
  logs are unaffected.

## Not yet verified from scratch

This path has been assembled from the exact commands and patches used by the
working build, and every script has passed static checks, but **nobody has yet
run it end to end from a fresh clone in a clean directory**. The from-scratch
clone, full build and launch test is the next step; treat that run as the
first verification of this document.
