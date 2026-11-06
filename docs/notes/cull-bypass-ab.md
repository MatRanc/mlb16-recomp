# Cull bypass A/B: skipping 0x25/0x26 removes the holes but costs 7.6 fps

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh`/`ab_live.sh`
configuration (`TEX_DIRTY`, `PIPE_MEMO`, `REMAP_FAST`, the four cheap wins, the
shader disk cache, the hitch recorder, the c690 cull union merged but off) plus
`METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`, `PIN=0`, `WCB=2`, x11, governor
`performance`. One launch (`ab_live_cullab_20261104_003029`): 18 segments
x 10 s, 2 s dropped, three arms rotating A/B/C x 6.

## TL;DR

* `MLB16_CULL_BYPASS` is now a **live key** (`PS3_LIVE_CULL_BYPASS`, value 0-3,
  environment fallback, inert at 0). `tools/c690_ref/patch_cull_hooks.py`
  installs the helper and its three dispatch guards reproducibly, including on
  a fresh lift.
* **The bypass is not free.** Relative to A, B (`=3`) saves **3.2 ms of c690
  burst wall** (36.09 -> 32.91 ms) and **15 % of c690 job wall**, but it draws
  **2.07x the vertices** and **1.28x the draw calls**. The walker drain rises
  **16.38 -> 22.46 ms** and the frame gets slower: **35.35 -> 27.79 fps**
  (paired **-7.56 +/- 0.50 fps** per segment), p50 **34.15 -> 38.12 ms**, p90
  **40.44 -> 45.12**, p99 **43.79 -> 48.73**.
* The frame does end on the c690 burst, but removing cull work moves the
  critical chain straight into the walker's extra geometry. It is not neutral
  and it is not faster.
* **Recommendation: leave `MLB16_CULL_BYPASS` off in both `play_windowed.sh`
  copies and in `ab_live.sh`.** It fails both gates (< 1 fps and no p90/p99
  regression) by a wide margin. The union fix stays the safe holes fix; its
  cost is smaller (**-4.8 fps**, p50 unchanged) and it remains default off.
* `PITCH_THRESH=35` is now the `ab_live.sh` default (25 never confirmed the
  pitch with the cube render targets).

## 1. The live key

The bypass used to be a one-shot `getenv` read inside the generated c690 tree,
so it disappeared on a relift and could not be flipped mid-run. It is now the
same pattern as the other c690 switches:

| layer | change |
|---|---|
| `libs/video/ps3_live_ctl.h/.c` | new key `PS3_LIVE_CULL_BYPASS`, name `MLB16_CULL_BYPASS` |
| `runtime/spu/c690_cull_fix.c` | `c690_cull_bypass_mode()`: live value while `PS3RECOMP_CTL` is configured, otherwise the environment value read once |
| `tools/c690_ref/patch_cull_hooks.py` | installs the generated helper and the three guards; migrates the old helper; inserts both on a fresh lift; idempotent |

Values: **0** normal (default, inert), **1** both families, **2** 0x2C only,
**3** 0x25/0x26 only. The guards sit in the 0x25/0x26 handlers (`0x13518`,
`0x13520`) and the 0x2C handler (`0x12DC0`), ahead of the cull body call. The
command opcode is still counted; only the cull body is skipped, which is what
the old switch did.

Reproduce after a relift:

```sh
python3 tools/c690_ref/patch_cull_hooks.py \
  ../../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c
