# The pitch meter is guest-work bound: main work plus the FIFO producer, not a 30 fps cap

Date: 2026-11-02 (late). Branch `exp/live-ab` in `ps3recomp-play`, built as
`recomp-work/build-play`. Windowed streamed runs: `ab_live.sh`, pitch meter
(X once), `METER=1 PIN=1`, x11, `N=6`, 10 s segments, 2 s dropped,
`GCM_FLIPCOUNT=1`, `PS3RECOMP_WALKER_PROF=1`, WCB=0 and WCB=2 as noted.
A 45 s `PS3RECOMP_TRACE` window was armed at the first arm switch in each run
(`PS3RECOMP_TRACE_SECS=45`, `PS3RECOMP_TRACE_MIN_US=150`). All numbers are one
game launch per run.

## TL;DR

* **It is (1), the guest side, not (2) a 30 fps target and not (3) our
  tick/flip path.** The title never calls `cellGcmSetFlipMode`, never imports
  `cellGcmGetVBlankCount` or `cellGcmSetVBlankFrequency`, and its vblank
  handler only bumps counters nobody reads. `Gfx_FrameEnd` lets the main run
  three frames ahead of the display flip and `Gfx_DisplayFlip` flips
  continuously; there is no two-vblank wait or flip-interval setting to find.
* **Halving the display tick does not raise fps; it lowers it.** WCB=0,
  both arms `EXP_NO_DRAW=1`: **31.32 fps at 16 ms** vs **28.05 fps at 8 ms**
  (paired +3.27, CI +2.30..+4.23). WCB=2: **28.89** vs **26.85** (paired
  +2.04, CI +0.47..+3.62). Vblank ticks per flip go **2.05 -> 4.48** (WCB=0)
  and **2.32 -> 4.68** (WCB=2) while the *wall* frame stays 32-38 ms: the frame
  is not tick-quantized. The 8 ms arm also adds walker time (+0.7 ms WCB=0,
  +3.4 ms WCB=2).
* **The PPU main's own work is about one tick (p50 14.7 ms WCB=0, 18.8 ms
  WCB=2), and the rest is the buffer-ref handoff in `FUN_000655cc`.** While
  main is blocked there, with NO_DRAW the RSX walker drains only **0.97 ms**
  and waits **16.8 ms for a producer**; a sample 13.8 ms ref wait contains
  **954 `gcm.spu_put` instants** (the SPU mesh producer). With WCB=2 the same
  wait is **16.8 ms of walker draw drain** and 4.3 ms of producer wait. So
  removing draws moves the wait from the walker to the producer, which is why
  `EXP_NO_DRAW` changes the meter by ~0 fps.
* **Benchmark consequence:** the meter is not a usable scene for judging a
  draw fast path. Use the pitch-wait / gameplay, where the guest asks for the
  next flip sooner (documented NO_DRAW ceiling 41 fps). The meter's "two
  ticks" is arithmetic (30 fps x 16 ms = 2), not a title setting.

## 1. Static: how the title paces itself

Ghidra (EBOOT `a58eb36f...`) on the import thunks and the render code:

* `cellGcmSetFlipMode` (import thunk `0x00f29d74`) has **no call site**. The
  only xrefs are the thunk itself and its descriptor (`0x010b171c`).
  `cellGcmSetVBlankFrequency` and `cellGcmGetVBlankCount` are not imported at
  all. So the title neither selects a flip mode nor counts vblanks itself.
* The vblank handler `FUN_00069288` (`0x00069288`) only does
  `DAT_011203e4++; DAT_011203e8 = now; DAT_011203f0 = now + DAT_011203f8;`
  (`DAT_011203f8` is 16683 us, or 20000 us in the `0x240` video mode, set by
  `FUN_00064b68`). `DAT_011203f0` has **no reader**, so this is a dead
  deadline, not a frame timer.
* `Gfx_FrameEnd` (`0x00066530`) queues the display flip and then waits only
  for the **flip queue depth**: `DAT_01120410` is set to 2 in
  `FUN_00067734` (0x00068... init), and the wait is
  `while (DAT_01120410 + 1 < submitted - completed) Cond_Wait(...)`, i.e. the
  main may be **3 frames ahead**. That is a pipeline depth, not a frame-period
  setting.
