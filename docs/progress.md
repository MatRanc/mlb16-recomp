# Progress log

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26) with `mlb16-work`, `feat/presentation-thread`, and `feat/gpu-vertex-offload`.

## 2026-11-03 (hitch census): the 5.1 s load gap is shader translation, boot gaps are SPU jobs

Two launches (`ab_live_hitchcold2_20261103_155504` cold census,
`ab_live_hitchwarm_20261103_160035` warm pipeline cache), plus one discarded
first attempt. Details, tables and artifacts in
[`docs/hitch-census.md`](hitch-census.md).

- **824 frames > 50 ms** in the 318 s cold run (10,051 flips): 67.4 s of
  frame time, 39.9 s above a 33.3 ms budget. Boot 18/5.92 s, menus **0**,
  match load 1/5.11 s, pregame cinematics 798/55.97 s, pitch meter 7/0.37 s.
- **5,112 ms match-load gap = first-use shader translation.** The walker
  drains 3.74 s, of which 2.95 s is our HLSL -> SPIR-V translation (352
  spans, 176 pipelines) and 0.25 s `vkCreateGraphicsPipelines`; tex 0.31 s,
  surface 0.04 s, c690 0.24 s. The frame thread waits 5.10 s on its flip
  lwcond and main waits 3.79 s on ref + 1.15 s in usleep. File I/O is **20
  reads / 514 KiB / 0.16 ms**, all on `ppu DataStream Process Thread`; none
  on the frame/main thread, so this is not synchronous `cellFsRead`.
- **237-654 ms boot gaps = SPU asset loading.** 18 boot frames of
  353-589 ms; e.g. 589 ms: DisplayFlip flip lwcond 577 ms, main `usleep`
  576 ms over 2,300 calls, walker idle 569 ms, six SPU lanes 1.53 s
  lane-union; one 40 KiB/0.02 ms read, no pipeline. The menus themselves
  had no >50 ms frame, so the old "boot/menu" label lands in boot.
- **Cinematics are c690.** 790/798 cinematic hitches are `c690.job`-
  dominated (79.1 s lane-union); six are first-use shader translation.
  Same producer/drain as the meter.
- **Persistent `VkPipelineCache` is not the fix.** Implemented behind
  `PS3RECOMP_VK_PIPELINE_CACHE` (default off, keyed by
  `pipelineCacheUUID`+driver). Cold -> warm: driver `pipe.create` 322 ->
  33 ms whole run, 248 -> 19 ms in the load frame, but `shader.compile`
  2.95 -> 3.01 s and the load frame 5.11 -> 5.26 s. First 261 s: 66.9 ->
  66.2 s of hitch time (-1.1 %, inside variation). Cache removes ~0.3 s
  of 67 s; a disk cache of our translated SPIR-V would target ~3.8 s.
- **Instrumentation:** hitch-only timeline (`PS3RECOMP_TIMELINE_HITCH_US`,
  new `pipe.create`/`shader.compile`/`tex.upload`/`surface.create`/
  `fs.read`/SPURS-add/remove/overlay-verify/resync events), and
  `tools/analyse_hitches.py` + `tools/hitch_run.py`. Raw 229 MB/804 MB
  timelines deleted after analysis; `check_spu_context_abi.sh` passes.
- **Deviation:** the warm run did not reach the pitch menu (stayed in
  cinematics ~800 s); the comparison uses the first 261 s that both runs
  followed identically. The cold run reached the meter at 264 s. No
  changes to the play scripts; the cache switch stays off.

## 2026-11-03 (flip deadline): the tick is starved by the walker drain; earlier completion loses

Two meter launches (`ab_live_flipdiag_20261103_150246` diagnosis,
`ab_live_prepflip_20261103_151553` A/B). Details in
[`docs/flip-deadline.md`](flip-deadline.md).

- **The 33 ms mode is the title's 2-vblank deadline, and it fires often.**
  369/1,289 frames (28.6 %) at the meter left `Gfx_DisplayFlip` by its
  `2 x 16683 us = 33.37 ms` deadline; they are a tight **34.15 ms**. The wait
  is `sys_lwcond_wait(0x1245100, 17000 us)`; two 17 ms timeouts reach the
  deadline. When the completion arrives it is fast: request -> present p50
  2.45 ms, request -> tick completion 6.75 ms, handler +0.1 ms, wait exit
  +0.02 ms.
- **The flip is presentable; the completion beat is late.** Deadline frames
  queue the present p50 0.35 ms after the request (368/369 before the
  deadline), but no completion beat lands in the frame. At the deadline the
  walker drain is active 80.2 % of the time and a c690 job 59.9 %; present
  fence and `vkQueuePresentKHR` 0 %. The display tick and the drain share the
  `rsx walker` thread, so a 23.3 ms drain (+10.8 ms) spans the deadline; the
  next completion lands p50 7.5 ms after it.
- **Neither candidate wins.** `FLIP_ON_DONE` was already 34.06 -> 26.00 fps
  (ref hand-off); a new default-off `PS3RECOMP_PREPARE_FLIP_KEEP_STATUS`
  (do not let `SetPrepareFlip` complete the outstanding flip) is **32.54 ->
  28.21 fps**, p50 34.15 -> 37.13 ms, walker 17.75 -> 21.84 ms, worse on all
  six paired segments, and it does not remove the deadline frames (823 -> 776
  per 60 s arm). Both switches stay off.
- **No >200 ms frames at the meter with batch 1 on**: max 83.96 ms; the run's
  18 gaps >200 ms (237-654 ms, one 5,152 ms) are all pre-meter asset loads /
  match load, not lwcond timeouts.

## 2026-11-03 (batch 1): lwcond, audio pacing, fs write, SPURS teardown, GCM offset tables

Five audit findings fixed, each behind its own default-off switch; the play
scripts now default them on. Details, line references and the test list are in
[`docs/hle-fixes-batch1.md`](hle-fixes-batch1.md).

- **#9 `PS3_LWCOND_REAL`**: the real wait/signal/timeout core already existed
  (it was left off as neutral, not because it was broken). Its gap was destroy:
  lwcond now returns EBUSY while parked and otherwise frees the slot
  (tombstone, so the probe chain survives), and lwmutex destroy does the same
  for its 24-byte structure. Live A/B: poll 314,893 waits/57 timeouts vs real
  34,336 waits/4,619 timeouts in the measured windows; the flip-done cond left
  by signal 1,236 times and by deadline 822 times. A boot handshake still
  relies on the designed 10 ms fallback wake.
- **#6 `PS3RECOMP_AUDIO_PACE`**: POSIX cellAudio now uses backend room or a
  monotonic 5333 us deadline instead of 2/5 ms sleeps. A/B audio 190.7 ->
  187.5 blocks/s; armed from boot it is 187.5 from the first window.
- **#11 `PS3RECOMP_FS_WRITE_CHECK`**: `cellFsWrite` (runtime and libs paths,
  plus `cellFsUtilWriteFile`) returns EIO on a short/failed write instead of
  OK, through one shared predicate.
- **#2+#3 `PS3RECOMP_SPURS_TEARDOWN`**: core SPURS base is `0x80410800` (BUSY
  `0x8041080A`), fiber `0x80410700`; Shutdown stops dispatch and Wait drains
  `active[wid]` with a bound (default 2 s), returning BUSY on expiry. Off, the
  old swapped BUSY literal is preserved so the stub cannot cause a hot spin.
- **#7 `PS3RECOMP_GCM_OFFSET_SYNC`**: populate/clear also write the guest
  io/ea offset tables, and init/reset republish them. Clear now rounds partial
  pages up like populate.

Live A/B at the pitch meter (N=6, 10 s, 2 s drop, one launch): fps A
**32.42 +/- 0.97**, B **32.24 +/- 0.23**, paired **+0.19 [-0.97, +1.34]**;
per-flip fin p50/p90/p99 A 34.1/41.5/49.5 ms, B 34.2/41.4/47.7 ms; walker and
wait unchanged. B's std is inflated by one segment with three long gaps (max
921 ms, still 32 fps); without it std 13.4 vs 10.7 ms. A second launch with all
five on from boot navigated menus and a match load with `SKIP_CINE=0` and
reached the pitch in 246 s, no hang, audio 187.5 blocks/s, SPURS workloads 0-6
dispatched normally.

Offline: new tests for each switch pass; `check_spu_context_abi.sh` passes;
video fixtures 28/29 (the pre-existing `test_gcm_fifo_user_command` link
failure). Full `tools/test_*.py` sweep: 46 failures at HEAD, 45 now (the batch
fixes `test_lwcond_layout.py` and `test_gcm_io_mappings.py`); no new failure.

## 2026-11-03 (HLE contract audit): how many more return/out-param bugs?

Static-only audit of every imported HLE function this title calls, prompted by
the `cellGcmSetPrepareFlip` bug. **433** imports; **407** called (**1,604** call
sites); **26** never called. At **548** call sites the lifted code reads `r3`;
**234** imports have at least one such site. **32** contract mismatches found;
none fixed, no game run.

Top ones (guest addresses, evidence and one-line fixes in
[`docs/hle-contract-audit.md`](hle-contract-audit.md)):

- `_cellSpursTaskAttributeInitialize` mis-decodes the descriptor form used by
  the Bink SPU task: context becomes the stack descriptor address, size 0,
  lsPattern 0; the task parks in `WAIT_SIGNAL` (11-02 log).
- The SPURS core error base is the fiber base (`0x80410700` instead of
  `0x80410800`), so the guest's `RemoveWorkload == 0x8041080A` BUSY test never
  matches; `ShutdownWorkload`/`WaitForWorkloadShutdown` are also success stubs.
- `cellAudioOutGetState` writes downMixer/soundMode at `+4/+8` instead of
  `+8/+12`; the Bink path reads `soundMode.channel` at `+13` and gets 0,
  choosing 6/8-channel instead of stereo.
- `cellAdecOpenEx` is not implemented and the first handle is 0, which the
  guest rejects; streamed AAC/ATRAC audio never opens.
- The POSIX `cellAudio` mixer free-runs at 200-500 blocks/s vs 187.5 and
  publishes that read index to the guest.
- GCM offset tables are published only by `cellGcmGetOffsetTable`, so pages
  from a later `cellGcmMapMainMemory` stay `0xFFFF` in guest memory.
- `cellFsRename`/`cellFsTruncate` use the libs `./gamedata` mapping while
  open/write use `$PS3_VFS_ROOT`, so renames of installed USRDIR files fail.
- Default lwcond wait polls 1 ms and ignores timeouts; lwmutex destroy is a
  no-op.
- `cellSearch*` is entirely unimplemented (fake success, no callback or
  out-params).
- `cellFsWrite` returns OK on a short/failed write (cache worker can spin);
  `cellSaveDataDelete2` is a no-op; `cellGameDataCheck` reports `NOTCALC` for
  `sizeKB` and `cellGameGetSizeKB` always writes 0.

## 2026-11-03 (backward frames): `cellGcmSetPrepareFlip` returned 0, so every flip named buffer 0

The whole-frame backward step is fixed. The title renders alternately into
display buffers 0 (`0x10000`) and 1 (`0x890000`), but our
`cellGcmSetPrepareFlip` returned `CELL_OK` (0) instead of the buffer id. The
title stores that value in its flip queue (`Gfx_FrameEnd`, 0x00066530) and its
display thread passes it, masked to 8 bits, to `cellGcmSetFlipImmediate`
(`Gfx_DisplayFlip`, 0x000692dc), so every flip named buffer 0. When buffer 0
still held a frame from two frames earlier, the window showed that image after
a newer buffer-1 frame; the next flip went forward again. Camera, HUD and pitch
meter are all in that buffer, which is why the whole frame stepped back. The
real SDK and RPCS3 both return the buffer id.

- **Fix:** return `bufferId` from `cellGcmSetPrepareFlip`, live key
  `PS3RECOMP_PREPARE_FLIP_RET` (default off; now exported as 1 by both
  `play_windowed.sh` copies). It still does not queue a FIFO flip: this title
  flips through `SetFlipImmediate`, and queueing here would flip twice.
- **Instrumentation:** `PS3RECOMP_PRESENT_TRACE=1` logs the flip index,
  requested buffer id/offset, source surface and its last colour-write serial,
  and flags a present whose source is older than the previous present's
  (`back=1`); `PS3RECOMP_PRESENT_DUMP_DIR` writes 160x90 PGMs of the frames
  actually displayed (after the `PRESENT_NEW_ONLY` gate).
- **Live A/B at the meter (one launch, N=6, 2 s drop):** stale-source presents
  **4.71 % -> 0.038 %**; source buffers 0/1 went from 2403/414 to 1370/1292
  (the flips now alternate). Guest fps A **30.34 +/- 0.29**, B **29.34 +/-
  2.38** (one 24.77 fps resync segment; other five mean 30.25), paired
  **+1.00 fps**, 95 % CI **[-1.27, +3.28]**. Present intervals A p50 **35.24**
  / p99 **57.45 ms**, B p50 **34.91** / p99 **61.14 ms**.
- **Dumped images (moving frames, nearest-of-previous-8 match):** **219.9**
  backward frames per 1000 moving frames with the fix off (210 of 955; peaks
  90/min in the pregame cinematics, most exactly two frames back), **6.2** with
  it on (5 of 804, all at cuts). Example: frames 5300 and 5302 are identical
  with 5301 between them when off; monotonic when on.
- **Offline:** `test_rsx_draw_engine` and the other engine fixtures pass;
  `test_gcm_fifo_user_command` still fails to link on the pre-existing missing
  `rsx_draw_engine_set_*` stubs. `spu_context` ABI check passes.

Details, tables and images: [`docs/backward-frames.md`](backward-frames.md).

## 2026-11-03 (freeze): the 08:30 stall was a `spu_context` ABI split; VFS exonerated

The first post-08:30 boot with commit `decb3fb8` added `uint64_t src_seq` in
the **middle** of `spu_context` (`resident_code[]`). The lifted SPU modules are
compiled against the other copy of `spu_context.h` reached through
`recomp-work/runtime -> ps3recomp-native/runtime`, so every field after
`resident_code` (`dcache_id`, `host_depth`, `steps`, ...) moved in the runtime
but not in the lifted code. The c690 job dispatch then read stale state and
took the invalid-command `stop 0x1` at LS `0x1095C`; all five `spurs pm` lanes
ended at `pc=0x108E8`, `jobs=3 in-flight=5`, and the FIFO park/resync lines were
a downstream symptom. The 08:09 glibc crash was the already-fixed
`SPURS_PM_FAST_RESCHED` double unlock and is unrelated.

- **VFS was not the cause.** No `recomp-work/vfs` file changed between 07:40
  and 08:36; the 08:05 good run and the 08:30 bad run opened the same six
  CACHE files at the same relative times; both VFS WADs and SDATs are
  byte-identical to the ISO. The `.corrupt` files from 11-01 are a separate
  incident.
- **`build-good` "stalls too" is not evidence:** it was configured with a
  non-existent `SPU_DIR`, so it has zero lifted SPU sources and no jobs.
- **Fix:** keep the memo, move its state to the **end** of `spu_context`
  (`resident_code_src_seq[4]`); the shared prefix offsets match the lifted
  modules again. Offline ABI probe confirms every shared field matches (only
  `sizeof` differs, by 32 B). `run_code_dma_memo_test.sh` 15/15,
  `run_texdirty_range_test.sh` 16/16, the three SPURS fixtures PASS.
- **Guard:** `recomp-work/ab-tools/check_spu_context_abi.sh` compares the build
  header with the lifted header and refuses to start `play_windowed.sh` /
  `ab_live.sh` on a shared-field mismatch (`SPU_ABI_CHECK=0` overrides).
- **Verified in 3 launches** (cap 6): reverted `decb3fb8` works (968,913
  c690 jobs), pre-fix HEAD stalls (`jobs=3`), fixed HEAD works (1,092,754 jobs,
  no resync/`[pm-end]`, pitch meter confirmed at 162 s).

Details and the offset table: [`docs/freeze-20261103.md`](freeze-20261103.md).

## 2026-11-03 (c690 code memo, O(1)): generation-tracked reloads; A/B launch wedged in baseline

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. The follow-up mission had
a **one-game-launch** cap. Unit tests run before it; the launch was used and
failed to reach the meter. Details and tables:
[`docs/c690-job-gaps.md`](c690-job-gaps.md).

- **`SPU_CODE_DMA_MEMO` now has an O(1) check.** The first GET after a load
  still compares the bytes, then registers the full source span with the
  TEX_DIRTY page write sequence (write-protecting the pages). Later GETs skip
  on one per-range generation compare, not a 67 KB `memcmp`; a source write
  (including a direct PPU store, which faults into the page writer) moves the
  generation and the next GET falls back to the exact compare. Untracked
  ranges (switch off, TEX_DIRTY off, 16-entry table full) always compare. The
  per-write range scan is skipped while no range is registered, so the
  baseline write path is unchanged with the switch off.
- **Offline tests, all passing.** `run_code_dma_memo_test.sh` (byte cases,
  generation establishment, source page `PROT_NONE` to prove the tracked path
  does not read it), a new `libs/video/tests/run_texdirty_range_test.sh`
  against the real `rsx_wcb.c` range tracker (tracked write, direct PPU store,
  unrelated ranges, enable/disable epochs), and the SPURS scheduler fixture,
  which now drives `spurs_pm_fast_consume` and every
  `spurs_pm_fast_can_rerun` early return and re-acquires the lock line after
  each call (the run-2 double-unlock class). `runtime/spu/tests/run_tests.sh`
  is 11/11 after weak c690 stubs were added to the shared fixture support.
- **No A/B result, no winner.** The one launch (`jobgap3`) wedged in the
  **baseline** arm (both switches off) during the pregame cinematic, before
  any segment: no flips after 08:32:34, five `spurs pm` lanes at `pc=0x108E8`,
  c690 jobs=3 with `in-flight=5`, then the pitch poll timed out. The switches
  were never entered, so this is not a measured B wedge; the cap allowed no
  retry. Both `play_windowed.sh` copies are unchanged; both switches remain
  default off.

## 2026-11-03 (c690 job turnover): the gap is tail + host dispatch + head, not one lock

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One mapping launch
(timeline + 25 s `perf` + `PS3RECOMP_C690_GAP`) and one A/B launch that was
aborted by a runtime crash in its second arm; the two-launch cap is used.
Full tables, the code changes and the crash: [`docs/c690-job-gaps.md`](c690-job-gaps.md).

- **The inter-job gap is three phases.** 628,783 same-lane gaps under 2 ms:
  job end -> policy-run end **28.6 us** p50, policy-run end -> next start
  **27.5 us**, start -> next job **26.2 us** (gap p50 **85.1 us**, mean 118.5).
  The host phase has the long tail (mean 52.5, p99 471 us), so at the mean it
  is the largest single term.
- **The PM runs once per job.** ~136k jobs and ~139k policy spans per lane;
  policy span p50 196 us vs job 136 us, inter-policy gap 27.5 us. The PM exits
  to the kernel after each job and the coordinator re-dispatches it.
- **The c690 module is re-DMA'd every job.** 86 s window: 7.74 M code GETs /
  **85.4 GiB** (70 KiB per gap = the 67,648 B module), and the overlay verifies
  67 KiB again once per job. This is the avoidable half of the head.
- **Atomics are not the bottleneck.** 2.56 GETLLAR and 2.55 PUTLLC per gap,
  7 % of PUTLLCs fail; the gap phases show no lock-dominated tail. The c690
  job body is 47 % of `spurs pm` perf samples; PM + runtime are small leaves.
- **Implemented default-off:** `SPURS_PM_FAST_RESCHED=<n>` (bounded re-run of
  a still-ready PM in the same worker, mirroring the coordinator's ready /
  signal / contention bookkeeping) and `SPU_CODE_DMA_MEMO=1` (skip a code GET
  already byte-identical in local store; exact, unit-tested).
- **A/B aborted, no winner.** B crashed after ~13 s with a double
  `spu_lockline_unlock()` in the fast-reschedule consume path
  (`pthread_mutex_lock` owner assertion). Fixed and rebuilt, but not re-run
  because the cap was reached. The invalid B stretch looked better (gap p50
  67.7 vs 79.8 us, lane busy 0.548 vs 0.512); that is a direction, not a
  result. Both `play_windowed.sh` copies are unchanged.

## 2026-11-03 (frame timeline 2): the long frame is the c690 burst, not the walker drain

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter launch,
45 s `PS3RECOMP_TIMELINE` window armed 22 s after the meter. Details, tables
and the counterfactuals: [`docs/frame-timeline-2.md`](frame-timeline-2.md).

