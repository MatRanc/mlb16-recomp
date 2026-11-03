# HLE library fixes, batch 1 (speed / stability)

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard configuration: `play_windowed.sh`
(WCB=2, PIN=1, x11, `PS3RECOMP_RSX_CUBE_RT=1`, `PS3RECOMP_VK_PACE=1`,
`PS3RECOMP_VK_PRESENT_NEW_ONLY=1`, `VK_DRAW_BATCH=1`,
`VK_FRAME_PIPELINE=1`), plus `METER=1 SKIP_CINE=1` for the A/B.

Fixes findings **#9, #6, #11, #2+#3, #7** from
[`hle-contract-audit.md`](hle-contract-audit.md). Every fix is behind its own
environment switch. The code default is **off** for all five; the two
`play_windowed.sh` copies and `ab_live.sh` now default them **on** after the
live A/B and the full boot/menu/match-load run below.

## Switches

| # | switch | default in code | live key | what it changes |
|---|---|---|---|---|
| 9 | `PS3_LWCOND_REAL` (+ `PS3_LWCOND_FALLBACK_MS`) | 0 | yes | real host-condvar lwcond wait/signal/timeout, lwmutex destroy |
| 6 | `PS3RECOMP_AUDIO_PACE` | 0 | yes | POSIX cellAudio at 187.5 blocks/s (5333 us), backend room or host clock |
| 11 | `PS3RECOMP_FS_WRITE_CHECK` | 0 | yes | `cellFsWrite` errors on a short/failed write |
| 2+3 | `PS3RECOMP_SPURS_TEARDOWN` | 0 | yes | corrected core BUSY code + real Shutdown/Wait drain |
| 7 | `PS3RECOMP_GCM_OFFSET_SYNC` | 0 | yes | guest io/ea offset tables follow map/unmap |

