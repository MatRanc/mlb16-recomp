# Present/pump stalls: the synchronous frame dump on the RSX walker (C4), and what is not it

Date: 2026-11-05. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Cap used: **two pitch-meter launches** (one diagnostic,
one confirm), X once per launch, both with the hitch recorder. The fix is behind
`PS3RECOMP_VK_DUMP_ASYNC` (**default off**). Follows
[`plan-meeting-2026-11-05.md`](plan-meeting-2026-11-05.md) §3 C4/C5,
[`long-frames.md`](long-frames.md), [`hitch-census.md`](hitch-census.md),
[`window-freeze-fix.md`](window-freeze-fix.md) and
[`menu-34ms.md`](menu-34ms.md).

## TL;DR

* **C4 is the frame dump.** `PS3RECOMP_VK_DUMP` writes a 2.7 MB PPM every
  `PS3RECOMP_VK_DUMP_EVERY` presents (`ab_live.sh:314` sets **every 10**), and
  `vk_dump_ppm` did the `fwrite` + `rename` **on the RSX walker thread**, inside
  the present path (`rsx_vulkan_backend.c:2847`, definition `:2711`). When the
  session filesystem throttles writeback the walker blocks; it cannot tick the
  display, so the guest display thread waits out its 17 ms fallbacks and the
  pump stops. Measured: **`stalltrace dump=717.6 ms`** on the walker, the exact
  717 ms "present/pump stall" in [`meter-p50-pin.md`](meter-p50-pin.md), and
  `dump=596.6 / 541.6 ms` on two measured-window stalls.
* **Fix (default off):** `PS3RECOMP_VK_DUMP_ASYNC=1` keeps the packed RGB copy
  on the walker but hands the buffer to a one-slot writer thread
  (`libs/video/vk_dump_queue.c`). The walker never waits for the filesystem (the
  hand-off measured < 20 ms, normally ~1 ms); a newer frame replaces an
  unwritten one, so the queue cannot grow or lag.
* **Confirm run (same config, same meter):** with the key on, **0 dump stalls**
  and **0 `stalltrace dump` lines** vs 2 dump stalls in the baseline meter phase
  (0.9/min -> 0/min). The remaining walker stalls are the first-use
  pipeline/shader translation at the pitch approach (before the meter), and the
  meter-segment present stalls left are the compositor's `vkQueuePresentKHR`
  (one 804 ms, 23 no-slot drops) — not this code and not the dump.
* **C5 does not share the cause.** The ~4.3 ms menu tail is the buffer-ref wait
  *after* the flip handler ([`menu-34ms.md`](menu-34ms.md) §1: 13.32 -> 17.60 ms),
  a producer-cadence wait; the dump fix does not change it and the dump is not
  in that path.
* **Nothing is default-on.** The async writer, the stall tracer
  (`PS3RECOMP_STALL_TRACE`) and the 200 ms threshold are diagnostics; the play
  scripts are unchanged.

## 1. Census of the existing logs (before any launch)

Every `recomp-work/*.log` was scanned for the two window counters:
`[RSX vulkan] window: ... max pump gap N ms` (the walker thread did not reach
`sdl` pump) and `longest vkQueuePresentKHR N ms` (the present thread did not
return). A stall line is >= 200 ms. "Meter window" means inside a measured
segment: the `.marks` file's `[switch + 2 s, switch + 10 s]` interval.

| population | lines | runs |
|---|---:|---:|
| all stalls >= 200 ms | **1,594** | 211 |
| all stalls >= 500 ms | 783 | - |
| all stalls >= 1 s | 233 | - |
| in a measured meter window | **160** | 68 |
| in-window >= 500 ms | 108 | - |
| in-window >= 1 s | 36 | - |
| of those, present-thread (`q >= 500 ms`) | 12 | - |

Runs with the most in-window stalls: `dispatch1` 10, `fixab` 8, `p50fix` 8,
`remapdirect` 8, `backon`/`exp_a_off`/`pinburst1`/`wcbstream` 5.

The population is not one bug. Three classes explain it:

1. **Frame-dump writeback (C4, this document).** 0.5-0.9 s, walker present
   path, no dropped frames (`no-slot 0`, `not-ready 0`), often clustered a few
   seconds apart while the disk is under writeback pressure. This is the class
   the plan cites; the exact 717 ms in `meter-p50-pin.md` is the dump.
2. **First-use pipeline/shader translation.** 0.25-2.1 s, *inside*
   `cellGcm_rsx_process_fifo`, not the present. The walker report shows
   `pipe=1267.7 ms`, `pipecrt=101` for one 2.05 s drain; this is the same
   `shader.compile` cost as the match load ([`hitch-census.md`](hitch-census.md)
   §3), met when the pitch/meter first draws new programs. It is cached by the
   shader disk cache; it is not C4's steady-state present stall.
