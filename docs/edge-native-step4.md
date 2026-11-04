# Native c690 screen cull, step 4: exact 0xD080 stage, 1.5x offline; the union still costs ~5 fps

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Standard `play_windowed.sh`/`ab_live.sh`
configuration (`TEX_DIRTY`, `PIPE_MEMO`, `REMAP_FAST`, the four cheap wins,
the shader disk cache, the hitch recorder, the c690 union merged but off) plus
`METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`, `PIN=0`, `WCB=2`, x11, governor
`performance`. Three launches: a shadow/live soak, the A/B at the meter, and a
timeline mapping run for the c690 burst wall. Native Edge is approved by the
user.

This follows [`edge-native-step3.md`](edge-native-step3.md),
[`edge-holes-fix.md`](edge-holes-fix.md) and
[`cull-bypass-ab.md`](cull-bypass-ab.md).

## TL;DR

* **The 0xD080 subtree is flattened and bit-exact.** A 24-block tail-call
  state machine (0xD080 through its `SPU_RET`) becomes one C function. On the
  400 captured cull pairs the native mode 0 reproduces the **full local store,
  all 128 GPRs and the return PC exactly** (0/400 differences). Live shadow
  compared **884,058 calls with 0 mismatches**, and 1.69 M live calls had **0
  fallbacks**.
* **It is 1.53x faster on the whole 0x132E0 command body offline** (22.9 ->
  15.0 us/call). The rest of the command (WWS ring helpers 0x49D4/0xE6B8)
  stays lifted; only the cull subtree is native.
* **At the meter it is neutral.** A/B (N=6 x 10 s): A 34.98 fps, B (native
  0xFDA0 + cull mode 0) 34.94 fps, paired **-0.04 +/- 0.60 fps**. The mapping
  run measured **+0.21 +/- 0.08 fps** in B's favour, but the c690 burst wall
  only fell **36.65 -> 35.70 ms** (-0.95 ms, -2.6 %), below the frame noise.
* **The union still costs ~5 fps.** C (native stage + the proven union driver)
  is **30.31 fps, -4.67 +/- 0.65** in the A/B and **30.00 fps, -5.17 +/- 0.47**
  in the mapping run; p90/p99 rise 40.7/44.1 -> 50.2/54.1 ms. It fails the
  "< 1 fps" gate by a wide margin, even though the native stage speeds the
  union up 1.14x offline (122.9 -> 107.9 us/call).
* **The one-pass union was not achieved.** The kept-index count is finalised
  by the 0x132E0 caller *after* 0xD080 returns; the 0xD080 hook cannot know
  the final list. Mode 1 therefore reuses the proven lifted union driver
  (two rule evaluations around the command, native stage in both) instead of
  a fused single pass. The reason and the next step are in §3.
* **Nothing is enabled by default.** B does not win cleanly and C costs
  ~5 fps. Both `play_windowed.sh` copies and `ab_live.sh` keep the screen key
  at 0. The play-test commands are in §6.

## 1. The native stage

### 1.1 What is flattened

The 0x25/0x26 handler calls `0xD080` as a subroutine. From there the cull is a
tail-call state machine with no real calls; it ends by writing the kept-index
list and returning through `r0` to the handler's `spu_drain_call`. I extracted
the 24 reachable blocks and flattened them with
`tools/c690_ref/gen_native_screen_stage.py` into
`runtime/spu/c690_native_screen_stage.inc`:

* `sr0..sr97` stay live in locals; the per-block `ctx->gpr` loads disappear;
* the per-block **stores stay**. A block's live-out set is what a continuation
  would reload, and the terminal writeback is what the caller sees. Dropping
  the stores made the first version differ on 79/400 pairs: a register that
  the lifted code modified but did not store leaked its new value at the
  return. Keeping the stores is exact by construction.
