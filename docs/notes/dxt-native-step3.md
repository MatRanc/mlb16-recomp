# Native DXT, step 3: the kernel is lifted, bit-exact, and removes the boot hitches

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. Two game launches (the hard cap): A lift off
`ab_live_dxtliftA_20261103_225911`, B lift on
`ab_live_dxtliftB_20261103_230713`. Both: start screen, standard
`play_windowed.sh`/`ab_live.sh` environment, `PIN=1 WCB=2`, governor
`performance`, capture on (`PS3RECOMP_DXT_CAPTURE_ENTRY=1`). No GPL emulator
source was read. The native encoder is **not** started; its plan is §5.

## TL;DR

* **The kernel is lifted.** `lift_jobmods.py --engine-elf` now accepts a
  module header whose link base is `0` (PIC) as well as `0x4000`, and lifts it
  at the code-buffer base `0x4000`, entry `0x4030`. The MLBeat module is
  `jobmod_647a2bf49000_e0030`, source EA `0x010A1700`, code span
  `[0x4030, 0x67DC)`, registered as a verified streamed region. The job
  body is called through the job-source function-pointer table, so the lift
  also seeds the PIC call-stub continuations (`0x4050, 0x5318, 0x6010,
  0x603C.., 0x61BC, 0x6734, 0x6748, 0x67D4`); without them the lift stopped
  at `0x600C` and the whole DXT encode/DMA body stayed interpreted.
* **The gate is 100 %.** The lifted kernel, run offline on the captured entry
  states with the captured GET/PUT stream, matches every captured PUT byte for
  byte: **346/346 complete job-body entries, 5,101 PUTs, 890,688 block pairs,
  0 mismatched, 0 missing input GETs** (one torn tail skipped). Lifted cost:
  **0.121 s / 136 ns per block**. The interpreter's measured cost is
  **18.50 s of lane time / 240,988,752 instructions** for the A run, i.e.
  **~20.8 us per block** (lane time over the 890,688-block workload; 23.9 us
  over the blocks the A capture replayed).
* **The instrumentation gap is closed.** `boot-spu-hitches.md` reported zero
  `spu.interp` because only `spu_run_interp_job` emitted the event; the
  in-context rejoin `spu_smc_microstep -> spu_interp_run_until` did not. The
  rejoin now emits `PS3_TR_SPU_INTERP` with the interpreted instruction count
  in `b`. The A timeline has **35,426 `spu.interp` spans, 18.51 s**, of which
  **18.50 s** is the DXT range.
* **B wins cleanly at the start screen.** A: **14 hitches, 5,389 ms** on the
  timeline (11 flip gaps > 250 ms, 7,623 ms, max 1,055 ms). B: the timeline
  recorded **no frame >= 250 ms**; the flip log has **one 308 ms gap** in the
  boot window. Wid-0 policy lane time from the capture: **11,722 ms (A) ->
  867 ms (B)**, on a larger B workload. No wedge; 621 flips vs 600.
* **Keep it on by default.** `PS3RECOMP_DXT_LIFT` defaults to on (`=0` turns
  dispatch off, the region is still installed so the capture sees the load).
  Play test: `cd recomp-work && ./play_windowed.sh`, reach the start screen,
  then start a game and trigger an instant replay.

## 1. The cost was interpreted, not lifted

The earlier note said the 14 boot hitches were "pure lifted compute" with zero
`spu.interp`. Both statements were true for different reasons: the job body
was interpreted, but the interpreter was entered through the drain's
in-context rejoin, and that path never emitted the `spu.interp` event.

`runtime/spu/spu_channels.c` now wraps the `spu_interp_run_until` call in
`spu_smc_microstep` with `ps3_trace_begin/end(PS3_TR_SPU_INTERP, pc,
g_spu_interp_steps)`. Run A (`PS3RECOMP_DXT_LIFT=0`) then reports:

| A metric | value |
|---|---:|
| `spu.interp` spans (all) | 35,426 |
| `spu.interp` lane time (all) | 18,509.4 ms |
| `spu.interp` spans in `[0x4000,0x6C10)` | 3,724 |
| DXT lane time | **18,499.2 ms** |
| DXT interpreted instructions | **240,988,752** |
| `spu.run` lane time | 19,601.5 ms |
| `spu.policy` lane time | 19,615.2 ms |

