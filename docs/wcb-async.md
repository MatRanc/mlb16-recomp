# WCB readback waits: two submits per copy, and a prefetch that removes the copy

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two pitch-meter A/Bs (`ab_live.sh`, X once, WCB=2,
`PS3RECOMP_RSX_CUBE_RT=1`, PIN=1, x11, N=6 x 10 s, 2 s dropped) plus one
`perf record` of the walker thread. The crowded-replay numbers are re-analysed
from the user's existing 40 ms recording
`/tmp/longframes_20261104_142459.pstl`; no replay run was made.
Follows [`c690-serialisation.md`](c690-serialisation.md),
[`readback-cost.md`](readback-cost.md), [`walker-per-draw.md`](walker-per-draw.md),
[`live-ab-and-ceilings.md`](live-ab-and-ceilings.md) and
[`nhl-lessons.md`](nhl-lessons.md).

## TL;DR

* **A demand readback is two GPU round trips, not one.** The walker first
  flushes the open draw batch and waits for it
  (`vk_eng_submit_and_wait`, `rsx_vulkan_backend.c:3088`), then records an
  image->host-buffer copy in a *new* command buffer and submits and waits
  again (`vk_eng_readback`, `:3247` -> `vk_submit_and_wait`, `:841`). There is
  no `vkQueueWaitIdle`: both waits are `vkWaitForFences(...UINT64_MAX)` on a
  single fence. The copy itself is into a host-visible staging buffer, then a
  row `memcpy` into guest memory.