* `SPU_DIRECT` tail transfers become `goto F_*`; `SPU_RET` becomes
  `goto L_DONE`; the return PC is published by the caller helper
  (`ctx->pc = gpr[0]`), as the native 0xFDA0 stage does.
* Interrupt polls are removed, the same contract as the native 0xFDA0 stage:
  the c690 job does not take an SPU interrupt in this subtree. The live shadow
  run is the evidence that this holds on the meter class.

The SPU arithmetic and local-store helpers are unchanged, so mode 0 is exact
by construction. The runtime TU is compiled with `-march=native` (AVX2/FMA on
this host, SSE4.1 baseline in the lifted module), so the helper paths are the
same validated ones.

### 1.2 Wiring

`tools/c690_ref/patch_native_screen.py` replaces the two
`spu_link(0x13400) ... 0xD080 ... spu_drain_call` brackets (opcode 0x25 and
0x26 paths) with `c690_native_screen_try()`. The helper returns 0 when every
key is off, so the lifted call is unchanged. It lives in
`runtime/spu/c690_native_screen.c`; no `spu_context` field was added (ABI
check PASS).

The shared WWS helpers around the cull (`0x49D4` ring/ticket allocator,
`0xE6B8`) are **not** part of the native subtree. They are shared with other
commands and dispatch back into the command loop; the native stage covers the
command's cull phase (0xD080 entry to the index-list writeback), not the whole
0x132E0 entry. That is a deliberate scope boundary, recorded as a deviation
from the literal "whole command" wording.

## 2. Gates

### 2.1 Offline, 400 captured pairs

`tools/c690_ref/run_native_screen_test.sh` runs four modes over the same
`corpus_edge3s2` spool as the lifted union gate:

| item | result |
|---|---:|
| pairs | 400 |
| baseline lifted vs capture | 0 mismatches |
| **native mode 0: LS hash differs** | **0** |
| **native mode 0: GPR hash differs** | **0** |
| **native mode 0: PC differs** | **0** |
| native mode 1: removed/reordered triangles | 0 |
| native mode 1: added triangles | **7** (same pair, max 7) |
| native mode 1 vs lifted union (full LS/GPR) | **0 differences** |
| cap hits / skips | 0 / 0 |

The union append is unchanged from
[`edge-holes-fix.md`](edge-holes-fix.md): original list and order preserved,
canonical vertex-set membership, appended in sample-grid list order, capacity
`(+0x1E0 - +0x110)/2` u16 entries.

### 2.2 Live shadow and live soak

Launch `ab_live_screen_shadow_20261104_010545`, one run, two arms x 45 s x 2
(90 s shadow, 90 s live), `PS3RECOMP_C690_NATIVE_SCREEN=1`, shadow key on in
the first arm:

| item | value |
|---|---:|
| shadow comparisons | **884,058** |
| shadow mismatches | **0** |
| live mode 0 calls (second arm) | 128,678 |
| native total / fallbacks | 1,012,736 / **0** |
| `std` of the shadow arm | 21.20 fps (shadow overhead; not a result) |
| live arm | 34.91 fps, p50 34.15 ms |

The second A/B launch then ran **1,690,624 live mode 0 calls with 0
fallbacks** and no wedge; the 0xFDA0 native stage added 250,368 calls with 0
fallbacks in the same run.

### 2.3 Offline speed, ns/call

Whole 0x132E0 body, 400 pairs, one process per mode:

| mode | mean ns/call |
|---|---:|
| lifted body | 22,932 |
| native mode 0 | **15,014** (**1.53x**) |
| lifted union driver | 122,900 |
| native-mode-1 union driver | **107,885** (**1.14x**) |

The native stage saves ~7.9 us per cull command; at ~1,000 culls/s that is
~8 ms/s of lane time, which is consistent with the ~2 % drop in c690 job wall
and the ~1 ms burst-wall drop measured live.

## 3. Why mode 1 is not a fused single pass