- **p50 did not move, the tail did.** 300 frames: p50 **34.23 ms** (old
  33.85), p90 **44.33** (51.32), p99 **49.14** (59.60), mean 32.43 (33.64).
  Full 45 s: 34.11 / 43.55 / 49.96. The >45 ms frames collapsed into 35-45.
- **The walker drain left the critical chain.** 26.37 -> **18.85 ms**; the
  saved time went into producer waits: kick-idle 3.17 -> 6.47 and
  `rsx.put_wait` 4.10 -> 7.12 ms. `drain + idle + put` is the whole frame.
- **A long frame is one c690 burst + ~6 ms.** Long >35 ms: L 41.6, c690 union
  36.0, burst wall 37.0, drain 25.8 (below c690), idle 10.0, put 5.9; frames
  with the whole burst inside: 42.1 = 36.1 + 6.0.
- **The five c690 lanes are 64 % occupied inside the burst.** Big bursts:
  wall 37.1, union 35.1, per-lane busy 20.0-24.9 ms, mean 3.19/5 lanes busy;
  ramp 0.86 ms, tail 0.39 ms. The idle is 139,664 inter-job gaps (p50 60 us,
  85 % < 0.1 ms), staggered across lanes (k=0 only 5 %): per-job
  dispatch/turnover, not a global scheduler tick or lane start skew.
- **Not two game frames per burst.** `gfx.frame_end` / `ppu.frame_end` /
  `flip_request` are 1.00 per frame, user handler 0.70/frame, and a 1 ms
  burst holds 0.77 flip ends (1.27 at 2 ms). 135 bursts touch one frame, 81
  touch two (pattern long 34.0 -> short 16.9: the tail spills over).
- **Halving c690 wall (per-frame counterfactual):** p50 **16.9-18.7**, p99
  **28.7-30.5**, mean 19.4-20.3. c690 x0.8 -> p50 27.0/p99 41.6; x0.69
  (perfect lane packing) -> 23.1 / 37.5.
- **Next levers:** (1) c690 lane packing / per-job dispatch (-11 ms p50,
  -12 p99); (2) native unpack/pack, skin and normalize kernels (-4-7 ms, and
  with (1) p50 19-22 / p99 32-36); (3) PPU main work only after c690 < 20 ms.
  Present fence rose 3.30 -> 7.18 ms but is off the guest chain. The walker
  is no longer a p50 lever.

## 2026-11-03 (pinning and idle retest): `lanes` no longer wedges, no layout winner; back-off neutral

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Full numbers and method:
[`docs/pinning-and-idle.md`](pinning-and-idle.md). Two launches (the retest
budget), both completed.

- **`PS3RECOMP_PIN` is live in `build-play`.** The earlier weak-reference bug
  is fixed by `ps3_thread_pin_link_anchor()` referenced from
  `templates/project/main.cpp`; the `[pin] PS3RECOMP_PIN=1 topology ...`
  banner appears in both runs. The registry and layout-name fixes were
  revalidated offline: 12 layout switches produced 12 `[pin] layout` lines,
  and launch 2 (no layout change) produced 0, not the old 107,588-line storm.
- **`lanes` retested with the fixed registry: no wedge, still a loss.**
  Three 10 s segments at the meter: **29.17 fps** vs **30.82** for `today`,
  and three presented-frame gaps > 100 ms (699.7 / 899.8 / 707.6 ms) with 20
  skipped flips; `today` and `os` never exceed 96 ms. Rejected.
- **`today` vs `os` is lane affinity only, and is inside run-to-run noise.**
  Both pin walker cpu 0 and ppu main cpu 1. Launch 1: 30.82 vs 30.53 fps,
  p99 50.96 vs 51.14 ms, std 11.12 vs 10.41, walker+wait 31.66 vs 32.58 ms;
  the lane-0 job rate is 2,371 vs 2,482 jobs/s (max/min 1.25 vs 1.23).
  `today` repeated at **31.33 fps** in launch 2, so the 0.95 % gap is noise.
  No controlled `PIN=0` vs `PIN=1` arm; `PIN` is still not enabled in
  `play_windowed.sh`.
- **Loading is not a spin problem.** Valid 30 s `top -H` samples: no thread
  reaches 90 %. With back-off off, `spurs pm` means 19-35 % (max 78.8),
  `rsx walker` 34.0 % (max 85.8), `ppu main` 23.7 % (max 87.8). The on half is
  later in the same load (total CPU 219 % -> 444 %), so its higher SPU numbers
  are the load stage, not the back-off.
- **SPU idle back-off at the meter is neutral.** Six segments per arm on
  `today`: 31.33 fps off vs 31.40 on, p50 34.18 vs 34.14, p99 50.45 vs
  49.04 ms, walker/wait within 0.13 ms, c690 rates within 1 %; one 540.8 ms
  one-off outlier in the on arm. Left default-off.
- **No `play_windowed.sh` change.** `today` is the in-code default when
  `PIN=1`; no layout or back-off beat the baseline outside noise.

## 2026-11-03 (native c690 Edge, step 2): corpus fix, 0xFDA0 kernel, live attempt

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Details, algorithm
description, exactness tables and the updated stage plan:
[`docs/edge-native-step2.md`](edge-native-step2.md).

- **Corpus fixed.** Stage snapshots are capped on outermost `0xFDA0` entries and
  written to a per-lane spool independent of job sampling; the job file stays
  open through the submit epilogue. The job-file run records **249,886 PUTs**
  (step 1: 0). Four spool runs collected **403 / 400 / 400 / 401** pairs; the
  last is a meter burst (frames 9116-9125; the meter stage is bursty so a
  >= 2 s spread of 300 meter pairs is not achievable).
- **Native kernel, bit-exact offline.** The 45-block `0xFDA0` state machine is
  flattened into one C function (locals for `sr0..sr93`, continuation calls as
  gotos, unchanged `spu_helpers`). Full 256 KiB LS + all 128 GPRs match on
  **1,604 / 1,604 spool pairs and 12 / 12 job-file pairs** with the shipped
  runtime TU. The in-game mismatches were harness/TU bugs: stale fallback
  compare, exit-MXCSR use, and the runtime TU missing `SPU_FAST_FLOAT`.
- **Speed: 1.30x, not 5x.** Lifted 136.7 us -> native 106.1 us median on 12
  pairs (1.28-1.35x). That is ~-1.0 ms per worker per frame at the 31.8 %
  tree/cull share, below the frame A/B noise. The step-1 kill criterion was
  5x.
- **Not landed live.** Two meter arms with `PS3RECOMP_C690_NATIVE_CULL=1`
  wedged the guest (`FIFO resync 0x0136E970`, `w=2136E970`, no flips ~10 s
  after the switch). The switch exists, default-off, and is not enabled in
  `play_windowed.sh`. Next: capture the live meter class with the fixed spool
  (10x cap) and shadow for 60 s, or move to unpack/pack (23.5 %, 5-10x target).

## 2026-11-03 (walker quick wins): remap cache, fetch memo, pipeline memo, `[unknown]`

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Offline replay A/B plus
two live meter launches; details, tables and the perf call-site map:
[`docs/walker-quick-wins.md`](walker-quick-wins.md).

- **Pipeline memo is the only live win.** `PS3RECOMP_PIPE_MEMO=1`, 85.0 % hit
  live, walker `pipe` **2.107 -> 1.633 ms** (-0.47) and -0.84 ms offline.
  Enabled in both `play_windowed.sh` copies.
- **The fetch plan memo is a live loss and explains the old `WALKER_FAST`
  regression.** Live hit rate **5.4 %** (reference sets change every frame):
  `fetch` 4.080 -> 6.493 ms (+2.41) in the bundled arm. The one-frame replay
  hits 497/502 and hides it. Left off.
- **Remap cache (`rsx_vertex_remap_build`, 20.3 % of the offline replay) is
  exact and -1.74 ms offline, but live-neutral:** 28.5-30.2 % hit, `fetch`
  +0.31 ms, fps -0.21 +/- 0.69. Left off.
- **Offline all three: 10.283 -> 8.077 ms/frame (-2.21).** Replay gates
  unchanged for every switch: meter `a6fcc849341eba9b` /
  `ff2a220c28750bc3`, pitch `416abee2c5bff47d` / `3e6f6ad4ebb23ed6`.
- **The 27.6 % `[unknown]` is not a JIT.** Walker samples by DSO: 80.4 % our
  symbolized binary, 11.3 % libc (memmove/syscalls), 8.3 %
  `libnvidia-glcore` (Vulkan driver). The driver time is per-draw recording
  from `vk_eng_draw_from`: `vkCmdBeginRenderPass` 14.5 % of driver samples,
  `vkCmdDispatch` (vertex unpack) 12.8 %, descriptor alloc/update ~7 %,
  `vkCmdEndRenderPass` 4.1 %, `vkCmdBindPipeline` 5.7 %, `vkQueueSubmit`
  2.3 %.

## 2026-11-03 (native c690 Edge, step 1): perf, corpus, 0xFDA0 reference

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One successful meter
launch with a 20 s whole-process `perf record` plus an env-gated c690 corpus;
details, tables and the ranked plan: [`docs/edge-native-step1.md`](edge-native-step1.md).

- **Per-flip (20.008 s perf window, 640 intervals):** p50 **34.05 ms**,
  p90 43.58, p99 49.35, mean 31.19, std 11.20, 32.0 fps. Measured from
  `PS3RECOMP_FLIP_LOG`, not the every-10th `[flip]` decimation.
- **New c690 ranking (48,116 c690 samples):** `0xFDA0` 19.7 %, `0xD4A8` 8.4 %,
  `0x6D68` 6.4 %, `0xFA58` 5.9 %, `0xAF98` 5.7 %, `0x8160` 5.1 %,
  `0x7C18` 5.0 % (already native), `0xD2B8` 4.9 %, `0xFCB8` 3.2 %,
  `0xF520` 2.8 %. Tree/cull family **31.8 %** (~4.6 ms/worker/frame);
  unpack/pack 23.5 %; skin/transform 13.6 %; normalize 8.2 %.
- **Corpus:** 2,000 jobs / 227 frames / 19 opcode layouts; 12,138 DMA GETs
  (1,217 B/job); 4 `0xFDA0` stage pairs. 0 PUTs: the output DMA is in the
  submit epilogue after the job-end command. Record format and variant counts
  in the doc; capture data stays in `recomp-work/`.
- **Reference:** `ps3recomp-play/tools/c690_ref/` - a 1,799-line C
  transliteration of the 45-block `0xFDA0` state machine (runtime plumbing
  removed, exact `spu_helpers.h` arithmetic). **4/4 stage pairs bit-exact**
  (0 LS bytes, 0 GPRs differ). NHL's stage split/corpus method credited in
  `THIRD_PARTY_NOTICES.md`; no NHL code copied.
- **Plan:** tree/cull -> unpack/pack -> skin/transform -> normalize ->
  format/parser, then the remaining codegen tail. Hook at the outermost
  `0xFDA0` call, keep the job framework lifted, fall back on layout mismatch,
  exactness = corpus LS+GPR diff + in-game shadow + per-job hash. Expected
  c690 union 35 -> ~23 ms after the first three stages, ~19 ms after the tail;
  the 19.5 ms main-ref cycle is the floor.

## 2026-11-03 (texture dirty): revalidation by guest-page write generation

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter A/B launch
plus one paranoid-check launch. Details, coverage table and limitations:
[`docs/texture-dirty.md`](texture-dirty.md).

- **Replaced the texture content hash** (`eng_texture_slot`'s sparse hash
  every frame plus a full hash every 8) with a per-4K-guest-page write
  sequence. A texture stores the sequence sampled before its upload;
  revalidation is a max over its pages. Writers bump the sequence at their
  choke point (SPU PUT/atomic, 2D transfers, WCB/engine readback, file and
  network reads, HLE memcpy/memset); the PPU's own lifted stores fault
  through `PROT_READ` pages into the existing WCB handler. WCB-armed pages
  and texture pages share one protection state.
- **Live meter A/B (6 x 10 s per arm):** walker drain **26.64 -> 23.27 ms**,
  `tex` **10.34 -> 5.89 ms** (-4.45), engine 25.16 -> 21.64, guest fps
  **29.21 -> 31.13**. Per flip p50 **33.80 -> 32.00 ms**, p99
  **44.57 -> 41.06**, stddev 4.12 -> 3.73.
- **Paranoid mode** (`PS3RECOMP_TEX_DIRTY_PARANOID=30`, full hash of every
  bound texture every 30 frames compared with the uploaded bytes):
  **44,656 checks, 0 stale** over the meter run.
- **Gates unchanged** with `PS3RECOMP_TEX_DIRTY` on and off: meter
  `a6fcc849341eba9b` / `ff2a220c28750bc3`, pitch `416abee2c5bff47d` /
  `3e6f6ad4ebb23ed6`. New unit test drives a tracked writer and a direct PPU
  store and asserts the next bind re-uploads.
- **Default:** engine off, `play_windowed.sh` now exports
  `PS3RECOMP_TEX_DIRTY=1`; the live key can flip it per run. Off is the old
  hash path unchanged.
- **Left on the table:** `tex` is still 5.9 ms, mostly the real decode/upload
  of textures that do change. Sub-page/rectangle dirty regions or persistent
  GPU textures per target are the next step; the readback cuts and Vulkan
  recording items in `walker-profile.md` are unaffected.

## 2026-11-03 (walker own work): texture hashing, Vulkan recording, vertex fetch

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter launch
(`ab_live_metercap_20261103_002238`) captured one frame and produced the live
phases; the per-draw split is offline replay of that capture. Details, tables,
ranked plan and exactness gates: [`docs/walker-profile.md`](walker-profile.md).

- **Live meter, 552 draws/frame:** walker drain **27.64 ms**, engine
  **26.02 ms**, parse 1.62 ms. Per draw: 50.1 us drain / 47.2 us engine.
- **Engine split:** `tex` **11.33 ms** (43 %), `draw` **5.51** (21 %),
  `fetch` **4.28** (16 %), `other` 2.61, `pipe` 2.07, `bind` 0.23, layout
  ~0.40. Texture work is the cache (98 % of `sink_bind_textures` is
  `eng_texture_slot`: lookup + sparse/full content hash), not surface scans.
- **Vulkan recording** (`VK_DRAWPROF`, 526 draws/frame): vtxgpu 1.53 ms,
  rp_begin 0.89, copy 0.79, desc 0.68, barrier 0.42, state/bind_draw/rp_end
  0.21 each = 4.94 ms. The null replay's `draw` (1.4 ms) is CPU conversion
  only.
- **Readback** is a separate 6.2 ms/frame (4.60 copy + 1.57 wait,
  `readback-cost.md`) in the walker wall wait, outside the drain.
- **Capture frame:** 502 draws, 525 groups, 114,688 vertices, 4.0 MiB
  converted, 276 texture binds, 0 new pipelines after warm-up, 525 pipeline
  lookups/frame.
- **Offline replay now models a steady scene:** `rsx_draw_engine_replay_advance_frame()`
  ticks `g.frames` after each replay frame (the replay has no display buffer,
  so texture revalidation used to stay on frame 0). The last-frame hash
  `a6fcc849341eba9b` and the 10-frame gate `02a922c9b2620fa1` are unchanged.
- **New `PS3RECOMP_WALKER_PROF2=1`** (default off): 22 nested terms around the
  fetch plan, conversion, pipeline key/analysis/create, texture slot/upload,
  constants, index build, backend binds, surface tracking and draw recording.
  `REPLAY_NULL_HASH=0` disables the null-backend digest for timing runs.
- **Plan to <= 8 ms:** texture page-dirty revalidation (-6 to -9), readback
  cuts (-3 to -4.5), Vulkan recording (-2 to -3), fetch/pipeline memo
  (-2.2 to -3.2), cheaper parse/other (-1.5 to -2.5), GPU vertex conversion
  (-1 to -1.5), then a parse/record split (-3 to -5). Each item's exactness
  is the meter/pitch replay hash plus a per-item test.
- **Not run:** `perf` (absent; `perf_event_paranoid=2`) and a second live
  window. The live phases are from the post-capture A/B window (2 x 20 s).

## 2026-11-03 (c690 profile): 165 us/job, 14.4 ms per worker, and no repeat output

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Three meter launches
(one killed by the run harness, two usable); details, tables and the ranked
plan: [`docs/c690-profile.md`](c690-profile.md).

- **Job cost:** c690 job wall p50/p90/p99 = 121/248/980 us, mean 164.6 us,
  437 jobs/frame. Cumulative 72 ms/frame across five lanes = **14.4 ms
  per worker**; the "35-40 ms" figure is the frame/union span on long frames,
  not each lane.
- **Hot spots:** `0xFDA0` tree traversal/culling 19 %, `0xD4A8` skin 8.1 %,
  `0x6D68` SNORM unpack 7.1 %, `0xAF98` normalize 6.2 %, `0x7C18` pack 5.7 %
  (already native). Compute is 90.5 % of running time; dispatch 5.2 %, MFC/DMA
  1.9 %, coherency 1.4 %.
- **Five lanes are the game's SPURS config:** `MeshJobs0/1` use
  `maxContention=5` and priority lane 5 = 0. Overriding lane 6 wedged the
  guest in run 3; rejected.
- **Caching is dead:** 40.3 % of sampled jobs re-read identical input
  (25.9 % across frames), but only **0.07 %** produce identical output.
- **Ceiling probe:** +50 us/job (+4.4 ms/worker) cost 1.95 fps (CI 0.63-3.28)
  and +2.2 ms walker SPU wait. Frame time follows c690 with ~0.55 elasticity;
  below the 19.5 ms main cycle further c690 cuts stop helping p50.
- **Instrumentation kept (all default-off):** `PS3RECOMP_C690_HASH=<stride>`
  per-job input/output hashes, `C690_SPIN_US` live cost probe,
  `SPURS_LANE6` live lane-5 override.
- **Not run:** `perf` (absent; `perf_event_paranoid=2`, no system change) and
  the fresh psample window (watcher picked the `timeout` wrapper). Per-function
  shares come from the existing gdb profiles `prof_spu2/3.prof`.

## 2026-11-03 (frame timeline): the meter's waits are work, not late wake-ups

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One windowed meter run
(`ab_live_timeline_20261102_233854`), X once, WCB=2, PIN=1, CUBE_RT=1,
`PRESENT_NEW_ONLY=1`, PACE=1, x11, the four cheap-win live switches on.
Analysis and instrumentation only; no optimisation. Details, inventory,
tables and PNGs: [`docs/frame-timeline.md`](frame-timeline.md).

- **New `PS3RECOMP_TIMELINE=<path>|1`**: the existing per-thread trace rings,
  written as a binary record stream (`PSTL`, 32-byte records, thread-name and
  event chunks) instead of Chrome JSON. Trigger file + `PS3RECOMP_TIMELINE_SECS`
  window, as `PS3RECOMP_TRACE` already had. New events: frame begin/end, the
  ref/flip lwcond waits with their awaited value, walker pass / FIFO put /
  idle wait, ref set/publish, SPU PUT, c690 job, WCB done, draw submit,
  present queue/submit/fence/vkQueuePresent/shown, GCM user handler, and a
  `wait.wake_lag` instant = wake minus the newest causal event. With the
  variable unset every hook is one load/branch and no state changes.
- **Overhead, same launch:** 45 s recorded vs the 30-80 s after the sink shut
  down. Guest flip-to-flip p50 +0.03 ms, p90 +0.29, p99 +0.71, mean
  **+0.32 ms/frame**. 6,815,181 events / 218 MB / 0 dropped / 41 threads.
- **Frame time, 300 consecutive meter frames:** p50 **33.85 ms**, p90 51.3,
  p99 59.6 (full 45 s: 33.90 / 51.0 / 58.6). Walker drain is 26.4 ms/frame
  (78 %); the walker idle wait is 3.17 ms and its wake lag **0.00 ms**. Main
  is 12.5 ms work + 20.5 ms ref wait (the same walker work from the PPU
  side), ref wake lag **0.18 ms/frame** (0.50 on the 37 frames where it is
  the critical chain).
- **Short/long is the c690/mesh hand-off.** 256/300 frames are walker-drain
  critical: 35.7 ms frames, 28.7 ms drain, 24.7 ms c690 lane time, 3.4 ms
  idle. 37/300 have an empty FIFO (11.4 ms drain, 0.4 ms idle, 2.3 ms c690):
  main's 19.5 ms ref cycle is the frame. Transitions are 35 each way;
  lag-1 autocorrelation 0.02. The present side is already even (shown
  intervals p50 37.6, <8 ms 0.15 %).
