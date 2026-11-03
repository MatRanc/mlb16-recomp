# HLE contract audit: return values and output parameters the game consumes

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`; EBOOT
`a58eb36f8bc058402021421cb63d18c4` (Ghidra image base 0). Static only: no game
runs and no behaviour changes.

This audit answers the question raised by
[`backward-frames.md`](backward-frames.md): `cellGcmSetPrepareFlip` returned
`CELL_OK` instead of the display-buffer id, the game stored that value in its
flip queue and later passed it to `cellGcmSetFlipImmediate`, so every flip
named buffer 0 and the display stepped backward. How many more HLE functions
disagree with the SDK in a way that this game can observe?

The known `cellGcmSetPrepareFlip` fix is the reference case, not a new finding.
It is enabled by `PS3RECOMP_PREPARE_FLIP_RET=1` in both `play_windowed.sh`
copies; the code default remains off (`libs/video/cellGcmSys.c:4045`).

## Summary

* **433** firmware imports across 32 libraries in the EBOOT.
* **407** of them have at least one direct call site in the lifted PPU code:
  **1,604** call sites total.
* **26** imported functions are never called by this title.
* At **548** call sites the lifted code reads `r3` before overwriting it;
  **234** distinct imports have at least one such site. This is a heuristic on
  the lifted C, refined in Ghidra whenever it drives a finding. It counts a few
  void calls as "used" (a later `gpr[3]` read that is not the call result), so
  it is an upper bound on candidates, not on bugs.
* **32** contract mismatches were found in the five audited library groups.
  The **top 15** are ranked below; the rest are in
  [Other findings](#other-findings). None are fixed here.

## Method and caveats

* Call sites and call counts come from the lifted guest
  (`recomp-work/recompiled/ppu_recomp_*.cpp`), then were re-read in Ghidra
  (`ghidra_*` on the MLB16 EBOOT) to establish what the guest does with `r3`
  and with each output pointer.
* Dispatch matters. `ps3_hle_call()` checks the runtime context table before
  the generated `libs/` table (`runtime/ppu/ppu_hle.cpp:700-726`). For this
  title, `cellFs` is mostly `runtime/ppu/ppu_fs.cpp`; `sys_lwmutex_*`,
  `sys_lwcond_*`, `sys_ppu_thread_*` and `sys_time_*` are
  `runtime/ppu/ppu_sysprx.cpp`; `_cellSpursWorkloadAttributeInitialize` is a
  context handler. Findings name the path that actually runs.
* An import with no handler falls through to
  `runtime/ppu/ppu_hle.cpp:185-211`, which returns `CELL_OK` (0) and writes
  nothing. Several findings below are exactly that.
* The clean-room rule was followed: only the game's own code (Ghidra and the
  lifted output), public PS3 SDK contracts, and our sources were used.
* `r3`-used rows in the appendix are not proof of a bug; they identify the
  call sites that had to be inspected. Functions whose result is ignored at
  every call site were skipped unless the function also writes an out-param or
  has a side effect the game reads.

## Ranked findings

### 1. SPURS task-attribute initializer mis-decodes the descriptor form

* **Severity:** high (SPU task cannot run; missing media / possible menu wait).
* **Guest:** `FUN_00EF8494`, call at `0x00EF879C` (`ppu_recomp_007.cpp:179498`).
  It builds a three-word descriptor at `sp+0x78` (`[0]` context EA, `[1]`
  context size, `[2]` `CellSpursTaskLsPattern*`), sets `r7 = sp+0x78`,
  `r8 = 0`, and calls `_cellSpursTaskAttributeInitialize`. This is the Bink
  raw SPU task path (`FUN_0097FB48`, names `"BinkUI"`/`"BinkJumbo"`).
* **Ours:** `libs/spurs/cellSpurs.c:1203`; the default R8 form at
  `:1237-1244` treats `r8` as the task argument and `r7` as the context EA, so
  `eaContext` becomes the stack descriptor address, `sizeContext` becomes 0,
  `lsPattern_ea` becomes 0, and `argument_ea` becomes 0. The descriptor branch
  that would decode `r7` exists but is default-off (`:1264-1275`,
  `SPURS_TASKATTR_DESC`).
* **Evidence from a live log (read only, not a run):**
  `recomp-work/play_20261102_115243.log:404-405` records
  `TaskAttr R8-form: argument_ea=0x00000000` and
  `_TaskAttributeInitialize(eaElf=0x01184E00 ctx=0x0FEFF5F8 szctx=0 lsp=0x00000000 arg=0x00000000)`;
  `:382` records `WAIT_SIGNAL#1 enter ... ran=0ms`.
* **SDK contract:** for this call shape `r7` points at
  `{context EA, context size, lsPattern}`; `r8` is the task argument.
* **Visible effect:** the Bink SPU task is created with a bogus context, zero
  size and no LS pattern, so it parks in `WAIT_SIGNAL`. If the title waits for
  a Bink movie, this is a missing intro/cutscene or a menu wait.
* **Proposed fix (not applied):** auto-detect the descriptor form (`r8 == 0`,
  `r7` a guest stack EA, `d[1]` a plausible size, `d[2]` a plausible EA) and
  take `eaContext/sizeContext/lsPattern_ea` from `d[0..2]`, with `argument_ea
  = r8`. Keep the R8 form for the callers that need it. A per-title default is
  the minimal version.
* **Verify:** offline test feeding both call shapes and checking the resulting
  `CellSpursTaskAttribute`; then log the Bink task and confirm it leaves
  `WAIT_SIGNAL`.

### 2. SPURS core error base is swapped with the fiber base

* **Severity:** high (teardown frees a live workload; possible freeze).
* **Guest:** `FUN_009FADF8` calls `cellSpursShutdownWorkload`,
  `cellSpursWaitForWorkloadShutdown`, then loops `cellSpursRemoveWorkload`
  while the return equals `0x8041080A`
  (`recompiled/ppu_recomp_009.cpp:24118-24129`, calls at `0x009FAE1C`,
  `0x009FAE34`, `0x009FAE44`).
* **Ours:** `include/ps3emu/error_codes.h:119-121` defines
  `CELL_ERROR_BASE_SPURS = 0x80410700` and
  `CELL_ERROR_BASE_FIBER = 0x80410800`; `libs/spurs/cellSpurs.h:27` builds
  `CELL_SPURS_CORE_ERROR_BUSY` as `0x8041070A`; `libs/spurs/cellSpurs.c:1675-1676`
  returns it.
* **SDK contract:** SPURS core errors are `0x804108xx`
  (`BUSY == 0x8041080A`); the fiber base is `0x80410700`. The two are swapped
  in our header, and the repo test `tests/test_error_bases.c:18` encodes the
  same wrong literal.
* **Visible effect:** the guest's equality test never matches, so it exits the
  retry loop on the first BUSY and clears the workload handle while our
  scheduler may still have it active. That is a use-after-free/dangling
  workload path at level or match teardown. It also leaves the workload
  enabled if the free path wins.
* **Proposed fix (not applied):** set the SPURS core base to `0x80410800` and
  the fiber base to `0x80410700`; update the test. Pair it with finding 3,
  otherwise the guest's no-sleep retry loop becomes a hot spin.
* **Verify:** assert the constant, then trace Shutdown -> Wait -> Remove with
  an active policy module and confirm the guest retries rather than freeing.

### 3. `cellSpursShutdownWorkload` / `WaitForWorkloadShutdown` are success stubs

* **Severity:** high (same teardown path as finding 2).
* **Ours:** `libs/spurs/cellSpurs.c:3419-3431` logs and returns `CELL_OK`
  without changing scheduler state or waiting.
* **Guest:** the same `FUN_009FADF8` sequence; the guest expects Shutdown to
  stop new dispatch and Wait to block until the workload has stopped using
  SPUs before it removes it.
* **Visible effect:** the scheduler can dispatch the workload again between
  Shutdown and Remove, and once finding 2 is fixed the unbounded BUSY loop can
  spin. Either way, teardown can stall or corrupt state at a level transition.
* **Proposed fix (not applied):** mark the workload shutting down (clear
  enabled/ready, stop dispatch), and have Wait drain `si->active[wid]` with a
  bound before returning.