* `Gfx_DisplayFlip` (`0x000692dc`) loops: wait for a queued flip, wait for
  the buffer ref, `cellGcmResetFlipStatus`, `cellGcmSetFlipImmediate`, then
  wait for the flip to complete at the next vblank tick. It flips as fast as
  flips complete; there is no "every second vblank" branch.
* The metered title/pitch-wait and the meter run the **same** path
  (`Gfx_FrameEnd` -> `FUN_00065760` put writer -> `FUN_000655cc` ref wait,
  and the same DisplayFlip thread). There is no scene-specific pacing switch.
  The difference between the two scenes is workload, not policy: at the
  pitch the walker drains in ~17 ms (`docs/ppu-critical-path.md`), on the
  meter the guest producer/walker mix is different (below).

The `FUN_000655cc` ref wait is the only per-frame PPU block. It waits for the
RSX ref of the buffer about to be reused; the RSX ref is published by the
walker as it consumes the FIFO.

## 2. Instrumentation added on `exp/live-ab`

* `PS3_TR_GCM_FLIP_VB` (`include/ps3emu/ps3_trace.h`): an instant
  `gcm.flip_vb` with `a = flip request count`, `b = s_vblank_count`, emitted
  from `gcm_flip_request`. This is what makes "vblank ticks per flip"
  measurable per frame.
* The `[flip]` line now appends `vc=<s_vblank_count>` (same cadence as
  before; no new flag).
* `mlb16-recomp/tools/analyse_meter_pacing.py`: streams a trace (the full SPU
  trace is ~5M lines / 0.6-0.8 GB), keeps only the PPU wait spans, walker
  spans and flips, splits frames into the in-run arms using `ab_live.sh`'s
  `.marks` and the trigger wall time, and prints the per-arm table below.
  The boot thread's `thread_name` event predates trace init, so it appears as
  `host thread 8`; the tool picks the track that blocks on lwcond
  `0x1245090` (the ref wait) as the main.

The trace adds no measurable fps cost at this resolution (the traced arms
land on the same per-segment fps as the untraced report).

## 3. Run 1: WCB=0, both arms `EXP_NO_DRAW=1`

A = NO_DRAW + 16 ms tick, B = NO_DRAW + 8 ms tick. `BOOT_ARM` drew the boot.

Full report (12 segments, 2 s drop):

| arm | fps | walker ms | wait ms |
|---|---:|---:|---:|
| A 16 ms | **31.32 +/- 1.06** | 3.63 +/- 0.26 | 28.26 +/- 0.94 |
| B 8 ms | **28.05 +/- 0.43** | 4.37 +/- 0.06 | 31.25 +/- 0.55 |

Paired A-B **+3.27 fps** (95 % CI +2.30..+4.23). The 8 ms tick is slower.

Trace window (45 s from the first switch, 2 s switch drop; A0 B0 A1 B1 and
part of A2; 579 A / 453 B frames):

| per frame (ms) | A 16 ms | B 8 ms |
|---|---:|---:|
| frame (flip to flip), mean / p50 | 32.34 / 34.34 | 36.24 / 35.99 |
| ppu main blocked | 19.57 | 21.04 |
| ppu main running | 12.77 (p50 14.66) | 15.20 (p50 15.13) |
| ref wait `lwcond 0x1245090` | 18.87 (p50 18.97) | 20.20 (p50 19.89) |
| — of which walker drain | **0.97** | 1.09 |
| — of which walker producer wait | **16.80** | 17.71 |
| rsx walker drain (total) | 2.39 | 2.68 |
| rsx walker put_wait | 28.65 | 31.73 |
| vblank ticks per flip | **2.05** | **4.48** |
| fps in trace window | 30.92 | 27.60 |

The main's own block is the ref wait, and with draws removed the walker is
**not draining** during it: 0.97 ms of the 18.87 ms is drain, 16.80 ms is the
walker waiting for a producer. In one sample frame (13.8 ms ref wait) 954
`gcm.spu_put` instants land, i.e. the walker is being fed by the SPU mesh
producer. Removing 15 ms of draw work therefore does not shorten the frame;
it exposes the producer wait that was previously overlapped by draws.

First attempt note: the first run (`ab_live_pacing_r1_nodraw_20261102_171413`)
had the trace trigger armed 180 s too early by the wrapper and produced no
measurement-window trace. It is not used for the breakdown; its untraced A/B
(29.51 vs 28.22 fps) points the same way and is kept only as a sanity check.

## 4. Run 2: WCB=2, A = 16 ms, B = 8 ms