* **At the meter the guest reads a six-level exposure mip chain** (32x32,
  16x16, 8x8, 4x4, 2x2, 1x1, RGBA8) by PPU/SPU page fault, 0.34-0.35 times
  per frame per level (about every 2.8 frames), plus in some sessions an
  NV3089 transfer out of a 512x512 target. Addresses are allocator-assigned;
  this run's chain was `0x019E1F00/0x019E2F00/0x019E3300/0x019E3500/
  0x019E3600/0x019E3680`.
* **In the crowded replay every readback is tiny but the wait is not.**
  18,060 `rsx.wcb_done` events in the 1,930 frames >= 40 ms: **p50 10
  copies/frame**, 96 % <= 4 KiB, 13.5 ms/frame of copy+sync (p90 31.6).
  The expensive bucket is the 4 KiB 32x32 exposure level (n=2,965, mean
  6.12 ms, p50 3.23 ms); the 0-512 B mip tips are cheap (p50 58 us). This is
  waiting for the *draws*, not PCIe: 4 KB cannot take 3 ms of bandwidth.
* **The first cut, `PS3RECOMP_RSX_WCB_STREAM`, is neutral.** It records the
  copy at the end of the open batch and waits once instead of twice. The
  per-draw walker is unchanged (32.62 vs 32.50 us/draw) and the 32x32 copy
  even costs 4.6 % more (0.985 vs 1.030 ms/f): the removed second submission
  was a small part of the cost, and the mid-batch full pipeline barriers add
  a little. The wait is the accumulated draw batch completing.
* **The winning cut is `PS3RECOMP_RSX_WCB_PREFETCH`: queue the copy and submit
  the batch at the sync point, not at the read.** The GPU then works through
  the frame while the walker records the rest, and the guest's later read
  collects an in-flight ticket. At the meter the readback engine cost falls
  **1.18 -> 0.17 ms/frame (-86 %)**, the per-copy guest wait **2.50 -> 0.93 ms
  (-63 %)**, the 32x32 copy **0.955 -> 0.117 ms/frame (-88 %)**, and the walker
  **32.39 -> 31.04 us/draw (-4.2 %)**. p50/fps do not move because the meter
  frame is the c690 producer (34.16 vs 34.17 ms); the walker mean is
  18.54 -> 18.17 ms.
* **Nothing is default-on.** Both keys are live keys, off by default. The
  prefetch changes the read's semantics from "the target when the guest
  touches it" to "the target as of the sync point it just passed" (the same
  contract as the existing WCB=3 async mode); it is exact for the title's
  reads, and is the one to test on the crowded replay.
* **Live per-draw profile (perf, walker thread):** remap build 16.3 %,
  `memmove` (vertex/constant/ring fills) 6.5 %, profiler timestamps ~6.7 %,
  `sink_end_work` 5.1 %, `eng_pipeline_get` 5.0 %, FIFO loop 4.9 %. Vulkan
  recording + descriptors are ~2 %, and **lock/futex samples are < 0.05 %**:
  contention with the SPU lanes is not a walker per-draw cost at the meter.

## 1. What a WCB readback does today

The module and its modes are described in the runtime's
`ps3recomp-play/docs/wcb-async.md`; this section is the part the mission needs.

`PS3RECOMP_RSX_WCB=2` (ondemand, the play default) arms every dirty
non-display colour target and dirty depth target at a sync point by taking its
guest pages away (`rsx_draw_engine.c:3830` -> `rsx_wcb_protect`). Nothing is
copied. The first guest touch faults (`wcb_signal`, `libs/video/rsx_wcb.c:688`)
and the faulting thread asks the walker (owner) to resolve the region. The
walker services requests between FIFO methods and in its idle wait
(`rsx_draw_engine_wcb_service`, `rsx_draw_engine.c:3890`).

Four triggers exist, and only two are CPU reads of render targets:

| trigger | code | what the guest is doing |
|---|---|---|
| `REQUEST` / `FAULT` | `eng_wcb_resolve`, `:3750` | a PPU load or SPU DMA touched the target's pages: **this is the readback** |
| `EXPLICIT` from `eng_guest_read`, `:2090` | an NV3089 transfer reads a rectangle out of a target in stream order |
| `EXPLICIT` from `sink_bind_textures` | a texture unit names a target in a format the host image cannot present |
| `FLUSH` | eviction / shutdown |

No report, label or zcull query is read back. Labels and references are
written by the FIFO walker into guest memory; they are not WCB copies.

### What the walker waits on

For a `REQUEST` (the common case), `eng_wcb_copy_surface` (`:3486`) does:

1. `g.be->submit_and_wait(RSX_BE_FLUSH_GUEST_REFERENCE)` — flush the open draw
   batch and wait on its fence (`vk_eng_submit_and_wait`,
   `rsx_vulkan_backend.c:3088` -> `vk_eng_flush`, `:4887` ->
   `vk_submit_and_wait`, `:841`);
2. `g.be->readback(...)` — begin a new command buffer, an image barrier, a
   `vkCmdCopyImageToBuffer` into a host-visible staging buffer, a host-read
   barrier, then `vk_submit_and_wait` again (`:3247`);
3. `vk_invalidate` + a row `memcpy` into guest memory, through the page
   protection (`rsx_wcb_store`).

So a single 4 KiB readback is **two `vkQueueSubmit` + `vkWaitForFences` round
trips**; no queue-idle wait and no per-row conversion for RGBA8. The first
wait is the one that matters: it waits for every draw recorded since the last
flush, which is most of the frame. The second (the copy's own submission) is
small. That split is what the two cuts below isolate.

### The minimum ordering the guest requires

The fault is synchronous: the guest is executing the load/store that needs the
bytes. Those bytes are the render as of the last sync point the guest passed
(`SET_REFERENCE` / `SEMAPHORE_RELEASE` / flip, where
`rsx_draw_engine_flush_dirty_to_guest`, `:6950`, runs). The title does not
read a target and then let the RSX redraw it before using the bytes; it
renders the exposure chain once, reads it, and uses it for that frame's
lighting. So a copy queued at the sync point is the copy the guest wants; it
does not have to be made at the instant of the fault. That is the opening the
prefetch uses.

The one exception is a read in command-stream order (an NV3089 transfer, a
texture upload over the target): it wants the bytes *now*, and that path keeps
the synchronous copy.

## 2. The crowded-frame cost, from the live recording

The 40 ms recording is one user play session (instant replay, zoomed out):
1,930 frames >= 40 ms, 14,260 walker drains, 18,060 `rsx.wcb_done` events
(`PS3_TR_WCB_DONE`, `rsx_draw_engine.c:1277`, a = KiB, b = duration us).
Analysing it directly:

| metric | value |
|---|---:|
| copies per hitch frame | p50 **10**, p90 11, p99 28, max 100 |
| copy+sync per hitch frame | p50 **13.47 ms**, p90 31.59, p99 44.59, mean 15.54 |
| copies <= 4 KiB | **96.0 %** (mean 1.385 ms each) |
| copies of 0-512 B | 69.5 % (p50 58 us, mean 0.43 ms) |
| copies ~4 KiB | 16.4 % (**mean 6.12 ms, p50 3.23 ms**) |
| copies > 1 MiB | 2.9 % (mean 10.97 ms) |
| total WCB time in the file | ~30 s, of which ~18.2 s in the 4 KiB bucket |

The 4 KiB entries are the 32x32 RGBA8 exposure level (pitch 128); the 0-512 B
ones are its 2x2 and 1x1 tips. The representative 86.1 ms frame in
[`c690-serialisation.md`](c690-serialisation.md) has 10 copies and 35.9 ms of
b-duration. The bandwidth is irrelevant; 3 ms for 4 KiB is waiting for the
draws that produced it (and for the batch in front of them).

## 3. Cut 1: `PS3RECOMP_RSX_WCB_STREAM` (neutral)

Live key, default off (`PS3_LIVE_WCB_STREAM`, `ps3_live_ctl.h:44`). In
`eng_wcb_copy_surface` (`:3529`) a demand copy is queued with
`readback_async` at the end of the open draw batch (`vk_eng_readback_async`,
`rsx_vulkan_backend.c:3298`) and fetched immediately (`:3348`), so the copy
and the draws share one submission. `eng_wcb_resolve` (`:3780`) sends
request/fault resolves through the existing request batching as well, so all
waiters of one service pass go to the GPU together. Without the key every
path is the old one.

Meter A/B (`ab_live_wcbstream_20261104_162915`, N=6 x 10 s per arm):

| metric | A off | B stream |
|---|---:|---:|
| fps (pin_ab pooled) | 31.62 | 31.35 |
| paired difference | **+0.59 fps, CI [-1.30, +2.49]** (noise) | |
| p50 / p90 / p99 ms | 34.16 / 41.70 / 48.07 | 34.19 / 41.23 / 47.43 |
| walker / wait ms | 18.69 / 12.67 | 18.76 / 13.17 |
| draws/frame | 584.4 | 594.9 |
| walker us/draw | 32.62 | **32.50 (-0.4 %)** |
| WCB copies/frame | 1.00 | 1.04 |
| WCB copy ms/frame | 1.12 | 1.13 |
| WCB guest wait ms/frame | 1.72 | 2.19 |
| 32x32 copy ms/frame | 0.985 | 1.030 (+4.6 %) |

Verdict: **neutral**. One round trip out of two was not the cost; the barrier
that has to sit in front of an in-batch copy costs about what the saved
submission did. The key stays in the tree (default off) because it is the
control that proves where the time is, not because it is a win.

## 4. Cut 2: `PS3RECOMP_RSX_WCB_PREFETCH` (the one to test)

Live key, default off (`PS3_LIVE_WCB_PREFETCH`). At a sync point, for a
target the guest read within the last 8 frames (`eng_wcb_hot`, `:3453`), the
walker queues a copy into the open batch, then submits that batch without
waiting (`g.be->submit_async`, `rsx_draw_engine.c:3887` ->
`vk_eng_submit_async`, `rsx_vulkan_backend.c:3381` -> `vk_eng_flush_async`,
`:4806`) and keeps recording into the other frame set. The guest's fault later
collects the ticket (`eng_wcb_copy_surface`, `:3520`): the copy is already in
flight or done, and the read only waits for the remainder of that submission.

The first read of a target is still synchronous (no history yet); an NV3089
transfer still copies in stream order; depth targets are unchanged; a target
that is not read is never prefetched. The bytes the guest sees are the target
as of the sync point it just passed, which is exactly what it read; it does
not see a newer render made after the sync point (see the test).

Meter A/B (`ab_live_wcbpre_20261104_163914`, N=6 x 10 s per arm):

| metric | A off | B prefetch |
|---|---:|---:|
| fps (pin_ab pooled) | 32.85 | 32.19 |
| paired difference | **+0.52 fps, CI [-0.88, +1.91]** (noise) | |
| p50 / p90 / p99 ms | 34.16 / 40.36 / 47.52 | 34.17 / 40.99 / 45.71 |
| walker / wait ms | 18.54 / 11.92 | **18.17** / 12.80 |
| draws/frame | 602.4 | 593.2 |
| walker us/draw | 32.39 | **31.04 (-4.2 %)** |
| WCB copies/frame | 2.08 | 2.02 |
| WCB copy ms/frame | 1.18 | **0.17 (-86 %)** |
| WCB guest wait ms/frame | 1.75 | **0.62** (0.70 x 2.50 ms -> 0.67 x 0.93 ms) |
| 32x32 copy ms/frame | 0.955 | **0.117 (-88 %)** |
| 16/8/4/2/1 mips, copy ms/f | 0.088/0.044/0.035/0.030/0.026 | 0.011/0.026/0.008/0.008/0.003 |
| light-probe reads | 3,513 `guestreq`, 12 `ticket` | 2 `guestreq`, **4,221 `ticket`** |
| walker engine phase ms/f | 16.94 | 16.22 |
| c690 lane jobs/s, lanes 0-4 | 3088-3171 | 3085-3187 (unchanged) |

The meter frame is the c690 producer, so fps and p50 are flat (34.16 vs
34.17 ms) and the two arms again sampled slightly different draw counts
(602 vs 593). The mechanism is not flat: the readback work is almost gone
from the walker, the guest's own wait for it fell by 63 %, and the walker is
**-1.35 us/draw** when normalised by draws. The draw-side cost that pays for
it is a few extra submissions and ~+0.36 ms of parse, visible in the phase
table; the net walker mean is still lower.

Where this should pay much more is the crowded replay: 10 copies/frame and
13.5 ms of copy+sync inside a 60-90 ms walker drain. The same tickets queue at
the same sync points and overlap with the rest of the drain.

## 5. Exactness

* `test_color_readback_guest` (CMake target, 85 checks) gained
  `test_stream_mode` and `test_prefetch_mode`. Prefetch asserts that a target
  with no read history is not prefetched, that the next sync point queues and
  submits one copy, that the read is served from that ticket with **no**
  synchronous readback, and that when the target is drawn again before the
  read the guest sees the *sync point's* bytes (the ticket) over the whole
  target. Stream asserts one queue+fetch and no separate submission.
* 30/30 `tools/test_video_fixtures.py` pass.
* The replay gate is unchanged (the replay harness does not exercise WCB, and
  the draw path is untouched): `replay_rsx_engine meter_frame --frames 64`
  still prints last `a6fcc849341eba9b`, gate `ff2a220c28750bc3`.
* No `spu_context` fields were added or moved; `check_spu_context_abi.sh`
  passes (run by `ab_live.sh` before each launch).

## 6. Live per-draw profile (perf, walker thread)

`perf record -t <rsx walker tid> -F 999 -g -- sleep 25` during the first A/B,
pitch meter, 13,033 samples, both arms (the mix is dominated by the same
draw path). Self time, and the same share in us/draw at the A arm's
32.39 us/draw (profiler instrumentation included):

| # | symbol | self % | us/draw | what |
|---:|---|---:|---:|---|
| 1 | `rsx_vertex_remap_build` | 16.31 | 5.28 | dedup of the reference list, every draw |
| 2 | `__memmove_avx_unaligned_erms` | 6.47 | 2.10 | vertex/constant/ring staging copies |
| 3 | `clock_gettime` vdso path (mostly `rsx_wprof_now`, `sink_end_work`) | ~6.7 | ~2.2 | **`PS3RECOMP_WALKER_PROF` instrumentation**, not the shipping path |
| 4 | `sink_end_work` | 5.14 | 1.66 | group setup / state decode |
| 5 | `eng_pipeline_get` | 5.00 | 1.62 | pipeline fingerprint + memo |
| - | `gcm_rsx_process_fifo_unlocked` | 4.90 | 1.59 | FIFO method loop |
| - | `wcb_resolve_span_owner` | 1.72 | 0.56 | the readback fault/service path |
| - | `sink_bind_textures` | 1.64 | 0.53 | per-unit texture bind |
| - | `vk_eng_draw_from` + `vk_eng_write_descriptors_to` | 1.05 + 0.85 | 0.62 | Vulkan recording + descriptors |

* **Locks are not a cost on the walker.** The largest lock/futex symbol in the
  whole profile is 0.03 % (`pthread_rwlock_wrlock`); the SPU lanes do not
  contend with the walker on its per-draw path at the meter.
* **Vulkan recording/descriptor/texture churn is ~2 %, not the 16-20 us/draw
  gap.** `vk_eng_draw_from` 1.05 %, `vk_eng_write_descriptors_to` 0.85 %,
  `eng_texture_slot` 0.26 %. The offline-vs-live gap in
  [`walker-per-draw.md`](walker-per-draw.md) is not one big symbol; it is the
  same CPU work plus texture/vertex traffic spread over `memmove` and the
  hashes.
* **The instrumentation is visible:** `WALKER_PROF` timestamps are ~6.7 % of
  the walker samples. A production run would be ~1-2 us/draw cheaper than the
  numbers above.
* The top lever remains the remap build (5.3 us/draw). The earlier offline
  memo is still neutral live; this mission did not retry it.

## 7. User command: zoomed-out replay test

Boot to a match, enter the instant replay, zoom out until the whole field is
on screen, and record 2-3 minutes per arm. Two runs, because the key is an
environment variable and each `play_windowed.sh` is one game instance.

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A: play defaults, prefetch off. 40 ms hitch recording on.
PS3RECOMP_TIMELINE=/tmp/wcbpre_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_RSX_WCB_PREFETCH=0 \
./play_windowed.sh

# Arm B: same shot, prefetch on.
PS3RECOMP_TIMELINE=/tmp/wcbpre_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_RSX_WCB_PREFETCH=1 \
./play_windowed.sh
```