* **Verify:** offline trace of Shutdown -> Wait -> Remove with an active PM;
  live level/match transition.

### 4. `cellAudioOutGetState` writes the wrong struct layout

* **Severity:** high (Bink audio configured as 6/8-channel instead of stereo).
* **Ours:** `libs/misc/cellAvconfExt.c:139-158` writes `downMixer` at `+4`,
  `soundMode.type/channel/fs` at `+8/+9/+10`, and `0` at `+12`.
* **SDK contract:** 0x14 bytes: `state@0`, `encoder@1`, `reserved[6]`,
  `downMixer@8`, `soundMode@12` (`type@12`, `channel@13`, `fs@14`,
  `reserved@15`, `u32 layout@16`).
* **Guest:** `FUN_0001D434` calls it at `0x0001D614`
  (`ppu_recomp_000.cpp:15584`) and reads the state/downmixer/sound-mode words.
  `FUN_0097FFF0` calls it at `0x00980080`
  (`ppu_recomp_004.cpp:371152`) and stores byte `state+13`
  (`soundMode.channel`) into `DAT_0115DA30`; that byte then selects the Bink
  channel count (`2` -> 2 channels, otherwise 6/8).
* **Visible effect:** the guest reads `channel = 0` from our `+9` write (real
  offset is `+13`), so the Bink/stream path sets up 6- or 8-channel processing
  and an 8-channel `cellAudio` port instead of stereo. It also reads a bogus
  downmixer and layout. This is a direct audio-mix/glitch candidate.
* **Proposed fix (not applied):** write the real offsets,
  `channel = 2`, `fs = 48 kHz`, `layout = 2ch`, and update
  `CellAudioOutSoundMode`/`CellAudioOutDeviceInfo` to the 8-byte-mode layout.
* **Verify:** unit test the five bytes; or log `DAT_0115DA30` once at
  `0x00980080` in a Bink open.

### 5. `cellAdecOpenEx` is missing and the first handle is 0

* **Severity:** high (streamed/decoded audio never opens).
* **Ours:** `libs/codec/cellAdec.c:139` implements only `cellAdecOpen`
  (`NID 0xD00A6988`, not imported by this title). There is no
  `cellAdecOpenEx`. The first free slot is index 0 and `:167` writes
  `handle = i`, so the first successful handle is 0.
* **Guest:** `FUN_0004E850` calls `cellAdecOpenEx` at `0x0004EA28`
  (`ppu_recomp_000.cpp:72559`); `FUN_00A22B78` calls it at `0x00A22D6C`
  (`ppu_recomp_004.cpp:558141`). The second checks `r3 == 0` and then requires
  `*(param+0x344) != 0` before `cellAdecStartSeq`; the first keeps the handle
  in a local and proceeds.
* **SDK contract:** `cellAdecOpenEx(type, resource, cb, handle)` returns
  `CELL_OK` and writes a non-zero decoder handle.
* **Visible effect:** `0x8B5551A4` has no handler, so the unresolved path
  returns success without writing the handle; even if aliased, the first
  decoder returns 0, which the guest rejects. AAC/ATRAC streamed audio
  (dialogue/music) then never decodes: silence or gaps.
* **Proposed fix (not applied):** add a four-argument
  `cellAdecOpenEx` that forwards to the existing `cellAdecOpen` body, and make
  handles 1-based (write `i+1`, translate on use). Keep 0 invalid.
* **Verify:** dispatch NID `0x8B5551A4` in the codec ABI test, assert a
  non-zero handle, then `DecodeAu`/`GetPcmItem`.

### 6. The POSIX audio mixer free-runs faster than the 187.5 blocks/s period

* **Severity:** high (audio drift/glitches, extra guest CPU, frame pacing).
* **Ours:** `libs/audio/cellAudio.c:652-672` (non-Windows thread) mixes one
  256-sample block, submits it, then sleeps 5 ms if more than four blocks are
  queued and 2 ms otherwise. That is 200-500 blocks/s; the period is
  `CELL_AUDIO_PERIOD_US = 5333` (`libs/audio/cellAudio.h:44`), i.e. 187.5
  blocks/s. Unlike the Windows branch (`:591-592`), it never asks the backend
  for room.
* **Game coupling:** `audio_mix_one_block()` publishes the ring read index at
  `libs/audio/cellAudio.c:526-531`; the guest producer follows that index
  (`recompiled/ppu_recomp_004.cpp:514947` in `FUN_009FD94C`, after
  `cellAudioGetPortConfig`).
* **Visible effect:** the guest is told to produce audio 6-160 % faster than
  real time, the SDL queue grows without bound (increasing latency), and the
  guest audio thread burns extra CPU that competes with the render/input
  threads. Slow pitch/speed drift, gaps, and frame-pacing/input jitter.
* **Proposed fix (not applied):** pace the loop to
  `CELL_AUDIO_PERIOD_US` on a monotonic clock, or use
  `audio_backend_room()`/`audio_backend_wait()` like the Windows branch.
* **Verify:** 1-2 s offline probe with `AUDIO_RATE=1`: blocks/s should be
  ~187.5, not 200+.

### 7. GCM offset tables are published only once, so later mappings are invisible

* **Severity:** medium-high (invalid EA->IO offsets; missing/corrupt geometry).
* **Ours:** `gcm_publish_offset_tables()` (`libs/video/cellGcmSys.c:4129`) is
  called only by `cellGcmGetOffsetTable()` (`:4137`). `populate_offset_table()`
  (`:452-472`) and `clear_offset_table()` (`:475-485`) update only the host
  arrays. `cellGcmMapMainMemory()` (`:4357`), `cellGcmMapEaIoAddress()`
  (`:4396`), the unmaps (`:4441`, `:4457`) and `cellGcm_syscall_iomap()`
  (`:5245`) never refresh the guest copy.
* **Guest:** `FUN_0006DE94` calls `cellGcmGetOffsetTable` once (ret
  `0x0006DF30`) and caches the two guest table pointers in
  `DAT_0124AD34`/`DAT_0124AD38`; 41 sites read them, including
  `Gfx_FrameEnd` and `PS3Crowd_BuildVertexBuffer`. `cellGcmMapMainMemory` is
  called later at `0x0006DE24` from the lazy shared-animation init.
* **SDK contract:** the returned tables are live; a map or unmap updates the
  guest-visible entries.
* **Visible effect:** a page mapped after init still reads `0xFFFF` in the
  guest EA table, converting to offset `0xFFFFFFFF`; an unmapped page still
  reads a valid page. Late animation/geometry buffers can therefore be drawn
  from nothing or from freed/reused memory.
* **Proposed fix (not applied):** write the guest entries in
  `populate_offset_table`/`clear_offset_table`, or re-publish after every
  map/unmap.
* **Verify:** unit test `cellGcmInit` -> `GetOffsetTable` ->
  `MapMainMemory(...,1 MB)` -> guest table entry equals the returned IO page;
  unmap -> `0xFFFF`.

### 8. `cellFsRename`/`cellFsTruncate` use a different VFS root from open/write

* **Severity:** medium (save/cache/custom-sound finalize fails).
* **Ours:** `cellFsRename` (`libs/filesystem/cellFs.c:956`) and
  `cellFsTruncate` (`:684`) are not context-registered, so they use the libs
  translator: `/dev_hdd0/` -> `./gamedata/dev_hdd0/`, `/app_home/` ->
  `./gamedata/app_home/`, root `"."` (`:65`, `:74-80`). Open/read/write use
  `runtime/ppu/ppu_fs.cpp:135-175`, which maps `/dev_hdd0/` to
  `$PS3_HDD0_ROOT/...` or `$PS3_VFS_ROOT/game/...`. The play scripts set
  `PS3_VFS_ROOT=$W/vfs`.
* **Guest:** `cellFsRename` at `0x0002793C`, `0x00028F5C` (`FUN_00027860`,
  `FUN_00028E64`) renames files created under the installed game-data USRDIR
  (`UserYells` and similar); `cellFsTruncate` at `0x00082384`
  (`FUN_00082314`) rewrites a file built by the generic path helper.