3. **Presentation-engine stalls.** a) The pre-2026-11-02 native-Wayland
   **MAILBOX** freeze: `vkQueuePresentKHR` 8-49 s with no-slot storms, already
   fixed by FIFO/Xwayland ([`window-freeze-fix.md`](window-freeze-fix.md)).
   b) Under x11/FIFO a single present can still block ~0.8 s and drop frames
   no-slot, then recover; that is the driver/compositor, not this code.

Why the trace classification alone is not enough: `tools/hitch_table.py`
compares the c690 lane-sum with the frame and labels a long frame c690- or
drain-dominated. In a dump stall the c690 batch finished normally in the first
~32 ms and the walker was then parked; the summed lane time made the frame look
c690-ish. The new stall tracer attributes the pass directly.

## 2. Diagnostic launch: the walker seat, then the dump

Instrumentation (default off): `PS3RECOMP_STALL_TRACE=1` times each part of the
frame-clock pass (`main.cpp:269`, print `:389`), and the backend prints a
`[stalltrace]` line for a slow dump/fence/acquire/window
(`rsx_vulkan_backend.c`). One launch (`ab_live_stalltrace1_20261105_054237`,
`N=6 SEG=10`, `PIN=0`, x11, `HITCH_US=200000`, recording in
`recomp-work/captures/stalls_20261105_054237.pstl`).

The tracker splits the 1.6 s class into exactly two seats:

```
05:43:32 [RSX vulkan] stalltrace dump=717.6ms
05:43:32 [stalltrace] loop total=720.3ms ticks=1 kick=2.6 tickp=0.0
          tickd=0.0 present=717.7 drain=0.0 pump=0.0

05:46:29 [RSX vulkan] stalltrace dump=596.6ms
05:46:29 [stalltrace] loop total=609.2ms ticks=2 kick=0.0 tickp=608.7 ...
05:46:32 [RSX vulkan] stalltrace dump=541.6ms
05:46:32 [stalltrace] loop total=563.4ms ticks=1 kick=0.0 tickp=0.0
          tickd=0.0 present=542.3 drain=20.9 pump=0.1
```

* `present`/`tickp` is the `present_guest_frame()` call; the fence wait,
  `vkAcquireNextImageKHR` and `vkQueuePresentKHR` all reported < 200 ms for
  those frames, and the `dump=` line lands in the same millisecond. The time is
  the synchronous `fopen/fwrite/fclose/rename` in `vk_dump_ppm`, not the GPU.