`play_windowed.sh` already sets `PS3RECOMP_RSX_WCB=2` and
`PS3RECOMP_RSX_WCB_STATS=60`; the key passes through. Then compare:

```sh
cd /home/m/Documents/mlb16-online
for f in /tmp/wcbpre_[AB]_*.pstl; do
  echo "== $f"
  python3 mlb16-recomp/tools/hitch_table.py "$f" \
    --min-ms 40 --bins --classify --baseline-jobs 680 --top 20
done
grep -aE '\[rsx wcb\]|\[walker-prof\].*draws=[1-9]' \
  recomp-work/play_*/play_*.log | tail -80
```

What to look for: the `[rsx wcb]` line in B should show
`async issued ≈ fetched` and a much smaller `copy ... ms` per frame; the
`[walker-prof]` `draws=` should be unchanged and `walker=` lower in the
crowded frames; the 80-250 ms band of `hitch_table.py` should shrink if the
readback was on the critical path. If B is not clearly better in the replay,
leave the key off (it is already off by default).

## 8. Risks and caveats

* **Semantics:** prefetch reads the target as of the sync point, not at the
  instant of the fault (the existing WCB=3 contract). For a title that lets
  the RSX redraw a target between the label and the CPU read and expects the
  later bytes, that would differ. MLB's exposure/face/crowd reads are
  render-once-then-read, so this is the intended value. `STREAM` and the
  baseline stay exact to the touch.