* **Visible effect:** the source path `./gamedata/dev_hdd0/...` does not exist
  in this setup, so rename returns `CELL_ENOENT` and the guest aborts
  (`if (iVar4 != 0) return 0;`). Custom sounds/cache files are created but
  never finalized. Truncate can miss or target the wrong file.
* **Proposed fix (not applied):** add context handlers for Rename/Truncate in
  `runtime/ppu/ppu_fs.cpp` using `host_path()`, or make the libs translator
  consult `PS3_VFS_ROOT`/`PS3_HDD0_ROOT` and strip the mount prefix.
* **Verify:** offline open/write/rename through the context path; live
  `PS3_FSLOG=1` should no longer show `rename('./gamedata/...') ENOENT`.

### 9. Default lwcond wait ignores the timeout; lwmutex destroy is a no-op

* **Severity:** medium (1 ms poll per wait; CPU contention; input/frame pacing).
* **Ours:** `runtime/ppu/ppu_sysprx.cpp:497-532` (default path) releases the
  paired lwmutex, sleeps 1 ms, reacquires, and returns `CELL_OK` regardless of
  the guest timeout or whether a signal happened. `signal`/`signal_all` are
  no-ops unless `PS3_LWCOND_REAL` is set (`:415-419`, default off). `:634-641`
  is a counted no-op `sys_lwmutex_destroy`.
* **Guest:** `FUN_00A1EE70` calls `sys_lwcond_wait(lwcond, 250000)` at
  `0x00A1EFA4` and checks the return for `0x8001000B` (ETIMEDOUT);
  `Cond_Wait` at `0x009A30A4` treats `r3 == 0` as signalled. `FUN_009FE358`
  at `0x009FE3AC` frees storage only if destroy returned 0.
* **Visible effect:** every condition wait becomes a 1 ms poll with lwmutex
  churn; timeouts never fire; teardown loops never see `EBUSY`; the
  address-keyed lwmutex table is never reclaimed. On a loaded machine this is
  extra CPU and wake latency on the render/input path. It is not a hard
  correctness failure for a predicate loop that tolerates spurious wakeups.
* **Proposed fix (not applied):** enable the real lwcond implementation by
  default (or implement a blocking wait with the guest timeout and real
  signal wake), and implement `sys_lwmutex_destroy` (`EBUSY` when held/waiting,
  release the slot, clear the guest structure).
* **Verify:** existing lwcond tests (signal wakes; timeout returns ETIMEDOUT);
  measure wake latency/CPU on the pitch meter.

### 10. `cellSearchUtility` is entirely unimplemented

* **Severity:** medium (custom soundtrack/music table never populates).
* **Ours:** no `cellSearch*` definition anywhere under `libs/`, `runtime/`,
  or `include/`; none of the ten imported NIDs is registered. Every call hits
  `runtime/ppu/ppu_hle.cpp:185-211`, which returns `CELL_OK` and writes
  nothing.
* **Guest:** `FUN_000CB0E8` calls `cellSearchInitialize(..., callback, ...)` at
  `0x000CB160`; `cellSearchStartContentSearch` at `0x000CB3D4` and
  `cellSearchStartListSearch` at `0x000CB43C`. `FUN_000CA590` calls
  `cellSearchGetContentInfoByOffset` at `0x000CA6E8` and
  `cellSearchGetContentInfoPath` at `0x000CA71C`, then copies name, type,
  duration and path into the content/music table. The registered callback
  (`FUN_000CB6F0`) is never invoked by us.
* **Visible effect:** the enumeration state machine (`DAT_0112531C`) never
  advances from the callback; the Music Source Table/custom-soundtrack list
  stays empty and any code waiting on that state can stall.
* **Proposed fix (not applied):** minimal synchronous search: keep a handle,
  enumerate the requested path (or return an empty set), write the content
  info/path out-params, and invoke the guest callback with a terminal code.
  Alternatively return a real not-supported error instead of fake success.
* **Verify:** unit test with a fake guest callback and out-params; live log of
  the callback and state transitions.

### 11. `cellFsWrite` returns `CELL_OK` on a failed/short write

* **Severity:** medium (cache/install worker can spin forever).
* **Ours:** `runtime/ppu/ppu_fs.cpp:551-585`: `fwrite` result `n` is stored at
  `:583` and `ctx->gpr[3] = CELL_OK` at `:584`, with no `ferror`/short-write
  check.
* **Guest:** `FUN_0002F0C4` (`ppu_recomp_000.cpp:36275/36293/36339/36357`)
  loops while `nwrite` advances; a zero `nwrite` with a success return never
  changes the cursor.
* **Visible effect:** on a read-only/full cache volume the worker has no
  progress signal and loops forever at start menu or match load. Otherwise a
  short write silently loses data.
* **Proposed fix (not applied):** return `CELL_FS_EIO` when `n != nbytes` or
  `ferror(f)`.
* **Verify:** write to a read-only fd / full filesystem in the fs test.

### 12. `cellSaveDataDelete2` is a no-op that reports success

* **Severity:** medium (delete appears to work; stale save returns).
* **Ours:** `libs/system/cellSaveData.c:1327-1334` logs and returns
  `CELL_OK` without deleting anything.
* **Guest:** `FUN_00018478` calls it at `0x000198FC` and clears the job as if
  it completed.
* **Visible effect:** the “delete save/game data” action does not remove the
  host files; the next boot loads the supposedly deleted save (stale or
  corrupt save state).
* **Proposed fix (not applied):** delete the save directory, or return
  `CELL_SAVEDATA_ERROR_NOTSUPPORTED` so the title does not claim success.
* **Verify:** create/delete/stat test; live delete then relaunch.

### 13. `cellGameDataCheck` reports `sizeKB = NOTCALC`; `cellGameGetSizeKB` always writes 0

* **Severity:** medium (install/free-space math is fabricated).
* **Ours:** `libs/system/cellGame.c:448-453` writes
  `CELL_GAME_SIZEKB_NOTCALC` whenever a size out-param is supplied, including
  when the game-data directory exists; `:767-778` writes 0.
* **Guest:** `FUN_00018478` at `0x000189E4` reads `sizeKB`, calls
  `cellGameGetSizeKB` at `0x00018A2C` when it is `0xFFFFFFFF`, then computes
  `(0x500000 - sizeKB) - freeKB`.
* **Visible effect:** install/free-space decisions and size-dependent UI run
  with a fabricated zero. On a small or nearly full host volume this can skip
  or falsely trigger a space check.
* **Proposed fix (not applied):** compute the host directory size and write it
  (or at least write 0 rather than NOTCALC for existing data), and implement
  `cellGameGetSizeKB`.
* **Verify:** unit test a known-size game-data dir against `du`.

### 14. `cellSaveDataAutoLoad2` converts `NODATA` to `CELL_OK`

* **Severity:** medium (deliberate compatibility; first-run state).
* **Ours:** `libs/system/cellSaveData.c:1323-1324` maps
  `CELL_SAVEDATA_ERROR_NODATA` to `CELL_OK`; the comment at `:1313-1322`
  documents the workaround.
* **Guest:** `FUN_00018478` at `0x00019BDC` and `0x0001A0B8` stores/logs the
  return; the stat callback already reported no data.
* **Visible effect:** a first boot or post-delete autoload looks successful
  to the title. Save creation or profile state can take the wrong branch if it
  trusts the return instead of the callback.
* **Proposed fix (not applied):** propagate `NODATA`; if one title needs the
  quirk, gate it behind an env var rather than making it universal.
* **Verify:** empty-save-root test; first-boot log line.

### 15. `cellGamePatchCheck` is unimplemented and always succeeds

* **Severity:** medium-low (patch/boot path selection).
* **Ours:** no `cellGamePatchCheck` definition; the NID falls through to the
  unresolved `CELL_OK` path.
* **Guest:** `FUN_00018478` calls it at `0x0001927C` and compares the return
  with `0x8002CB27` (`CELL_GAME_ERROR_NOTPATCH`) and `0x8002CB50`
  (`CELL_GAME_ERROR_BOOTPATH`); normal disc boot expects `NOTPATCH`.
* **Visible effect:** the guest takes the “patched/OK” branch on a normal
  boot. The current path is mostly benign for this title, but patch/game-data
  path selection is wrong.
