# Bink video: why the intro never opened, and the two HLE fixes

Date 2026-11-05. Code on ps3recomp `exp/live-ab`, build `recomp-work/build-play`.
Clean room: the EBOOT in Ghidra, this repo's runtime, the existing play logs and
the offline fixtures only. No RPCS3 or other GPL emulator source was read or
cited.

## 1. The symptom, narrowed

The 00:47 log opens `/dev_bdvd/PS3_GAME/USRDIR/PS3/UI/Movies/intro/MLB_INTRO.BIK`
(fd 12) and never reads fd 12; every following read is the GD WAD (fd 7). With
`PS3_FSLOG=1 FS_CALLER=1 PS3_FSLOG_READS=12` and a write-watch on the Bink error
buffer (`PPU_WWATCH=0x01476028`) the failure is exact:

```
[fs] open '.../MLB_INTRO.BIK' -> fd 12 (opened by func_0007F600+0x849)
[fs] fstat fd=12 (by func_0007F600+0xD13)
[ww] 0x01476028 <- 0x4E ... guest-fn=0x00EE5938 lr=0x00EE5B10   # 'N'
... 0x6F 0x74 0x20 0x61 0x20 0x42 0x69 0x6E 0x6B ...
```

The buffer spells **"Not a Bink file."**, written at `0x00EE5B10` right after the
SDK's header-read callback, and **no** `cellFs`/AIO read of fd 12 occurs. So
BinkOpen runs, its 0x2c-byte header read returns no data, the magic check at
`0x00EE5B14` sees stale stack and the game abandons the movie.

## 2. Root cause: the two fd tables

The Bink SDK's default PS3 IO is statically linked into the EBOOT:

| EA | Role |
|---|---|
| `0x00EE7D00` | default Bink IO open (`*(0x010D9760)`), installed by `0x00EE5938` |
| `0x00A42008` | `sys_fs_open` wrapper (only used when flag `0x800000` is clear) |
| `0x00A41F1C` | `sys_fs_lseek` wrapper |
| `0x00A420A0` | `sys_fs_read` wrapper |
| `0x00EE8690` | default IO read: `sys_fs_lseek(fd, off)` then `sys_fs_read(fd, buf, n)` |

The open flags this title passes are `0x00904400`, which **has** `0x800000`, so
`0x00EE7D00` takes the "fd already open" branch: it stores the handle the game
supplied in the Bink IO struct and performs `sys_fs_lseek` on it. That handle is
the fd the game's own file layer produced with **`cellFsOpen`** (fd 12).

On hardware `cellFsOpen` and the raw `sys_fs_*` calls share **one kernel fd
space**, so `sys_fs_read(12)` reads the file cellFs opened. Our runtime kept two
independent tables (`runtime/ppu/ppu_fs.cpp` for cellFs, `runtime/syscalls/
sys_fs.c` for sys_fs), so the sys_fs slot was empty, `sys_fs_read` returned
`CELL_EBADF` silently (no `[fs]` line), the header stayed stale, and the SDK
reported "Not a Bink file.".

This also explains why nothing else looked wrong: the open and the fstat are
`cellFs` and succeed; only the Bink reader crosses into the sys_fs table.

## 3. The fixes

### 3.1 One fd space (the movie open)

* `runtime/ppu/ppu_fs.cpp` exposes `ppu_cellfs_file_for_fd(int)` and never hands
  out a number the sys_fs table already owns (`sys_fs_fd_active`, weak so the
  standalone filesystem fixtures still link).
* `runtime/syscalls/sys_fs.c`:
  * `sys_fs_read`, `sys_fs_lseek` and `sys_fs_fstat` fall back to that accessor
    when the fd is not one of theirs;
  * `sys_fs_open` skips fd numbers cellFs owns.

The fallback is deliberately one-way and miss-only: the sys_fs table stays
authoritative for its own descriptors. With both allocators avoiding the other
table, a number can no longer name two different files.

### 3.2 The Bink SPU task attribute (the decode)

The Bink task (`BinkSPU.spu.elf`, image 5 at `0x01184E00`) was created with
`szctx=0 lsp=0` and parked in `WAIT_SIGNAL` from boot:

```
[cellSpurs] TaskAttr R8-form: argument_ea=0x00000000
[cellSpurs] _TaskAttributeInitialize(eaElf=0x01184E00 ctx=0x0FEFF5F8 szctx=0 lsp=0x00000000 ...)
[spu_workload] WAIT_SIGNAL#1 enter task=0 taskset=0x0147ED00 ran=0ms
```