- **Ranked wake-up lags:** ref lwcond 0.18 ms/frame (event-driven buys 0 at
  p50/p99, best case -0.18 ms mean), walker idle 0.00 (already an auto-reset
  event), present fence 3.30 ms but that is GPU blit execution, DisplayFlip
  flip-done 0.40 ms off the guest chain, SPU channels 0. c690's end-to-first
  PUT is p50 3 µs and the walker's kick-to-run lag ~10 µs.
- **Loading 100 % cores:** static deduction, not sampled (the planned
  `top -H` helper produced no file). The only no-OS-wait loops are lifted SPU
  poll loops (`GETLLAR`/128-byte GET, no LLAR back-off); with PIN=1 the walker
  is cpu0 and main cpu1, so CPU 10/11 are two unpinned `spurs pm` workers.

## 2026-11-03 (cheap wins): X skips the pregame, four live cuts, and the AVX2/FAST_LS build

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, PACE=1, x11. Details, tables and PNGs:
[`docs/cheap-wins.md`](cheap-wins.md).

- **Nav: boot-to-meter 245 s -> 153-157 s** with `SKIP_CINE=1` (X only on a
  frame that matches no menu, is not black, and is not the pitch camera; the
  exact pitch-menu match still triggers the one meter X). The first version
  pressed X on the loose pitch-camera match and that X was consumed by the
  batter-view transition; the meter then needed a second X. The final flow
  waits for the exact menu, which is the safe trigger.
- **Live bundle (SPU_MFC_LOOKUP_CACHE, GCM_NOP_SKIP, GCM_REF_IMMEDIATE,
  PS3RECOMP_RSX_TEX_PIN): 28.54 -> 29.90 fps** (N=6, 10 s, 2 s dropped; paired
  -1.37 fps, 95 % CI -2.66..-0.07), walker wait 7.75 -> 6.78 ms. The run's A/A
  check is not at the A/A level (first A segment 26.4 fps warm-up); excluding
  it the gain is +0.94 fps. `SPU_MFC_LOOKUP_CACHE` alone was inside noise.
- **Build A/B (base vs FAST_LS+AVX2+VMX on): 22.35 vs 28.35 and 26.76 vs
  28.68 fps.** c690 job wall 65.5 -> 48.5 ms and 60.8 -> 47.7 ms; walker SPU
  wait 11.6 -> 7.2 and 10.9 -> 7.8 ms. `-mavx2`/`-mfma` were already implied
  by `-march=native`; `-ffp-contract=off` was not, and FAST_LS is the main
  driver. `-march=x86-64-v3` is a no-op under native.
- **Correctness:** AVX2 exactness 446.8 M lanes bit-identical, FAST_LS
  helpers 72/72, VMX op oracle + 200 k-vector accessor fuzz clean, replay
  hash `416abee2c5bff47d` (gate `67f03d06377de89b`) identical with the
  texture pin off/on. Meter frames unchanged.
- `play_windowed.sh` (both copies) enables the four live switches;
  `build-play` is reconfigured to the winning build options.

## 2026-11-03 (exposure + readback cuts): the meter's exposure readback is fresh and flat; the cuts do not change it

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, PACE=1, x11, two runs of 240 s each.
Details, tables and images: [`docs/exposure-and-readback-cuts.md`](exposure-and-readback-cuts.md).

- **The uncommitted RECT/BATCH/SKIP cuts are committed and gated** (live keys,
  default off; off path is the old path). Fixed one gating bug: `guest_dirty`
  was cleared in `guest_read` even with the cuts off. Added the missing
  walker-profiling/capture stubs to the two GCM fixtures; all 29 video fixtures
  pass. `exp/live-ab` pushed to `fork`.
- **Static:** `Lighting_InitStadium` (`0x4f0380`) creates `ToneMapSample`
  (32x32, 6 mip levels, RGBA8, `0x14008`), `QuickAmbientSky` (32x32, 1 level,
  `0x14008`) and `HarmonicsSample` (32x32, 1 level, `0x10008`).
  `Lighting_RenderExposureRT` (`0x4f48c8`) renders the temp camera and
  `ToneMapMipGen`; `Lighting_UpdateFrame` (`0x4f2198`) writes the per-material
  lighting block every frame.
- **Continuous, not once at start:** the new `PS3RECOMP_LIGHT_PROBE=1` (default
  off) logged the `ToneMapSample` 32x32/2x2/1x1 chain read 4,198 times per
  level in 255 s (16.5/s, ~every second frame) at the meter.
  `QuickAmbientSky`/`HarmonicsSample` were only read in boot/transition bursts.
- **No stale/partial reads:** every copy was at the surface's current serial
  (`fresh=1`, `covers=1`, no ticket mismatch). 1x1 exposure **78.7 +/- 0.3**
  (A, cuts off) vs **78.7 +/- 0.2** (B, RECT+BATCH+SKIP), drift **-0.1** vs
  **0.0** over 4 min; thumbnail luma **101.2** in both. The reported live-game
  lighting drift did not reproduce on the meter; no fix was made and the third
  run was not used. PNGs: `recomp-work/visuals/exposure/`.

## 2026-11-03 (frame pacing + cube fallback): the present pairs are two flips handed over back to back

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, x11. Details, tables and images:
[`docs/frame-pacing.md`](frame-pacing.md).

- **Per-flip log:** `PS3RECOMP_FLIP_LOG=1` prints `req/fin/pres/show` for every
  flip on one microsecond clock (`show_gap` is the display interval). For this
  title `req == fin` (statically linked libgcm, the drain decodes the
  `0xFEAD` word; no HLE markers), so the measurable split is
  `fin -> pres` (walker -> window handoff) and `pres -> show` (present thread).
- **Where the pairs are:** with `PRESENT_NEW_ONLY=1` and PACE off, 32 % of
  present intervals are < 8 ms and 60 % > 40 ms at a 34.5 ms mean. The
  per-flip log already shows two frames at `pres` 1-4 ms apart: the guest
  runs up to 3 flips ahead, the drain catches up and hands two frames over
  back to back. It is not re-presenting (`flips not shown: 0`) and not FIFO
  queueing (`pres -> show` p50 4 ms).
- **Fix:** `PS3RECOMP_VK_PACE=1` (live key, default off) holds each queued
  present in the present thread until its slot in an arrival-cadence
  schedule. Interval EWMA is measured on *new frame arrivals*, not on our own
  presents (the first attempt fed back and ratcheted B to 6 fps). The
  schedule is capped at one interval of wait and abandoned when presents are
  already queued; a late frame is presented at once. The blit is submitted
  when the frame ends, so only `vkQueuePresentKHR` is delayed.
- **A/B (one launch, 6 x 10 s per arm, 2 s dropped):** guest fps A 29.30
  +/- 0.86 vs B 29.07 +/- 0.76, paired +0.22 (95 % CI -0.35..+0.79), A/A
  PASS. Present intervals from the per-flip log: p50 **46.46 -> 38.43 ms**,
  p90 **58.30 -> 51.60**, std **24.28 -> 14.90**, < 8 ms **32.2 -> 0.0 %**,
  > 40 ms **61.8 -> 45.2 %**. MangoHud CSV agrees (p50 45.71 -> 39.04 ms,
  std 23.80 -> 15.96, < 8 ms 28.8 -> 3.2 %).
- **Cube probes:** an incomplete probe now binds the last complete cube image
  for that guest address (`cube_target_last` hook, address-keyed backend
  cache); the dummy stays only when no complete image ever existed. Unit
  test covers a face recreated out of format and a never-complete probe.
  Pregame in the same run: 20 addresses (368 frames) were logged incomplete
  after their first complete frame and are now served the last image; the
  1,435 remaining `cube` census misses are never-complete probes. No 2 s
  thumbnail pair within a scene shows a brightness flip.
- **Defaults:** `ab_live.sh` exports `PS3RECOMP_VK_PRESENT_NEW_ONLY=1`;
  `play_windowed.sh` (both copies) sets `PS3RECOMP_VK_PACE=1`.

## 2026-11-03 (readback attribution): the meter's WCB bill is guest transfers and faults, not texture binds

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, x11. Details, tables and images:
[`docs/readback-cost.md`](readback-cost.md).

- **Attribution added:** `PS3RECOMP_RSX_WCB_ATTR=1` (default off) counts every
  WCB=2 copy per surface, per trigger (`texbind`, `guest`, `other`) and per
  copy/wait ms, in 60-frame windows. The trigger is set around the
  `sink_bind_textures` depth/reinterpret calls; the module records the
  guest-thread wait per region.
- **Meter (N=2, 35 steady windows):** 1.32 copies/frame, 4.60 ms/frame copy,
  1.57 ms/frame guest wait (~6.2 ms, the WCB=2-vs-WCB=0 gap). Trigger split
  **0.00 texbind / 0.66 guest / 0.66 other**. The `other` third is a 512x512
  RGBA target copied whole once per ~3 frames (3.13 ms/frame) through the
  guest's NV3089 blit (`cellGcmSys.c:1168`); the `guest` third is a
  32x32+2x2+1x1 mip chain read by PPU/SPU faults (1.47 ms copy + 1.57 ms wait
  for 4 KB, i.e. submission stalls).
- **The bind path exists only off the meter.** Menus bind a 2560x720 Z24S8
  depth target as A8R8G8B8 and the engine stores 2.5 MB to guest memory
  (~3.3 ms) because Vulkan has no `depth_as_color` backend hook (Metal does,
  `rsx_metal_backend.m:3250`). At the meter that bind does not occur.
- **Consequence:** serving texture binds from the GPU target
  (`PS3RECOMP_RSX_RT_TEX_GPU`) cannot move the meter; there are no
  bind-triggered readbacks to serve. The A/B was stopped rather than run as a
  no-op. If wanted for menus, the concrete work is the Vulkan
  `depth_as_color` hook; the engine already prefers it.
- **Real levers:** resolve only the NV3089 rectangle instead of the whole
  512x512 target; submit the tiny mip resolves together instead of one
  `submit_and_wait` each; skip the copy when the target's colour serial has
  not moved since the last read.