* **Proposed fix (not applied):** return `CELL_GAME_ERROR_NOTPATCH` when there
  is no patch data, and fill the size out-param.
* **Verify:** return-code test per boot type; live log of the branch taken.

## Other findings

These are real contract mismatches but are either not consumed by this title,
reachable only online/with optional hardware, or lower impact than the top 15.

| # | lib:function | our file:line | mismatch | MLB16 impact |
|---|---|---|---|---|
| 16 | `cellRtc:cellRtcParseRfc3339` | no implementation (`libs/system/cellRtc.c` lacks it) | unresolved returns `CELL_OK` without writing the `CellRtcTick`; guest `FUN_00DA1B10` (`0x00DA1BC4`, `0x00DA22E8`) then converts/sets an uninitialised tick | wrong date/time for date-derived data (HTTP/save metadata/display) |
| 17 | `cellSysutilAvconfExt:cellVideoOutGetScreenSize` | no implementation | unresolved returns `CELL_OK`, screen-size float unwritten; guest `0x000650E0` reads it | only the 3D res `0x81` branch; dormant because `cellVideoOutGetResolutionAvailability` reports 3D unavailable (`libs/video/cellVideoOut.c:248-260`) |
| 18 | `cellSysutil:cellAudioOutGetSoundAvailability` | `libs/misc/cellAvconfExt.h:26-27`, `.c:48-57` | `AC3=4`, `DTS=8`; the game's own decoder table uses `AC3=1`, `DTS=6`; guest probes types 0/1/6 at `0x1D670/0x1D68C/0x1D6A8` | capability view says Dolby/DTS unavailable; only the debug/info overlay reads the results |
| 19 | `sys_fs:cellFsUnlink` / `cellFsRmdir` | `runtime/ppu/ppu_fs.cpp:859-860` | both return `CELL_OK` without removing anything; the libs `cellFsUnlink` implementation is shadowed | temp/cache files accumulate; stale files remain |
| 20 | `cellSpurs:cellSpursGetInfo` | `libs/spurs/cellSpurs.c:3360-3369` | writes only `nSpus` and zeros `+4..+0x27`; the 280-byte `CellSpursInfo` (`spuThreads`, handler ids, ...) is left stale | only the SPU-exception handler reads more, and finding 22 means it is never invoked |
| 21 | `cellSpurs:cellSpursGetWorkloadInfo` | `libs/spurs/cellSpurs.c:3414` | `hasSignal` is derived from `vm_read32(signal1) >> 16` instead of the `wklSignal1` u16 bit; names are not filled | field unused by this title |
| 22 | `cellSpurs:cellSpursSetExceptionEventHandler` | `libs/spurs/cellSpurs.c:3372-3379` | handler/argument discarded, returns `CELL_OK` | an SPU fault is never reported to the game's handler; fault path only |
| 23 | `cellVideoOut:cellVideoOutGetDeviceInfo` | `libs/video/cellVideoOut.h:120-127` | header 12 bytes and modes at `+12`; SDK has color info + modes at `+32` | this title reads only `portType@0` and `state@5`, so unconsumed |
| 24 | `cellSysutil:cellAudioOutGetDeviceInfo` | `libs/misc/cellAvconfExt.h:47-60`, `.c:73-107` | modes at `+6` with 4-byte stride and 70-byte total; SDK has an 8-byte header and 8-byte modes (136 bytes) | the only consumer `FUN_0001CAAC` calls a no-op (`FUN_0001CA80` is `blr`) |
| 25 | `sys_io:cellKbRead` | `libs/input/cellKb.c:158` implements `cellKbGetData` only | unresolved returns `CELL_OK` without filling `CellKbData`; guest `0x00533D7C` reads `len`/keycodes | keyboard subsystem never receives keys |
| 26 | `cellSsl:cellSslCertificateLoader` | `libs/network/cellSsl.c:57-64` | always writes `*required = 0` and returns success; guest `FUN_00DA0970` (`0x00DA0A40`, `0x00DA0AAC`) allocates 0 and initialises HTTPS with an empty CA list | online TLS only; boot proceeds because the assert is a no-op |
| 27 | `cellHttp` request/response/cookie names | `libs/network/cellHttp.c:795`, `:959` use different names | this title imports `cellHttpRequestAddHeader`, `cellHttpRequestGetHeader`, `cellHttpResponseGetHeader`, `cellHttpInitCookie`, `cellHttpSessionCookieFlush`; all are unresolved | online HTTP loses headers/cookies |
| 28 | `sys_net` offline socket model | `runtime/ppu/ppu_sysprx.cpp:772-779` | returns `-1` instead of `0x800102xx`; `connect/listen/accept/send/getsockopt` unregistered | netplay only; `connect`/`accept` look successful |
| 29 | `sceNp*` / `sceNp2` | `libs/network/` | `sceNpLookupNpIdAsync`, `sceNpLookupUserProfileWithAvatarSizeAsync`, the OAuth surface and several `sceNpBasic` calls are missing and fake success | online login/social/profile only |
| 30 | `sceNpTrophy:sceNpTrophyUnlockTrophy` | `libs/network/sceNpTrophy.h:30` | `ALREADY_UNLOCKED` is `0x8055160A`; the game compares `0x80022915` | duplicate unlocks reported as an unknown failure |
| 31 | `sys_fs:cellFsGetFreeSize` | `runtime/ppu/ppu_fs.cpp:891-897` | fixed 4 KiB blocks / 32 GiB free regardless of path | can allow install/cache on a nearly full host |
| 32 | `sys_fs:cellFsMkdir` / `cellFsClose` | `runtime/ppu/ppu_fs.cpp:850-858`, `:437-...` | Mkdir returns OK even when host mkdir fails; Close returns OK for an invalid fd | mostly masked by callers; low |

## Coverage: checked and found consistent for this title

`cellGcmGetControlRegister`, `cellGcmGetConfiguration`,
`cellGcmGetLabelAddress`, `cellGcmMapMainMemory` (return/out-param),
`cellGcmUnmapIoAddress`, `cellGcmSetDisplayBuffer`,
`cellGcmSetTileInfo`/`BindTile`/`UnbindTile`/`BindZcull`/`UnbindZcull`,
`cellGcmSetFlipImmediate`, `cellGcmResetFlipStatus`,
`cellGcmSetFlipHandler`/`VBlankHandler`/`UserHandler`,
`cellVideoOutGetState`/`GetResolution`/`GetResolutionAvailability`/
`Configure`, `cellVideoOutGetDeviceInfo` (the fields this title reads),
`cellAudioOutGetState` fields other than sound-mode (see finding 4),
`cellAudioOutGetNumberOfDevice`, `cellAudioInit`/`PortOpen`/
`GetPortConfig`/`PortStart`, `cellAdecQueryAttr`/`StartSeq`/`DecodeAu`/
`GetPcm`/`GetPcmItem`/`Close` (blocked by findings 5), `cellPadInit`/
`GetData`/`GetInfo2`/`SetPortSetting`/`SetActDirect`/`ClearBuf`,
`cellMicInit`/`End`/`IsAttached`/`IsOpen` (no mic path),
`cellVoiceInit`/`CreatePort`/`Connect`/`Start`/`Read`/`Write`,
`cellSpursAddWorkloadWithAttribute`/`AttachLv2EventQueue`/
`DetachLv2EventQueue`/`CreateTasksetWithAttribute`/
`CreateTaskWithAttribute` (forwarding)/`EventFlagSet`/
`ReadyCountStore`/`RequestIdleSpu`/`GetWorkloadInfo` consumed fields,
`sys_ppu_thread_create`/`get_id`, `sys_lwmutex_create`/`lock`/`unlock`/
`trylock`, `sys_time_get_system_time`, `sys_get_random_number`,
`cellFsOpen`/`Read`/`Lseek`/`Stat`/`Fstat`/`Opendir`/`Readdir`/
`Closedir`/`AioRead`/`AioInit`/`AioFinish`,
`cellGameBootCheck`/`ContentPermit`/`CreateGameData`/`DataCheck` return
codes, `cellSaveData` list/stat/file marshalling, `cellMsgDialog*`,
`cellRtcGetCurrentClockLocalTime`/`ConvertUtcToLocalTime`/`SetTick`,
`cellSysmoduleLoadModule`/`UnloadModule`, `cellSysutilGetSystemParam*`,
`cellSysutilCheckCallback`/`RegisterCallback`, `cellSysCacheMount`.