The per-entry breakdown names the hot body: `0x61BC` 18,491.6 ms /
240,928,116 instructions, the encode loop; `0x4050` 4.4 ms; the six DMA/stub
entries `0x603C..0x6180` 0.5 ms each. That is the exact shape of the unlifted
job body, and it is why the earlier timeline looked like lifted compute: the
work was hidden under `spu.run`.

The capture's own timestamps agree on the size: 273 wid-0 policy runs, **11,722
ms of policy lane time** in A (per lane 2,695-3,451 ms). The difference from
the 18.5 s interpreter sum is the span definition (the interpreter spans are
per rejoin; the policy run is the enclosing wall interval) and the capture
covering 273 of the boot's runs.

## 2. The lift

### 2.1 Header base 0

`tools/lift_jobmods.py --engine-elf` used to require the module header's link
base (`+0x28`) to be `0x4000`. The DXT module says `0`, because it is
position-independent: the entry computes its base with
`brsl r126 / sf r126, 0x68, r126` (0x4060-0x4068), which yields `0x4000` when
the title streams the image there. `find_engine_modules` now accepts a header
base of `0` or `0x4000` and always lifts at the job code buffer base
`0x4000`. The manifest records the header base (`"header_base": 0`) next to
the lift base.

### 2.2 The function-pointer table

The module is not one direct-call tree. Its entry at `0x4030` loads a job
descriptor, then branches through function pointers with `bisl`; the compiler
also emits PIC call stubs of the form `brsl rT, next; ai rT, +8; bi rT`, whose
target is the instruction after the `bi`. A static reachability pass stops at
the first such stub, which is exactly what happened: the first lift found 22
functions and `code_end 0x600C`, omitting the whole DXT encode/DMA body at
`0x603C..0x67D8`.

The interpreter's rejoin PCs on the corpus name the real entries. They are
recorded in `EXTRA_FUNCS_BY_NAME` in `tools/lift_jobmods.py` for this module
(content hash + entry offset, so a different module is unaffected):
`0x4050, 0x5318, 0x6010, 0x603C, 0x607C, 0x60BC, 0x60FC, 0x613C, 0x6180,
0x61BC, 0x6734, 0x6748, 0x67D4`. With them the lift is **300 functions,
0 unsupported, `code_end 0x67DC`**; the data tables start at `0x67E0`, so the
registered span is the code only.

### 2.3 Registration and selection

The new registration TU is generated separately, so the hand-patched c690 lift
is untouched:

```sh
cd ps3recomp-play
python3 tools/lift_jobmods.py --engine-elf /home/m/Documents/mlb16-online/EBOOT.elf \
  --only-ea 0x010A1700 --out /tmp/dxtout8 --register /tmp/dxtout8/jobmods_register_dxt.c \
  --symbol-prefix mlb16_dxt --image-base 207 --register-env PS3RECOMP_DXT_LIFT
# copy jobmod_647a2bf49000_e0030/ and jobmods_register_dxt.c into
# recomp-work/spu_lifted/, then cmake --build recomp-work/build-play -j6
```

`jobmods_register_dxt.c` registers `(content_ea=0x010A1700, load_lsa=0x4000,
span=0x27DC, image=207, segment [0x4030,0x67DC))` through a new
`spu_overlay_register_verified_region_gated`. The region is always installed
so the capture and residency diagnostics see the code GET; dispatch is enabled
unless `PS3RECOMP_DXT_LIFT=0`. The first dispatch compares the lifted code
bytes once per load (the existing exactness rule): a different module streamed
from the same EA is rejected and runs interpreted. `templates/project/CMakeLists.txt`
now includes `jobmods_register_dxt.c` in the optional generated TUs.

The module's code span is the code only, not the whole `0x2C10` image: the
job's own data DMAs into the image above the code do not clear residency. The
existing verified-region test (`tools/test_lift_jobmods_verified.py`) still
passes (it now also links `dxt_capture.c`/`c690_corpus.c`, needed by
`spu_channels.c`).

## 3. Offline gate: 100 % byte match

`mlb16-recomp/tools/replay/test_dxt_job_replay.c` is the isolation harness. It
reads the spool, applies the PM's outstanding GETs to the captured entry LS,
runs the lifted `0x4030` with the WWS policy module's lifted helpers linked in
(the job calls back into the job-manager API at `0x2F18` etc.), services the
job's GETs from the captured guest payloads, and compares every PUT payload,
EA, LSA and size in order. It models the SPURS services at `0x9C0`/`0x9D0`
and the lock-line queue reads.