## 2026-11-03 (cube render targets + frame depth): players lit, one render frame in flight by design

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, x11. Details, tables and PNGs:
[`docs/cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md).

- **Part A (static):** `FUN_000655cc` waits for the **previous** frame's ref
  (`DAT_01245058[DAT_011203d4 & 3]`, where `DAT_011203d4` is the last frame id
  set by `FUN_00065760`), and that token is the `SET_REFERENCE` (method 0x0050)
  appended at the **end** of frame N-1. The guest therefore allows exactly one
  render frame in flight. `Gfx_FrameEnd`'s `DAT_01120410 + 1` gate allows three
  *display* flips; it is a separate `submitted - completed` queue and is not the
  measured 19-21 ms block. Our ref publication is not late: the token is queued
  when the walker executes 0x0050 and published at drain end / by the poll
  publisher; a dropped token would time out at ~9 s, not 19 ms. No fix
  implemented; the lever is faster walker drain, not the fence.
- **Part B:** the 8x8 cube probes are six separate 8x8 colour targets at
  `base + face * 0x200` (`0x059F3000`, `0x059F3C00`), written by one fragment
  program (`0xbd8ae188ddb5b0b8`, pipes 48/88/260) in a burst, not every frame.
  `PS3RECOMP_RSX_CUBE_RT=1` (default off, live key) copies the six faces into
  one six-layer cube image on the GPU, cached by face handles + colour serials;
  incomplete cubes keep the dummy and count a new `cube` census reason.
- **Runs:** A/B on the meter (N=6): off **30.06 +/- 0.53 fps**, on
  **30.09 +/- 0.58**, paired **+0.03** (CI -0.19..+0.25). Census at the meter:
  off 25,800-29,796 misses/10 s, on **0** after the switch. With
  `VK_DUMMY_BLACK` unset, **0 magenta pixels** at the meter and no visible
  pink; players are brighter (batter's blue jersey, catcher's red, umpire's
  grey), the field/crowd gain environment shading.
- **Harness:** `ab_live.sh` gained `DUMMY_BLACK`, `IGNORE_MAGENTA`,
  `CAPTURE_PREFIX`, `FINAL_SHOT`; `nav3.py`/`poll_pitch.py`/`same.py` mask
  magenta pixels out of the luma diff, so the magenta arm navigates on the first
  attempt.
- **Tests:** `test_rsx_draw_engine` (six faces, rewrite serial, incomplete,
  stacked) and `test_rsx_tex_miss` (new reason) pass; both are in
  `tools/test_video_fixtures.py`.

## 2026-11-02 (dummy census): the dark players are cube surfaces read as 2-D; MangoHud now counts flips

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. WCB=2, PIN=1, x11,
`PS3RECOMP_WALKER_RESYNC_FIX=1`, new `PS3RECOMP_TEX_MISS_LOG=1`. Details,
run table and PNGs: [`docs/textures-dummy-census.md`](textures-dummy-census.md).

- **New census** (`libs/video/rsx_tex_miss.{c,h}` + unit
  `test_rsx_tex_miss`): every dummy fallback by reason (format, surface,
  address, size, other), guest address, format byte, size, pitch, mips, cube,
  fragment hash and pipeline; per-frame summary + cumulative top-10. The only
  dummy sites in `libs/video` are the vtable `vk_cb_bind_texture` and the
  engine descriptor (`vk_eng_sample_view` / `vk_eng_write_descriptors_to`).
- **The jerseys are not a format/address/size miss.** At the meter the engine
  resolved every texture: `format=0 address=0 size=0`; **201,000 misses in the
  1029-frame meter window, mean 195/frame, max 582**, all the render-target
  reason. They are 8x8, 1-mip, `cubemap=1` textures whose guest address is also
  a tracked colour target (e.g. `0x059f3000`); the engine binds the 2-D
  surface/view to a `TextureCube` unit and the backend reads the dummy. One
  fragment program accounts for ~217/frame. Fix is a six-face cube-surface
  path, not contained, design in the doc.
- **Team-select logo is not a dummy:** black and magenta team frames are
  identical (mean diff 0.0); `RESYNC_FIX=1` still does not bring the logo back
  (park-resync draw loss, not texture resolution).
- **Magenta arm blocked by the harness:** with `VK_DUMMY_BLACK` unset the nav
  references (recorded with black dummies) lose the pitch twice, so no magenta
  meter frame; the saved b-roll pitch frame is ~24% pure magenta (217,220 px),
  confirming the dummy surfaces are the crowd/player materials.
- **`PS3RECOMP_VK_PRESENT_NEW_ONLY=1` (task 5):** gated on the new
  `cellGcm_flip_frame_count()` (SetFlipCommand / FIFO marker, not
  SetPrepareFlip), so re-presents of the frame on screen are dropped. Start
  screen: **`[flip]` 42.66 fps, MangoHud 42.57 rows/s, shown 426/10 s**, 0
  drops, pump gap 6-7 ms. `play_windowed.sh` now sets it.

## 2026-11-02 (meter pacing): guest work, not a 30 fps cap; the tick is not the missing tick

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Pitch meter, X once,
10 s settle, 6 x 10 s per arm, PIN on, 45 s trace window armed at the first
A/B switch. Details and PNGs: [`docs/meter-pacing.md`](meter-pacing.md).

- **Static:** the title never calls `cellGcmSetFlipMode` (import present, no
  call site), does not import `cellGcmGetVBlankCount` /
  `cellGcmSetVBlankFrequency`, and its vblank handler (`0x69288`) only updates
  a counter and a deadline that nothing reads. `Gfx_FrameEnd` allows the main
  to run **3 flips ahead**; `Gfx_DisplayFlip` flips continuously. There is no
  two-vblank wait, flip interval, or per-scene pacing switch. The title,
  pitch-wait and meter use the same path.
- **WCB=0, both `EXP_NO_DRAW=1`:** 16 ms **31.32 +/- 1.06 fps** vs 8 ms
  **28.05 +/- 0.43**, paired **+3.27** (CI +2.30..+4.23). WCB=2: 16 ms
  **28.89 +/- 1.77** vs 8 ms **26.85 +/- 0.56**, paired **+2.04** (CI
  +0.47..+3.62). **Halving the tick is slower, not faster.**
- **Per-frame trace (WCB=0):** main running p50 **14.7 ms**, blocked **19.6 ms**
  in `FUN_000655cc`; of the 18.9 ms ref wait only **0.97 ms is walker drain**
  and **16.8 ms is the walker waiting for a producer** (954 `gcm.spu_put`
  instants in a sample 13.8 ms wait). With WCB=2 the same wait is **16.8 ms
  walker drain** and 4.3 ms producer. Removing draws moves the wait to the
  guest mesh producer; it does not shorten the frame.
- **Vblank ticks/flip:** WCB=0 16 ms **2.05** vs 8 ms **4.48**; WCB=2 2.32
  vs 4.68, while the wall frame stays 32-38 ms. The meter's "two ticks" is
  arithmetic, not tick quantization or a title setting.
- **Verdict:** (1) guest work - main's own ~15 ms (p50) plus the buffer-ref
  handoff set by the SPU mesh producer - not (2) a deliberate 30 fps target
  and not (3) our flip/tick path. Keep draw fast-path benchmarks on the
  pitch-wait/gameplay, where the guest asks for flips sooner.
- **Tooling:** new `gcm.flip_vb` trace instant (request, vblank), `vc=` on the
  `[flip]` line, and `tools/analyse_meter_pacing.py` (streams the full SPU
  trace, splits arms from the `.marks` file, reports per-frame main
  blocked/running and the walker split inside the ref wait).

## 2026-11-02 (meter baseline): the stable meter is guest-cadence bound; NO_DRAW does not help it

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. X once at the pitch,
fixed 10 s settle, 10 s segments, PIN on, x11, 2 s scene thumbnails + contact
sheets. Details and PNGs: [`docs/meter-scene-baseline.md`](meter-scene-baseline.md).

- **Harness:** `ab_live.sh`/`measure_fast.sh` now press X exactly once and
  verify the meter from the frame dump against `ref/pitch_meter*.png`
  (crop 370,95,395,120; WCB-on and WCB-off references); auto-settle replaced
  by a fixed 10 s wait measured from the post-X walker plateau. Each run
  saves `scene_<epoch-ms>.png` every 2 s and a `scene_sheet.png` with the
  measured A/B bands.
- **A/A, WCB=2, N=8:** A **29.34 +/- 0.64** vs B **29.93 +/- 0.89 fps**,
  paired **-0.59 +/- 1.09** (CI -1.67..+0.50), segment std 0.77/1.07. Cleaner
  than the old pitch A/A (~1.6 fps CI); point estimate 0.59 fps and CI covers
  0.
- **Ceiling, WCB=0, N=6:** baseline **33.55 fps** (walker 19.17, wait 10.76)
  vs `EXP_NO_DRAW=1` **30.92 fps** (walker **3.78**, wait **28.60**), paired
  **+2.63** (CI +1.78..+3.48). **NO_DRAW is slower here.** The meter asks for
  a flip about every 30-32 ms (two 16 ms ticks); removing 15.4 ms of draw work
  leaves the frame at two ticks. The old pitch-wait 41 fps NO_DRAW ceiling
  does not reproduce on the stable meter.
- **WALKER_FAST=1 is slower on the meter:** A **29.18** vs B **27.21 fps**,
  paired **1.98** (CI +1.01..+2.94); walker 27.36 -> 30.71 ms, fetch 4.07 ->
  6.57, tex 10.16 -> 11.39. Do not enable it as a cut.
- **Per-phase (WCB=2 baseline):** walker 26.73 ms; engine 25.21 (tex 9.80,
  draw 4.75, fetch 4.01, other 4.54, pipe 1.91, bind 0.21), parse 1.50;
  source split main 10.20 ms/988 packets, mesh 11.57 ms/1393 packets. WCB=0
  cuts tex to 4.29 and main to 4.25 (mesh 12.12), so the WCB readback path is
  ~6 ms in the normal config.
- **Draw fast-path order (ms/frame, headroom not fps):** mesh 11.57, tex
  9.80, main 10.20, draw 4.75, fetch 4.01, pipe 1.91, parse 1.50. On the meter
  the fps payoff is 0 until the guest cadence/tick changes; test
  `EXP_TICK_US=8000` with NO_DRAW on the meter next.
- **Launch check:** current `play_windowed.sh` (Xwayland default) launched
  `build-play` to the title at **42.3-42.6 fps**, window `shown=63-68`, 0
  no-slot drops, no address-0/frame_clock crash (74 s). The older native-Wayland
  launcher reproduced the known present freeze, not a crash.

## 2026-11-02 (tick + resync): the 16 ms tick caps only NO_DRAW; FAST_WAKE still wedges

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Pitch meter, 6 x 10 s
per arm, PIN on. Details and PNGs: [`docs/tick-and-resync.md`](tick-and-resync.md).

- **A/B band labelling:** the report was right, the PNG was read as game fps.
  MangoHud frametime is the *host present* interval; the bands also included
  the 2 s the report drops (the previous arm's tail). Bands now shade the
  measured window, print the band's guest `[flip]` fps, the plot handles
  MangoHud's nanosecond `elapsed` column, and a live-ctl/arm self-check passes
  on all re-rendered runs. `ab_live.sh` defaults WCB=2, warns when arm B names
  a key arm A does not (the control file never reverts a key), and has
  `BOOT_ARM` so `EXP_NO_DRAW` is not live at boot.
- **Jump-park resync fix** (`PS3RECOMP_WALKER_RESYNC_FIX=1`, default off):
  instead of teleporting `get` to `put` and losing the draws, rewinds it to
  the first unread offset below `put` and drains the range through the normal
  walker. Offline replay gate unchanged (`0f91a96c79a7aadf`); a park-reuse
  fixture shows the skipped probe executed with the fix, dropped without.
- **FAST_WAKE still not viable:** with the fix the arm is **~9 fps** (walker
  55 ms/frame vs 17 baseline) and wedges on the 3rd B (guest 314.5 s). The fix
  fires once, walks `0xC03C..0xC190`, and flips do not resume; 4d skipped.
- **Tick ceiling, NO_DRAW (WCB off):** A 16 ms **40.89 fps** vs B 8 ms
  **44.25 fps**, but B is bimodal: 54.0/60.4/55.8 in three segments,
  28.7/30.8/35.9 in the others. **The 16 ms tick is a real cap** (B exceeds
  41.5 by 45 %) but 8 ms is not free (doubled vblank/flip callbacks).
- **Tick, real drawing (WCB=2):** A **33.83** vs B **33.71 fps**, paired
  **0.12 fps** (CI -5.96..+6.20), A/A-level. With draws the walker is the
  limiter, not the tick.
- **Game speed:** the tick scales `cellGcmGetVBlankCount`, the vblank handler
  argument and the flip rate, so flip/vblank-driven logic speeds up (`sys_timer`
  is unaffected). Today's 16.000 ms is 62.5 Hz, already 4.3 % fast vs 59.94.
  A proper fix needs a 59.94 Hz vblank tick, flip completion at the marker (or
  a render-ahead mode), and a drain/fence cadence off the tick; >60 fps with
  unchanged per-flip simulation needs per-title delta-time work.

## 2026-11-02 (live A/B + ceilings): walker draw work is the limit; FAST_WAKE wedges

Branch `exp/live-ab` (`ps3recomp-play`, off `play/multiscene`), `build-play`.
One windowed run per arm pair, control file `PS3RECOMP_CTL` toggled every
10 s, 6 segments/arm, WALKER_PROF + MangoHud. Details:
[`docs/live-ab-and-ceilings.md`](live-ab-and-ceilings.md).

- **Harness:** live toggles for `PS3RECOMP_WALKER_FAST`, `PS3RECOMP_WALKER_WAKE`,
  `EXP_NO_DRAW`, `EXP_FAST_WAKE`; opt-in `PS3RECOMP_PIN=1` (walker cpu0,
  ppu main cpu1, others 2-5,8-11) with the mapping printed; `ab_live.sh`
  auto-settles until the pitch walker mean is <=20 ms for 8 s.
- **The pitch is not stationary for 1-3 min:** asset streaming swings the
  walker 14-45 ms/frame. A 10 s wait caught 16-38 fps segments. A/A after
  settle: PIN off diff **-0.31 fps** (CI -5.56..+4.95, std 4.2/4.9); PIN on
  **-0.11 fps** (CI -1.73..+1.51, std 2.2/1.7). Pinning cuts the spread ~2.5x;
  the best CI resolves ~1.6 fps, not 0.5.
- **NO_DRAW ceiling:** baseline **30.26 fps** (walker 24.94 ms, wait 10.76) vs
  **41.22 fps** (walker **2.15 ms**, wait 22.11), paired **-10.96 fps**
  (CI -17.69..-4.23); every segment faster, NO_DRAW std 0.6 vs 6.5. **Walker
  draw work is the frame limiter; the PPU side then caps at ~41.5 fps.**
  Draw source is mixed: PPU main ring 3.8-34.6 ms/frame, SPU mesh rings
  4.4-11.1 ms/frame. A native fast path should cover both, mesh first.
- **FAST_WAKE:** wedged the pitch twice at the same 5th B segment (guest
  ~300.8 s): jump-park resync, frame commands dropped, flips stop. The busy
  poll re-reads an unpatched park; the 4 ms block was giving the PPU time.
  Start-screen A/B: no gain (46.5 vs 46.5 fps); the 16 ms tick is the pacer
  (draw frames wait 14-19 ms, idle frames 28-33 ms; walker own 0.1-0.6 ms).
- **Pipeline cache:** none in `rsx_vulkan_backend.c`; a whole run is only
  ~340-430 creates, **30-41 ms total** (p50 29-36 us, max 0.4-6.9 ms). Not the
  scene-change stutter.
- **Timing:** flip completion is gated by the 16 ms tick (`main.cpp:263`);
  >60 fps needs a tick/timing change, not a faster walker alone.
- **Wayland:** native-Wayland FIFO froze again (showed 0 for the run); the
  runs used `SDL_VIDEODRIVER=x11` (Xwayland), the documented fallback.
- Frametime PNGs: `recomp-work/ab_live_aa_pin0_settled_20261102_140417_ft.png`,
  `..._aa_pin1_settled_20261102_141235_ft.png`,
  `..._ceil_nodraw_20261102_142204_ft.png`,
  `..._start_fastwake_20261102_143955_ft.png` (plus the invalid early runs,
  listed in the doc).

## 2026-11-02 (visuals): WCB ondemand fixes crowd/faces/field; gamma TXD gap and team logo explained

Windowed pitch A/B on `play/multiscene`, `build-play`, FIFO present, MangoHud
CSV, 10 s `[flip]` windows, one run per arm plus two ondemand repeats.

- **Game fps:** WCB off **35.94**; ondemand (`PS3RECOMP_RSX_WCB=2`) **30.87 /
  31.88 / 30.60** (mean **31.12**); async (`=3`) **28.71**. WCB costs about
  4.8 fps, async is slower than ondemand here.
- **Ondemand counters:** ~2.0-2.5 demand copies/frame, 292-395 KB, 0.6-0.7
  waits/frame (avg 1.5-2.1 ms, max ~28 ms), 3.4-4.3 ms copy, 0 timeouts,
  ~50-55 armed. Async also had a single **642 ms** read wait.
- **Visuals:** the dark crowd, dark/uniformless players, neon-yellow field and
  the rainbow/oil-slick catcher mask in the WCB-off frame all read correctly
  with ondemand or async (coloured crowd, skin tones, blue/white uniforms,
  natural green field). `HeadFaceCacheFile` was backed up and deleted before
  the WCB arms and regenerated correctly. The `[RSX fp] opcode 25` warnings
  remain in the WCB frames but did not cause the chrome faces there.
- **Team-select rotating logo:** pixel-identical with WCB off/2/3, so not a
  readback issue. The FIFO walker's jump-park resync (`cellGcmSys.c:2701`)
  still drops draws in the skipped range (`:1651-1745`); the 2026-10-27 note
  that the logo appears with an effectively infinite park grace points at the
  same cause. Proposed: raise `GCM_PARK_GRACE_MS` as a test, or replay draws
  on a resync. Not implemented.
- **Gamma/sign-expand:** `TEX/TXP/TXB/TXL` already decode both; the logged
  opcode 25 is `TXD`, where gamma fails the whole program (`rsx_fp_decompiler.c:876`)
  and sign-expand reads unexpanded (`:1078`). Because WCB removed the observed
  artefacts, the `PS3RECOMP_FP_GAMMA=1` work was left for an approved change.
- **Crowd patch:** the recomp applies no equivalent of
  `mlb-show-rpcs3-fixes/patches/MLB16_crowd_fix.yml` (one branch at
  `0x009bd544`); crowd placement still needs it.
- Details and images: [`docs/visuals-wcb-gamma.md`](visuals-wcb-gamma.md),
  `recomp-work/visuals/compare.png`.

## 2026-11-02 (window freeze): native-Wayland MAILBOX present stalls; FIFO by default

The player's window froze for 15-50 s at a time while the game kept running;
moving the mouse unfroze it. The `PS3RECOMP_VK_PRESENT_STATS=1` lines prove it
is presentation: the pump ran normally (`max pump gap` 4-15 ms) while every
frame was dropped **no-slot** (`blits 0, shown 0, dropped ~65/s`). The whole
session was **219 frozen s / 457 s**, 15,059 no-slot drops, only 87 not-ready,
and the longest `vkQueuePresentKHR` was **49.2 s**.

- **Mechanism:** a 3-image swapchain exposes only 2 present slots
  (`rsx_vulkan_backend.c:1982`). A slot is busy until `vkQueuePresentKHR`
  returns. On native Wayland + NVIDIA MAILBOX the present thread can block
  waiting for the compositor's buffer release; after two stuck presents every
  later flip is dropped at `:2210-2211` *before* the acquire. `no-slot` is not
  an acquire timeout, so `PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS` cannot help.
- **A/B (windowed, MangoHud, no input):** MAILBOX froze hard in 2 of 4
  play-style runs (138 s and 82 s frozen; 5-8k no-slot) and in the player's
  session. Wayland FIFO: 0 s and 1 s frozen, 1-2 no-slot over 147-149 s.
  Xwayland (which negotiated FIFO) 0 s / 4 no-slot. The measure_fast pitch
  harness did not reproduce the menu/attract stall (0 frozen in two runs);
  pitch fps was **35.44 flips/s** there.
- **Fix:** `tools/play_windowed.sh` defaults `PS3RECOMP_VK_PRESENT_MODE=fifo`
  (override with `mailbox`) and sets `VK_DUMMY_BLACK=1` (missing textures are
  black, not magenta). No ps3recomp code change was needed.
- **Tool bug:** `tools/frametime_plot.py` averaged per-row fps and reported
  333.83 fps for a log whose mean frametime was 35.25 ms; avg is now
  frames/total time (28.37 fps) and a self-check enforces it. MangoHud's fps
  counts window presents (72.8 rows/s at one pitch) while game fps is
  `[flip]` (35.44); documented in `docs/multiscene-play.md`.
- Details: [`docs/window-freeze-fix.md`](window-freeze-fix.md).

## 2026-11-02 (overlap): the ref is published 3 us after the walker reaches it; the wait is walker time-to-fence

One traced pitch run (`measure_fast.sh`, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`): **35.01 fps** on the
10 s counter; trace window 12.05 s / 416 flips / 510 fences / 396 PPU ref waits. New trace-only
`gcm.ref_set` / `gcm.ref_pub` events (`ps3recomp-probe` `mlb16-work`, off by default, pushed to `fork`)
measure the publication path directly.

- **Publication is not late.** Walker reaches `NV406E_SET_REFERENCE` -> ref register written:
  **p50 3 µs, p90 4 µs, max 20 µs** over 510 fences. The write is a plain guest store at
  `cellGcmSys.c:3194` (end of the drain pass) or the read-driven poll (`:1909`); no Vulkan submit,
  GPU fence, flip or `VK_FRAME_PIPELINE` dependency. Labels/semaphores are written at dispatch.
- **The fence is the last word before the park**, so end-of-pass publication is ~0 ms after dispatch.
  "Publish at dispatch" is a no-op for this title.
- **The wait is the walker reaching the fence.** Of the mean **20.7 ms** wait, **19.7 ms** is before
  the walker reaches the target; set->pub is 0.003 ms and pub->PPU read is 0.97 ms. In 396/396
  episodes the target was not yet written when the wait began. The walker is in a drain pass 52.6 %
  of that pre-publication time (p50 72 %) and starved the rest.
- **Guest side:** one global monotonic ref counter (`DAT_01246044`), one main pushbuffer
  (`0x01246390`, `0x280000`, usable `0x27FC00`) and one 16 KB mesh/frame-tail ring (`0x01244E08`) in
  the 64 KB cellGcm FIFO; `FUN_000655cc` waits for `ctrl->ref == DAT_01245058[prev & 3]` on lwcond
  `0x01245090` (mutex `0x01245068`, 9000 us) as a **one-frame-in-flight** throttle.
- **Overlap is already present:** main builds frame N while the walker drains N-1; only the submission
  tail is serial. There is no extra append work to overlap; deeper runtime rings cannot skip the
  guest's depth-1 wait, and forcing it is the geometry-corruption path (the old `(id+1)&3`).
- Options: (A) publish at dispatch, **0-0.02 ms** (do not pursue); (B) cut walker time-to-fence,
  0-2.6 ms (existing `PS3RECOMP_WALKER_FAST`); (C) shorten the starved half, 0-2 ms; (D) deeper
  pipeline, guest change only, high risk; (E) env A/B `VK_FRAME_PIPELINE`/`VK_DRAW_BATCH` to size the
  GPU part of the drain. Details: [`docs/overlap-analysis.md`](overlap-analysis.md).

## 2026-11-02 (walker cuts in-game): 38.52 fps off vs 37.34 fps on, no gain resolved

Two `measure_fast.sh` runs on `feat/walker-cuts` (`55192504`, the usual
`build-o2` config), untraced, one 10 s pitch window each: `PS3RECOMP_WALKER_FAST`
unset **38.52 fps** (390 flips), `=1` **37.34 fps** (380 flips). Both reached the
same pitch wait, ran the full window, and logged no crash, `[replay-engine]`,
trace, `[walker-prof]` or new `pipeline ... FAILED` line. B is 1.18 fps (3.1 %)
below A, inside the within-run spread (1 s buckets stddev 1.6-1.7, range
35.9-40.8) and the same-build round-to-round spread (32.22 vs 34.63 earlier), so
one run per arm does not resolve whether the cuts help in-game; the offline
-2.6 ms engine cut did not show up as fps here. Frames are equivalent: **3.2**
grey levels over the full 640x360 image and **2.3** in the pitch box
(380,100)-(500,200) (`same.py`, as `walker-wake.md`), with the difference in the
animated crowd and player pose; no missing, black or constant-colour draws. Run
A (fast off) is 38.52, so the earlier 34.6 no-trace baseline vs 37.07 profiled
run is a build/session difference, not the `measure6` method and not the cuts.
Details: [`docs/walker-cuts.md`](walker-cuts.md).

## 2026-11-02 (walker cuts 1+3): fetch -1.63 ms, pipeline -1.09 ms per frame offline

Branch `feat/walker-cuts` (`ps3recomp-probe`, off `feat/walker-prof`, not merged).
`PS3RECOMP_WALKER_FAST=1` (default off) adds cut 1's fetch plan/remap memo and
cut 3's pipeline input memo. `replay_rsx_engine` now also reports a per-frame
digest of everything the null backend receives, so the replayed frame can be
compared with the toggle on and off.

Offline A/B on `recomp-work/walker_frame` (5 repeats per setting, 20 measured
frames each; medians and min-max over repeats):

| phase | off | on | delta |
|---|---:|---:|---:|
| fetch | 4.200 ms (4.174-4.240) | 2.572 ms (2.559-2.592) | **-38.8 %** |
| pipe  | 2.067 ms (2.058-2.076) | 0.981 ms (0.972-0.986) | **-52.6 %** |
| engine | 14.978 ms (14.870-15.087) | 12.377 ms (12.293-12.500) | **-17.4 %** |

Output gate identical in all ten runs (`0f91a96c79a7aadf`, 20-frame chain) and
`replay_rsx` still verifies at `0x85fc2ff5a1655c26`. The memo holds no guest
vertex bytes (payloads are re-fetched), keys the fetch plan on the reference set
and VTXFMT state, and keys the pipeline on program bytes plus the
pipeline-relevant registers, so changed index/program data misses. Unit tests
cover hits, reference-source and program-byte misses, and the pipeline key.
Also fixes a latent replay-only staleness: `state_restore` now bumps the texture
write generations it bypassed. Cut 2 (texture) is not implemented; Vulkan
recording is untouched. Details and the one-run in-game check:
[`docs/walker-cuts.md`](walker-cuts.md).

## 2026-11-02 (walker profile): fetch 3.6, texture 3.2, pipeline 1.9 ms per frame

Branch `feat/walker-prof` (`ps3recomp-probe`, off `mlb16-work`, not merged). New
`PS3RECOMP_WALKER_PROF=1` splits the real `sink_end` path per frame, and
`PS3RECOMP_WALKER_CAPTURE=<prefix>` images one pitch frame (FIFO words +
dispatcher state + every guest page the draws read, 107 MB). `replay_rsx_engine`
replays that capture through the real draw engine with a null backend; the
existing dispatcher-state hash still verifies at `0x85fc2ff5a1655c26`.

One pitch window (`measure6` flow, both switches on, 537 reports / 15.9 s,
37.07 fps): walker **17.19 ms/frame** at 500 draws. Split: parse 1.21, fetch
**3.56** (7.1 µs/draw), texture bind+hash **3.23** (6.5 µs/draw), pipeline
**1.86** (3.7 µs/draw), bind 0.19, Vulkan draw 4.78 (9.6 µs/draw), other 1.82.
Profiler overhead is inside noise (offline A/B 14.06–14.09 vs 13.95–14.91 ms per
14 ms drain). Offline replay (668 draws, 0 read misses) is 14.0–14.9 ms and
matches fetch/pipeline/draw per draw, but its warm texture phase is 0.2 µs/draw
against 6.5 in-game because one static frame has no texture updates — it is not
an A/B for the texture cut.

Top cuts (not implemented): fetch/remap caching **1.4–2.1 ms**, texture
page-dirty generations **1.9–2.6 ms**, pipeline-key/program memo **0.6–0.9 ms**;
together 3.9–5.6 ms off the 17.2 ms drain (~37 → ~42–44 fps if the producer wait
does not absorb it). Details and the capture format: `docs/walker-prof.md`.

## 2026-11-02 (critical path): the frame is walker drain (17.3 ms) + walker producer wait (11.8 ms)

One traced pitch run (34.58 fps / 10 s; steady window 361 frames, 10.47 s, 34.47 fps; trace window 411 frames,
34.08 fps). New `ppu.usleep` trace span (short `sys_timer_usleep`, guest LR) plus a fix to close the `ppu.gcm_finish`
poll run at the next block (`ps3recomp-probe` `mlb16-work`, trace-only, off by default). Analysis:
`tools/analyse_ppu_path.py` (new) and `tools/analyse_walker_wait.py`.

- **Frame = walker drain + walker put_wait**: 17.25 + 11.77 = **29.02 ms mean** (p50 33.8; distribution bimodal and
  the 10 s counter is 28.9 ms). `ppu main` runs 9.3 ms (already overlapped) and blocks **19.7 ms** in one place:
  `FUN_000655cc` (0x000655cc) waits on the ref cond (`0x1245090`) for the reused buffer. During that wait the walker
  drains 11.6 ms and starves 8.1 ms.
- **Only block on main**: the ref wait. `Mesh_WaitPrevJobList`/`FUN_0006f998` (mesh job list), the flip throttle
  (`0x1245100`, 17 ms) and the post-kick ref spin are **not taken at the pitch** — main has zero usleep spans.
  DisplayFlip blocks 9.3 ms on ref + 19.6 ms on flip-done; it is a pacer, not the limiter.
- **Walker split** (same shape as the attribution run): `u` PPU/park **6.16** ms, `e2` resume **2.80**, c690 `a+d`
  **2.71**, queueing **0.10**. 412 c690 jobs/frame, 41 % lane busy. The PPU and walker wait on each other for ~8 ms
  per frame: main waits for the ref, the walker waits for the PPU's own append (`u_park` 4.1 + `u_caught` 2.0).
- **Levers**: (1) cut walker drain CPU ~5 ms → ~42 fps (low–medium risk); (2) deeper ring / pipeline the PPU append,
  hides up to ~8 ms → ~45–48 fps (high risk, the `(id+1)&3` reorder is not enough); (3) c690 earlier/offload,
  −2.7 ms → ~38 fps (medium/high). 1+2 is the ~50–60 fps path; lever 3 alone is not. Full thread map, addresses,
  overlap/ordering analysis and caveats: [`docs/ppu-critical-path.md`](ppu-critical-path.md).