## Appendix: every imported function the game calls

Format: function, direct call sites, sites where the lifted code reads `r3`
before overwriting it, sites where it does not. This is the candidate list;
only the functions in the ranked/other sections above were judged against the
SDK. "Ignored" does not mean safe when the function writes an out-param.


### cellAdec (7)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellAdecClose` | 3 | 0 | 3 |
| `cellAdecDecodeAu` | 4 | 4 | 0 |
| `cellAdecGetPcm` | 2 | 1 | 1 |
| `cellAdecGetPcmItem` | 2 | 2 | 0 |
| `cellAdecOpenEx` | 2 | 2 | 0 |
| `cellAdecQueryAttr` | 2 | 0 | 2 |
| `cellAdecStartSeq` | 2 | 2 | 0 |

### cellAudio (7)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellAudioGetPortConfig` | 2 | 1 | 1 |
| `cellAudioInit` | 1 | 1 | 0 |
| `cellAudioPortClose` | 2 | 0 | 2 |
| `cellAudioPortOpen` | 2 | 2 | 0 |
| `cellAudioPortStart` | 2 | 1 | 1 |
| `cellAudioPortStop` | 1 | 0 | 1 |
| `cellAudioQuit` | 1 | 0 | 1 |

### cellCamera (10)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellCameraGetBufferInfoEx` | 1 | 0 | 1 |
| `cellCameraGetType` | 3 | 3 | 0 |
| `cellCameraInit` | 2 | 1 | 1 |
| `cellCameraOpenEx` | 2 | 0 | 2 |
| `cellCameraReadEx` | 1 | 1 | 0 |
| `cellCameraRemoveNotifyEventQueue2` | 1 | 0 | 1 |
| `cellCameraReset` | 1 | 0 | 1 |
| `cellCameraSetAttribute` | 2 | 0 | 2 |
| `cellCameraSetNotifyEventQueue2` | 1 | 0 | 1 |
| `cellCameraStart` | 1 | 0 | 1 |

### cellGame (6)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellGameBootCheck` | 1 | 0 | 1 |
| `cellGameContentPermit` | 5 | 4 | 1 |
| `cellGameCreateGameData` | 1 | 1 | 0 |
| `cellGameDataCheck` | 2 | 2 | 0 |
| `cellGameGetSizeKB` | 1 | 0 | 1 |
| `cellGamePatchCheck` | 1 | 1 | 0 |

### cellGcmSys (22)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `_cellGcmInitBody` | 1 | 0 | 1 |
| `cellGcmBindTile` | 1 | 0 | 1 |
| `cellGcmBindZcull` | 1 | 0 | 1 |
| `cellGcmGetConfiguration` | 1 | 0 | 1 |
| `cellGcmGetControlRegister` | 2 | 2 | 0 |
| `cellGcmGetDefaultCommandWordSize` | 1 | 1 | 0 |
| `cellGcmGetDefaultSegmentWordSize` | 1 | 1 | 0 |
| `cellGcmGetLabelAddress` | 2 | 2 | 0 |
| `cellGcmGetOffsetTable` | 1 | 0 | 1 |
| `cellGcmMapMainMemory` | 1 | 1 | 0 |
| `cellGcmResetFlipStatus` | 1 | 0 | 1 |
| `cellGcmSetDefaultFifoSize` | 1 | 0 | 1 |
| `cellGcmSetDisplayBuffer` | 1 | 0 | 1 |
| `cellGcmSetFlipHandler` | 1 | 0 | 1 |
| `cellGcmSetFlipImmediate` | 1 | 1 | 0 |
| `cellGcmSetPrepareFlip` | 2 | 2 | 0 |
| `cellGcmSetTileInfo` | 1 | 0 | 1 |
| `cellGcmSetUserHandler` | 1 | 0 | 1 |
| `cellGcmSetVBlankHandler` | 1 | 0 | 1 |
| `cellGcmUnbindTile` | 1 | 0 | 1 |
| `cellGcmUnbindZcull` | 1 | 0 | 1 |
| `cellGcmUnmapIoAddress` | 1 | 1 | 0 |

### cellHttp (38)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellHttpClientCloseAllConnections` | 1 | 0 | 1 |
| `cellHttpClientGetAutoRedirect` | 1 | 0 | 1 |
| `cellHttpClientGetCookieStatus` | 1 | 0 | 1 |
| `cellHttpClientGetKeepAlive` | 1 | 0 | 1 |
| `cellHttpClientGetUserAgent` | 2 | 0 | 2 |
| `cellHttpClientGetVersion` | 1 | 0 | 1 |
| `cellHttpClientSetAuthenticationCallback` | 1 | 0 | 1 |
| `cellHttpClientSetAutoRedirect` | 1 | 0 | 1 |
| `cellHttpClientSetConnTimeout` | 1 | 0 | 1 |
| `cellHttpClientSetCookieStatus` | 2 | 0 | 2 |
| `cellHttpClientSetKeepAlive` | 1 | 0 | 1 |
| `cellHttpClientSetRecvBufferSize` | 1 | 0 | 1 |
| `cellHttpClientSetRecvTimeout` | 1 | 0 | 1 |
| `cellHttpClientSetRedirectCallback` | 1 | 0 | 1 |
| `cellHttpClientSetSendTimeout` | 1 | 0 | 1 |
| `cellHttpClientSetSslCallback` | 1 | 0 | 1 |
| `cellHttpClientSetTotalPoolSize` | 1 | 0 | 1 |
| `cellHttpClientSetTransactionStateCallback` | 1 | 0 | 1 |
| `cellHttpClientSetUserAgent` | 1 | 0 | 1 |
| `cellHttpClientSetVersion` | 1 | 0 | 1 |
| `cellHttpCreateClient` | 1 | 0 | 1 |
| `cellHttpCreateTransaction` | 1 | 1 | 0 |
| `cellHttpDestroyClient` | 1 | 1 | 0 |
| `cellHttpDestroyTransaction` | 2 | 0 | 2 |
| `cellHttpEnd` | 2 | 0 | 2 |
| `cellHttpEndCookie` | 2 | 0 | 2 |
| `cellHttpInit` | 1 | 1 | 0 |
| `cellHttpInitCookie` | 1 | 1 | 0 |
| `cellHttpRecvResponse` | 2 | 2 | 0 |
| `cellHttpRequestAddHeader` | 1 | 1 | 0 |
| `cellHttpRequestGetHeader` | 2 | 2 | 0 |
| `cellHttpRequestSetContentLength` | 3 | 0 | 3 |
| `cellHttpResponseGetContentLength` | 2 | 0 | 2 |
| `cellHttpResponseGetHeader` | 2 | 2 | 0 |
| `cellHttpResponseGetStatusCode` | 2 | 0 | 2 |
| `cellHttpSendRequest` | 2 | 2 | 0 |
| `cellHttpSessionCookieFlush` | 1 | 1 | 0 |
| `cellHttpsInit` | 2 | 2 | 0 |

### cellHttpUtil (2)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellHttpUtilBase64Encoder` | 1 | 0 | 1 |
| `cellHttpUtilParseUri` | 2 | 2 | 0 |

### cellJpgDec (7)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellJpgDecClose` | 10 | 0 | 10 |
| `cellJpgDecCreate` | 2 | 2 | 0 |
| `cellJpgDecDecodeData` | 2 | 2 | 0 |
| `cellJpgDecDestroy` | 12 | 0 | 12 |
| `cellJpgDecOpen` | 2 | 2 | 0 |
| `cellJpgDecReadHeader` | 2 | 2 | 0 |
| `cellJpgDecSetParameter` | 2 | 2 | 0 |