ninja -C ../../recomp-work/build-play
```

The run below proves the key is live: the control file logs
`MLB16_CULL_BYPASS=3 (was 0)` at every B switch, and the c690 job wall drops
exactly in those windows. With the key at 0 the c690 command mix is unchanged
(0x25 3.7 %, 0x26 1.2 %, 0x2C 0.0 %), as documented in
[`edge-native-step3.md`](edge-native-step3.md).

## 2. Run and arms

One launch, meter confirmed at 148 s. Arms, all naming both keys (the control
file does not revert a key an arm omits):

| arm | control file | meaning |
|---|---|---|
| A | `MLB16_CULL_BYPASS=0; PS3RECOMP_C690_CULL_FIX=0` | baseline |
| B | `MLB16_CULL_BYPASS=3; PS3RECOMP_C690_CULL_FIX=0` | bypass 0x25/0x26 |
| C | `MLB16_CULL_BYPASS=0; PS3RECOMP_C690_CULL_FIX=1` | union fix, same run |

`PS3RECOMP_C690_GAP=1` and `PS3RECOMP_WALKER_PROF2=1` were on for all arms. A
full `PS3RECOMP_TIMELINE` window (190 s, armed at the first switch) recorded
2,652,631 c690 job spans, 5,710 `gfx.frame_end` events and 3,930 c690 batches
(union of the lane spans split at 1 ms), which is where the burst-wall numbers
come from. The run ended 183 s after the first switch, so the last < 1 s of the
window is missing; every measured segment is complete.

## 3. Per-arm results

Per-flip `fin` is the walker executing the flip; fps is frames per
(segment - drop). MangoHud fps is the CSV **row rate** in the same window (its
instantaneous `fps` column is noisy; the row rate agrees with the per-flip
count to within ~0.5 fps). `c690 job wall` is the sum of all lane job time per second;
`burst wall` is the mean c690 batch wall from the timeline.

| metric | A (off) | B (bypass 3) | C (union) |
|---|---:|---:|---:|
| frames | 1,697 | 1,334 | 1,465 |
| fps, per-flip | **35.35** | **27.79** | **30.52** |
| fps, MangoHud rows | 35.31 | 27.35 | 30.31 |
| p50 ms | **34.15** | **38.12** | **34.15** |
| p90 ms | 40.44 | 45.12 | 50.45 |
| p99 ms | 43.79 | 48.73 | 54.39 |
| std ms | 11.35 | 10.00 | 13.94 |
| mean ms | 28.28 | 36.00 | 32.76 |
| walker drain ms | 16.38 | 22.46 | 17.24 |
| walker wait ms | 11.72 | 14.19 | 15.51 |
| draws / frame | 53.93 | 69.23 | 57.83 |
| vertices / frame | 6,267.5 | 12,937.8 | 6,681.0 |
| c690 job wall (lane-s/s) | 2.69 | 2.30 | 3.42 |
| c690 burst wall mean ms | 36.09 | 32.91 | 45.83 |
| c690 burst p90 ms | 38.78 | 35.58 | 48.97 |
| c690 lane busy | 0.539 | 0.462 | 0.684 |

Paired per-segment fps differences: **B-A -7.56 +/- 0.50 (SE)**,
**C-A -4.83 +/- 0.24**. The paired form removes most of the slow scene drift;
both costs are far outside the noise.

### 3.1 The scene drifts, and B improves with it

B's extra geometry is not constant. Its extra vertices fall from 13.5-13.8 k in
the first three B segments to 11.6-12.6 k in the last three, and its p50 falls
from 41.2 to 34.2-35.9 ms. A is flat (6.1-6.4 k vertices, p50 34.15 every
segment), so this is the bypassed geometry, not a global scene change. Even in
B's best window (segments 10, 13, 16) it is still slower than the adjacent A
segments:

| late window (3 pairs) | A | B |
|---|---:|---:|
| fps | 35.46 | 28.96 (-6.50) |
| p50 ms | 34.15 | 35.00 |
| p90 ms | 40.34 | 44.17 |
| vertices / frame | ~6.2 k | ~12.2 k |

So the conclusion does not depend on the early, heavier B segments.

### 3.2 Per-flip 2 ms histogram

Bins are 8, 10, ... 52 ms; the last column is > 52 ms.

| arm | 8 | 10 | 12 | 14 | 16 | 18 | 20 | 22 | 24 | 26 | 28 | 30 | 32 | 34 | 36 | 38 | 40 | 42 | 44 | 46 | 48 | 50 | 52 | 52+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 46 | 44 | 113 | 155 | 146 | 56 | 16 | 4 | 0 | 1 | 0 | 0 | 1 | **635** | 85 | 144 | 125 | 66 | 10 | 2 | 0 | 0 | 0 | 1 |
| B | 4 | 6 | 8 | 17 | 31 | 57 | 44 | 33 | 34 | 21 | 10 | 3 | 5 | **324** | 54 | 41 | 150 | 263 | 132 | 60 | 19 | 2 | 0 | 3 |
| C | 0 | 11 | 110 | 143 | 131 | 87 | 13 | 6 | 2 | 0 | 0 | 0 | 0 | **502** | 1 | 0 | 0 | 0 | 39 | 96 | 153 | 109 | 0 | 62 |

A's short frames (8-16 ms, main's own ref cycle) mostly disappear under B;
B has a second mode at 40-48 ms. C keeps A's p50 bin but loses everything above
42 ms into the 44-52 ms tail.

### 3.3 Why B is slower although the burst is shorter

The timeline shows the cull body really is skipped: c690 job wall falls 15 %
and the burst wall 3.2 ms. But the same command list without the screen cull
adds triangles to the walker:

* vertices/frame **6,267 -> 12,938** (+106 %), draws **53.9 -> 69.2** (+28 %);
* walker drain **16.38 -> 22.46 ms** (+6.1 ms), wait **11.72 -> 14.19 ms**;
* the frame's long class moves from c690-bound to walker-bound.

The 3.2 ms saved on the producer is spent twice over in the drain, so p50 and
the tail both rise. This is the mirror image of
[`frame-timeline-2.md`](frame-timeline-2.md): that note said the walker is not
the p50 lever because the frame ends on the c690 burst, but adding walker work
still lengthens the frame once it exceeds the burst.

C behaves exactly like the union fix in [`edge-holes-fix.md`](edge-holes-fix.md)
§6: p50 is unchanged, the c690 work rises (job wall **+27 %**, burst wall
**+9.7 ms**), the vertices barely move (+6.6 %), and the loss is a heavier tail
(p90/p99 **50.45/54.39 ms**). The same-run cost here is **-4.83 fps**, close to
the earlier **-3.70 +/- 0.86 fps**.

## 4. Recommendation

**Leave `MLB16_CULL_BYPASS` off.** It fails the stated gate:

| gate | result |
|---|---|
| costs < 1 fps | **no: -7.56 fps** |
| p90 / p99 do not worsen | **no: 40.44/43.79 -> 45.12/48.73 ms** |

The switch remains available as a live diagnostic (it is inert at 0) and is not
enabled in either `play_windowed.sh` or `ab_live.sh`. `PITCH_THRESH=35` is now
the `ab_live.sh` default. The union fix also stays off by default; at -4.8 fps
it is the smaller cost but still not a shipping default.

## 5. Reproduce

```sh
# patch the generated module (idempotent; also works after a fresh lift)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/c690_ref/patch_cull_hooks.py
ninja -C ../recomp-work/build-play