The D080 subtree writes the filtered indices into the buffer but leaves the
**candidate count** at `+0x114`; the 0x132E0 handler finalises the kept count
after the call. At D080 entry on the first pair, `+0x110/+0x114/+0x1E0` are
`0x2CE60 / 783 / 0x2D6A0`, and D080 returns with `+0x114` still 783, while
the command's final count is 264. A union implemented inside the D080 hook
therefore cannot read the rule's kept count, and appending there would corrupt
the caller's count update.

The shipped mode 1 enables `c690_cull_fix`'s proven union driver (family 1)
through the same key. The driver patches the screen transform, runs the whole
command once at the 2x sample grid, restores the entry image, runs it once at
the game grid, and appends sample-grid-only triangles. Both command runs use
the native 0xD080 stage. So the union is one native stage per rule, not one
pass over triangles.

A true fused pass needs the caller-side post-processing modelled (or a hook
after the count writeback), then the two rule results can be merged before the
single writeback. That is the next step; it is not claimed here.

One useful build detail: `c690_cull_fix_enabled()` and
`c690_cull_fix_maybe()` treat `PS3RECOMP_C690_NATIVE_SCREEN=2` as "union on",
so the new key alone selects the union. `PS3RECOMP_C690_CULL_FIX` still works
independently for the lifted union.

## 4. Live A/B

### 4.1 Arms

All arms name both keys (the control file does not revert an omitted key):

| arm | control file | meaning |
|---|---|---|
| A | `C690_NATIVE_CULL=0; C690_NATIVE_SCREEN=0` | today |
| B | `C690_NATIVE_CULL=1; C690_NATIVE_SCREEN=1` | native 0xFDA0 + cull mode 0 |
| C | `C690_NATIVE_CULL=1; C690_NATIVE_SCREEN=2` | native 0xFDA0 + cull mode 1 (union) |

`C690_CULL_FIX=0` and `MLB16_CULL_BYPASS=0` were named in every arm. Both runs
used `PS3_LWCOND_STATS=1`, `PS3RECOMP_C690_GAP=1`, `PS3RECOMP_WALKER_PROF2=1`,
`PS3RECOMP_C690_STATS=1`, alternating A/B/C.

### 4.2 A/B at the meter (N=6 x 10 s, 2 s dropped)

Run `ab_live_screenab_20261104_011215`:

| metric | A | B | C |
|---|---:|---:|---:|
| frames | 1,679 | 1,677 | 1,455 |
| fps | **34.98** | **34.94** | **30.31** |
| p50 ms | 34.15 | 34.15 | 34.15 |
| p90 ms | 40.67 | 40.64 | 50.19 |
| p99 ms | 44.07 | 44.99 | 54.09 |
| std ms | 11.50 | 11.63 | 13.60 |
| mean ms | 28.55 | 28.64 | 32.97 |
| walker drain ms | 16.80 | 16.89 | 17.40 |
| walker wait ms | 11.58 | 11.65 | 15.61 |
| draws / frame | 55.01 | 55.67 | 58.59 |
| vertices / frame | 6,373.8 | 6,452.8 | 6,751.1 |
| c690 job wall (lane-s/s) | 2.72 | 2.68 | 3.42 |
| c690 lane busy | 0.539 | 0.537 | 0.683 |
| c690 burst wall mean ms | n/a* | n/a* | n/a* |
| deadline frames / 10 s | 103.8 | 100.2 | 83.3 |

*The first run's timeline was recorded in hitch mode by mistake
(`PS3RECOMP_TIMELINE_HITCH_US` defaults to 250000 in `ab_live.sh`), so it only
kept hitch windows. Burst wall comes from the third launch below.

Paired per-segment fps: **B-A -0.04 +/- 0.60 (SE)**, **C-A -4.67 +/- 0.65**.
2 ms histogram of per-flip `fin` (8..52 ms, 52+):