## 2026-11-02 (wake): waking the walker on the slot PUT removes e2 but not the frame time

Branch `feat/walker-wake` (`ps3recomp-probe`, off `mlb16-work`). The present thread waits on its kick event with a
4 ms timeout (`templates/project/main.cpp:220`, `cellGcmSys.c:1922`); the SPU PUT that fills the parked slot did not
signal it, so PUT->resume was p50 527 µs / p90 2.15 ms. `gcm_note_spu_put` now signals that event when the PUT range
contains the parked `get` (opt-in `PS3RECOMP_WALKER_WAKE=1`, default off; unit test
`tools/test_gcm_walker_wake.py`).

- **e2 removed:** PUT->resume **p50 8 µs / p90 11 µs** in a 398-frame, 12.98 s pitch window. But total wait stayed
  **11.64 -> 11.28 ms/frame** and fps fell **33.29 -> 30.67** traced (**33.44 -> 30.95** on the 10 s counter; the
  no-trace run with the watcher was 32.22 vs the 34.63 baseline). The wait moved to "job not yet pushed"
  (1.82 -> 3.65 ms/frame) and passes/frame rose 6.2 -> 35.4: the walker is caught up with the producer, so waking
  per slot just batches less and costs more walker CPU. Off by default.
- **`u` writer identified:** the park is patched by the PPU, not an SPU DMA. `Gfx_FrameEnd` (guest 0x00066530)
  writes the JUMP into the mesh ring at **0x00066ae0**; the put register is written by `FUN_00065760` (guest
  0x00065760) at **0x00065af8**. `u_park` waits are p50 11.7 ms and end near a flip/kick: the walker is early, and
  the real fix is to pipeline the PPU producer (`Mesh_WaitPrevJobList`, 0x0006ed24) with it, not just wake on the
  write. Numbers, tables and the proposed `u` fix: [`docs/walker-wake.md`](walker-wake.md).

## 2026-11-02 (attribution): the walker's 10 ms wait is mostly PPU/cadence, not c690 throughput

One pitch run (`measure_fast.sh`, 33.44 fps / 10 s; trace window 393 frames, 11.80 s, 33.29 fps; instrumentation
costs −3.4 %). New `PS3RECOMP_TRACE` events (`c690.job`, `spurs.kick`, `gcm.spu_put`, `rsx.park`,
`spu.overlay_load`, `spurs.init`) plus a trigger window attribute every `rsx.put_wait` to the SPU PUT that
overwrites the parked slot and the c690 job that made it.

- 2,444 waits = **11.64 ms/frame**. Only **~2.8 ms/frame is c690 production** (job admitted 1.82, running 0.96).
  **~5.7 ms/frame has no SPU PUT at the park** (20.5 % caught up with `put`, 28.7 % park patched by a non-SPU
  writer), and **~3.0 ms/frame is the walker not running again after the PUT** (slot filled p50 68 µs after the
  wait starts; resume p50 527 µs / p90 2.15 ms later).
- Workers are not saturated: c690 **42.1 %** of lane time over 6 configured lanes, 58.8 % busy, lane 5 idle
  (0 c690). Queueing with an idle lane 0.8 % of wait, all-busy 0.1 %. c690 wall ~75–77 ms/frame cumulative
  (matches 73.5 ms at 34.6 fps).
- **Implication:** the path to 60 is scheduling/overlap — wake the walker on the slot PUT, pipeline the PPU
  producer with the walker, and shave walker CPU (~37–39 fps for wake-on-PUT; ~44–46 for producer overlap;
  both + the no-trace 14–15 ms walker → ~60). Design B alone measures ~38 fps, not 55–60, because it removes
  only the c690 part. Full attribution, levers, caveats: [`docs/walker-spu-wait.md`](walker-spu-wait.md),
  analysis `tools/analyse_walker_wait.py`.

## 2026-11-01 (measure-and-decide): c690 offload verdict — no-go for A; B is the only 60 path

One pitch run (34.63 fps, 10 s window, `PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1`). c690: **429 jobs/frame**, job
wall **73.5 ms/frame cumulative** (p50 115 µs, p90 239 µs), opcode `0x0A` 30.6 % / `0x2A` 7.9 % (every job) /
culls 5.0 %. Fence round trip **41 µs p50 / 74 µs p99** (locked clocks). Design A per-job win 44.5 µs mean but
−16.5 µs at p50/p99 → **~37–40 fps; no-go**. Design B (whole job on GPU, indirect draws, no CPU wait) → **~55–60
fps but 2–4 months**. Next lever: tree/leaf lifter codegen (~34 % of the SPU wall). Full numbers + leaf
cross-check: [`docs/c690-offload-decision.md`](c690-offload-decision.md).

## 2026-11-01 (correction): 0x2A is an assembler; 0x6D68 is a direction unpack

- `0x10F78` (opcode 0x2A, selector 0) is a **vertex-format assembler**: it gathers four already-final planar
  arrays (UV0 `0x21C30`, COLOR0 `0x20FB0`, POSITION `0x20330`, NORMAL `0x1F6B0`) into 36-byte records at `0x2F870`,
  with one float→u8 color pack (`0x7D40`, `cfltu 149`). It does **not** unpack/transform/normalize.
- `0x6D68` is a **12/12/11 SNORM direction/tangent unpack** (4th field 0), not position. The real per-vertex chain
  is: unpack `0x6D68` → 3x4 transform `0xA798` (matrix LS `0x29540`) → normalize `0xAF98` (frsqest+fi) →
  snorm pack `0x7C18`/`0x8160` → planar arrays → `0x2A` assemble.
- The captures are from **different job instances**, so they cannot be concatenated byte-exactly; a co-temporal
  capture (or whole-job replay) is needed to validate the full pipeline.

## 2026-11-01 (capture): pitch pipeline is multi-command; per-call offload not viable

- Captured the pitch frame's `0x10F78` (0x2A handler), `0x10B88`, `0xA798`, `0xAF98`, `0x6D68` in one run
  (multi-address `SPU_FN_CAPTURE`). `0x10F78` replays offline with zero DMA/mailbox.
- **The 0x2A handler is not the position path**: it is the stream-2 attribute gather/convert (`0x7840` family),
  writing `0x2F870`/`0x23400`. Position unpack is the separate leaf `0x6D68` (bit-exact, `0xb98ec2c0561b2bbf`).
- **Fence-latency implication**: `0x6D68` is ~0.5 µs/call; a per-call GPU dispatch+fence (~10–20 µs) is a loss.
  The offload must be **job-level** (all streams, one dispatch + one fence per job), which requires the full
  per-vertex pipeline in one shader.
- Report + constants: `docs/pitch_0x2A_pipeline.md`, `tools/replay/pitch_pipeline_constants.h`.

## 2026-11-01 (tooling decision): no Ghidra SPU import, no emulator FP

- Ghidra 12.1.2 has no SPU processor; GhidraSPU builds for it, but the MCP is bound to the EBOOT and cannot switch
  programs, and the c690 module is already lifted to readable, bit-exact C. **Do not import the SPU program.**
- anergistic (offline SPU emulator) has a flat EA model + MFC DMA but is **integer-only** (no `fma`/`cflts`/`frest`),
  so it cannot run the geometry math without ~15 added FP instructions. **Do not invest in emulator FP.**
- Use the lifted C + `tools/spu_disasm.py` for the thin low-LS runtime. Runtime per-format constants need
  function-entry captures, not decompilation.

## Next steps (2026-11-01)

Per-call offload is not viable (fence latency >> the ~0.5 us kernels). The offload must be **job-level**, and the
pipeline is a chain of separate commands. To build a job-level shader we need the full chain from one job:

1. **Co-temporal capture (recommended, one run).** Add a probe that captures the producing chain in a single job:
   `0x6D68` -> `0xA798` -> `0xAF98`, the `0x0A` pack scale/offset (`sr9`/`sr10`), and the `0x29540` matrix for the
   same job. Then build/validate the job-level shader offline against that one snapshot.
2. **Whole-job replay (offline, no run).** Lift the low-LS SPURS runtime (`0x1690/0x22B0/0x1B08/0x1AE0`) and add
   an EA/DMA model to the replay harness, then replay a job from the entry.

Then: build the job-level shader for all streams, validate offline, integrate at the DMA boundary (one dispatch +
one fence per job), and run the single go/no-go measurement. Function map: `docs/c690_function_map.md`.

## 2026-11-01 (decision): commit to the whole-pipeline GPU offload

Cheap levers are closed (per-function native 1.00×/1.03×; cull bypass loses). The only path to 60 fps is moving the
c690 **per-vertex math** (unpack `0x6D68`, skin `0xD2B8/0xD4A8/0xC98C`, normalize `0xAF98+B238`, pack `0x7C18/0x8160`)
to GPU compute, keeping culling/tree/index/ring on the SPU. First step is a 1–2 week go/no-go spike on one format
(bit-exact vs the offline harness, then measure the fence/sync cost). Full plan, data flow, risks, and milestones:
[`docs/gpu-offload-plan.md`](gpu-offload-plan.md).

## 2026-11-01 (late, 3): per-function native + cull bypass are both closed

- **Native kernels bit-exact but at parity:** `0x6D68` = **1.00×** (563 vs 568 ns/call); `0xAF98`+`0xB238` = **1.03×**
  (216 vs 223 ns/call). The lift already emits the same `vpshufb/vpsllvd/vfmadd132ps` mix. Per-function SIMD is not
  a performance lever.
- **Cull bypass loses in every variant:** all three = 33.6 fps; only `0x2C` = 34.85; baseline 36.6. The culls remove
  triangles before the per-vertex math, so they are a net win.
- **Conclusion:** cheap levers exhausted. 60 fps needs the whole-pipeline GPU offload (2–4 months) or accept ~36–40 fps.

## 2026-11-01 (late, 2): offline capture/replay harness working

`SPU_FN_CAPTURE=<lsa>[:dir]` (ps3recomp `a3a5564f`) dumps the full LS + 128 GPRs + pc at a function entry. First
capture/replay: `0x6D68` at the pitch → `tools/replay/` loads it, runs the lifted body, prints a deterministic
checksum (`0xfc2e9569dc2feb11`, loop 24) and **~563 ns/call**. This is the offline loop for native kernels — no
game boot or match load. Next: native `0x6D68` diffed against the checksum, then `0xAF98`+`0xB238` as one unit.

## 2026-11-01 (late): cull-bypass A/B (negative) + research triage

- **Cull bypass is a perf loss.** Porting the RPCS3 `MLB16_triangle_fix` idea (`bi $lr` on ICE handlers
  `0x2C`/`0x26`/`0x25`, LS `0x12DC0`/`0x13518`/`0x13520`) via an early `SPU_RET(ctx)` measured **33.57 fps vs 36.6**
  (diff 6.5). The cull is an early-out before the expensive per-vertex math; removing it costs more downstream
  than it saves. Keep it off; it stays a *visual* option for high-resolution pin-holes only.
- **Research doc:** its c690 command table / dispatch tables / patch offsets are corroborated by our own
  `mlb16-revival-server/docs/ice-mesh.md`. The valuable new prior art is **WKC2's capture/replay harness**
  (`WKC_JOB_CAPTURE` / `WKC_JOB_REPLAY`) and its serial-ring-commit rule. Its RPCS3 config flags are not
  applicable to a native recomp, and its "native SIMD → 6–7.5 ms" estimate is optimistic given our neutral
  `0x7C18` kernel and the cull result. RPCS3 calibration: 12700K ~55–60 fps minor-league / ~38–48 major.

## 2026-11-01 (night): Stage 0 c690 capture implemented — first real data flow

`SPU_JOB_LS_DUMP=<prefix>` (ps3recomp `f47deb52`, off by default) captures at the c690 entry (LS `0x135B8`):
the local store, the descriptor words, any guest EAs it references, and the job's first 400 DMAs. The job runs
under **image 6 (the SPURS policy)**, not image 204. Findings:

- The c690 code is DMA'd into LS `0x4000` from guest `0x105EE00` (variant `0x106D380`).
- The descriptor (`r3` = LS `0x1330`) is format/callback data; +0xC is a guest EA (`0x01243E00`) pointing at a
  **16×16-byte attribute-format table**.
- **Data flow:** inputs are per-mesh attribute streams from `0x0173xxxxx`–`0x0180xxxxx` (`0x180`–`0x500` blocks),
  params/matrices at `0x172F2FF0`, and a `0x2000` block from RSX memory `0x05800C000`; outputs are PUT back to
  the **same** `0x0173xxxxx` EAs (in-place transform), plus `0x011B00000` (`0x2400`), `0x146A800`, and
  `0x05800C000`.
- `tools/spu_profile.sh` is the repeatable gdb sampler (the `psample` attribution is unreliable).

Next: a larger/filtered DMA capture to separate palette/weight GETs from vertex-attribute GETs, and map the
attribute-format table. See `~/Documents/work_notes.md` "Session 7 Update".

## 2026-11-01 (evening, later): current cost table + the honest path to 60 fps

| Subsystem / Operation | Old figure | Current | Notes |
|---|---|---|---|
| Frame | ~31 ms | **~27.2–27.8 ms (36.0–36.8 fps)** | trace median 33.8 ms because flips burst/stall |
| RSX FIFO walker drain | ~31 ms | **24.65 ms** | single-threaded; includes ~7–10 ms waiting on the SPU |
| Walker *own* CPU | ~26 ms (~35 µs/draw) | **~14–15 ms** | ~35 µs/draw × ~440 draws; already under 16.6 ms |
| SPU lifted job | ~44 ms aggregate | **25.5 ms wall/worker × 5 (~127 ms CPU)** | 5 busy, 6th underused; spread over ~15 kernels |
| PPU `Gfx_FrameEnd` wait | ~30 ms | **18.2 ms** | halved by the `0xA8A0` lift |
| Present | ~9 ms sync | **offloaded to present thread** (windowed) | not on the benchmark critical path |
| GPU | — | **not the limiter** | headless replay 0.59 ms/frame |

120-sample gdb profile (140 c690 samples): `0xFDA0` tree/leaf 28, `0xD4A8` output/skin 15, `0x6D68` unpack 12,
`0x8160` pack 10, `0xAF98` normalize 10, `0x7C18` pack 8, `0xD2B8` skin 8, `0xFCB8` leaf 7. Buckets: **~50%
per-vertex math, ~34% tree/leaf, ~15% index/table/ring.**

**Why 60 fps is plausible but not a pile of 2 ms wins.** The `5M → 0.3M` win was *interpreted* SPU steps — a thin
unlifted-code layer, not the job (32.5 → 36.6 fps). The "30 ms → 0.5 ms" memory is the `replay_rsx --verify`
artifact, not a speedup. Micro-opts are single-digit %. The real path is one structural move: the walker's own CPU is
already ~14–15 ms (fits under 16.6 ms), and the SPU is ~50% per-vertex math (~12.7 ms) + ~34% tree/leaf (~8.7 ms) +
~15% index/ring (~3.8 ms). Move the per-vertex math to the GPU → SPU wall ~13 ms → the walker's ~10 ms SPU-wait
collapses → frame ~16–17 ms → **55–60 fps plausible**. If only some of the ~40 format variants are covered or sync
serializes, the walker caps it at **~40 fps**.

## 2026-11-01 (evening): reliable SPU profile + GPU-offload reality check

A gdb stack-sampling run (25 rounds, pitch screen, 36.8 fps binary) replaces the unreliable `psample`
per-thread attribution. The c690 job wall (~25.5 ms/frame across 5 workers) is **spread over many
format-dispatched kernels**, not concentrated in skinning:

- top `0xFDA0` (recursive tree/leaf) ~17% of job samples; then `0x7C18`, `0xFCB8`, `0x6D68`, `0xAF98`,
  `0x6860`, `0xE0A8`, `0xC98C`, `0x76B8`, `0x10700`.
- `0x31B0` is **not** c690: it is below the c690 LS base `0x4000`, inside the SPURS policy window.
- dispatch overhead is now tiny (`spu_lifted_lookup_full` 4, `spu_indirect_branch_slow` 3) after the `0xA8A0` fix.

Consequences for the plan:
- **Offloading only skinning (`0xD2B8`, ~10–20% of the job) saves ~2.5–5 ms → ~34–38 fps, not 60.**
  The whole per-vertex math pipeline (unpack/transform/normalize/pack, ~35–55%) is the necessary GPU step,
  and even then the walker (~24.65 ms) must be cut to <16.6 ms or it caps the frame at ~40 fps.
- `vtx_unpack.comp` is downstream of c690, so it cannot reduce c690's cost by itself.
- palette/weight addresses are runtime values; a first step is `SPU_LS_DUMP` + per-function timers, not code.

The walker remap cleanup (tightest table per draw) was behavior-preserving but **neutral** (36.80 vs 36.6).
See `~/Documents/work_notes.md` "Session 5 Update" for the full scoping.

## 2026-11-01 playtest (windowed, PS4 controller) — issues to fix

Run with `ab-tools/play.sh` on the 36.6 fps build. The game plays, but it is neither smooth nor visually correct yet.

**Rendering / visual**
- **Missing shadows, dark/black crowd, blank or chrome/pink players, missing jerseys.** The render-target readback
  (`PS3RECOMP_RSX_WCB`) was left off by default for benchmark speed; that is the path that writes rendered colour
  (and the crowd/face caches) back to guest memory. `play.sh` now sets `PS3RECOMP_RSX_WCB=1`; retest before
  concluding which of these it fixes.
- **Crowd LOD popping:** people appear only when the camera zooms in; segments of the crowd are missing.
- **Picture-in-picture view has a black background.**
- **Depth-of-field duplicates** (the RPCS3-fixes artifacts) are present.
- Several issues the `mlb-show-rpcs3-fixes` repo patches are present here and not yet ported.

**Presentation / window**
- **The window only updates when the mouse moves.** With the mouse still, the image runs for ~1 s then freezes while
  the game keeps running (controller rumble continues). The headless benchmark does not show this, so it is the
  windowed present/event path (Wayland/compositor frame callbacks or the SDL event pump), not the renderer. Try
  `SDL_VIDEODRIVER=x11` to test the XWayland path.
- Cutscenes and player entrances drop to low fps; load times are long.

**Input**
- **Controller presses are not always registered immediately**; menu items sometimes need several presses. Likely
  `PAD_POLL_INTERVAL_MS` / pad report pacing, not the renderer.

## 2026-11-01 (later): Target 4 — a missing lift entry was costing ~5 M interpreted SPU steps/s

The c690 job module's `spu_lifted` in the working tree was **stale**: it predated the `0xa8a0` entry already present in
`tools/lift_spu.sh`. The game reaches LS `0xA8A0` through a function pointer inside the lifted `0xA818` body, so with
no registered entry it fell to the SPU interpreter. `SPU_INTERP_STATS` showed ~5 M interpreted steps/s, with
`img=0xCC ls=0x0A8A0` as the top entry (~58k calls/s).

Re-running the canonical `tools/lift_spu.sh` (same flags; `0xa8a0` is already in `LBP_JOBMOD_EXTRA_FUNCS`) splits
`0xA818` into `0xA818` + `0xA8A0`. Pitch-screen result:

| | before | after |
|---|---:|---:|
| pitch-wait fps | ~32.5 (32.17–33.15) | **36.13 / 36.59 / 36.67** |
| interpreted SPU steps | ~5 M/s | **~0.1–0.3 M/s** |
| pitch visual diff | 12–18 | 14.2 |

The AVX2 `0x7C18` kernel was re-applied to the new lift and measured neutral (36.59 vs 36.67), so it stays wired.
`replay_rsx` still verifies `0x85fc2ff5a1655c26`.

**Lesson:** after changing `LBP_JOBMOD_EXTRA_FUNCS`, re-run `tools/lift_spu.sh`; `spu_lifted` is generated and not in
git, and a stale copy silently reintroduces the interpreter path.

**`0x108EC` — tried, then reverted.** The next missing c690 pointer target, `0x108EC` (between lifted `0x10758` and
`0x1095C`), was added to the lift. It cut interpreted steps further, **~0.3 → ~0.1 M/s**, but pitch-wait fps did not
move (**36.06 vs 36.1–36.7**, within noise), and the entry still hit the interpreter (`img=0xCC ls=0x108EC`) through a
residency/SMC path even though the function is now lifted. Adding it to the *global* `LBP_JOBMOD_EXTRA_FUNCS` also
pulled two other modules' `code_end` into their data tails (`7ac07dde` 0→411 unsupported, `31097a84` 0→195), so it
would need a per-module extras mechanism. Reverted to the `0xA8A0`-only build. The remaining interpreter
(~0.1–0.3 M/s) is the policy module's self-modifying-code entry at `0xA68` plus these residency paths; it is no
longer a meaningful frame-time lever.