`FUN_00EF8494` (0x00EF8768-0x00EF8798) builds a three-word descriptor at
`r7`: `[0]` aligned context EA, `[1]` context save-area size (from
`FUN_00EF8BA8`), `[2]` lsPattern; it passes `r8 = 0`. Our `_cellSpursTaskAttribute
Initialize` default is the six-argument R8 form (correct for YDKJ/Jackbox, where
`r8` is a `CellSpursTaskArgument*`), so the descriptor was interpreted as the
context EA and size/lsPattern were left zero -- the task library refuses a
blocking wait without them.

`libs/spurs/cellSpurs.c` now auto-detects the descriptor form when `r8 == 0` and
the three words at `r7` are plausible (context EA and size in the guest range;
`lsPattern` may be 0 because `spurs_pm_build_context` installs a full-coverage
pattern for every task). The R8 form is untouched, and `SPURS_TASKATTR_DESC`
still forces the old always-descriptor behaviour for other titles.

## 4. Offline verification

Two new fixtures compile the real sources; no game run:

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_sys_fs_cellfs_bridge.py     # 12 checks PASS
python3 tools/test_spurs_taskattr_desc.py      # 18 checks PASS
python3 tools/test_cellfs_vfs_root.py          # unchanged, 5 cases PASS
```

`test_sys_fs_cellfs_bridge.py` opens a real temp file, hands its `FILE*` to
`ppu_cellfs_file_for_fd`, then calls `sys_fs_read`/`sys_fs_lseek`/`sys_fs_fstat`
on that fd and checks the bytes, the stream position and the stat size.
`test_spurs_taskattr_desc.py` feeds the real initializer the MLB descriptor
(including a heap context EA in the `0x16xxxxxx` range), the R8 form, and an
implausible descriptor.

## 5. Live verification status (2-launch cap)

Two automated launches were used (boot -> intro -> pitch meter, X once at the
meter):

1. `recomp-work/ab_live_binkdiag_20261105_014019.log` -- diagnosis: the error
   buffer says "Not a Bink file.", no fd-12 read, nav reaches the meter.
2. `recomp-work/ab_live_binkfix_20261105_015216.log` -- with the stricter SPURS
   change only (it did not fire: `d[2] == 0` and the first heap bound were too
   strict), the same error at 01:52:29; the Bink IO table read is
   `0x010D9760`, i.e. the default SDK table. The automated nav still reached the
   pitch meter (01:55:52).

The fd bridge and the relaxed descriptor bounds landed **after** the cap, so
they have offline tests but **no live verification yet**. The intro may now
decode; if it does not, the next log will show whether the header read happened
(a `[fs] read fd=12`/`[fs] aio read id=... fd=12` line and no "Not a Bink
file.") and whether the Bink task left `WAIT_SIGNAL`.

## 6. User check (~2 minutes)

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3_FSLOG=1 ../mlb16-recomp/tools/play_windowed.sh
```

Do **not** press X during the first ~30 s: that skips the intro on many titles.
Watch the intro play, then go to team select and look at the floating 3D team
logo. Then close the window and:

```sh
L=$(ls -t play_*.log | head -1)
grep -nE "MLB_INTRO|read fd=12|aio read id=.*fd=12|Not a Bink|WAIT_SIGNAL" "$L" | head -40
```

* `read fd=12` / `aio read ... fd=12` and no "Not a Bink file.": the open path
  is fixed; if the picture is still missing, the remaining loss is after the
  header (decode/upload/composite).
* "Not a Bink file." again: the header read still did not reach the file; send
  that log.
* The Bink task still sleeping in `WAIT_SIGNAL` after the intro starts: the
  descriptor form did not apply; send the `TaskAttr` lines.

## Live check by Claude (2026-11-05 02:10, build-play with 1dc55e13)

One automated boot (`ab_live_binkcheck_20261105_021037`, START_SCREEN=1). The Bink task now takes the descriptor form
(`TaskAttr DESC-form (auto) ... size=5120`), the game creates the `Bink Snd` / `Bink IO` threads at the intro open
(02:10:49), and the Bink SPU task runs in WAIT_SIGNAL cycles (~30 ms each) until 02:10:57, i.e. the intro is being
decoded for ~8 s instead of being dropped at the header. The start screen afterwards runs at ~47 fps ([flip] lines are
every 10th flip). Whether the picture is shown is not visible in this run's thumbnails (they start after boot): user
check pending. Note the decode cadence (~30 ms per task run) if the intro looks slow.