Build and run:

```sh
cc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -Irecomp-work/runtime/spu -Ips3recomp-native/include \
  -o /tmp/opencode/test_dxt_replay mlb16-recomp/tools/replay/test_dxt_job_replay.c \
  recomp-work/spu_lifted/jobmod_647a2bf49000_e0030/spu_recomp.c \
  recomp-work/spu_lifted/wwsjob_pm/spu_recomp.c -lm
/tmp/opencode/test_dxt_replay recomp-work/dxt_capB_20261103
```

Result on the B corpus:

```
DXT job lift gate: 347 runs (346 pass, 0 fail, 1 skipped)
PUTs compared: 5101, mismatched: 0, missing GETs: 0
lifted kernel: 0.121 s total, 890688 blocks, 136.2 ns/block
```

The one skipped entry is the torn tail (SIGKILL, no captured PUTs), not a
mismatch. The A corpus has the same zero-mismatch result on its 254 complete
segments; its 12 incomplete segments are the capture-hook limitation fixed in
the final hook (the old hook recorded only the first of a policy run's two
job-body dispatches). The B capture records every distinct entry.

Interpreter comparison: A's 18,499.2 ms lane time over the same workload is
`18.4992 s / 890688 = 20.77 us/block` (or 23.94 us/block over the 772,608
blocks the A capture replayed). Lifted is 136 ns/block, about **150x less
work per block**.

## 4. Live A/B: start-screen hitches

Both launches booted with the same config and capture; A used
`PS3RECOMP_DXT_LIFT=0`, B `=1`.

| metric | A (off) | B (on) |
|---|---:|---:|
| run | `ab_live_dxtliftA_20261103_225911` | `ab_live_dxtliftB_20261103_230713` |
| timeline hitches >=250 ms | **14** | **0** (empty PSTL) |
| timeline hitch total | **5,389 ms** | n/a |
| flip gaps >250 ms, boot window 20-35 s | 11, **7,623 ms** | 1, **308 ms** |
| max flip gap, whole run | 1,055 ms | **308 ms** |
| wid-0 policy lane time (capture) | **11,722 ms** | **867 ms** |
| captured policy runs | 273 | 344 |
| captured block pairs | 772,608 replayed | 890,688 |
| flips | 600 | 621 |
| frozen/no-slot window stats | 0 frozen | 0 frozen |
| wedge | none | none |

The B timeline file is empty because the timeline keeps only frames at or
above `PS3RECOMP_TIMELINE_HITCH_US=250000`; no boot frame crossed it. That is
the intended signal. The one 308 ms flip gap is below the recorder's hitch
threshold and is the largest gap in the whole B session. The B workload was
larger (344 vs 273 captured policy runs, 890,688 vs 772,608 replayed blocks),
so the lane-time win is not a smaller-workload artifact: per policy run A is
42.9 ms, B 2.52 ms.

### Default and play test

`PS3RECOMP_DXT_LIFT` is unset in the standard environment, which means the
lift is on. `=0` keeps the verified region resident but routes the span to the
interpreter (the A arm). No change to `play_windowed.sh` is needed.

Play test (user, windowed):

```sh
cd /home/m/Documents/mlb16-online/recomp-work && ./play_windowed.sh
```

Reach the press-start screen, start a game, and trigger an instant replay. The
boot should no longer pause in the 21-30 s window; the replay camera is the
other place the DXT workload appears (`boot-spu-hitches.md` §2: 5,504.7 ms of
DXT lane time in the 2,050 ms hitch).

### Frame check and caveats

* A's raw `recomp-work/frame.ppm` was overwritten by the B launch before it
  was copied, so there is no direct A/B pixel diff. B's start-screen frame is
  saved as `recomp-work/dxtlift_B_frame.png` (1280x720); it is the normal
  start screen and the run never wedged. This is the one measurement the
  mission did not complete as planned.
* Both arms ran the entry capture (about 230 MiB per run), so the A/B is
  conservative. The original no-capture `boot-spu-hitches.md` baseline had
  10.1 s of boot hitches; A with capture/off had 5.4 s, so the capture did not
  inflate the A arm in this session.