* **Extra submissions:** one asynchronous submit per frame when a hot target
  was drawn since the last one, plus one per sync point where a target moved.
  With `VK_EB_SETS=2`, a second submit before the first completes makes the
  walker wait in `vk_ebs_swap`; the measured meter result already includes
  that. Without `VK_FRAME_PIPELINE`, `vk_eng_submit_async` is a no-op (it
  never turns a sync point into a stall), so the key does nothing.
* **Prefetch is speculative:** a hot target that the guest skips this frame
  still gets one GPU copy (0-4 KiB typical). `async_discarded` counts the
  tickets dropped unread; in the meter run it stayed 0.00/frame.
* **Meter p50 is c690**, so a readback win cannot show up as fps there. Judge
  this key on the crowded replay's walker drain and hitch bands, not on the
  meter p50.
* **Two launches, 12 segments each**; the arms sampled 1-2 % different draw
  counts, so the frame-level fps is only a noise check. The readback numbers
  are per copy/per frame and do not depend on that.
* `perf` sampled both arms (the A/B rotates every 10 s); the symbol mix is
  dominated by the shared draw path, not by the readback key.
* The crowded-replay analysis is one recording, one build; the 4 KiB bucket
  identification comes from size/format (32x32 RGBA8 pitch 128) and the
  meter's light probe, not from a per-address trace in the replay.