### cellMic (14)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellMicClose` | 4 | 1 | 3 |
| `cellMicEnd` | 5 | 0 | 5 |
| `cellMicGetDeviceAttr` | 4 | 2 | 2 |
| `cellMicGetFormatDsp` | 2 | 0 | 2 |
| `cellMicInit` | 2 | 2 | 0 |
| `cellMicIsAttached` | 8 | 8 | 0 |
| `cellMicIsOpen` | 8 | 8 | 0 |
| `cellMicOpenEx` | 1 | 1 | 0 |
| `cellMicReadDsp` | 8 | 8 | 0 |
| `cellMicRemoveNotifyEventQueue` | 1 | 0 | 1 |
| `cellMicSetDeviceAttr` | 6 | 0 | 6 |
| `cellMicSetNotifyEventQueue` | 2 | 2 | 0 |
| `cellMicStart` | 1 | 1 | 0 |
| `cellMicStop` | 2 | 0 | 2 |

### cellNetCtl (9)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellNetCtlAddHandler` | 1 | 1 | 0 |
| `cellNetCtlDelHandler` | 4 | 0 | 4 |
| `cellNetCtlGetInfo` | 3 | 2 | 1 |
| `cellNetCtlGetNatInfo` | 1 | 1 | 0 |
| `cellNetCtlGetState` | 1 | 0 | 1 |
| `cellNetCtlInit` | 2 | 0 | 2 |
| `cellNetCtlNetStartDialogLoadAsync` | 2 | 2 | 0 |
| `cellNetCtlNetStartDialogUnloadAsync` | 2 | 1 | 1 |
| `cellNetCtlTerm` | 4 | 0 | 4 |

### cellPngDec (7)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellPngDecClose` | 12 | 0 | 12 |
| `cellPngDecCreate` | 3 | 3 | 0 |
| `cellPngDecDecodeData` | 3 | 3 | 0 |
| `cellPngDecDestroy` | 15 | 0 | 15 |
| `cellPngDecOpen` | 3 | 3 | 0 |
| `cellPngDecReadHeader` | 3 | 3 | 0 |
| `cellPngDecSetParameter` | 3 | 3 | 0 |

### cellRec (5)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellRecClose` | 2 | 0 | 2 |
| `cellRecOpen` | 2 | 1 | 1 |
| `cellRecQueryMemSize` | 1 | 1 | 0 |
| `cellRecStart` | 2 | 0 | 2 |
| `cellRecStop` | 4 | 0 | 4 |

### cellRtc (6)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellRtcConvertUtcToLocalTime` | 2 | 2 | 0 |
| `cellRtcGetCurrentClockLocalTime` | 3 | 3 | 0 |
| `cellRtcGetCurrentTick` | 3 | 0 | 3 |
| `cellRtcGetTime_t` | 1 | 0 | 1 |
| `cellRtcParseRfc3339` | 2 | 2 | 0 |
| `cellRtcSetTick` | 3 | 2 | 1 |

### cellScreenShotUtility (2)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellScreenShotEnable` | 1 | 1 | 0 |
| `cellScreenShotSetParameter` | 1 | 0 | 1 |

### cellSearchUtility (10)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellSearchCancel` | 1 | 1 | 0 |
| `cellSearchEnd` | 4 | 2 | 2 |
| `cellSearchFinalize` | 2 | 1 | 1 |
| `cellSearchGetContentInfoByOffset` | 3 | 3 | 0 |
| `cellSearchGetContentInfoPath` | 1 | 1 | 0 |
| `cellSearchInitialize` | 1 | 1 | 0 |
| `cellSearchPrepareFile` | 8 | 0 | 8 |
| `cellSearchStartContentSearch` | 1 | 1 | 0 |
| `cellSearchStartContentSearchInList` | 1 | 1 | 0 |
| `cellSearchStartListSearch` | 1 | 1 | 0 |

### cellSpurs (26)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `_cellSpursAttributeInitialize` | 1 | 0 | 1 |
| `_cellSpursEventFlagInitialize` | 1 | 1 | 0 |
| `_cellSpursTaskAttributeInitialize` | 1 | 1 | 0 |
| `_cellSpursTasksetAttributeInitialize` | 1 | 0 | 1 |
| `_cellSpursWorkloadAttributeInitialize` | 2 | 0 | 2 |
| `cellSpursAddWorkloadWithAttribute` | 2 | 0 | 2 |
| `cellSpursAttachLv2EventQueue` | 2 | 0 | 2 |
| `cellSpursAttributeEnableSpuPrintfIfAvailable` | 1 | 0 | 1 |
| `cellSpursAttributeEnableSystemWorkload` | 1 | 0 | 1 |
| `cellSpursCreateTaskWithAttribute` | 1 | 1 | 0 |
| `cellSpursCreateTasksetWithAttribute` | 1 | 1 | 0 |
| `cellSpursDetachLv2EventQueue` | 2 | 0 | 2 |
| `cellSpursEventFlagSet` | 1 | 0 | 1 |
| `cellSpursFinalize` | 2 | 0 | 2 |
| `cellSpursGetInfo` | 3 | 2 | 1 |
| `cellSpursGetWorkloadInfo` | 8 | 0 | 8 |
| `cellSpursInitializeWithAttribute` | 1 | 0 | 1 |
| `cellSpursReadyCountStore` | 1 | 0 | 1 |
| `cellSpursRemoveWorkload` | 2 | 2 | 0 |
| `cellSpursRequestIdleSpu` | 2 | 0 | 2 |
| `cellSpursSetExceptionEventHandler` | 2 | 0 | 2 |
| `cellSpursShutdownTaskset` | 1 | 0 | 1 |
| `cellSpursShutdownWorkload` | 2 | 0 | 2 |
| `cellSpursTasksetAttributeSetName` | 1 | 0 | 1 |
| `cellSpursWaitForWorkloadShutdown` | 2 | 0 | 2 |
| `cellSpursWorkloadAttributeSetName` | 2 | 0 | 2 |

### cellSsl (3)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellSslCertificateLoader` | 4 | 4 | 0 |
| `cellSslEnd` | 1 | 0 | 1 |
| `cellSslInit` | 2 | 2 | 0 |

### cellSysmodule (2)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellSysmoduleLoadModule` | 35 | 18 | 17 |
| `cellSysmoduleUnloadModule` | 56 | 3 | 53 |

### cellSysutil (32)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellAudioOutGetDeviceInfo` | 1 | 1 | 0 |
| `cellAudioOutGetNumberOfDevice` | 1 | 1 | 0 |
| `cellAudioOutGetSoundAvailability` | 3 | 3 | 0 |
| `cellAudioOutGetState` | 3 | 3 | 0 |
| `cellMsgDialogAbort` | 4 | 0 | 4 |
| `cellMsgDialogOpen2` | 15 | 1 | 14 |
| `cellMsgDialogProgressBarInc` | 2 | 0 | 2 |
| `cellMsgDialogProgressBarReset` | 8 | 0 | 8 |
| `cellMsgDialogProgressBarSetMsg` | 11 | 0 | 11 |
| `cellOskDialogAbort` | 1 | 0 | 1 |
| `cellOskDialogGetInputText` | 1 | 1 | 0 |
| `cellOskDialogLoadAsync` | 1 | 1 | 0 |
| `cellOskDialogSetInitialKeyLayout` | 1 | 0 | 1 |
| `cellOskDialogSetKeyLayoutOption` | 1 | 0 | 1 |
| `cellOskDialogSetLayoutMode` | 1 | 0 | 1 |
| `cellOskDialogUnloadAsync` | 1 | 1 | 0 |
| `cellSaveDataAutoLoad2` | 2 | 2 | 0 |
| `cellSaveDataAutoSave2` | 1 | 1 | 0 |
| `cellSaveDataDelete2` | 1 | 1 | 0 |
| `cellSaveDataListLoad2` | 2 | 2 | 0 |
| `cellSaveDataListSave2` | 1 | 1 | 0 |
| `cellSysCacheMount` | 1 | 1 | 0 |
| `cellSysutilCheckCallback` | 51 | 1 | 50 |
| `cellSysutilGetSystemParamInt` | 3 | 1 | 2 |
| `cellSysutilGetSystemParamString` | 10 | 10 | 0 |
| `cellSysutilRegisterCallback` | 3 | 1 | 2 |
| `cellSysutilUnregisterCallback` | 1 | 0 | 1 |
| `cellVideoOutConfigure` | 2 | 0 | 2 |
| `cellVideoOutGetDeviceInfo` | 8 | 0 | 8 |
| `cellVideoOutGetResolution` | 2 | 0 | 2 |
| `cellVideoOutGetResolutionAvailability` | 2 | 2 | 0 |
| `cellVideoOutGetState` | 4 | 4 | 0 |