# one launch, 3 arms, live keys
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=cullab PITCH_THRESH=35 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  PS3RECOMP_C690_GAP=1 PS3RECOMP_WALKER_PROF2=1 \
  ARMS='A=MLB16_CULL_BYPASS=0;PS3RECOMP_C690_CULL_FIX=0|B=MLB16_CULL_BYPASS=3;PS3RECOMP_C690_CULL_FIX=0|C=MLB16_CULL_BYPASS=0;PS3RECOMP_C690_CULL_FIX=1' \
  BOOT_ARM='MLB16_CULL_BYPASS=0;PS3RECOMP_C690_CULL_FIX=0' \
  ./ab_live.sh

# per-flip/arm table
python3 pin_ab.py --log ../ab_live_cullab_<ts>.log \
  --marks ../ab_live_cullab_<ts>.marks --seg 10 --drop 2
```

The burst-wall numbers came from a full `PS3RECOMP_TIMELINE` window
(`PS3RECOMP_TIMELINE_TRIGGER` touched at the first `switch A`, `SECS=190`,
hitch mode off) and a streaming reader that keeps only c690 job spans and
`gfx.frame_end`, maps monotonic ns to guest ms with the `[flip]`/`[fliplog]`
anchor, and bins 1 ms-gap batches by the run marks. The 1.4 GB binary was
deleted after the analysis; the run folder keeps the log, marks and MangoHud
CSV.

## 6. Caveats and deviations

* **Deadline frames were not captured.** `PS3_LWCOND_STATS` was left off, so
  the `[lwcond-stats]` counter never printed. The Vulkan present path reports
  **0 not-ready drops** in every window; there are three isolated no-slot
  drops (2 at boot, 1 just before the A/B, 1 in an A segment), no frozen
  segment. The run was not repeated: it reached the meter and the hard cap is
  two launches.
* `PIN=0` (the `ab_live.sh` default) rather than the `PIN=1` used in some
  earlier A/Bs. Both arms share it, so the paired comparison is unaffected;
  the absolute A fps (35.35) is higher than the PIN=1 runs.
* The timeline is a diagnostic that runs on all arms. The earlier overhead
  check was +0.03 ms p50 / +0.67 ms mean, far below the 7.6 fps B difference.
* B's extra vertex load drifts down over the run (section 3.1); the late
  window still loses 6.5 fps, so the recommendation is unchanged.
* One launch, one meter window. The meter scene is stable for the A and C
  arms; B's cull result is scene-dependent because it draws back faces.

## 7. Clean room

This is our own reverse engineering of the game's c690 job module and the
captured LS/GPR pairs. The stage-split/corpus method follows NHL Legacy Native
(MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied. No other emulator source was
read or cited.