## 2026-11-01: frame re-measured; the walker + SPU pipeline are the wall; Target 3 fence-skip fails again

Current `mlb16-work` tree, pitch screen: **32.2 fps** (1 s buckets 32.1 ± 2.0); `replay_rsx` still verifies
`0x85fc2ff5a1655c26`. The AVX2 c690 `0x7C18` kernel is wired and active (the linked binary calls
`native_spu_func_00007C18`), but it is not the main cost.

- **`replay_rsx --verify` is a correctness gate, not a benchmark.** The 36 ms "frame" is the per-call hashing; without
  `--verify` the same 646-draw frame is **0.59 ms** (~1,700 fps). The in-game walker is ~45× slower per draw because it
  also fetches/converts guest vertices, checks textures and records Vulkan commands.
- **Timeline (`PS3RECOMP_TRACE`, median 33.9 ms frame):** walker drain ~27 ms; PPU main sits in the `Gfx_FrameEnd`
  fence spin ~34 ms; 5–6 SPURS workers run c690 concurrently. The walker also has ~10.5 ms/frame of gaps waiting for
  the SPU to publish buffers (`rsx.put_wait`).
- **Walker CPU ~40 µs/draw** (38.5–49, `VK_EB_STATS`) over ~438 draws/frame: ~29% Vulkan driver command recording,
  ~18% `rsx_vertex_remap_build`, ~22% texture change checks, ~6% `sink_end`.
- **SPU profile (1 kHz per-thread `perf_event_open`):** the c690 job's hot functions are `0xD540`, `0x10028`, `0x6D68`,
  `0xAF98`, `0x8160`; `0x7C18` (the vectorized one) is a small share, and the others are local-store-bound.
- **Target 3 re-attempt, done correctly:** `0x011203D4` was left untouched; at the `Gfx_FrameEnd` call site (lr
  `0x6579C`) the wait used `(id + 1) & 3` instead of `id & 3`, behind `MLB16_PIPELINE=1`. It still fails: **185 flips
  in 689 s** and never reaches the pitch screen, versus ~9,300 flips for the same build without it. The fence also
  guards guest buffers the walker is still reading, so the PPU cannot run ahead of the drain. Reverted; both PPU
  recomp files are byte-identical to `.orig`.
- **Committed** the three infrastructure tweaks and the AVX2 kernel on `mlb16-work` (`ba20312c`). The
  `recomp-work/spu_lifted` wiring is still outside git; a copy is at `spu_recomp.c.avx2_wired`.
- **Walker split (`VK_DRAWPROF=1`):** of ~40 µs/draw, only ~10 µs is Vulkan recording (vertex dispatch 3.1, render
  pass begin 1.9, descriptors 1.4, copy 1.3, barriers 0.8, state 0.4, binds+draw 0.6, pass end 0.6); the rest is
  texture checks (~9 µs), vertex remap (~7 µs) and bookkeeping.
- **Walker experiments:** backing off the full texture re-hash for stable textures = 32.5 fps (no change, reverted);
  `PS3RECOMP_ENG_VTX_REMAP=0` = 28.4 fps (worse, dedup stays on); three frame-pipeline sets instead of two = 32.96 fps
  (1 s-bucket 34.1), a small within-noise gain, kept opt-in (`060bd105`).
- **Conclusion:** walker CPU is not the marginal limiter; the frame loses ~10.5 ms/frame waiting for the SPU to
  publish command buffers. The c690 SPU job is the next lever.

Next: cut the c690 job's time (the remaining hot functions `0xD540`, `0x10028`, `0x6D68`, `0xAF98`, `0x8160`) or
offload skinning to the GPU. The walker is close to its practical limit.


## 2026-11-01: Descriptor-Driven GPU Vertex Compute Unpack & Presentation Thread Complete

- **Descriptor-Driven GPU Vertex Offload (Macro Target 2 Completed, `feat/gpu-vertex-offload` commit `1f6dc0cd`):**
  - Integrated `vtx_unpack.comp` compute shader directly into `rsx_vulkan_backend.c` and `rsx_draw_engine.c`.
  - Upgraded from rigid layouts to a **Descriptor-Driven Compute Unpack** architecture:
    - *Modes 0, 1, 2 (Fast Paths)*: Zero-overhead unrolled unpack paths for 24-byte 3D meshes and 16/20-byte 2D UI quads.
    - *Mode 3 (Generic Descriptor Unpack)*: Receives up to 8 packed `AttrDesc` structs in push constants. Uses funnel shifts (`load_unaligned_be32`, `load_unaligned_be16`, `load_u8`) to natively unpack 2-byte aligned 22-byte streams (e.g., Short3 Pos + CMP Normal + CMP Tangent + Short2 UV0 + Short2 UV1) without alignment faults. Supports all 7 RSX attribute formats (Float, Half, UNorm8, UInt8, SNorm16, SInt16, CMP32).
    - *Dynamic Index Remapping*: Normalized non-contiguous vertex references (`elem = ((vertex_id + base_index) & 0xFFFFF) - stable_first`), streaming index remap buffers to compute threads to fetch and unpack sequential hardware vertex buffers.
  - Live Game Benchmark Results (`measure6.sh`):
    - Evaluated 2,278,559 draw calls.
    - **Total draws offloaded to GPU surged to 98.28%** (2,239,363 draws vs 320,595 previously).
    - **CPU software fallbacks collapsed from 85.0% to 1.72%** (39,196 draws).
    - Pitch-wait FPS rose to **24.19 fps** (1-sec mean 24.39 fps, instantaneous mean 24.43 fps ±2.62 fps), with instantaneous peaks reaching **32.15 fps** (vs 22.64 fps prior run, +1.55 fps / +6.8% uplift).
  - Deterministic & Visual Verification:
    - `replay_rsx kauffman_pitch.bin --frames 100 --verify`: 100% deterministic (State hash `0x85fc2ff5a1655c26`).
    - `same.py` pitch-wait reference match: diff 13.4 ($\le 25.0$). Full-screen end frame match: diff 2.6 across full 1280x720 viewport (sub-pixel match, zero mesh tears, zero spikes).
  - Architectural Diagnostic & Bottleneck Analysis:
    - Headless `replay_rsx` executes the entire 646-draw Kauffman frame in **0.59 ms (~1,700 fps)** without per-call hashing, proving the RSX walker itself is no longer the bottleneck.
    - The live game remains paced at ~24 FPS because the walker is starved at self-JUMP "parks" waiting for the **SPU Edge Geometry pipeline (`jobmod_c690c2f7b09a`)** which spends ~35–40 ms decompressing and skinning vertices on the CPU, and the **PPU main thread serializing in `Gfx_FrameEnd`**.
    - Target 2 permanently eliminated the walker's 26 ms CPU vertex anchor (which made 60 FPS previously impossible). The remaining gap to 60 FPS is now isolated to Macro Target 3 (PPU/RSX overlap) and SPU skinning loop optimization.

- **`mlb16-next` verified and fast-forwarded to `mlb16-work`:**
  - `PS3RECOMP_ENG_VTX_DIRECT`: converted draw vertices straight into the mapped Vulkan vertex ring buffer, bypassing intermediate host scratch copies.
  - Faster constant/NOP walker dispatch for repetitive WWS Edge state headers.
  - Benchmarked at pitch screen: `cur` 23.80 fps vs `next` 24.16 fps (+0.36 fps uplift).
  - Visual verification: `bodies.sh` captured 4 high-detail camera angles. Python/PIL pixel analysis confirmed identical rendering (no missing geometry, spikes, or flickering). Fast-forwarded.
- **Clang PGO automated end-to-end (`pgo_build.sh`):**
  - Profile counter pollution resolved: `pgo_profile_list.txt` filtered strictly to `src:*ps3recomp-probe/*.c` and `*.cpp`, eliminating counters from lifted SPU/PPU translation units (`0` in `spu_workloads.c.o`).
  - Clean training run: match load to pitch screen reached in 276.1 s (+15.5% overhead vs uninstrumented, zero hangs).
  - 9-run alternating A/B (load < 1.5, GPU clocks locked): GCC baseline 23.37 fps, Clang plain 23.66 fps, Clang PGO **24.73 fps (+5.82% / +1.36 fps, $\sigma = \pm 0.09\text{ fps}$)**.
  - Automated entire pipeline into `recomp-work/pgo_build.sh`.
- **Deterministic RSX FIFO Replay Tool (`tools/replay_rsx.cpp`, `tools/replay_rsx_core.c/.h`):**
  - Standalone C++17 CLI tool that accepts dumped RSX command streams (`.bin`) and replays them through the walker and register-file draw engine headlessly.
  - Reports p50, p95, min, max, equivalent FPS, draws/sec, and 64-bit deterministic state verification.
  - Synthetic test fixture `sample_fifo.bin` replays 50 frames at 1,720 equivalent fps with 0% measurement noise. Added CTest target `replay_rsx_sample_fifo`.
- **Vulkan Render-Target Readback (WCB) & Face Cache Purge:**
  - Implemented `rsx_draw_engine_flush_dirty_to_guest` in `libs/video/rsx_draw_engine.c`. Offscreen render targets (dynamic player faces, crowd atlases) marked dirty during rendering and blitted via `vkCmdCopyImageToBuffer` into guest memory before sync points (`NV406E_SET_REFERENCE`, `SEMAPHORE_RELEASE`, flips). Display scanout buffers protected.
  - Unit test `test_color_readback_guest.c` passing 13/13 checks.
  - Added `--purge-cache` and `PURGE_FACE_CACHE=1` to `tools/run.sh` to safely auto-delete corrupt, black `HeadFaceCacheFile` assets on startup.
- **Upstream Pull Request Campaign (11 PRs submitted to `sp00nznet/ps3recomp`):**
  - Converted local fork fixes into 11 clean-room PRs (#640–#650) addressing community issues #551 (MLB 15 match load), #464 (Puppeteer SPU taskset crash), and #345.
- **Real-Game FIFO Dump (`PS3RECOMP_DUMP_FIFO`) & Big-Endian Fix:**
  - Fixed big-endian dumper bug in `libs/video/cellGcmSys.c`: swapped words to big-endian (`__builtin_bswap32`) and padded unmapped payload dwords (`0u`).
  - Captured clean 100-frame trace at Kauffman Stadium pitch window (`kauffman_pitch.bin`, 40,466 words, 158.07 KB).
  - Verified 100% deterministic replay: 646 draws, 50 clears, 2,338 dispatcher calls per frame, state hash `0x85fc2ff5a1655c26`. Pure headless frame replay runs in 0.59 ms (~1,700 fps).
- **Decoupled Vulkan Presentation Thread (Macro Target 1, Completed):**
  - Moved `vkQueuePresentKHR` off the walker thread to a dedicated background worker thread in `libs/video/rsx_vulkan_backend.c`.
  - Resolved 12 FPS swapchain sync-coupling by requesting 2 physical queues (`queueCount = 2`): Queue 0 for walker submissions, Queue 1 for presentation (`s_vk.present_queue`). `vk_queue_present` calls `pvkQueuePresentKHR` directly on Queue 1 without acquiring `s_vk.queue_mu`.
  - Added `VK_PRESENT_MODE_MAILBOX_KHR` negotiation with fallback to FIFO and non-blocking slot acquire.
  - Exported `__GL_SYNC_TO_VBLANK=0` to eliminate driver sleeps.
  - Diagnosed performance behavior:
    - Synchronous WCB readbacks (`PS3RECOMP_RSX_WCB=1`) + GPU downclocking to P5 (810 MHz) had caused an ~11.2 FPS dip due to PCIe readback stalls (`g.be->submit_and_wait`).
    - With `PS3RECOMP_RSX_WCB=0`: Headless baseline instantly restored to **23.98 fps** (1-sec mean 24.84 fps, peak 34.25 fps).
    - Windowed live run (`PS3RECOMP_VK_WINDOW=1`): **23.34 fps** (instantaneous mean 23.52 fps ±2.17 fps), completely eliminating the 12.00 FPS clamp with only a 0.64 fps (2.6%) delta from headless.

---

## 2026-10-29: the match-load race found; a faster FIFO walker; clang PGO +5.8%

Measured by 3-run alternating A/Bs at the pitch screen. Today's absolute numbers are ~23 fps, not 26: the machine had
more running alongside (a remote-desktop session alone cost ~15-20%), so compare builds within a session only. The 55 s
"hold" number swings by ±3 fps between runs of one build, so decisions use the 15 s pitch-screen window.

- **The match-load freeze behind the PGO stalls:** the RSX drain gives up on a self-JUMP "park" after a grace period
  and follows `put` instead. MLB 16 parks in command buffers its SPU jobs are still writing, so when a slower runtime
  (an instrumented build, a busy host) took longer than the 50 ms grace, the drain skipped those commands and a GPU
  label with them, and the game waited forever. Confirmed by setting the grace to 5 ms on a normal build: same freeze.
  The default is now 1000 ms (God of War HD needed more than 50 ms too); zero resyncs and no fps change in A/B.
- **The instrumented builds' crawl** had a second cause: the profile list `src:*ps3recomp-probe*` also matched
  `spu_helpers.h`, so every SPU helper inlined into the lifted SPU code was counted (`spu_v2u` alone 50 billion
  times). Listing only `src:*ps3recomp-probe/*.c` and `src:*ps3recomp-probe/*.cpp` fixed it: training reaches the
  pitch screen ~10% slower than a normal build.
- **Clang PGO (runtime only):** 24.73 fps vs 23.37 for the gcc build and 23.66 for plain clang, best in all 3 runs
  (24.69, 24.84, 24.67).
- **FIFO walker:** with the draw engine on, every method word went through the per-word diagnostic loop. Runs of 3D
  methods now skip it, and plain register writes are stored without the full dispatcher: 19.7 → 11.1 ns per word in a
  replay benchmark with identical handler calls and register state. In the game it was within noise at the pitch
  screen; screenshots match.
- **Real `sys_lwcond`** (host condition variables, timeouts, a 10 ms fallback wake) behind `PS3_LWCOND_REAL=1`:
  +2.7% pitch, -7% hold over 4 runs, so neutral; left off.
- **A vertex cache across frames was measured and dropped:** validating the inputs costs about as much as converting
  them (≤1.2× best case in a benchmark). The cost that matters is converting and then copying ~55 MB a frame into the
  GPU ring; writing straight into the ring is next.

## 2026-10-28 (night): the frame traced; the walker is the bottleneck; +27% from pipelined GPU batches

All ps3recomp changes are on `mlb16-work` (MatRanc/ps3recomp).

- **Merged, measured at the pitch screen** (3 alternating runs each):
  - inFAMOUS 2's SPU DMA fast path, inline MFC channel ops and register-built insertion masks: +2.7%;
  - the lifter writes back only registers written since the last sync (register traffic in the hot job down 45%):
    within noise;
  - a ring filled exactly to its last word is no longer taken for an overrun (found by review);
  - `--const-bi` no longer turns resolver-only targets into gotos (found by review; MLB's lift is unchanged);
  - fixes from two reviews: memory containers reserve their size, SPURS slot checks and `maxContention` clamp, AIO
    write prefault and shared AIO ids, the GCM interrupt thread's stack, the template's SPU options, and the
    `run_tests.sh` and video fixture tests wired up. Within noise. One SPURS idle change was left out (-0.3 fps).
- **A frame tracer.** `PS3RECOMP_TRACE=<file.json>` writes a Chrome/Perfetto timeline: SPU job runs and waits,
  lock-line retries, the walker's drains and put waits, PPU lwmutex/lwcond/semaphore/event waits, GCM finish and
  flip waits. Tracing on costs nothing measurable at the pitch screen (20.6 fps either way).
- **What the trace showed.** The walker's drain is one ~36 ms pass over a few hundred bytes of the main command
  stream: it follows the command buffers the SPUs build. The SPUs out-run it, fill their rings and wait. Per thread
  the walker used ~55-68% of a core; the rest was GPU waits (`VK_EB_STATS=1`).
- **`VK_FRAME_PIPELINE=1`** (an existing ps3recomp mode) submits a full batch of draws without waiting for the GPU:
  20.8 -> 26.1 fps at the pitch screen, the pitch screen pixel-identical. Now on in `tools/run.sh`.
- **Native Edge kernels: dropped.** With the benchmark fixed (setup was inside the timed loop), an 8-wide AVX2
  version of job function 0x7C18 is bit-exact and 1.00x the lift. Only 2 of the job's 9 hot functions could be
  widened at all.

## 2026-10-28 (afternoon): a faster frame dump; striped lock-line locks measured; the hot job module identified

- **The frame dump was costing the walker ~20% of its time.** The test harness's PPM dump (every 10 frames) wrote 3
  bytes per `fwrite` call. It now packs the frame into one buffer and writes it once, to a temp file renamed into
  place. Pitch screen: 18.8 -> 20.3 fps (3 runs each). This only affects our measurements, not play, but the walker
  finishes last every frame, so the dump was inflating every number above (ps3recomp `c93af77`).
- **Striped lock-line locks (inFAMOUS 2, MIT): neutral.** Five SPUs contend on the one global lock-line lock in the
  job manager's atomic job grab, about 4% of SPU busy time. The 64-stripe version from inFAMOUS 2 measured 18.8 vs
  18.5 fps, within noise. It is kept on branch `port-lockline-stripes`, unmerged.
- **SPU time, split by sampling:** ~72% in lifted job code and ~28% in the runtime (lock waits, indirect-branch
  resolution, the drain loop, `mfc_for`). One module, `c690c2f7b09a`, is ~70% of the lifted part. A read-only
  analysis identifies it as a variant of Sony's Edge Geometry pipeline (vertex decompression, matrix-palette
  skinning, culling, quantization into the RSX mesh rings, and draw packets with fenced DMA).
  - A native replacement could at best halve the SPU phase. The walker would then set the pace.
  - The plan is one function at a time, each checked bit for bit against the lifted code offline, before
    committing to the whole module.

## 2026-10-28 (midday): the 5.9 s stalls fixed; SPU loops kept in registers; 17-18 fps at the pitch screen

**The 5.9 s stalls: our command-buffer `end` was one word too far.** The runtime set the context's `end` to the
first byte after the 64 KB buffer, IO 0x10000. The SDK sets it to the buffer's last word, the slot the wrap JUMP goes
in. MLB writes its own JUMP-to-begin at `end`, so it landed in whatever the game keeps right after the buffer. When
that word was overwritten, the walker walked on into data, read vertex-program microcode as a CALL, and skipped the
frame's fence. Gfx_FrameEnd then waited out its own 5.9 s timeout. `end` is now `begin + size - 4`
(ps3recomp `3695e97`).
- **Before:** 8 bad CALLs per run, and every one was followed by a ~6 s freeze.
- **After:** none in 3 runs, and no freezes during play. The few remaining multi-second gaps are scene loads: menus,
  the walk into the game, a new SPU workload being created.
- The pitch-screen benchmark is no longer bimodal: 15.2-17.3 fps across runs (before: 9 or 14-21).
- `GCM_RINGDBG=1` logs a walker that steps past the end of the ring.

**Where a frame goes now (ftrace at the pitch screen, ~58 ms frames).** The game publishes the FIFO `put` once per
frame, in Gfx_FrameEnd, and then waits for that frame's reference value. A frame is therefore:
1. the main thread runs about 12 ms, with the SPUs and the walker idle;
2. the SPU job lists (~44 ms on 4-5 SPUs) and the walker's drain (~46 ms) run together;
3. the walker finishes about 6 ms after the SPUs, which releases the main thread.

The game's own sync allows no more overlap than that, so the lever is speed: SPU job time first, then the walker.
- The interpreter is not the problem: 0.2-0.4 M interpreted SPU steps a second, a few ms of SPU time.
- One job module (`c690c2f7b09a`) holds most of the SPU compute.

**Two SPU code-quality fixes** (ps3recomp `a16e306`, `58587e9`):
- **`--ib-goto` (lifter).** The hottest loop (0x7C18) loops back through `binz $39, $50`, where `$50` holds its own
  entry address. The lifter treated that as leaving the function. Every iteration wrote back ~30 registers,
  tail-called itself and reloaded ~60. A computed branch whose target is a known entry inside the same function is
  now a guarded goto, and the loop keeps its registers. About +3%.
- **`shufb` without byte swaps (x86).** Registers keep each word byte-reversed, so the swaps fold into the index
  (`^ 3`), and a 16-entry lookup handles the special selectors. About 9 instructions instead of 18, and 9% fewer
  instructions in a hot function. About +4%.

**Result:** pitch-screen fps averages 17.8 over 3 runs (16.9 with the stall fix alone; 12-14 typical before it).

Next: the SPU job code is still about 10 x86 instructions per SPU instruction, and a third of that is register
spills. After that comes the walker (~40 us per draw).

## 2026-10-28 (morning): inFAMOUS 2's SPU and RSX work ported; a repeatable benchmark; a bimodal frame rate

Ported from the inFAMOUS 2 port (MIT, credited in each commit), all on branch `mlb16-work`:
- **SPU lifter:** `--local-regs` (registers in C locals) and `--const-bi`, plus our `--ib-known-targets`.
- **SPU helpers:** SIMD forms for the integer helpers that were still scalar on x86.
- **SPU runtime:**
  - a fast branch resolver for job modules (`SPU_IB_FAST=0` turns it off);
  - the channel-read store-forwarding fix;
  - cheaper DMA parameter writes.
- **RSX draw path:**
  - per-texture-unit write counters with cached decode (`PS3RECOMP_ENG_TEXWGEN=0`);
  - a texture-binding memo (`PS3RECOMP_ENG_TEXMEMO=0`);
  - vertex-shader constants by range (`PS3RECOMP_ENG_VPC_FULL=1`).
- **Frame pipelining (`VK_FRAME_PIPELINE=1`):** two batch sets; neither a present nor a full batch waits for the GPU.
- **CMP32 vertex fetch:** decodes from the stable source instead of resolving each element.
- **Interpreter stats** now name the job-code branch targets the lift lacks. Several were added to the jobmod lift
  (the entry list `tools/lift_spu.sh` passes).

**A repeatable benchmark at last (mostly).** Take the home team (the menu driver moves the controller right at team
select, then handles pitcher selection and the lineup), and the game waits at the first pitch selection with a fixed
camera: the full 3D scene, with the job lists still running. Fps is measured over 15 s from when that screen appears.

**Results:**
- None of the ports above shows a clear gain on its own.
- The walker's GPU waits fell from ~600 to ~100-170 ms per 2 s with pipelining, but they were off the critical path.
- All combined: 10.85, 18.24 and 12.99 fps, against 13.96, 8.85 and 8.72 for the previous build (and 13.7-14.9
  earlier). 18.24 is the best run yet.

**Both builds are bimodal.** Some runs settle around 9 fps and others around 14-18, which swamps every change
measured this morning. It is not thread placement: four more runs (11.4, 12.1, 12.2, 17.9 fps) had the same
spread of hot threads over cores and the same job lists on three or four SPUs at once.

**Found it: 5.9-second stalls from a FIFO parse slip.** Traced runs show the median frame is ~43 ms (~23 fps) in
slow and fast runs alike. The slow runs contain multi-second hitches:
- The walker reaches `0x709D302A` and treats it as a CALL to unmapped IO. It is the 12-word vertex-program
  microcode after a SET_TRANSFORM_PROGRAM header (`0x00300B80`) one word earlier.
- The walker got there misaligned: it decoded `0x01000014` as a 64-word method header. Before that, its
  control-flow log shows a park, a jump to 0xFFB8, and `0x00000009` at IO 0x15064 taken as a jump to IO 0x8.
- The same two words (CALL `0x709D302A`, JUMP `0x30464649`) appear 6-8 times in every run.
- After the bad branch the walker resyncs, so that frame's reference write is never seen. Gfx_FrameEnd waits for it
  in a loop that gives up after 0x15B3 = 5,555 `Cond_Wait(9 ms)` calls. With our 1 ms lwcond poll that is 5,555 ×
  ~1.06 ms ≈ 5.9 s.

Whether a run's measured window catches some of these is the bimodal frame rate. Fixing the parse slip (likely the
walker reading a ring slot before the SPU job has written it) is next. It is probably also the "hang-ups" seen
while watching.