## 9. Reproduce

```sh
# unit tests
cd /home/m/Documents/mlb16-online/recomp-work/build-play
ninja test_color_readback_guest && ./ps3recomp_sdk/test_color_readback_guest
cd /home/m/Documents/mlb16-online/ps3recomp-play && python3 tools/test_video_fixtures.py

# replay gate
cd /home/m/Documents/mlb16-online/recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10

# meter A/B (stream, then prefetch)
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=wcbstream METER=1 PIN=1 WCB=2 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
  PITCH_THRESH=35 LIMIT=900 SETTLE=10 \
  PS3RECOMP_RSX_WCB_STATS=60 PS3RECOMP_RSX_WCB_ATTR=1 PS3RECOMP_LIGHT_PROBE=1 \
  VK_EB_STATS=1 ARM_A="PS3RECOMP_RSX_WCB_STREAM=0" \
  ARM_B="PS3RECOMP_RSX_WCB_STREAM=1" BOOT_ARM="PS3RECOMP_RSX_WCB_STREAM=0" \
  ./ab_live.sh
TAG=wcbpre METER=1 PIN=1 WCB=2 N=6 SEG=10 DROP=2 VIDEODRIVER=x11 \
  PITCH_THRESH=35 LIMIT=900 SETTLE=10 \
  PS3RECOMP_RSX_WCB_STATS=60 PS3RECOMP_RSX_WCB_ATTR=1 PS3RECOMP_LIGHT_PROBE=1 \
  VK_EB_STATS=1 ARM_A="PS3RECOMP_RSX_WCB_PREFETCH=0" \
  ARM_B="PS3RECOMP_RSX_WCB_PREFETCH=1" BOOT_ARM="PS3RECOMP_RSX_WCB_PREFETCH=0" \
  ./ab_live.sh
```

## 10. Clean room

This is our own runtime, trace stream, replay captures and EBOOT. The
deferred/coalesced readback idea was checked first against the MIT reference
ports: the inFAMOUS 2 notes' frame-pipelining/deferred-writeback design
(`docs/NOTES.md`, "the small-target writeback ... is now recorded into the
frame's own batch before the present, and the rows reach guest memory when
that batch completes") and the runtime's own `wcb-async.md`. No RPCS3 or
other GPL emulator source was read or cited.