Full report:

| arm | fps | walker ms | wait ms |
|---|---:|---:|---:|
| A 16 ms | **28.89 +/- 1.77** | 26.90 +/- 1.10 | 7.81 +/- 1.60 |
| B 8 ms | **26.85 +/- 0.56** | 29.88 +/- 0.74 | 7.42 +/- 0.17 |

Paired A-B **+2.04 fps** (95 % CI +0.47..+3.62). Again the 8 ms tick is slower,
and the walker is 3 ms slower.

Trace window (512 A / 434 B frames):

| per frame (ms) | A 16 ms | B 8 ms |
|---|---:|---:|
| frame, mean / p50 | 36.69 / 33.92 | 37.75 / 34.35 |
| ppu main blocked | 21.75 | 22.70 |
| ppu main running | 14.94 (p50 18.82) | 15.05 (p50 19.08) |
| ref wait `lwcond 0x1245090` | 21.15 (p50 21.10) | 22.03 (p50 22.44) |
| — of which walker drain | **16.83** | 18.92 |
| — of which walker producer wait | 4.26 | 3.05 |
| rsx walker drain (total) | 26.83 | 30.25 |
| rsx walker put_wait | 9.78 | 7.42 |
| vblank ticks per flip | **2.32** | **4.68** |
| fps in trace window | 27.26 | 26.49 |

With WCB=2 the same ref wait is mostly **walker draw execution** (16.8 of
21.2 ms), and the total drain (26.8 ms) is the frame's largest term. That is
the scene `docs/meter-scene-baseline.md` already ranks for a draw fast path
(mesh 11.6, tex 9.8, main ring 10.2 ms). Yet the 8 ms tick still does not
help: the frame stays ~35-38 ms and the extra drain eats the tick's
potential.

## 5. Which of (1)/(2)/(3)

* **(2) is out.** There is no flip-interval or vblank-count pacing in the
  title, no `cellGcmSetFlipMode` call, no two-vblank loop, and the main is
  allowed to run three flips ahead. If the game wanted 30 fps it would
  throttle flips; it does not.
* **(3) is out.** Our tick is not adding the missing tick: halving it leaves
  the wall frame at 32-38 ms (vblank/flip doubles), and it makes the walker
  slower. The extra time is in the guest ref/producer handoff, not in the
  flip completion or handler path.
* **(1) is the answer, with the guest producer named.** The PPU main's own
  work is p50 14.7 ms (WCB=0) / 18.8 ms (WCB=2), about one 16.7 ms tick; the
  other 19-21 ms is the `FUN_000655cc` ref wait. In NO_DRAW that wait is 89 %
  walker producer starvation (SPU mesh PUTs); with WCB=2 it is 80 % walker
  draw drain. Either way the frame length is set by guest work feeding the
  FIFO, not by a display-rate policy.

**What fps would a PS3/RPCS3 show there?** This session did not run the
console or RPCS3 (the run budget was the two recomp A/Bs), so the report
does not claim a number for them. What the static path does say is that the
meter uses the ordinary gameplay render/flip path with no menu-specific cap,
so a faster guest pipeline asks for flips sooner; the recomp is slow because
its guest/SPU producer is slow here, not because the title asked for 30 fps.
The right benchmark for the draw work is therefore the scene where the guest
already asks for flips sooner: the pitch-wait / ball-in-play (NO_DRAW ceiling
41 fps), not the stable meter.

## 6. Reproduce

The trace must be armed before the game starts and its 45 s window started at
the first arm switch. This wrapper does that; it waits for `ab_live.sh` to
write its first `.marks` line, then touches the trigger and records the wall
time for the offline arm split.