* The A and B launches are separate boots. The texture-cache state and the
  number of DXT jobs differed; the per-run lane time and the hitch/flip metrics
  are the fair comparisons.
* The final source was rebuilt after the two runs with one line that sets
  `enabled = 1` for non-gated overlay sources (`spu_overlay_register_source`).
  The DXT path is a gated verified region and sets `enabled` explicitly, so the
  A/B binary and the final binary are identical on this path. The
  `spu_context` ABI check passes.

## 5. Native encoder: plan (not started)

The lift already removes the boot hitches, so the encoder is no longer on the
critical path. It remains useful for the in-game replay/DOF frame and for CPU
headroom. The gate is now ready to certify it.

1. **Reference implementation.** Use the lifted kernel as the oracle. It is
   our own recompiled output from the EBOOT module and is bit-exact on 890,688
   block pairs. Do not read GPL emulator source; the only external facts are
   the public BC1/BC3 block layouts.
2. **Algorithm extraction.** The hot body is `0x61BC` (240.9 M interpreted
   instructions in A). Walk its lifted C to recover the endpoint search,
   quantisation, tie-breaking and index-selection order, and the DXT5 alpha
   path. The corpus supplies the exact expected output for each block; the
   lifted C supplies the exact intermediate arithmetic. Keep the SPU SIMD
   lane order and the integer/float rounding identical (the kernel uses
   `shufb`, `sumb`, `mpyu`, `clgtb`, `selb` and saturating ops).
3. **Native kernel.** C with an AVX2 path, scalar fallback, same LS layout and
   DMA calls. Start as a drop-in for the job body behind
   `PS3RECOMP_NATIVE_DXT=1`, default off, with a shadow mode that runs both
   and compares the PUT bytes on the live stream (the c690
   `job_c690_native.c`/`edge-native-step3.md` pattern).
4. **Offline gate.** Replay the B corpus through the native kernel and require
   the same 346/346 segments, 5,101 PUTs, 890,688 blocks, 0 mismatches. Report
   ns/block against the lifted 136 ns/block. Only then run a live shadow A/B
   and, if clean, switch it on.
5. **Priority.** Low until a scene shows the lifted DXT cost again. The boot
   burst is now 867 ms of policy lane time instead of 11.7 s; the replay frame
   is the next target to measure.

## 6. What was not done

* No native encoder, no `PS3RECOMP_NATIVE_DXT`, no shadow/live encoder A/B
  (§5).
* No third launch; the hard cap of two launches was spent on A and B.
* No direct A/B `frame.ppm` diff (A's was overwritten; see §4).
* No B timeline lane-time sum (the timeline is empty by design); B's wid-0
  lane time is from the capture's run timestamps.
* No test of a deliberate code-mismatch live; the verified-region unit test
  covers the wrong-bytes-stay-interpreted case.

## 7. Artifacts

* Code: `ps3recomp-play` branch `exp/live-ab` (pushed to `fork`):
  `runtime/spu/dxt_capture.{c,h}` (entry records 7/8, hash dedup),
  `runtime/spu/spu_channels.c` (gated region, in-context `spu.interp` span,
  entry hook), `runtime/spu/spu_overlay_residency.h`, `tools/lift_jobmods.py`
  (base 0, `--only-ea`, `--image-base`, `--register-env`, extra funcs),
  `templates/project/CMakeLists.txt`,
  `tools/test_lift_jobmods_verified.py`.
* Generated: `recomp-work/spu_lifted/jobmod_647a2bf49000_e0030/`,
  `recomp-work/spu_lifted/jobmods_register_dxt.c` (not committed; generated
  from the EBOOT like the other lifts).
* Harness: `mlb16-recomp/tools/replay/test_dxt_job_replay.c`.
* Corpus: `recomp-work/dxt_capA_20261103` (231 MiB) and
  `recomp-work/dxt_capB_20261103` (237 MiB), game data, not committed.
* Timelines: `recomp-work/dxtlift_A_20261103.pstl`,
  `recomp-work/dxtlift_B_20261103.pstl` (empty: no hitches).
* Logs: `recomp-work/ab_live_dxtliftA_20261103_225911.log`,
  `recomp-work/ab_live_dxtliftB_20261103_230713.log`.
* Clean room: the differential/gate method follows NHL Legacy Native (MIT,
  (c) 2026 HeckYouMan) with attribution; no NHL, RPCS3 or other GPL emulator
  source was read or cited.