`PS3_LWCOND_STATS=1` prints a periodic `[lwcond-stats]` line (waits,
timeouts, fallbacks, and the flip-done cond's signal/deadline split). It is
diagnostic only.

## 9. lwcond and lwmutex destroy

`PS3_LWCOND_REAL` already implemented the real wait/signal/timeout core
(`runtime/ppu/lwcond_real.h`) and was left off because a 4-run pitch A/B called
it neutral (+2.7 % pitch, -7 % hold). It was not unsafe by measurement; it was
incomplete: `sys_lwcond_destroy` returned EBUSY but never released the slot,
and `sys_lwmutex_destroy` was still a counted no-op. Slots were permanent and
the table could fill.

* `runtime/ppu/lwcond_real.h`: destroyed slots become tombstones; the probe
  chain stays intact and a later create reuses the slot. `lwc_slot_free()`
  checks the waiter queue and the release under the slot lock, so a waiter
  that enqueues just before destroy cannot lose its slot.
* `runtime/ppu/ppu_sysprx.cpp:505`: `sys_lwcond_destroy` returns EBUSY
  (`0x8001000A`) while any waiter is parked, otherwise frees the slot and
  clears the 8-byte guest structure.
* `runtime/ppu/ppu_sysprx.cpp:713`: `sys_lwmutex_destroy` returns EBUSY while
  the guest owner/recursion stamps are set or the host slot is contended,
  otherwise releases the slot and clears the 24-byte structure. Off, it keeps
  the old counted no-op.
* The switch is a live key (`PS3_LIVE_LWCOND_REAL`); the environment name is
  unchanged (`PS3_LWCOND_REAL`). Timeouts still return ETIMEDOUT
  (`0x8001000B`); real signal/signal_all/signal_to were already there.

Tests: `tests/sync_stress/test_lwcond_real.c` gains a `destroy` phase (idle
slot freed, parked waiter -> EBUSY); `tools/test_lwmutex_destroy.py` drives the
real HLE function (held -> EBUSY, slot busy -> EBUSY, free -> release + clear).
`tools/test_lwcond_layout.py` still passes.

Live (`[lwcond-stats]`, measured A/B windows): arm A (poll)
314,893 waits / 57 timeouts / 118 fallbacks; arm B (real)
34,336 waits / 4,619 timeouts / 8,620 fallbacks. B's flip-done cond
(`0x1245100`, Gfx_DisplayFlip) left the 17 ms wait **by signal 1,236 times**
and **by the timeout/deadline 822 times**; A polled 18,718 times with 11
deadline hits. The fallbacks are the designed 10 ms spurious wake for infinite
waits whose signal the HLE never sees (one boot handshake at `0x1246A98`);
they kept the full boot from parking.

## 6. POSIX cellAudio pacing

The POSIX mixer mixed a block, submitted it, then slept 5 ms/2 ms depending on
the queue: 200-500 blocks/s against the 187.5 blocks/s period. The guest
producer follows the published read index, so it was told to produce audio too
fast, the SDL queue grew, and the guest audio thread burned CPU.

* `libs/audio/audio_pace.h` (new): deadline = origin + blocks * 5333 us;
  a >50 ms gap re-anchors instead of mixing a burst.
* `libs/audio/cellAudio.c:659`: with the switch on, use the backend's room
  (`audio_backend_room`/`audio_backend_wait`, as the WASAPI branch does) and,
  with no device, the monotonic deadline above. The old loop is untouched when
  off. `AUDIO_RATE` reporting now exists on POSIX too.
* A/B at the meter: arm A 190.7 blocks/s (189.4-191.6); arm B 187.5 blocks/s
  after the first switch. That first B window read 2.2 blocks/s because it was
  draining the backlog the old arm had already queued; with the switch on
  from boot the run below is 187.5 blocks/s from the first 5 s window.
* Test: `tools/test_audio_pace.py` (1876 blocks in 10 simulated seconds,
  re-anchor after a 1 s stall).

## 11. cellFsWrite short/failed writes

`runtime/ppu/ppu_fs.cpp:572` now checks `fwrite` count and `ferror()`;
`libs/filesystem/cellFs.c:586` (the libs path) and
`cellFsUtilWriteFile` do the same through
`include/ps3emu/fs_write_check.h`. With the switch on, a short or failed write
returns `CELL_FS_EIO` / `CELL_EIO` instead of `CELL_OK`; the `nwrite`
out-parameter still carries the byte count so a caller can inspect progress.
The AIO write path already checked the count.

Test: `tools/test_fs_write_check.py` (exact, zero-progress, short, ferror,
zero-byte cases). This title uses the runtime path; the libs path is fixed for
the same defect.

## 2 + 3. SPURS error bases and workload teardown

The two are one switch on purpose: correcting BUSY without stopping dispatch
would turn the guest's `RemoveWorkload == 0x8041080A` retry loop into a hot
spin against a stub Shutdown/Wait.

* `include/ps3emu/error_codes.h:119-121`: core SPURS base is `0x80410800`
  (BUSY `0x8041080A`), fiber base is `0x80410700`. `tests/test_error_bases.c`
  now asserts both plus the BUSY literal.
* `libs/spurs/cellSpurs.c:3454` `ShutdownWorkload` (switch on) marks the
  workload `shutting_down`, clears its ready count and signal bit, sets guest
  state away from runnable, and clears its enabled bit; the coordinator and
  the fast re-run path both skip it.
* `libs/spurs/cellSpurs.c:3483` `WaitForWorkloadShutdown` waits on the
  instance condition until `active[wid]` drains, bounded by
  `SPURS_SHUTDOWN_MS` (default 2000 ms); on expiry it returns the corrected
  CORE_ERROR_BUSY. It drops the registry lock while waiting so a policy
  returning from its fast re-run loop can take it (holding it would deadlock
  the drain).
* `RemoveWorkload` returns the corrected BUSY when the drain switch is on.
  With it off it keeps returning the old swapped literal `0x8041070A`, so the
  default path is exactly the old behaviour and cannot hot-spin.

Tests: `tools/test_spurs_teardown.py` covers default-off (stubs, old BUSY
literal), a real drain (Shutdown stops dispatch, Wait blocks until the policy
returns, Remove succeeds), and the bound (a stuck policy makes Wait return
`0x8041080A` in 150 ms). `test_spurs_scheduler.py`/`test_spurs_api_limits.py`
still pass; the scheduler BUSY immutability check now runs with the teardown
switch on.

## 7. GCM guest offset tables

`cellGcmGetOffsetTable` published the guest copy once; later maps and unmaps
only changed the host arrays, so a page mapped after init stayed `0xFFFF` for
the 41 guest sites that cache the table pointers.

* `libs/video/cellGcmSys.c:474/496`: `populate_offset_table` and
  `clear_offset_table` also write the guest entries when
  `PS3RECOMP_GCM_OFFSET_SYNC` is on. `clear_offset_table` now rounds its page
  count up exactly like populate (a partial final page is unmapped too).
* `cellGcmInit` and the reset path republish the whole guest table after
  clearing the host arrays, so a re-init cannot leave old entries visible.
* This covers `MapMainMemory`, `MapEaIoAddress`, both unmaps and the
  `sys_rsx_context_iomap/iounmap` bridge.

Test: `tools/test_gcm_map_flags.py` now asserts the guest tables after
`MapEaIoAddress` **without** an intervening `GetOffsetTable`, and after an
unmap. `tools/test_gcm_io_mappings.py` exercises the same functions with the
switch off (host tables only).

## Offline verification

* New/updated: `test_error_bases.py`, `test_audio_pace.py`,
  `test_fs_write_check.py`, `test_spurs_teardown.py`,
  `test_lwmutex_destroy.py`, `test_lwcond_layout.py`,
  `sync_stress/test_lwcond_real` (all phases), `test_gcm_map_flags.py`,
  `test_gcm_io_mappings.py`, `test_cellfs_vfs_root.py`,
  `test_spurs_scheduler.py`, `test_spurs_api_limits.py` -- pass.
* `check_spu_context_abi.sh`: pass (no `spu_context` change).
* Video fixtures: 28/29 pass. `test_gcm_fifo_user_command` still fails to link
  on the pre-existing missing `rsx_draw_engine_set_fetch_memo` /
  `_set_pipe_memo` / `_set_remap_cache` stubs, the same failure recorded
  before this batch.
* Full `tools/test_*.py` sweep: 46 failures at HEAD, 45 with this batch. The
  batch fixes `test_lwcond_layout.py` and `test_gcm_io_mappings.py`; every
  remaining failure is in the HEAD failure set (missing Win32/RSX stubs, SPU
  corpus objects, guest-hook symbols, MinGW TLS, `test_gcm_fifo_user_command`).
  No new failure.

## Live A/B at the pitch meter

One launch (`ab_live_hlefix1ab_20261103_143008`), X once for the meter,
N=6 x 10 s per arm, 2 s dropped. A = all five switches 0; B = all five 1.
The live-ctl self-check passes for all 12 segments. Same binary as the offline
tests.

| metric | A off | B on |
|---|---:|---:|
| guest fps | 32.42 +/- 0.97 | 32.24 +/- 0.23 |
| walker ms | 18.26 | 18.36 |
| wait ms | 12.47 | 12.44 |
| per-flip fin p50 / p90 / p99 (us) | 34103 / 41522 / 49479 | 34159 / 41380 / 47718 |
| fin std (us) | 10712 | 32452 (13420 without segment 5) |
| c690 jobs/s (mean of periodic) | 5894.6 | 6284.2 |
| audio blocks/s | 190.7 | 187.5 (after backlog drain) |

Paired fps difference A-B **+0.19 +/- 1.15**, 95 % CI **[-0.97, +1.34]**: the
batch is level. B's larger std is three long gaps in one segment (one 921 ms,
at 14:34:14-20); that segment still read 31.99 fps, and the next segment was
clean. Excluding it B's fin p90/p99 are slightly better than A's and std is
13.4 ms.