### cellSysutilAvconfExt (1)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellVideoOutGetScreenSize` | 2 | 2 | 0 |

### cellVideoExportUtility (3)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellVideoExportFinalize` | 1 | 1 | 0 |
| `cellVideoExportFromFile` | 1 | 1 | 0 |
| `cellVideoExportInitialize2` | 1 | 1 | 0 |

### cellVoice (17)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellVoiceConnectIPortToOPort` | 2 | 2 | 0 |
| `cellVoiceCreateNotifyEventQueue` | 1 | 1 | 0 |
| `cellVoiceCreatePort` | 4 | 4 | 0 |
| `cellVoiceDeletePort` | 4 | 0 | 4 |
| `cellVoiceDisconnectIPortFromOPort` | 2 | 0 | 2 |
| `cellVoiceEnd` | 1 | 0 | 1 |
| `cellVoiceInit` | 1 | 1 | 0 |
| `cellVoicePausePort` | 3 | 0 | 3 |
| `cellVoiceReadFromOPort` | 1 | 1 | 0 |
| `cellVoiceRemoveNotifyEventQueue` | 1 | 0 | 1 |
| `cellVoiceResetPort` | 4 | 0 | 4 |
| `cellVoiceResumePort` | 3 | 0 | 3 |
| `cellVoiceSetNotifyEventQueue` | 1 | 1 | 0 |
| `cellVoiceSetPortAttr` | 2 | 2 | 0 |
| `cellVoiceStart` | 1 | 1 | 0 |
| `cellVoiceStop` | 1 | 0 | 1 |
| `cellVoiceWriteToIPort` | 1 | 0 | 1 |

### libgem (17)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellGemCalibrate` | 1 | 0 | 1 |
| `cellGemClearStatusFlags` | 2 | 0 | 2 |
| `cellGemForceRGB` | 7 | 0 | 7 |
| `cellGemGetInertialState` | 1 | 0 | 1 |
| `cellGemGetInfo` | 2 | 1 | 1 |
| `cellGemGetMemorySize` | 1 | 0 | 1 |
| `cellGemGetRGB` | 2 | 0 | 2 |
| `cellGemGetState` | 3 | 3 | 0 |
| `cellGemGetStatusFlags` | 1 | 0 | 1 |
| `cellGemGetTrackerHue` | 2 | 0 | 2 |
| `cellGemInit` | 1 | 1 | 0 |
| `cellGemInvalidateCalibration` | 2 | 0 | 2 |
| `cellGemPrepareCamera` | 2 | 0 | 2 |
| `cellGemSetRumble` | 2 | 2 | 0 |
| `cellGemTrackHues` | 2 | 0 | 2 |
| `cellGemUpdateFinish` | 1 | 0 | 1 |
| `cellGemUpdateStart` | 1 | 0 | 1 |

### sceNp (45)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `sceNpBasicAddBlockListEntry` | 1 | 1 | 0 |
| `sceNpBasicAddFriend` | 1 | 1 | 0 |
| `sceNpBasicAddPlayersHistory` | 1 | 1 | 0 |
| `sceNpBasicGetBlockListEntry` | 1 | 1 | 0 |
| `sceNpBasicGetBlockListEntryCount` | 1 | 1 | 0 |
| `sceNpBasicGetEvent` | 2 | 2 | 0 |
| `sceNpBasicGetFriendListEntryCount` | 1 | 1 | 0 |
| `sceNpBasicGetFriendPresenceByIndex2` | 1 | 1 | 0 |
| `sceNpBasicGetFriendPresenceByNpId2` | 2 | 2 | 0 |
| `sceNpBasicGetPlayersHistoryEntry` | 1 | 1 | 0 |
| `sceNpBasicGetPlayersHistoryEntryCount` | 1 | 1 | 0 |
| `sceNpBasicRecvMessageAttachmentLoad` | 1 | 0 | 1 |
| `sceNpBasicRegisterContextSensitiveHandler` | 1 | 1 | 0 |
| `sceNpBasicSendMessage` | 1 | 1 | 0 |
| `sceNpBasicSendMessageGui` | 2 | 2 | 0 |
| `sceNpBasicSetPresenceDetails2` | 1 | 0 | 1 |
| `sceNpBasicUnregisterHandler` | 1 | 0 | 1 |
| `sceNpDrmIsAvailable2` | 1 | 1 | 0 |
| `sceNpInit` | 1 | 1 | 0 |
| `sceNpLookupCreateTitleCtx` | 1 | 1 | 0 |
| `sceNpLookupCreateTransactionCtx` | 8 | 8 | 0 |
| `sceNpLookupDestroyTransactionCtx` | 16 | 1 | 15 |
| `sceNpLookupInit` | 1 | 1 | 0 |
| `sceNpLookupNpIdAsync` | 7 | 7 | 0 |
| `sceNpLookupPollAsync` | 8 | 8 | 0 |
| `sceNpLookupTerm` | 1 | 0 | 1 |
| `sceNpLookupUserProfileWithAvatarSizeAsync` | 1 | 1 | 0 |
| `sceNpManagerGetChatRestrictionFlag` | 1 | 1 | 0 |
| `sceNpManagerGetContentRatingFlag` | 1 | 1 | 0 |
| `sceNpManagerGetEntitlementById` | 1 | 1 | 0 |
| `sceNpManagerGetEntitlementIdList` | 4 | 4 | 0 |
| `sceNpManagerGetNetworkTime` | 1 | 0 | 1 |
| `sceNpManagerGetNpId` | 6 | 6 | 0 |
| `sceNpManagerGetStatus` | 1 | 0 | 1 |
| `sceNpManagerGetTicket` | 2 | 2 | 0 |
| `sceNpManagerRegisterCallback` | 1 | 0 | 1 |
| `sceNpManagerRequestTicket2` | 2 | 2 | 0 |
| `sceNpSignalingActivateConnection` | 1 | 1 | 0 |
| `sceNpSignalingAddExtendedHandler` | 1 | 1 | 0 |
| `sceNpSignalingCreateCtx` | 1 | 1 | 0 |
| `sceNpSignalingDeactivateConnection` | 1 | 0 | 1 |
| `sceNpSignalingDestroyCtx` | 2 | 1 | 1 |
| `sceNpSignalingGetConnectionStatus` | 1 | 1 | 0 |
| `sceNpSignalingTerminateConnection` | 1 | 0 | 1 |
| `sceNpTerm` | 3 | 0 | 3 |

### sceNp2 (5)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `sceNpAuthCreateOAuthRequest` | 1 | 1 | 0 |
| `sceNpAuthDeleteOAuthRequest` | 2 | 1 | 1 |
| `sceNpAuthGetAuthorizationCode` | 1 | 1 | 0 |
| `sceNpAuthOAuthInit` | 1 | 1 | 0 |
| `sceNpAuthOAuthTerm` | 4 | 1 | 3 |

### sceNpCommerce2 (19)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `sceNpCommerce2CreateCtx` | 2 | 2 | 0 |
| `sceNpCommerce2CreateSessionFinish` | 1 | 1 | 0 |
| `sceNpCommerce2CreateSessionStart` | 1 | 1 | 0 |
| `sceNpCommerce2DestroyCtx` | 2 | 0 | 2 |
| `sceNpCommerce2DestroyReq` | 1 | 0 | 1 |
| `sceNpCommerce2DoProductBrowseFinishAsync` | 1 | 0 | 1 |
| `sceNpCommerce2DoProductBrowseStartAsync` | 1 | 1 | 0 |
| `sceNpCommerce2GetCategoryContentsCreateReq` | 1 | 1 | 0 |
| `sceNpCommerce2GetCategoryContentsGetResult` | 1 | 1 | 0 |
| `sceNpCommerce2GetCategoryContentsStart` | 1 | 1 | 0 |
| `sceNpCommerce2GetCategoryInfo` | 1 | 1 | 0 |
| `sceNpCommerce2GetCategoryInfoFromContentInfo` | 1 | 1 | 0 |
| `sceNpCommerce2GetContentInfo` | 1 | 1 | 0 |
| `sceNpCommerce2GetGameProductInfoFromContentInfo` | 1 | 1 | 0 |
| `sceNpCommerce2GetGameSkuInfoFromGameProductInfo` | 1 | 1 | 0 |
| `sceNpCommerce2GetPrice` | 1 | 0 | 1 |
| `sceNpCommerce2Init` | 2 | 2 | 0 |
| `sceNpCommerce2InitGetCategoryContentsResult` | 1 | 1 | 0 |
| `sceNpCommerce2Term` | 2 | 0 | 2 |