```sh
cat > /tmp/run_pacing_ab.sh <<'EOF'
#!/bin/bash
set -u
TAG=$1; SECS=${2:-45}; shift 2
W=/home/m/Documents/mlb16-online/recomp-work
export PS3RECOMP_TRACE=/tmp/pacing_${TAG}.json
export PS3RECOMP_TRACE_TRIGGER=/tmp/pacing_${TAG}.go
export PS3RECOMP_TRACE_SECS=$SECS
export PS3RECOMP_TRACE_RING_MB=16 PS3RECOMP_TRACE_FLUSH_MB=8
export PS3RECOMP_TRACE_MIN_US=150
rm -f "$PS3RECOMP_TRACE" "$PS3RECOMP_TRACE_TRIGGER" /tmp/pacing_${TAG}.origin
cd "$W/ab-tools" || exit 1
./ab_live.sh "$@" &
AP=$!
M=""
for _ in $(seq 1 3600); do
    M=$(ls -t "$W"/ab_live_${TAG}_*.marks 2>/dev/null | head -1)
    if [ -n "$M" ] && [ -s "$M" ]; then break; fi
    kill -0 "$AP" 2>/dev/null || break
    sleep 0.2
done
T0=$(date +%s%3N); touch "$PS3RECOMP_TRACE_TRIGGER"
printf '%s\n' "$T0" > /tmp/pacing_${TAG}.origin
wait "$AP"
EOF
chmod +x /tmp/run_pacing_ab.sh

cd /home/m/Documents/mlb16-online

# Run 1: NO_DRAW vs NO_DRAW + 8 ms, WCB=0
TAG=pacing_r1b_nodraw METER=1 PIN=1 WCB=0 N=6 SEG=10 DROP=2 \
  BOOT_ARM=$'EXP_NO_DRAW=0\nEXP_TICK_US=16000' \
  ARM_A=$'EXP_NO_DRAW=1\nEXP_TICK_US=16000' \
  ARM_B=$'EXP_NO_DRAW=1\nEXP_TICK_US=8000' \
  bash /tmp/run_pacing_ab.sh pacing_r1b_nodraw 45

# Run 2: WCB=2 baseline vs 8 ms
TAG=pacing_r2_wcb2 METER=1 PIN=1 WCB=2 N=6 SEG=10 DROP=2 \
  ARM_A=$'EXP_TICK_US=16000' ARM_B=$'EXP_TICK_US=8000' \
  bash /tmp/run_pacing_ab.sh pacing_r2_wcb2 45

# breakdown (origin-ms is /tmp/pacing_<tag>.origin)
R=recomp-work/ab_live_pacing_r1b_nodraw_20261102_172146
python3 mlb16-recomp/tools/analyse_meter_pacing.py \
  /tmp/pacing_pacing_r1b_nodraw.json --marks $R.marks \
  --origin-ms 1793658377099 \
  --md $R/pacing_summary.md --csv $R/pacing_per_frame.csv
```

The per-frame CSVs and summary tables are saved next to each run:
`recomp-work/ab_live_pacing_r1b_nodraw_20261102_172146/pacing_{per_frame.csv,summary.md}`
and `.../ab_live_pacing_r2_wcb2_20261102_172832/pacing_{per_frame.csv,summary.md}`.

## Frametime PNGs and contact sheets

* Run 1 (WCB=0, NO_DRAW vs 8 ms):
  `recomp-work/ab_live_pacing_r1b_nodraw_20261102_172146_ft.png`;
  `recomp-work/ab_live_pacing_r1b_nodraw_20261102_172146/scene_sheet.png`.
* Run 2 (WCB=2, 16 ms vs 8 ms):
  `recomp-work/ab_live_pacing_r2_wcb2_20261102_172832_ft.png`;
  `recomp-work/ab_live_pacing_r2_wcb2_20261102_172832/scene_sheet.png`.
* First (infra-failed) Run 1 attempt, for the retry record:
  `recomp-work/ab_live_pacing_r1_nodraw_20261102_171413_ft.png`;
  `recomp-work/ab_live_pacing_r1_nodraw_20261102_171413/scene_sheet.png`.

Read the PNGs as before: the green line is the host present interval, the
number under each band is the guest `[flip]` rate. The B (8 ms) bands are not
faster; the guest fps is lower in both runs.

## Caveats

* One run per arm pair. The trace window is 45 s, so its per-arm fps covers
  four segments, not all twelve; the full-run fps comes from `[flip]` and
  matches the trace direction.
* `EXP_NO_DRAW` leaves a stale image, so the contact sheet is the scene proof,
  not the B-arm render.
* The walker can run on more than one host thread, so summing raw `put_wait`
  spans overcounts wall time; the per-frame table unions them and the
  `ref_drain` / `ref_putwait` split is a union inside the main's ref wait.
* The boot thread's trace name is missing (named before trace init); main is
  identified by the `0x1245090` block. The pitch-wait comparison in the text
  is from `docs/ppu-critical-path.md`, not a new run.
* Numerical differences of ~2 fps between the traced and untraced reports are
  within the trace's small overhead and the meter's own warm-up drift.