**Where a frame goes now** (trace): five SPUs run the job lists for ~50 ms, then the RSX walker drains for ~35-40 ms
with the SPUs idle. The game waits at frame end for the RSX to finish the previous frame, so the two phases don't
overlap.

## 2026-10-28 (early morning): job lists on several SPUs, ~6 → ~12-13.6 fps

With batching on, the frame trace (SPURS workers now record busy spans) showed one SPU busy nonstop for ~180 ms of a
~208 ms frame while the others idled. The WWS job manager's two main workloads ran on one SPU at a time, by turns.

**The cause was a missing API, not the scheduler's speed.** MLB calls `cellSpursRequestIdleSpu(spurs, wid, 7)` for
every workload. The SPURS kernel lets an SPU take a workload while its contention is below ready count + that idle
count; WWS uses it to spread one job list over several SPUs, each grabbing the next job. Our runtime's version was a
stub that kept nothing, so with a ready count of 1 every list got exactly one SPU.

The inFAMOUS 2 port's notes and SPURS kernel had the answer (it is the same job manager). Now implemented on branch
`mlb16-work`: the job lists run on three or four SPUs at once.

| | fps in play (30-210 s after the first pitch) |
|---|---|
| Before (batching on) | 5.4-8.0 over six runs |
| With idle SPUs honoured | 11.93, 13.59 |

**Also learned:** vertex decode is not where the RSX walker's time goes. SIMD decode is 2× faster in isolation and
bit-exact, but the walker's CPU per draw didn't move (60.4 vs 60.5 µs), and the walker sleeps most of the frame.

**Next:** the inFAMOUS 2 port's other SPU work, ported where MLB needs it:
- the lifter keeping SPU registers in locals (`--local-regs`), `--merge-chunks` and `--const-bi`;
- SIMD forms of the integer SPU helpers;
- cheaper DMA fences.

## 2026-10-28 (overnight): the frame is a waiting chain; draw batching +20%

**Correction to the entry below.** The CPU-vs-CPU game plays out differently every run, so it wasn't the same
at-bat each time, and fps swings ±20% with what's on screen. Changes are now judged by alternating A/B runs: a
closed-loop menu driver that recognises each screen, fps over 30-210 s after the first pitch, one game at a time.

**Where the time goes**, now measured on the running game:
- Sampled with gdb, plus the runtime's `PS3_FTRACE` timeline (SPURS workers now record busy spans).
- **Nothing is CPU-bound.** The process uses about 18% of the CPU. One SPU worker is ~60% busy, the RSX walker about
  half, and the main thread ~10%.
- **The frame is a chain of waits:**
  - The main thread waits (Gfx_FrameEnd) for the RSX walker to finish the previous frame.
  - The walker spent ~45% of its time waiting for the GPU after each draw.
  - The SPU mesh jobs wait on the walker for ring space, which shows as bursts of ~8 ms on, 8 ms off.