| arm | 8 | 10 | 12 | 14 | 16 | 18 | 20 | 22 | 24 | 26 | 28 | 30 | 32 | 34 | 36 | 38 | 40 | 42 | 44 | 46 | 48 | 50 | 52 | 52+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 27 | 68 | 94 | 141 | 130 | 47 | 18 | 3 | 2 | 1 | 0 | 0 | 0 | **620** | 82 | 153 | 133 | 72 | 15 | 2 | 1 | 0 | 0 | 0 |
| B | 31 | 54 | 112 | 163 | 108 | 35 | 18 | 5 | 0 | 0 | 0 | 0 | 1 | **612** | 80 | 165 | 149 | 53 | 15 | 6 | 1 | 0 | 0 | 2 |
| C | 0 | 14 | 83 | 152 | 126 | 80 | 23 | 10 | 2 | 1 | 0 | 0 | 0 | **489** | 1 | 0 | 0 | 7 | 46 | 118 | 147 | 88 | 0 | 68 |

B keeps A's p50 bin; C moves everything above 42 ms into the 44-52 ms tail,
exactly the union signature from `edge-holes-fix.md` §6.

### 4.3 Burst-wall mapping run (N=3 x 10 s)

Run `ab_live_screenmap_20261104_011956`, timeline armed at the first switch
with `PS3RECOMP_TIMELINE_HITCH_US=0` (110 s window; 1.31 M c690 spans, 1,940
batches):

| metric | A | B | C |
|---|---:|---:|---:|
| fps | 35.17 | **35.38** | 30.00 |
| p50 / p90 / p99 ms | 34.15 / 40.84 / 44.29 | 34.15 / 40.33 / 44.71 | 34.15 / 50.32 / 54.97 |
| c690 burst wall mean ms | **36.65** | **35.70** | **45.87** |
| burst wall p50 / p90 ms | 36.68 / 39.39 | 35.93 / 38.37 | 45.84 / 49.23 |
| c690 union / frame ms | 23.11 | 22.61 | 30.60 |
| c690 job wall (lane-s/s) | 2.74 | 2.68 | 3.42 |
| c690 lane busy | 0.548 | 0.537 | 0.682 |
| draws / vertices per frame | 54.16 / 6,302 | 54.63 / 6,361 | 59.05 / 6,850 |

Paired per-segment fps: **B-A +0.21 +/- 0.08**, **C-A -5.17 +/- 0.47**. B's
direction is positive in this run and the burst wall drops 0.95 ms, but the
frame gain is ~0.2 fps, not a clean win. C doubles the cull work: burst wall
+9.2 ms and the frame loses ~5 fps.

### 4.4 Interpretation

The frame still ends on the c690 burst, so a 2.6 % shorter burst is worth only
the small fraction of frames whose burst was the binding term. The native
0xFDA0 stage was already neutral for the same reason
([`edge-native-step3.md`](edge-native-step3.md)); the cull stage adds a second
small saving. The union's two command passes cost far more than the native
stage saves (c690 job wall +27 %, lane busy 0.54 -> 0.68), so the holes fix
remains an expensive diagnostic, not a shipping default.

## 5. Decision

| gate | result |
|---|---|
| B wins cleanly | **no**: main A/B -0.04 +/- 0.60, mapping +0.21 +/- 0.08 |
| C costs < 1 fps | **no**: -4.67 +/- 0.65 (main), -5.17 +/- 0.47 (mapping) |

**Enable nothing.** `PS3RECOMP_C690_NATIVE_SCREEN` stays 0 in both
`play_windowed.sh` copies and in `ab_live.sh`; the native 0xFDA0 key stays 0
as before. The native stage is exact and safe, and its live switches remain
available for diagnostics. The holes are **not** fixed by default.

## 6. Play test

From `recomp-work`, on the pitch close-up where the pitcher's eyes are
visible, then let it run into a cinematic:

