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

## 7. 2026-11-05: the team-select green flash

Date 2026-11-05. **No game was launched.** Sources: the EBOOT in Ghidra, the
`ps3recomp-play` runtime and fixtures, and the 2026-11-05 user log
`recomp-work/play_20261105_110451.log`. No RPCS3 or other GPL emulator source
was read or cited.

### 7.1 The colour is the cleared movie plane

Scrolling the team list quickly shows a solid green rectangle exactly the size
of the floating logo movie for a frame or two; slow switching does not. The
logo is a Bink movie: `TeamSelect_UpdateLogoSprites` (`0x0028d78c`) loads
`LOGO_AWAY`/`LOGO_HOME` and, for sprite type 7, calls `UI_Movie_LoadBik`
(`0x006d204c`) on the same path as the intro. The movie is sampled as three
8-bit planes (`binkY` on TEXUNIT3, `binkCr`/`binkCb` on 4/5, `binkA` on 6, the
`BinkOpaque`/`BinkAlpha` techniques, string table `0x011234c0`) and converted
in the fragment program. All-zero planes give Y=0, Cb=0, Cr=0, which the BT.601
conversion clamps to RGB ~(0, 135, 0): the green in the report.

### 7.2 The game clears the planes to zero, then draws them on a blind 4-tick gate

`UI_Movie_Open` (`0x0097fff0`) allocates two frame-buffer sets with
`UI_Movie_AllocPlanes` (`0x0097f464`): `BinkY` at w x h and `BinkCR`/`BinkCB`
at w/2 x h/2, format `0xe` (8 bpp), plus an optional `BinkA`. It then, for each
set:

1. clears every texel to 0 through `Texture_ClearLevelById` (`0x004db4d4`) ->
   `Texture_FillLevel` (`0x004f7a80`, format-0xe store at `0x004f7bd8`);
2. locks the level (`Asset_LockTextureLevel` `0x004b907c`) and stores the CPU
   pointer in the Bink frame-buffer descriptor;
3. calls `BinkRegisterFrameBuffers` (`0x00ee033c`) and `BinkDoFrameAsync`
   (`0x00ee9484`).

`UI_Movie_Close` (`0x0097fd18`) clears all eight handles again at `0x0097fdd8`,
so there is no retained previous logo in the buffers.

The sprite is drawn only when `UI_Movie_GetTextureIfReady` (`0x00981488`)
returns the plane handles. Its test is:

```
if (ctx->playing == 0) return 0;          /* +0x1d5 */
if (ctx->update_tick < 4) return 0;       /* +0x1d0, 0x009814ac..0x009814bc */
return &ctx->planes[(ctx->decoded_frames & 1) * 0x10];  /* +0x110 */
```

`UI_Movie_Update` (`0x00980f20`) increments `+0x1d0` on every update call
*before* `BinkWait` (`0x00ee29d8`) at `0x00981214`, and increments the
decoded-frame counter `+0x110` only after a frame completes (`0x00981268`,
`0x009812a8`). The getter never checks `BinkWait` or `+0x110`. So the first
three updates draw nothing and the fourth draws whatever the planes hold. If
the first decode has not landed by then, that is exactly the zeroes the open
wrote: green, at the video's size.

### 7.3 Why fast scrolling and not slow

The gate is a fixed number of update calls (four UI frames), not a wait for
the decoder. With a slow switch the movie has been open long enough for the
first frame to land before the fourth draw; with a fast scroll the same frame
is presented while the planes are still the cleared zeroes. The 2026-11-05
user log is consistent: the Bink task is idle at team select (`WAIT_SIGNAL`
between the intro and the switches), the two 1024x512 local targets appear at
06:25 and a new `Bink IO` thread is created at each switch (06:31, 06:44).
On hardware/RPCS3 the first Bink frame lands inside that window; in our port
the Bink decode and the bind/upload do not.

### 7.4 What our renderer adds, and the fix