## Full boot / menus / match load with B on

One launch (`ab_live_hlefix1boot_20261103_143658`), all five switches on from
boot, `SKIP_CINE=0`, `METER=0`, full navigation. It reached the pitch in 246 s
after the press-start screen, menus and match load, with no hang and no
teardown failure. `[audio-rate]` was 187.4-188.9 blocks/s from the first
window and 187.5 for the rest. SPURS added workloads 0-6 (WWS job lists, MP3,
crowd, kinematics) without a stalled dispatch; the match load and cinematic
path ran normally. This run also exercised the boot handshake that relies on
the lwcond fallback wake (8 capped `[LWCOND] ... fallback wake` lines at
`0x1246A98`), so the real implementation does not park it.

## Kept

All five switches are clean in both runs and level or better in the A/B, so
both `play_windowed.sh` copies and `ab_live.sh` default them on. Each remains
individually switchable with the names above; setting any to 0 restores the
old path, and the code default stays off for a run that does not use those
scripts.

## What to watch in a play test

1. Menus, match load and level/team teardown: the new SPURS Shutdown/Wait drain
   and lwmutex destroy are exercised hardest there. A stall or a
   `ShutdownWorkload`/`RemoveWorkload` loop would show up first at a transition.
2. Audio: speech/music should stay at the right pitch and not slowly drift.
   A stutter or queue growth would point at the pacing switch; `AUDIO_RATE=1`
   should read ~187.5 blocks/s.
3. Input/frame pacing under load: real lwcond blocks instead of polling, so
   watch for a freeze rather than a slow frame. `PS3_LWCOND_STATS=1` prints the
   timeout/fallback split if one appears.
4. Custom sounds/saves/cache writes: a failed write should now surface as an
   error instead of a silent loop; a message or a clean abort is better than
   the old hang.
5. Late-loaded geometry/animation: the offset tables should follow a map made
   after init. Missing or corrupt late geometry would point at the GCM switch.
6. Any of the above can be isolated live by setting that one switch to 0 in
   `PS3RECOMP_CTL` (or the environment) without restarting the game.