### sceNpTrophy (8)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `sceNpTrophyCreateContext` | 1 | 1 | 0 |
| `sceNpTrophyCreateHandle` | 1 | 1 | 0 |
| `sceNpTrophyGetRequiredDiskSpace` | 1 | 1 | 0 |
| `sceNpTrophyGetTrophyUnlockState` | 1 | 1 | 0 |
| `sceNpTrophyInit` | 1 | 1 | 0 |
| `sceNpTrophyRegisterContext` | 1 | 1 | 0 |
| `sceNpTrophyTerm` | 1 | 1 | 0 |
| `sceNpTrophyUnlockTrophy` | 1 | 1 | 0 |

### sysPrxForUser (21)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `_sys_process_at_Exitspawn` | 1 | 0 | 1 |
| `_sys_process_atexitspawn` | 1 | 0 | 1 |
| `sys_get_random_number` | 1 | 1 | 0 |
| `sys_initialize_tls` | 1 | 0 | 1 |
| `sys_lwcond_create` | 2 | 1 | 1 |
| `sys_lwcond_destroy` | 4 | 4 | 0 |
| `sys_lwcond_signal` | 3 | 2 | 1 |
| `sys_lwcond_signal_all` | 1 | 0 | 1 |
| `sys_lwcond_wait` | 2 | 2 | 0 |
| `sys_lwmutex_create` | 16 | 12 | 4 |
| `sys_lwmutex_destroy` | 17 | 4 | 13 |
| `sys_lwmutex_lock` | 29 | 15 | 14 |
| `sys_lwmutex_trylock` | 4 | 4 | 0 |
| `sys_lwmutex_unlock` | 35 | 11 | 24 |
| `sys_ppu_thread_create` | 14 | 8 | 6 |
| `sys_ppu_thread_exit` | 21 | 0 | 21 |
| `sys_ppu_thread_get_id` | 412 | 6 | 406 |
| `sys_process_exit` | 2 | 0 | 2 |
| `sys_prx_exitspawn_with_level` | 2 | 0 | 2 |
| `sys_prx_register_library` | 3 | 0 | 3 |
| `sys_time_get_system_time` | 12 | 8 | 4 |

### sys_crashdump (1)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `sys_crash_dump_set_user_log_area` | 2 | 0 | 2 |

### sys_fs (24)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellFsAioFinish` | 1 | 0 | 1 |
| `cellFsAioInit` | 2 | 1 | 1 |
| `cellFsAioRead` | 2 | 1 | 1 |
| `cellFsAioWrite` | 1 | 0 | 1 |
| `cellFsClose` | 15 | 4 | 11 |
| `cellFsClosedir` | 2 | 0 | 2 |
| `cellFsFstat` | 7 | 5 | 2 |
| `cellFsFsync` | 1 | 0 | 1 |
| `cellFsFtruncate` | 1 | 1 | 0 |
| `cellFsGetFreeSize` | 2 | 2 | 0 |
| `cellFsLseek` | 30 | 15 | 15 |
| `cellFsMkdir` | 3 | 2 | 1 |
| `cellFsOpen` | 11 | 11 | 0 |
| `cellFsOpendir` | 1 | 1 | 0 |
| `cellFsRead` | 20 | 18 | 2 |
| `cellFsReaddir` | 1 | 0 | 1 |
| `cellFsRename` | 3 | 3 | 0 |
| `cellFsRmdir` | 1 | 0 | 1 |
| `cellFsSdataOpen` | 1 | 1 | 0 |
| `cellFsStat` | 8 | 7 | 1 |
| `cellFsTruncate` | 1 | 1 | 0 |
| `cellFsUnlink` | 1 | 0 | 1 |
| `cellFsUtime` | 1 | 0 | 1 |
| `cellFsWrite` | 5 | 5 | 0 |

### sys_io (13)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `cellKbClearBuf` | 1 | 0 | 1 |
| `cellKbEnd` | 1 | 0 | 1 |
| `cellKbGetInfo` | 1 | 1 | 0 |
| `cellKbInit` | 1 | 0 | 1 |
| `cellKbRead` | 1 | 1 | 0 |
| `cellKbSetCodeType` | 1 | 0 | 1 |
| `cellKbSetReadMode` | 1 | 0 | 1 |
| `cellPadClearBuf` | 1 | 0 | 1 |
| `cellPadGetData` | 1 | 1 | 0 |
| `cellPadGetInfo2` | 3 | 3 | 0 |
| `cellPadInit` | 1 | 0 | 1 |
| `cellPadSetActDirect` | 1 | 1 | 0 |
| `cellPadSetPortSetting` | 1 | 0 | 1 |

### sys_net (18)
| function | call sites | r3 used | r3 ignored |
|---|---:|---:|---:|
| `_sys_net_errno_loc` | 19 | 16 | 3 |
| `accept` | 1 | 1 | 0 |
| `bind` | 2 | 2 | 0 |
| `connect` | 1 | 1 | 0 |
| `gethostbyname` | 1 | 1 | 0 |
| `getsockopt` | 1 | 1 | 0 |
| `listen` | 1 | 1 | 0 |
| `recv` | 1 | 1 | 0 |
| `recvfrom` | 2 | 2 | 0 |
| `send` | 1 | 1 | 0 |
| `sendto` | 2 | 2 | 0 |
| `setsockopt` | 5 | 5 | 0 |
| `socket` | 3 | 3 | 0 |
| `socketclose` | 3 | 3 | 0 |
| `socketselect` | 2 | 1 | 1 |
| `sys_net_abort_socket` | 1 | 0 | 1 |
| `sys_net_finalize_network` | 4 | 0 | 4 |
| `sys_net_initialize_network_ex` | 1 | 1 | 0 |


## Imports never called by this title

- `cellSpursSetPriorities` (cellSpurs)
- `cellSpursJoinTaskset` (cellSpurs)
- `cellGcmSetQueueHandler` (cellGcmSys)
- `cellGcmDumpGraphicsError` (cellGcmSys)
- `_cellGcmSetFlipCommand` (cellGcmSys)
- `cellGcmAddressToOffset` (cellGcmSys)
- `cellGcmSortRemapEaIoAddress` (cellGcmSys)
- `_cellGcmFunc15` (cellGcmSys)
- `cellGcmSetFlipMode` (cellGcmSys)
- `cellGcmSetDebugOutputLevel` (cellGcmSys)
- `cellGcmMapEaIoAddressWithFlags` (cellGcmSys)
- `cellGcmSetGraphicsHandler` (cellGcmSys)
- `_cellGcmSetFlipCommandWithWaitLabel` (cellGcmSys)
- `cellRtcFormatRfc3339LocalTime` (cellRtc)
- `cellHttpClientSetPipeline` (cellHttp)
- `cellHttpClientGetPipeline` (cellHttp)
- `cellHttpClientGetProxy` (cellHttp)
- `cellHttpClientGetAutoAuthentication` (cellHttp)
- `cellHttpClientGetResponseBufferMax` (cellHttp)
- `cellHttpClientSetAuthenticationCacheStatus` (cellHttp)
- `cellHttpClientSetAutoAuthentication` (cellHttp)
- `cellHttpClientSetResponseBufferMax` (cellHttp)
- `cellHttpClientSetProxy` (cellHttp)
- `cellHttpClientGetAuthenticationCacheStatus` (cellHttp)
- `sys_process_is_stack` (sysPrxForUser)
- `sys_lwcond_signal_to` (sysPrxForUser)

## Deliverable note

No fixes were applied and no game instance was started. Each proposed fix is a
one-line or small change, and each can be checked offline before any live run.