TEX_DIRTY does cover the plane writes (the SPU MFC PUT path calls
`ps3_guest_write`, and PPU inlined stores fault into the same counter). Two
renderer details still diverge from hardware, which samples the live bytes:

* the cache revalidates a texture at most once per presented frame
  (`last_hash_frame == g.frames` skips every later bind), and the per-unit
  texture memo answers same-frame repeat binds without revalidating at all;
* the default host-write order charged the write generation *before* the
  memcpy, so a bind racing the copy could sample the new generation with the
  old bytes and then pin that stale upload until some unrelated write.

`exp/bink-green` (build `recomp-work/build-green`) adds, behind
`PS3RECOMP_BINK_GREEN_FIX` (**env, default 0**):

* an ordered SPU MFC PUT: open the tracked pages, copy, then charge the write
  (`rsx_texdirty_prepare_write_host` + `ps3_guest_write`);
* same-frame revalidation of a dirty-tracked texture (`eng_texture_slot_work`)
  and a memo bypass when the generation moved (`sink_bind_textures`).

The first 16 hits log `[bink-green] same-frame reupload 0x... WxH fmt=...`;
engine shutdown logs the session count.

**Limit, stated honestly.** The fix closes the window between our bind/upload
and the GPU's sampling. If the first decoded frame lands after the frame's
draw, the guest's own four-tick gate still shows the cleared planes; removing
that residue needs a faster Bink decode (the intro's 100-132 ms frames are the
same decoder, [`boot-gaps.md`](boot-gaps.md) §4) or a guest-side wait, not a
renderer change. The user check below says which case we are in.