```sh
# native cull mode 0 + native 0xFDA0 (exact, ~neutral):
PS3RECOMP_C690_NATIVE_SCREEN=1 PS3RECOMP_C690_NATIVE_CULL=1 ./play_windowed.sh

# holes fix: native stage + the proven union driver (costs ~5 fps):
PS3RECOMP_C690_NATIVE_SCREEN=2 PS3RECOMP_C690_NATIVE_CULL=1 ./play_windowed.sh

# control: the family bypass, also removes the holes (costs ~7.6 fps):
MLB16_CULL_BYPASS=3 ./play_windowed.sh
```

Expected: `=2` matches the union's result (the 2x sample-grid triangles are
added, nothing removed); `=3` still removes the holes and draws the extra
geometry. If `=2` does not remove them on the cinematic, the union need is
scene-dependent, as `edge-holes-fix.md` warned.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-play
# regenerate the flattened subtree after a lift (game-derived, not committed)
python3 tools/c690_ref/gen_native_screen_stage.py \
  ../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  runtime/spu/c690_native_screen_stage.inc
# install the call-site hook (after patch_cull_hooks.py)
python3 tools/c690_ref/patch_native_screen.py
ninja -C ../recomp-work/build-play
# offline gate: exact + union on 400 pairs
tools/c690_ref/run_native_screen_test.sh ../recomp-work/corpus_edge3s2

# live A/B
cd ../recomp-work/ab-tools
export PS3_LWCOND_STATS=1
TAG=screenab PITCH_THRESH=35 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  PS3RECOMP_C690_GAP=1 PS3RECOMP_WALKER_PROF2=1 PS3RECOMP_C690_STATS=1 \
  ARMS='A=PS3RECOMP_C690_NATIVE_CULL=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0|B=PS3RECOMP_C690_NATIVE_CULL=1;PS3RECOMP_C690_NATIVE_SCREEN=1;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0|C=PS3RECOMP_C690_NATIVE_CULL=1;PS3RECOMP_C690_NATIVE_SCREEN=2;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  BOOT_ARM='PS3RECOMP_C690_NATIVE_CULL=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  ./ab_live.sh
```

For burst wall, add `PS3RECOMP_TIMELINE=/tmp/tl.bin`,
`PS3RECOMP_TIMELINE_TRIGGER=/tmp/tl.go`, `PS3RECOMP_TIMELINE_SECS=110`,
`PS3RECOMP_TIMELINE_HITCH_US=0`, touch the trigger at the first switch, then
`tl_c690.py /tmp/tl.bin <log> <marks> 10 2`.

## 8. Deviations from the mission plan

* **Mode 1 is not a fused single pass.** The kept count is finalised after
  0xD080 (§3); mode 1 reuses the proven union driver with the native stage in
  both passes. It is the same output as the lifted union (0 differences on
  400 pairs) but costs ~5 fps.
* **The native stage covers 0xD080..return, not the whole 0x132E0 entry.**
  The shared WWS ring helpers remain lifted (§1.2).
* **The A/B is N=6 x 10 s** as planned; the shadow soak is N=2 x 45 s per arm
  (90 s shadow, 90 s live) rather than a separate 60 s run, to stay inside the
  three-launch cap. The first A/B timeline was in hitch mode by mistake; the
  third launch provided the burst wall.
* **Cinematics were not driven.** The rules reserve multi-scene checks for
  the user's windowed run; the play-test command is in §6.
* **Deadline frames** come from the periodic `[lwcond-stats]` deltas (the
  harness SIGKILLs the process, so the atexit totals are lost).

## 9. Clean room and licence

This is our own reverse engineering of the game's lifted SPU c690 module and
the captured LS/GPR pairs. The stage-split/corpus method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied.
The channel-inline comments in `spu_helpers.h` follow the inFAMOUS 2 port
(MIT) as documented there. No RPCS3 or other GPL emulator source was read or
cited.