* The 717.6 ms value is the 717 ms stall listed in
  [`meter-p50-pin.md`](meter-p50-pin.md) §4 ("B has three unrelated
  present/pump stalls (window `max pump gap` 717, 898, 898 and 827 ms ... 0
  no-slot/not-ready drops)"). Same shape: walker present, present thread fine,
  no drops.
* The drain-seat stalls in the same run are the other class: at 05:44:51 the
  walker report was `walker=1426.6 ms engine=1422.0 pipe=1267.7 pipecrt=101`
  (first-use pipeline/shader), i.e. *inside* the drain. They happen at the
  pitch approach, before the meter segments start (meter confirmed 05:45:27;
  segment A starts 05:45:38).

Why the filesystem blocks: the dump rewrites 2.7 MB ~3.5 times/s (every 10 of
~35 presents/s) on the same btrfs as the log, the trace and the thumbnails
(`/home` is btrfs `compress=zstd:1`, 90 % full, 24 GB free). Most writes are
page-cache cheap; when writeback throttles, `write()` blocks for the 0.5-0.9 s
seen here. The dump is only needed for the pitch detector and the 2 s
thumbnails, so this is measurement/tooling cost on the game's critical thread.

## 3. Fix: a one-slot asynchronous dump writer (default off)

`libs/video/vk_dump_queue.{c,h}`: `init(path)` starts one writer thread;
`submit(rgb,len,w,h)` takes ownership of the packed buffer and returns without
touching the filesystem; the thread writes `<path>.tmp` and renames it over the
target. There is one pending slot: a newer frame replaces an unwritten one
(drop-oldest), so memory and lag are bounded. `shutdown()` flushes, joins and
frees; `submit` after shutdown returns -1 and the caller keeps ownership.

`rsx_vulkan_backend.c:2701` reads `PS3RECOMP_VK_DUMP_ASYNC` (default 0). At
`:2711` (`vk_dump_ppm`) the packed RGB conversion stays on the walker (a CPU
loop, no I/O) and, when the key is on, the buffer is submitted at `:2720`;
otherwise the old synchronous path runs unchanged. `:6242` joins the writer in
`rsx_vulkan_backend_shutdown`. No `spu_context` field was added.

Tests: `libs/video/tests/test_vk_dump_queue.c` (new, in
`tools/test_video_fixtures.py`) submits two frames and checks the last one lands
complete, that a rejected submit after shutdown is not freed, and that re-init
works. `python3 tools/test_video_fixtures.py`: **33/33 pass**.
`check_spu_context_abi.sh`: shared field offsets agree.

## 4. Confirm launch and numbers

Same command as the diagnostic, plus `PS3RECOMP_VK_DUMP_ASYNC=1`
(`ab_live_stallasync1_20261105_055040`); both runs have the same arms (no
control-file body), the same `N=6 SEG=10 DROP=2`, x11/FIFO, `PIN=0`.

| metric (meter phase: confirm -> segments done) | sync dump (baseline) | async dump |
|---|---:|---:|
| meter phase | 05:45:27-05:47:40 (133 s) | 05:53:18-05:55:30 (132 s) |
| walker dump stalls >= 200 ms | **2** (608, 563 ms) | **0** |
| dump stalls per minute | **0.90** | **0.00** |
| `stalltrace dump` lines | 3 (717.6, 596.6, 541.6 ms) | **0** |
| in-window present-thread stall | 0 | 1 (804 ms, 23 no-slot, recovered) |
| measured-window fps A / B | 33.59 / 34.10 | 35.96 / 36.47 |

The fps difference is run-to-run drift (different sessions, no A/B within a
run); the async writer is not a speed change. The important number is the 0.9
stalls/min of 0.5-0.7 s disappearing. The remaining x11/FIFO present stall
(804 ms, `vkQueuePresentKHR`, `no-slot 23`) is the compositor class (3b); it did
not exist in the baseline run and is not caused by the key.

## 5. C5: no, the menu tail is a different wait

[`menu-34ms.md`](menu-34ms.md) §1 places C5 at the *end* of the one-tick menu
frame: the flip handler runs at +13.3 ms, then `Gfx_DisplayFlip` waits 4.3 ms
for the next frame's buffer ref, published by the producer at +17.3 ms. That is
the guest's producer cadence and the ref-publication hand-off; the dump is on
the present path and fires every 10 presents, and the async key does not touch
the ref queue. The menu captures show no 0.5-0.9 s pump stall after the flip
fix, so C5 is not this bug. It remains a separate ~4.3 ms pacing item.

## 6. Repro

```sh
# build + unit
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja -j8 MyGameRecomp
cd /home/m/Documents/mlb16-online/ps3recomp-play && python3 tools/test_video_fixtures.py

# diagnostic (sync dump) and confirm (async dump): one launch each
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=stalltrace1 N=6 SEG=10 DROP=2 METER=1 PIN=0 VIDEODRIVER=x11 \
  PS3RECOMP_STALL_TRACE=1 \
  PS3RECOMP_TIMELINE=$PWD/../captures/stalls_$(date +%Y%m%d_%H%M%S).pstl \
  PS3RECOMP_TIMELINE_HITCH_US=200000 ./ab_live.sh
TAG=stallasync1 ... PS3RECOMP_VK_DUMP_ASYNC=1 ... ./ab_live.sh

grep -E '\[stalltrace\]|max pump gap' recomp-work/ab_live_stall*.log
```

## 7. Caveats and clean room

* The stall tracer adds clocks per pass and one `fprintf` per long pass only;
  it is default off. The two runs used different sessions and no within-run key
  A/B; the dump attribution is from the tracer's own `dump=` line, not from an
  fps comparison.
* The dump rate is a harness choice (`ab_live.sh:314` every 10, `play_windowed`
  every 30). The runtime change removes the walker block for both; the harness
  defaults are untouched. Making the dump land on tmpfs would also help, but
  would change the detector's file contract.
* The async writer drops a pending frame under sustained write stalls. That is
  deliberate for a diagnostic image (the detector and thumbnails only need a
  fresh frame); it is not used for anything the guest sees.
* The first-use pipeline/shader stalls at the meter start are real and remain;
  they are the documented shader-translation cost, not C4's present stall.
* Census runs span different builds, scenes and the old Wayland MAILBOX
  path; the 160 in-window lines are a population, not a controlled A/B.
* All of this is our own runtime, captures, logs and EBOOT. No RPCS3 or other
  GPL emulator source was read or cited.