### 7.5 Offline verification

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-green
python3 tools/test_video_fixtures.py
```

`test_rsx_draw_engine` gains three checks: a write after the first bind is
re-uploaded on a second bind in the **same** frame; opening the pages alone
does not invalidate the texture; the post-copy note does. The suite passed
34/34. (`test_gcm_tick_in_drain` is a pre-existing ~50 % flake: it fails the
same way on unmodified `exp/live-ab`; every other fixture is deterministic.)
The full `build-green` binary builds with the play options copied from
`build-play` (`-O2 -DNDEBUG -march=native`, Vulkan, 14 SPU images).

### 7.6 User check (~2 minutes)

```sh
cd /home/m/Documents/mlb16-online/recomp-work
BUILD=build-green PS3RECOMP_BINK_GREEN_FIX=1 ../mlb16-recomp/tools/play_windowed.sh
```

Reach team select and scroll the team list quickly with the d-pad. Look at the
floating logo:

* green gone or visibly shorter: the bind/GPU window was the loss; keep the
  key on and report the `[bink-green]` lines;
* green unchanged: the first frame lands after the draw, i.e. the guest's
  own four-tick race; the next step is the decode side, not the renderer.
  Send the `[bink-green]` summary (it will be absent or small) and the Bink
  task `WAIT_SIGNAL` `ran=` values.

Then close the window and check:

```sh
L=$(ls -t play_*.log | head -1)
grep -n 'bink-green' "$L" | head -20
```

Run once more without `PS3RECOMP_BINK_GREEN_FIX=1` to confirm the A/B.

### 7.7 Ghidra renames made for this mission

| Old | New | EA |
|---|---|---|
| `FUN_00980f20` | `UI_Movie_Update` | `0x00980f20` |
| `FUN_00981488` | `UI_Movie_GetTextureIfReady` | `0x00981488` |
| `FUN_0097fff0` | `UI_Movie_Open` | `0x0097fff0` |
| `FUN_0097f464` | `UI_Movie_AllocPlanes` | `0x0097f464` |
| `FUN_0097fd18` | `UI_Movie_Close` | `0x0097fd18` |
| `FUN_00980b00` | `UI_Movie_OpenFile` | `0x00980b00` |
| `FUN_004db4d4` | `Texture_ClearLevelById` | `0x004db4d4` |
| `FUN_004f7a80` | `Texture_FillLevel` | `0x004f7a80` |
| `FUN_0028d78c` | `TeamSelect_UpdateLogoSprites` | `0x0028d78c` |
| `FUN_006a8108` | `UI_Sprite_Render` | `0x006a8108` |
| `FUN_006d17b0` | `UI_Movie_TickAndGetTexture` | `0x006d17b0` |
| `FUN_00981de0` | `UI_Movie_KickNextDecode` | `0x00981de0` |
| `FUN_0098145c` | `UI_Movie_UpdateThunk` | `0x0098145c` |

### 7.8 Caveats

* The `+0x1d0 >= 4` gate and the zero clear explain the colour and the fast/slow
  difference statically; no live trace of `+0x1d0` was taken.
* Which of the two renderer windows (bind-vs-GPU, or decode landing after the
  draw) dominates is not decided offline; the keyed A/B in 7.6 decides it.
* The two 1024x512 local targets at team entry are consistent with the movie
  planes but not proven to be them; the colour reasoning does not depend on
  their identity.

**Follow-up (2026-11-05):** the RSX-side ordering is now analysed separately,
with the guest's `SET_REFERENCE` reuse fence and the write-vs-pending-draw
window: [`rsx-sync.md`](rsx-sync.md) (fix `PS3RECOMP_RSX_SYNC_FIX`, default 0).

## 8. 2026-11-05 evening: the real cause and the fix (verified live)

**Cause:** the green flash comes from `UI_Movie_Close` (`0x0097fd18`), not from the open path. On a team switch it:
1. stops the Bink thread;
2. clears all eight planes with `Texture_ClearLevelById` (`0x004db4d4` → `Texture_FillLevel` `0x004f7a80`, a CPU fill, call at `0x0097fdd8`), with **no GPU wait** first;
3. only then runs `Gfx_WaitLastFrameSubmit` / `Gfx_WaitLastFrameRef` and `Asset_Release`.

The RSX has long sampled the planes by step 2. Our walker samples a pending draw about one frame (~17-19 ms) after `UI_Movie_GetTextureIfReady` handed the planes out, so about 1 switch in 4 it uploads the cleared planes. All planes read zero there, alpha included.

**How it was found** (diagnostics, all opt-in):
- `PS3RECOMP_BINK_LOG=1` (`[bink-open]`, `[bink-ready]` with ms and flip) and `PS3RECOMP_TEX_ZERO_LOG=1` (`[tex-zero]`, every logo-plane upload with ms) gave the timeline: good upload, Bink thread exit, blank upload, and only then the next open.
- `PS3RECOMP_BINK_DMA_LOG=1` (`[bink-dma]`, SPU PUTs into the planes) showed the SPU decoder never writes zero Y data.
- `PPU_WWATCH=1594BD80 PPU_WWATCH_LEN=0x10 PPU_WWATCH_MAX=0` caught `Texture_FillLevel` called from lr `0x004DB4E4` between the two uploads.

**Fix:** `mlb16-recomp/tools/patch_bink_ready.py` (`patch_close`) skips the close-path clear. Key `PS3RECOMP_BINK_CLOSE_KEEP`, **default on**; set it to 0 to restore the clear. The planes are released right after, or cleared again by the next open, so nothing else depends on that clear.
- Live, user run 16:45 (`play_20261105_164539.log`): 0 of 449 logo-plane uploads were blank, and the user saw no green. The three runs before it had ~30 blank uploads each and visible green.
- The patch is applied to the generated code, so rerun `patch_bink_ready.py <recompiled>` after every regeneration.

**Earlier attempts that did not help** (all still opt-in, default off):
- `BINK_GREEN_FIX`: renderer same-frame revalidation.
- `BINK_WAIT_DECODE`: decode was never late.
- `BINK_KEEP_PLANES`: skipped only the open path's reuse clear; `reuse` was always 0.
- `RSX_SYNC_FIX` ([rsx-sync.md](rsx-sync.md)): the close-path stores land on pages the SPU decode had already opened, so they never fault into the wait. Its "half a second of green" was the decoder being slowed down by the wait.