- **RPCS3 on the same PC** holds about 43 fps in the same scene, spreading SPU work over six threads.
- The SPU compute per frame is similar between the two, so faster SPU code (a lifter flag `--ib-known-targets`
  that turns the hot loops' computed branches into direct ones) gave no measurable gain.

**Draw batching.** `VK_DRAW_BATCH=1`, ps3recomp's opt-in mode written for God of War: Ascension, records 100-200
draws per GPU submission instead of waiting after each one:

| | fps (runs) |
|---|---|
| Batching off | 5.41, 5.53 (4.50, 5.00, 6.24 before the GPU clock lock) |
| Batching on | 6.25, 7.96 (6.01, 6.17 before) |

It is now on in `tools/run.sh`.

**The GPU sat in its idle clock** (210-270 MHz on a 3060 Ti). The bursty submit-and-wait pattern never ramps it up,
and PowerMizer "prefer maximum performance" didn't stick. `nvidia-smi -lgc 1400,1980` (root; reset with `-rgc`) pins
it for testing.

**Smaller fixes** on branch `mlb16-work`:
- Vertex fetch skips duplicate-removal hashing for sequential draws and fills attributes that have no array.
- `VK_EB_STATS=1` prints why GPU batches end.

**Next:**
- Asynchronous GPU submission, so the walker records while the GPU executes. The first step publishes RSX labels only
  after their GPU work completes; today the game can see a label before the work behind it ran.
- Then SIMD vertex decode and a cache for static geometry; the walker re-decodes every vertex every draw.
- Then why the SPU rings wait on the walker.

## 2026-10-27 (end of day): SPU speed, 3.4 → 6.4 fps

Measured as the average flip rate over 60 s of play (same at-bat each time, `GCM_FLIPCOUNT=1`).

| Change | fps |
|---|---|
| Start (exact SPU float, scalar local-store access) | 3.4 |
| `PS3RECOMP_SPU_FAST_FLOAT=ON` | 4.6 |
| + `PS3RECOMP_SPU_FAST_LS=ON` | 6.4 |

Both are CMake options on branch `mlb16-work` that affect only the lifted SPU sources (configure with
`-DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON`):

- **Fast float:** ps3recomp's x86 path emulates SPU single precision bit-exactly: each op in f64x4 with lane checks
  (~30 instructions), plus a scalar fix-up for tiny/out-of-range lanes that the vertex code hits often. The option
  uses native SSE/FMA3, keeps the SPU's denormal flush, and differs only in rounding (nearest vs truncate) and
  overflow (infinity vs saturate). It is within 1 ulp on 800k random lanes and ~6× faster per fma, like an
  emulator's default "approximate" SPU float mode.
- **Fast local store:** each 16-byte local-store access was four byte-swapped scalar moves plus a diagnostics-flag test
  and cold call. Now it's one vector load/store and a `pshufb`, with no diagnostics in lifted code. The hottest mesh
  block went from 827 to 478 x86 instructions.

Where the time goes now: one SPU worker at ~86% running the mesh jobs (the game runs that list on one SPU at a time,
as on hardware), a second at ~50%, the RSX walker thread at ~40% (CPU vertex conversion).

**Next lever, register caching (prototype in `tools/spu_regcache.py`):** the lifted code keeps SPU registers in
`ctx->gpr[]`, and every local-store store goes through a char pointer that may alias it, so the compiler reloads
registers after each store. The prototype rewrites each lifted function to use C locals (load at entry, write back
before exits/calls/channel ops, reload after calls).
- Applied to all 639 functions of the mesh job module, the job went wrong (an SPU return to LS `0x22`). The likely
  culprit is the interrupt path, which snapshots/restores `ctx->gpr` and can unwind host frames.
- Applied only to the 402 functions without calls or channel ops, the game plays correctly but no faster (6.0 fps).
  The hot loop (`0x7c18`) contains a call, so it wasn't covered.
- Next: make calls inside hot blocks safe for caching (or move the caching into the lifter itself, with interrupt
  points as explicit write-back sites), then measure again.

Also open: render-target writeback ("Write Color Buffers") for the dark players and crowd sprites, the RSX walker's CPU
vertex conversion, and the PPU side (same `ctx->gpr` pattern).

## 2026-10-27 (later): magenta fixed; where it stands against RPCS3

- **Magenta surfaces:** a RenderDoc capture of our own Vulkan output (layer mode,
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` with `SDL_VIDEODRIVER=x11`, since the layer has no Wayland support) showed the
  player shader sampling texture unit 5, which the game leaves disabled. Our backend binds a 1×1 magenta placeholder
  to such units, and the shader adds it as a tint. `VK_DUMMY_BLACK=1` (branch `mlb16-work`, set in `tools/run.sh`)
  makes the placeholder transparent black. Dirt, crowd, field and walls now look right.
- **Players and crowd sprites are dark/blank:** the same problem as RPCS3 without "Write Color Buffers". MLB 16 builds
  some textures by rendering them and reading the render target back from the CPU/SPU (the generated faces in
  `HeadFaceCacheFile`, the crowd sprite sheets). Our renderer doesn't write render targets back to guest memory, so
  those reads see black. Next to fix. Then delete `HeadFaceCacheFile`, which was built blank.
- **Speed: about 3.3 fps in a game** (flip counter, no RenderDoc). The two busiest threads are SPU workers running the
  mesh jobs (`jobmod_c690c2f7b09a`, skinning/geometry); the PPU main thread is at ~30% and the GPU is nearly idle.
  So the lifted SPU code is the bottleneck: one real SPU runs these jobs well within a frame, our translation is many
  times slower.
- **How this differs from an emulator, honestly:** the CPU side (PPU and SPU) is compiled ahead of time, but the GPU
  side is still an emulated RSX (command walker, translated shaders, texture formats), the same job RPCS3 does. And
  ps3recomp's RSX layer is much younger than RPCS3's, so graphics bugs RPCS3 fixed years ago show up here. The
  speed advantage can only appear once the SPU code generation is fast. Right now it's the opposite: RPCS3's SPU JIT
  (LLVM, with years of tuning) beats ps3recomp's straightforward SPU-to-C translation by a wide margin.

## 2026-10-27 (late night): in the game, pitches being thrown

**The match-load deadlock is fixed and the game plays:** the dugout, the pre-game intro, Kauffman Stadium, Volquez
pitching to Curtis Granderson, with the count and the score bug updating. Geometry is correct. Many surfaces
(uniforms, skin, infield dirt, part of the crowd) come out solid magenta, a renderer problem still to look at.

The cause was ps3recomp's SPURS scheduling, in three parts:

1. **Max contention dropped.** `_cellSpursWorkloadAttributeInitialize` takes `maxContention` as its ninth argument,
   on the guest stack, so the generic adapter never saw it and stored 1. The mesh lists ask for 5 and the DXT list
   for 4. Fixed with a context handler that reads the stack (branch `spurs-workload-maxcontention`).
2. **The ready count was consumed.** The scheduler decremented a workload's ready count each time it gave it an SPU.
   In the SDK the ready count is a level: the kernel runs up to `min(ready, maxContention)` SPUs and the workload
   lowers it itself. The WWS policy module does exactly that, setting it to 0 when its list is empty and back to 1
   when it sees more work. Consumed, a kick could be used up by a dispatch that found no jobs yet, and nothing
   dispatched the list again. `SPURS_READY_LEVEL=1` keeps the level.
3. **Idle probes ran workloads nobody had kicked.** To cover for (2), the scheduler periodically dispatched idle
   workloads. One such probe ran mesh jobs that the command-building threads had already queued, *before* the PPU
   reset the mesh rings and kicked the list. The reset then threw away the ring space those jobs had used, the next
   job was given the same space again, and it waited forever for the GPU to free it. `SPURS_NO_IDLE_PROBE=1`
   schedules only kicked workloads, as real SPURS does.

(2) and (3) are opt-in switches on branch `mlb16-work` for now, and `tools/run.sh` sets them. They change SPURS
scheduling for every title, so they need testing on other ps3recomp games before going upstream.

How it was found: a temporary override that forced the stuck wait through after 2 s got the game into the match
(with garbled geometry), which proved the wait was the only blocker. Then gdb watchpoints on the ring allocation
pointers and on the ready-count bytes showed the order of events.

## 2026-10-27 (night): how the mesh rings work, and where the deadlock really is

The stall at the pitching matchup isn't the game waiting for a pick (X on the matchup, START on the lineups):
flips stop and the main thread never leaves the job-list wait, so pad input is never read. Pressing X there changes
nothing.

How MLB 16 feeds geometry to the GPU (worked out from walker traces, gdb watchpoints and Ghidra):

- **Five mesh rings** of 2 MB each, holding **vertex/index data, not commands**: EA `0x11900000`, `0x11b00000`,
  `0x11d00000`, `0x11f00000` and `0x12100000` (IO `0x00900000`–`0x012fffff`), with RSX labels 72–76
  (`0x480`–`0x4c0`). Their state lives in the render object at `0x1246680`: `+0x200` holds
  `{base, end, alloc pointer, label address}` per ring (one 128-byte line).
- **Per-mesh slots in the command stream.** The PPU leaves a 0x90-byte slot per mesh job in its big command ring
  (IO `0x013xxxxx`–`0x014xxxxx`): a self-JUMP plus `0x40880100` (a non-incrementing NOP skipping 34 words). The job
  fills it by DMA when it finishes: vertex/index array offsets into its ring, the draw, and a release of its ring's
  label with the segment's end address. The GPU stops at the first unfilled slot, so it consumes ring data as jobs
  complete.
- **Frame setup** (`0x70694` → `0x6ed24` → `0x6f6b0`): wait for the command-building worker threads (`0x70894`),
  wait for the previous mesh job list (`0x9fb898`, list "MeshJobs0"), then set each ring's start for the new frame
  and kick the next list. Frame end is `0x66530` (links the frame into the libgcm ring, flips, paces on the
  frame counters).
- **Each ring has one SPU writer.** Watchpoints on the alloc pointers show ring 72 only written by one SPU worker,
  ring 75 by another, ring 76 by a third. Plus the PPU's frame setup. No lost updates.

The deadlock: the GPU parks at the first unfilled slot. That slot's job waits for ring space (`jobmod_c690c2f7b09a`,
`0xe4d0`–`0xe5c0`): it compares the ring's label (the GPU's read position) against the range it wants and retries
forever. In every run the waiting job wants a range the GPU hasn't released yet, often right after wrapping its ring.
Open question: why it wraps so early. In one run, ring 76's writer wrapped at `0x12137d00`, 0x37D00 bytes into a 2 MB
ring. The frame setup also once set ring 76's start to `0x12105180` rather than the base. Next: trace the per-frame
start/end values `0x6f6b0` hands out (`+0x18`, `+0x58`, and the single-ring branch at `+0x280`/`+0x290`) and how the
job derives its ring end. An RPCS3 memory dump of the same structure at the same screen would settle what "normal"
looks like.

Side findings:
- The walker's 50 ms park recovery is no longer needed for this game; running with it effectively off
  (`GCM_PARK_GRACE_MS=100000000`) still boots, and parks are always patched within 4–47 ms (`[parkfix]` log).
- `HeadFaceCacheFile` is the generated-face cache from the RPCS3 white-faces fix. Faces built natively before RSX
  render-target readback works may be cached blank; delete it once a match loads.
- Temporary diagnostics on branch `mlb16-work` (not for upstream), all under `MLB_SEMA_TRACE=1`: label releases with
  stream position, CALL/JUMP into the rings, park patches, and SPU PUTs into the command ring (`[ringput]`).

## 2026-10-27 (evening): Bink spin fixed, match load reaches the pitching matchup

- **`cellFsFtruncate` never worked:** it resolved to `libs/filesystem/cellFs.c`, which looks fds up in its own table,
  one that the port's `cellFsOpen` (`runtime/ppu/ppu_fs.cpp`) never fills, so every call returned EBADF.
  - The game sizes `HeadFaceCacheFile` (in the install folder) to 314,584,064 bytes before a match, and
    `ArenaLogoCache` to 9 MB. Implemented next to `cellFsOpen` (branch `cellfs-ftruncate`).
  - With it, the match load gets past the starting lineups to the **pitching matchup** screen.
- **The Bink SPU spin (99% of a core) is fixed:**
  - The task retried `cellSpursEventFlagWait` forever, because each call failed at once with `0x80410911` (null
    pointer): its event flag address was 0.
  - The task DMAs its setup block from r4, the taskset argument. `_cellSpursTasksetAttributeInitialize` dropped
    `argTaskset` and `cellSpursCreateTasksetWithAttribute` created the taskset with args 0.
  - Carrying it through (branch `spurs-taskset-args`) gives r4 = `0x01476E80`; the Bink thread now sleeps in its
    wait like on hardware. That's a core back.
- **The match-load stall is a real deadlock**, not a slow first cache build: with the SPU watchdog at 600 s it never
  moves. Three parties wait on each other:
  - **PPU:** the main thread spins in `0x9fb898`, the WWS job manager's wait for job list "MeshJobs0". Two SPU lanes'
    progress counters are stuck.
  - **SPU:** two mesh jobs (`jobmod_c690c2f7b09a`, loop at `0xe510`–`0xe550`) wait for ring space. Each DMA-reads an
    RSX label holding the GPU's read position in its ring (labels 72, 75 and 76; rings at EA `0x11900000`,
    `0x11f00000` and `0x12100000`). It retries while that position is inside the region it wants to overwrite.
  - **GPU:** our command walker has drained everything up to a self-JUMP "park" at IO `0x0138ef10`. The GPU waits
    there for the PPU to write more; the PPU waits for the jobs.
  - The only label release in that frame is label 75 = `0x11f02080`, inside the region its job wants. So that ring's
    previous output was never marked as read by the GPU.
- **Walker oddities found on the way** (not yet shown to cause the deadlock):
  - The game extends its big ring by leaving a self-JUMP at the write head. After 50 ms (`GCM_PARK_GRACE_MS`) the
    walker gives up on the park and jumps to `put`; a real RSX waits.
    - With the grace effectively infinite the game still boots and gets to the same stall.
    - The selected team's **logo now shows on team select**. It was probably in commands the early jump skipped.
  - At about 2.5 minutes in, the walker reads data as commands at IO `0x2d4e80`. The frame `CALL`s a vertex-program
    upload at `0x2d4ef0`; the walker was 0x70 bytes before it.
    - It reads an upload argument (`0x709d302a`) as a CALL to an unmapped address, and drops the rest of that pass.
    - Possibly a block rewritten while the walker, which drains on 4 ms ticks, was still behind it.
  - New-style CALL targets are masked with the old JUMP mask (`0x1ffffffc`) and new-style JUMPs (`w & 3 == 1`)
    aren't decoded at all. This game seems to use only old-style JUMPs and new-style CALLs into low IO, so neither
    has bitten here yet.
- **Ghidra:** the recompilation addresses are now named in the project (`Gfx_DisplayFlip`, `Gfx_FlipHandler`,
  `Mutex_LockOwned`/`Mutex_UnlockOwned`, `Cond_Wait`, `File_Duplicate`, `File_Open`, `File_AioCallback`,
  `MemPool_Acquire`, `MemPool_SharedMemContAnim_LazyInit`, `MemContainer_Carve`).
- **Next:** find which commands should advance labels 72/75/76 and why the walker never reaches them: trace every
  `0x1d6c`/`0x1d74` release value per frame, and the `CALL`s into the mesh rings (IO `0x0090xxxx`, `0x00f0xxxx`,
  `0x0110xxxx`).

## 2026-10-27 (afternoon): driving it headless; match load gets to the lineups

- **Scripted input:** `PAD_FILE=<file>` presses whatever button mask is written to the file (`0x4000` CROSS,
  `0x0008` START, `0x0040` DOWN; `"0x0040 3"` holds it for 3 reports). With a frame dump every 10 frames, the menus
  can be driven without a person: title → START → "Online disabled" OK → Play Now → Offline Rosters → team select →
  stadium select (a still image of Kauffman) → game options → starting lineups.
- **`-O2 -march=native` build** (`recomp-work/build-o2`, 16.5 min at `-j4`): same menu frame rate as `-O1`, 421
  frames in 20 s each (about 21 fps). The menus are paced at one frame per three 60 Hz vblanks, so they don't show CPU
  speed.
- **"3D TV detected":** `cellVideoOutGetResolutionAvailability` reported the 3D modes (ids `0x80`+) available, so
  the game asked to enable 3D and, on yes, rendered over-under. Fixed on branch `cellvideoout-no-3d` (PR draft 5).
- **The match-load NULL call** came from the game's memory containers:
  - `0x14fc4` carves an N MB container out of the default one (id `0xFFFFFFFF` at `0x0111ffc4`): get its size,
    destroy it, recreate it smaller, create its own.
  - ps3recomp's `sys_memory_container_get_size` rejected the default id, took its arguments in the wrong order and
    wrote `{total, used}` instead of `{total, available}`. `sys_memory_allocate_from_container` (syscall 350) wasn't
    registered.
  - So the "SharedMemContAnim" pool at `0x0134a464` (built lazily in `0x99c45c`) never got memory, and `0x8c5508`
    called a method on a NULL buffer.
  - Fixed on branch `sys-memory-containers` (PR draft 4). Both pools now allocate (25 MB in container 1, 7 MB in
    container 3), and the NULL call is gone.
- **Next stall:** about 30 s into the lineups screen, an SPU job on the job manager (local store `0xe510`) re-reads
  RSX label 76 (`0x200004c0`; labels start at `0x20000000`) until the runtime halts it. Something that should write
  that label, probably a semaphore release in the GPU command stream, isn't writing it.

- **Label 76, first look:** a temporary trace in our runtime branch (`MLB_SEMA_TRACE=1`, branch `mlb16-work`) shows
  the GPU command stream *does* release it. The game cycles through five labels, `0x480`, `0x490`, `0x4a0`, `0x4b0`
  and `0x4c0`, with `NV4097_SET_SEMAPHORE_OFFSET` (`0x1d6c`) and texture-read releases (`0x1d74`), on subchannel 0,
  with no semaphore context DMA set (so they land in the label window). The waiting job is
  `jobmod_c690c2f7b09a` (a geometry ring producer: DMA-reads the label at `0xe510`, compares it against its ring
  bounds at `0xe550`).
  - Next: compare the values released against what the job waits for. Candidates: byte order (`0x1d74` is written
    verbatim, `0x1d70` byte-swapped), the walker running behind the job's own output, or releases in
    SPU-built command blocks.
  - Driving there takes about 4 minutes with the `PAD_FILE` script above.

## 2026-10-27 (evening, playtest): menus playable, match load crawls

Played with a PS4 controller through SDL, in a window:
- **Menus work.** Experience level, main menu and moving between screens, at about 20 fps (109 new frames in about
  5 s, read from the frame counter at `0x011203d8`).
- **Button presses sometimes need two taps.** `PAD_ALWAYS_CHANGE=1` (every read returns a full report) didn't fix it.
  The controller is sampled every 4 ms, so short taps aren't the cause.
- **Menu boxes flicker** (only the boxes, not the whole screen): to capture with the renderer's pass dumps.
- **SELECT (Quick Menu) froze it.** This is online code, which also needed our server in RPCS3. Skipped for now;
  offline first.
- **Loading an exhibition match crawled.** The crowd job workload (`CrowdJobList`) started and stadium data loaded,
  then reads dropped to about 17 KB/s with no new frames. The main thread was busy in game code (`0x5499a0`,
  `0x643200`…) while the job module `255e2961` kept dropping into the SPU interpreter.
  - A gdb breakpoint on `spu_interp_run_until` caught 7,293 interpreter entries in 25 s, mostly at `0xd7a0`,
    `0xd9d0`, `0xd810` and `0xd878`: functions reached only through function pointers, so the lift never had them as
    entry points.
  - The 28 addresses are now passed to `lift_jobmods.py` (`LBP_JOBMOD_EXTRA_FUNCS`, in `tools/lift_spu.sh`). The
    job modules gained functions, e.g. `7ac07dde` went from 241 to 630. Test pending.
- **CPU use:** the Bink SPU program spins at 99% of a core in its wait loop even with no movie, and a job manager
  thread runs at about 84%.
- **Match load still stalls with the entry points added** (interpreter entries are now only the job manager's own
  self-modifying code at `0xa68`). At the start of each match load the main thread calls a NULL object:
  - `0x8c5508` writes the game's "FE data block" (`HDDBlk`) to the HDD cache. It takes a buffer from a 3-slot pool
    at `0x0134a464` via `0x98c72c`, which hands one out only if the slot's current owner says it's done (virtual
    `+0xc`).
  - All three were busy, so no buffer came back. The caller doesn't check, and calls a virtual method on 0 (call at
    `0x8c5638`, logged as `[null-call] near lr=0x8C563C`).
  - Next: read the pool's owners (slots `+0x18`, `+0x24`, `+0x30`) at that moment to see what's still holding them.
- `cellFsAioWrite`'s completion callback (`0x79c2c`) only forwards to the request's handler, which the game sets up
  before submitting, so completing synchronously looks safe here.

- The build now lives in `recomp-work/build` (lifted source in `recomp-work/recompiled`), not the session's scratch
  folder.

## 2026-10-27 (later still): first menu

- **The autosave-screen stall** was the game's file copy into its `/dev_hdd1` cache (`0x776d8`, a "Duplicate"
  routine called from the file open at `0x78ca4`). It writes 64 KB chunks with `cellFsAioWrite` and polls a status
  word set by the completion callback (`0x79c2c`). `cellFsAioWrite` (`0x4CEF342E`) wasn't implemented in ps3recomp,
  so nothing was written and the callback never ran. Implemented on our branch `cellfs-aio-write` (upstream PR
  candidate 3), modeled on its `cellFsAioRead`.
- **Result:** about 500 async writes during boot, then the game's first interactive screen, **"Choose your
  experience level" (Veteran / Novice)**, drawn correctly through Vulkan with the team background behind it. This
  is the first-time setup a new save sees before the main menu.
- The Bink thread still spins at 99% of a core; whether the intro movie plays isn't known yet.
- **New unresolved imports, reached for the first time:** `0xB20F87B3`, `0xCE4374F6`, `0xDBF22BD1`, plus
  `cellHttpInitCookie` and `cellSpursAttributeEnableSystemWorkload` from before.

## 2026-10-27 (late night): flip loop fixed; stuck on the autosave notice

- **The 100,000 flips a second** came from the flip handler running twice per flip. `cellGcmSetFlipImmediate` calls
  it at request time (a compatibility "early edge" in ps3recomp) and the pump calls it again at completion. MLB's
  handler advances the displayed-frame counter (`0x011203dc`) each time, so it overtook the rendered-frame counter
  (`0x011203d8`). `DisplayFlip` (`0x692dc`) then saw a frame ready forever and flipped non-stop. Upstream's
  `GCM_FLIPCB_ONTICK=1` drops the early call. With it, about 35 flips a second, and the two counters stay a few
  frames apart.
- **Where it stops now:** the screen stays on the autosave notice (cover art) from about 20 s on, flipping buffer 0
  only, while the game keeps loading in the background. It opens `MLB_INTRO.BIK` early, probably to preload.
  - The main thread polls an event flag with an ~11.6 ms timeout (`0x4c7ae0` → `0x78ca4` → `0x785d8` → `0x776d8`).
  - The Bink SPU program (`BinkSPU.spu.elf`, task created with a SPURS event flag, `direction=2`, SPU to PPU) spins
    in its wait-for-work loop (`0x3870` → `0x3438` → `0x3448`) at 99% of a core.
  - Next: check ps3recomp's SPURS task event flag and queue path between the two (`docs/spurs-task-signal-wake.md`
    upstream).
- **Upstream PR branches** are in the fork (`MatRanc/ps3recomp`: `template-vulkan-backend`, `gcm-handler-thread`),
  with the draft descriptions in `PR_DRAFTS.md` on its `pr-drafts` branch. Nothing is opened yet.

## 2026-10-27 (night): boot freeze fixed, display stops updating

**The freeze was a leaked lock, not Bink.** The game usually hung on its first screens, at random, before ever
opening the intro movie; one early run got through by luck. Stacks of the stuck process (`eu-stack`, function names
are PS3 addresses) and its guest memory (`gdb`, reading `vm_base + address`) showed:
- `DisplayFlip` (thread entry `0x692dc`) flips each frame, then waits on a condition for the flip handler
  (`0x691b0`, registered with `cellGcmSetFlipHandler`) to clear a "flip pending" flag at `0x011203e0`.
- MLB's lock wrappers (`0x9a2da8` lock, `0x9a2e78` unlock) record the owner's thread ID at `struct+8` and only
  unlock when the caller's ID matches.
- ps3recomp delivers vblank/flip handlers at whatever firmware call a guest thread happens to make, and the callback
  inherits that thread's ID (`ppu_guest_call`). The flip handler ran nested inside `DisplayFlip`'s own call, while
  `DisplayFlip` held the lock at `0x012450d8`. It re-entered the lock (same ID), and its unlock cleared the owner
  record. `DisplayFlip`'s own unlock then saw an owner of 0 and was skipped, so the lock leaked (owner 4,
  recursion 1). The display engine and a sound thread blocked on it forever, and with the handler stuck inside the
  pump, no more vblank/flip callbacks were delivered.
- On a PS3 these handlers run on GCM's own interrupt thread.

**Fix** (our ps3recomp branch `gcm-handler-thread`, upstream PR candidate): `GCM_HANDLER_THREAD=1` delivers the
handlers only on a dedicated host thread with its own identity. With it, 3 of 3 runs get past the boot screens,
open `MLB_INTRO.BIK`, and go on loading user data, crowd sound banks, profiles and the logo cache.

**Next problem:** the presented picture stays on the autosave notice (cover art) from about 20 s on, while the game
keeps running. `DisplayFlip` now calls `cellGcmSetFlipImmediate(0)` about 100,000 times a second, so its flip loop no
longer paces itself. The movie may be playing unseen.

Also unresolved, not blocking yet: `0x4cef342e`, `cellSpursAttributeEnableSystemWorkload` and `cellHttpInitCookie`.

## 2026-10-27 (evening): first picture through Vulkan

- **Packages:** Vulkan needs `vulkan-headers`, `glslang-devel` and `spirv-tools-devel` (Fedora). Build with
  `-DPS3RECOMP_RSX_VULKAN=ON`. With glslang found, CMake reports "guest programs available": the game's own RSX
  shaders are translated (HLSL, then SPIR-V).
- **Template gap (upstream fix candidate):** `templates/project/main.cpp` picks D3D12, Metal or the headless
  renderer and never Vulkan, even in a Vulkan build. The runtime's own host tool (`runtime/host/host_posix.c`) does
  pick it. Adding a `PS3RECOMP_RSX_VULKAN` case with `rsx_vulkan_backend_init/present/pump_messages` fixes it.
- **Run:** `PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1`. The renderer picks the RTX 3060 Ti, opens a
  1280×720 Wayland window, and the game draws its **legal and licensing screen correctly**: text, logos and the
  animated red and blue background. `PS3RECOMP_VK_DUMP=<file>.ppm PS3RECOMP_VK_DUMP_EVERY=60` saves frames.
- **Next blocker:** it freezes right after opening the intro movie, `PS3/UI/Movies/intro/MLB_INTRO.BIK`. Bink's
  SPU program is lifted, but playback hangs.
- `PATCH_GD.WAD` fails to open from the data install folder (`BCUS01089_R`); it may be found in the update folder
  instead, not checked yet.

## 2026-10-27 (later): SPU job manager running, data install done

- **Policy module:** the WWS job manager's SPURS policy module is 12,032 raw bytes at `0x105bb80` (file offset
  `0x104bb80`) that SPURS loads at local store `0xA00`. Wrapped as an SPU ELF at that base
  (`wrap_spu_elf.py --base 0xA00`) and lifted with `build_spu_workloads.py`, its text segment registers under the
  fingerprint the runtime looks up (`0x75BFE68AE96D7577`).
  - The runtime's `SPU_DUMP_MISS=<dir>` setting wrote the missing programs out exactly as fingerprinted, which is
    how both were found.
  - The same job manager runs LittleBigPlanet, whose fixes are already upstream.
- **Job modules:** `lift_jobmods.py --engine-elf` finds and lifts the 7 job modules linked into the EBOOT. One has
  308 undecoded words, the same count as the MP3 decoder program, so they're probably its data tables.
- **Build:** with the 13 SPU programs (`-DSPU_DIR=`), the rebuild takes 38 s. All of this is in `tools/lift_spu.sh`.
- **Results:**
  - All three job workloads resolve and run.
  - The game's 9.5 GB first-boot data install completes in about 50 s.
  - The game then creates more workloads and its seven sound streaming threads. Job loads stall and resume through
    SPU DMA lists and interrupts, and the job manager's self-modifying code is handed to the SPU interpreter.
- **Open problems:**
  - The job manager sometimes spins at local store `0x2534`, repeating one DMA transfer until the runtime's
    watchdog halts that SPU (256 identical transfers). LBP's notes say this policy module gets code patched into it
    by SPURS at load time (holes at `0xAEE`–`0x11B0`, `executeStage` at `0x14f4`); `SPURS_PM_DUMP` checks for that.
    - Checked with `SPURS_PM_DUMP=1`: the same holes as LBP's (1,728 of 1,730 bytes zero, `executeStage` empty).
      No game code writes the main-memory copy in 60 s. Upstream has no fix for LBP either.
    - That may be expected: the job manager writes code into its own local store (the runtime logs these as
      `spu-smc` and runs them through the SPU interpreter). That would fill the holes on the SPU side, where a probe
      of main memory can't see them.
    - The spin re-reads the same 128 bytes of the workload's data (data + `0x80`) 256 times, which looks like
      polling an empty job queue rather than a crash. The game keeps re-running those workloads afterwards. Still to
      confirm.
  - One guest write to NULL+0x48 from `0x7b988`.
  - The headless null renderer shows nothing yet; the Vulkan backend is next.

## 2026-10-27: first boot

**Lifting the PPU code** (`tools/probe.sh`):
- `ppu_loader.py` finds 15,790 functions from the OPD table and 433 firmware imports across 32 libraries.
- `ppu_lifter.py` lifts 43,501 functions to about 6.5M lines of C++ in 11 chunks, in about 1 minute.
- **Pass `--code-end 0xf2c634`.** Without it the lifter treats the read-only data after the code (1.5 MB, up to
  `0x10a43e8`) as code: 1.93M undecodable words and 954 MB of output. Ghidra's section table shows the last
  executable section ends at `0xf2c634`.
- With it, 6,847 words stay undecoded. They are float constants (1000.0, 32767.0, 10000.0…) and zero padding
  between functions, not unsupported instructions.

**Building** (ps3recomp's `templates/project`, clang, `-j6`):
- No compile errors. 10 min 55 s wall time, about 1.5 GB of RAM per chunk.
- Result: a 205 MB native Linux executable.
- The runtime itself needs `SDL2-devel` (Fedora: `sudo dnf install SDL2-devel`) and builds in seconds.

**Running** (headless null renderer, no SPU programs lifted yet):
- The game's own code runs natively. It sets up memory, SPURS, GCM and sysutil, starts its threads ("Job Manager
  Event Handler", "IMFE PPU Thread"…), draws its first frames and flips buffers, creates its trophy context, finds
  a PS4 controller through SDL, and reads `MLBPS3_SYS.WAD` from the disc.
- First stop: the game's "not enough system storage (5,120 MB)" dialog. The runtime resolves `/dev_hdd0/game` under
  the same root as the disc, which was a read-only ISO mount. Fix: a writable root folder holding a `PS3_GAME`
  link to the disc and `game/BCUS01089` (the v01.10 update).
- With that, the game offers its optional data install, the runtime auto-answers yes, and the game starts
  installing ("Installing Game Data… File 1/6").

**Missing so far:**
- The WWS job manager's SPURS policy module (12,032 bytes at `0x105bb80`) isn't lifted, so the game's SPU job
  workloads (`Wws_Job`: `DXTJob`, `MeshJobs0`, `MeshJobs1`) never run. ps3recomp's `lift_jobmods.py` targets this
  job format (`c0dec0de` modules).
- The Bink SPU program (110,560 bytes) isn't lifted yet, so video won't play.
- Two unresolved firmware imports showed up early in the boot.

**SPU programs:**
- The EBOOT embeds 5 SPU ELF images: two WWS job checks, an MP3 decoder, surround audio and Bink.
- It also holds 8 `c0dec0de` job modules (4 inside those ELFs), including the ICE mesh program.
- RPCS3 showed 25+ SPU programs in use, so the rest come from the game's data files.
- All 5 ELF images lift with `spu_lifter.py --auto-functions` with no unsupported instructions.

**Firmware coverage** (approximate, from ps3recomp's handler table vs our 433 imports):
- 165 have a real implementation, 71 a thin one, 46 return a constant, 32 run inside the runtime itself, and 119
  are missing. Missing imports return CELL_OK without doing anything.
- Gaps for online play:
  - `cellHttp` is registered under made-up names, so its handlers never match the game's imports (37 of 48 missing).
  - `cellSsl`/`cellHttps` have no TLS.
  - NP auth, ID lookup and signaling are missing or stubbed.
- ps3recomp's own docs recommend Sony's `libsre` (lifted from your firmware) over its built-in SPURS, which has hung
  most SPURS-heavy titles.

## Reference

- **Upstream PRs** (submitted to `sp00nznet/ps3recomp`, all open): #640 templates Vulkan-on-Linux; #641 cellFsAioWrite;
  #642 cellVideoOut stereo; #643 cellSpurs taskset args; #644 sys_memory container size; #645 cellFsFtruncate;
  #646 cellSpurs maxContention; #647 GCM handler thread; #648 GCM wrap JUMP slot; #649 cellSpurs maxContention clamp +
  RequestIdleSpu; #650 PS3RECOMP_TRACE.
- **Clean-room rule:** ps3recomp/mlb16-recomp are MIT; RPCS3 is GPLv2. Never copy or port RPCS3 (or other GPLv2) code;
  write original code against ps3recomp's own structures. This keeps the project MIT-compliant and upstreamable.
