# Walker MT M4 live: per-worker recording is not viable yet

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play` at `87cfe3aa`
(plus one measurement-only live-ctl key, `e381d650`), built and measured in
`recomp-work/build-play`. **One game launch**, pitch meter, `ab_live.sh`.
Follows [`walker-mt-m4.md`](walker-mt-m4.md),
[`walker-mt-m3.md`](walker-mt-m3.md),
[`walker-mt-m1m2.md`](walker-mt-m1m2.md) and
[`walker-multithread-design.md`](walker-multithread-design.md) sections 3.5,
3.7, 5 and 7. No RPCS3 or other GPL emulator source was read or cited.

## TL;DR

* **The M4 live test did not produce a usable A/B/C sample.** The first
  launch measured one 8 s window of A (MT off) and one of B (MT W=3,
  `TEX_RESOLVE=0`) and then **hung on the first frame after C**
  (`TEX_RESOLVE=1`): zero flips and zero `[walker-prof]` lines for 13 s
  until the harness killed the game. Per the stop rule the run was recorded
  and ended; no second launch was made.
* **B is slower and renders the frame wrong.** p50 **34.76 -> 38.08 ms**
  (+9.5 %), fps **34.0 -> 27.5** (-19 %), walker **12.2 -> 24.0 ms**,
  `tex` **0.44 -> 14.47 ms**. Per draw (draws/frame rose 553 -> 617, +11.6
  %, so normalise): walker **23.7 -> 37.2 us/draw**, `tex` **0.86 -> 22.4
  us/draw**. The state-thread `draw` phase does fall **7.3 -> 2.0 us/draw**
  and `bind` goes to zero -- per-worker recording does what it says on the
  state thread -- but the texture drain costs 22 us/draw and swamps it.
* **B's scene dump lost the field.** Mean absolute thumbnail diff vs A is
  **78.9** (A/A **2.2**); pixels below RGB 12 rise **14.5 % -> 58.6 %**
  overall and **13.5 % -> 93.9 %** in the lower half of the frame. The
  pitcher/batter/crowd strip remains; the infield and most of the frame are
  black for the whole B window. This is a visual regression, not A/A noise.
* **C hung with no error.** The walker stopped making progress on the first
  frame that used the ordered texture-group path; the PPU event thread kept
  logging, there was no Vulkan error, no validation layer, and no
  `[walker-mt]` fallback. The leading code-level candidate is a circular
  wait between the worker's pre-segment `tex_taken` wait
  (`rsx_draw_engine.c:7630`) and the resolver's group-turn wait
  (`:7207`), but the stop rule excluded the repro needed to prove it.
* **Verdict: stop MT in its current form.** Per-worker recording reduces
  the live state-thread draw/bind cost, but the texture hand-off is still
  the serial chain and the live frame is wrong before the resolver helps.
  The one change with the most evidence behind it is **ordered texture
  upload records** (record the upload in the worker's command buffer at the
  token position, with the cache decision kept on the state thread) -- but
  the turn-ordering bug must be fixed first. Nothing is default-on and no
  user zoomed-out command is issued.

## 1. Setup

### 1.1 Build

`build-play/MyGameRecomp` was built 19:11, before the 19:48 M4 commit, and
had no M4 symbols (`vk_mt_chunk_submit`, the `TEX_RESOLVE` name). It was
rebuilt at 19:52 from `87cfe3aa`; the new binary contains
`vk_mt_chunk_submit`, `eng_tex_resolver_main`, `[walker-mt] per-worker
recording active` and `PS3RECOMP_WALKER_MT_TEX_RESOLVE`.

`build-mt` was left untouched after an incremental rebuild failed on an
**unrelated latent flag problem**: `runtime/spu/c690_native_cull.c` needs
`-mfma`, but `build-mt/build.ninja` has no `-mfma` (build-play does). The
file had not been recompiled since the header change that made it a
dependency. This is not an M4 regression; it only blocked a full offline
re-gate. The offline check below was run from `build-play`'s
`replay_rsx_engine`, which builds with the same flags.

### 1.2 The measurement-only live key

`PS3RECOMP_WALKER_MT_TEX_RESOLVE` was environment-only, so the three arms
(B and C differ only in that key) could not be switched in one launch.
Commit `e381d650` adds it to `ps3_live_ctl` and gates the per-draw async
path on it, with two small safeguards:

* `ps3_live_ctl_file_set()` distinguishes "the file never named the key"
  from "the file set it to 0", so the historical default (**on** when MT is
  on) is preserved for runs without a control file;
* the `rsx-tex` thread now starts with the MT workers even when the resolver
  is off, so a later live turn-on does not need an MT re-init.

Exactness after the change, `build-play/ps3recomp_sdk/replay_rsx_engine`
on `meter_frame`, 5 frames + 2 warm-up, `REPLAY_NULL_VSRANGE=1`:

| arm | draws/frame | output hash |
|---|---:|---|
| MT off | 502.0 | `461ff3a834c62331` |
| MT on, W=3, `TEX_RESOLVE=0` | 502.0 | `461ff3a834c62331` |
| MT on, W=3, `TEX_RESOLVE=1` | 502.0 | `461ff3a834c62331` |

All equal the historical `meter_frame` VSRANGE value. (The null replay does
not link live-ctl, so a control-file test cannot run there; the file path was
exercised by the live launch below.)

### 1.3 Validation layer

**Not installed.** `/usr/share/vulkan/explicit_layer.d/` is empty and no
`libVkLayer_khronos_validation.so` exists under `/usr/lib*`. Nothing was
installed. There is therefore no validation arm; all "no validation
message" statements below mean the app logged no Vulkan error.

### 1.4 The run

One `ab_live.sh` launch, `build-play`, pitch meter, X once, `WCB=2`, `PIN=0`,
x11, `PITCH_THRESH=35`, `SEG=10`, `DROP=2`, `LIMIT=1100`, boot arm A.
Planned rotation `A,B,C` x4 (12 segments); `REPORT=0` because the report
script only understands A/B marks.

| arm | control file |
|---|---|
| A | `PS3RECOMP_WALKER_MT=0`, `WALKER_MT_WORKERS=0`, `TEX_RESOLVE=1` |
| B | `PS3RECOMP_WALKER_MT=1`, `WALKER_MT_WORKERS=3`, `TEX_RESOLVE=0` |
| C | `PS3RECOMP_WALKER_MT=1`, `WALKER_MT_WORKERS=3`, `TEX_RESOLVE=1` |

Run `ab_live_walkermt4live_20261104_195351`. It reached the meter at 19:53
and emitted three marks:

| seg | arm | switch at flip-ms | measured window |
|---:|---|---:|---|
| 0 | A | 189767 | 191767-199767 ms |
| 1 | B | 199838 | 201838-209838 ms |
| 2 | C | 210071 | (no flips) |

**Sampling deviation:** one 8 s measured window for A and one for B, none
for C, instead of the planned four each. C is the third segment; it wedged
on its first frame, so the rest of the rotation never happened.

## 2. Results

Frame times are the MangoHud CSV rows in the measured window (fps and
fractional times cross-checked with the guest `[flip]` counter). Walker
numbers are the `[walker-prof]` lines in the same window, after the known
sampling bug filter (`engine=0.000` with `draws>0`: 1 of 42 A lines, 2 of 31
B lines dropped; C has none).

### 2.1 Frame time, fps, draw count

| metric | A (MT off) | B (MT W=3, TEX_RESOLVE=0) | C |
|---|---:|---:|---:|
| frames in window | 272 | 207 | 0 |
| fps (CSV) | 33.96 | 27.51 | - |
| fps (`[flip]` counter) | 34.32 | 27.74 | - |
| p50 / p90 / p99 ms | **34.76 / 40.07 / 54.86** | **38.08 / 49.68 / 54.67** | - |
| draws/frame (mean) | 553.5 | 617.4 | - |
| draws/frame (median) | 548.3 | 636.9 | - |
| walker ms (mean / median) | 12.22 / 8.89 | 24.03 / 25.03 | - |
| wait ms (mean) | 15.45 | 13.59 | - |

The draw count drifts +11.6 % between the two windows (the meter is not
perfectly static), so the per-draw column below is the honest comparison.

### 2.2 Walker phases and per-draw cost

Mean ms/frame in the measured window; last column normalises by that
window's draws.

| phase | A ms | B ms | A us/draw | B us/draw |
|---|---:|---:|---:|---:|
| walker | 12.216 | 24.029 | 23.72 | 37.22 |
| engine | 11.159 | 22.108 | 21.66 | 34.23 |
| parse | 1.057 | 1.921 | 2.05 | 2.98 |
| fetch | 2.751 | 0.955 | 5.34 | 1.48 |
| **tex** | **0.440** | **14.467** | **0.86** | **22.44** |
| pipe | 1.151 | 1.749 | 2.23 | 2.71 |
| bind | 0.175 | 0.000 | 0.34 | 0.00 |
| **draw** | **3.357** | **1.317** | **7.30** | **2.02** |
| other | 3.285 | 3.620 | 6.37 | 5.61 |

Reading it:

* **Per-worker recording does remove the state thread's draw/bind work.**
  `draw` falls 7.3 -> 2.0 us/draw and `bind` to zero; `fetch` also falls
  (5.3 -> 1.5 us/draw) because M3 preparation moved off the state thread.
  This is the part the offline null replay could not see.
* **The texture path eats all of it and more.** With `TEX_RESOLVE=0` every
  texture create/upload/snapshot calls `eng_mt_sync`, which drains the
  worker chunks since the previous texture op. `tex` becomes 14.5 ms, 60 %
  of the walker, and 22.4 us/draw against A's 0.86. The state thread is not
  doing more texture work; it is waiting for the workers it just fed.
* **The 8 s B window is long enough to see the median too:** walker median
  25.0 ms, with the phase profile above repeated across 29 valid reports.
  The frame is not a single transition spike.

### 2.3 `[walker-mt]` and errors

Only the M4 start-up lines appear, at the A -> B switch:

```
[live-ctl] flip=11354 PS3RECOMP_WALKER_MT=1 (was 0)
[live-ctl] flip=11354 PS3RECOMP_WALKER_MT_WORKERS=3 (was 0)
[live-ctl] flip=11354 PS3RECOMP_WALKER_MT_TEX_RESOLVE=0 (was 1)
[walker-mt] 3 record workers up (queue 16 MB, chunk 4096 KB)
[walker-mt] per-worker recording active
[walker-mt] rsx-tex resolver up (plain guest textures)
```

No `[walker-mt] ... fallback` line, no `chunk begin failed`, no Vulkan
error, no VUID (there is no layer), and the harness's window watchdog never
reported a frozen present before C.

### 2.4 Scene thumbnails

`ab_live.sh` writes a thumbnail every 2 s from `frame.ppm`; the marks anchor
each thumbnail to an arm. In the measured windows:

| comparison | pairs | mean abs diff (0-255) |
|---|---:|---:|
| A vs A (A/A baseline) | 3 | **2.18** |
| B vs B (temporal) | 6 | 3.04 |
| C vs C (frozen) | 6 | **0.00** |
| B vs A | 12 | **78.91** |
| C vs A | 12 | 78.61 |

The within-arm numbers are the A/A level; the cross-arm number is a
different image. The B thumbnails keep the crowd/scoreboard strip, the
pitcher, batter and catcher, and lose the field: pixels below RGB 12 go from
**14.5 % (A) to 58.6 % (B)**, and in the lower half of the frame from
**13.5 % to 93.9 %**. C's thumbnails are byte-identical to each other
(frozen) and essentially the last B frame (mean abs diff 1.8): no new frame
was presented. This is a visual failure of the live MT W=3 path with the
resolver off. It is not caused by C and it is present before the hang.

### 2.5 The hang

Timeline from the log:

```
19:57:22  last [walker-prof] at=210023ms  (walker 44.9 ms, tex 30.2 ms)
19:57:23  [live-ctl] flip=11841 TEX_RESOLVE=1 (was 0)
19:57:23  [ab_live] switch C at flip-ms 210071
19:57:23  last [fliplog] n=7654
19:57:36  [ab_live] STOP: no flips during segment 2 (C): 210071 -> 210071
```

No `[flip]`, `[fliplog]` or `[walker-prof]` line appears after the switch;
the PPU-side `[WAIT] event_queue_receive` lines continue every 1-2 s, so the
process was alive but the RSX walker made no progress. There is no Vulkan
error or fallback message. The first frame that used the `rsx-tex` resolver
under per-worker recording wedged.

The code path has an obvious circular-wait candidate:

* the worker will not begin a chunk segment until
  `tex_taken >= p->tex_epoch` (`rsx_draw_engine.c:7630`);
* the resolver will not advance `tex_taken` until it takes the group turn
  `eng_mt_wait_turn(seq)` (`:7207`);
* the group turn was allocated by the state thread at the bind
  (`:7147`) and the packet carries the epoch at finalise (`:7991`), while
  the worker may still owe the turns of packets that precede the group.

That is exactly the new shape: an ordered turn *inside* the chunk the worker
has not started recording. The null replay never takes this path because
its backend has no `mt_init`, so M4's exactness tests only exercised the
per-packet turn path. Proving which side of the wait is stuck needs a stack
dump of the wedged process or a C-from-boot repro; the stop rule says a hang
ends the measurement, so neither was done. What is certain is that C did not
produce a single frame, so there is no C timing.

## 3. What this says about M4

**Does per-worker recording reduce the live `draw` phase?** Yes. On the
state thread `draw` falls 7.3 -> 2.0 us/draw and `bind` to zero, with
`fetch` down to 1.5 us/draw. The recorded command buffers are not on the
state thread any more. But the win is 5-6 us/draw against a 22 us/draw
texture cost, and the resulting frame is visibly wrong.

**Where does the state thread spend its time live with MT on?** In `tex`:
14.47 of 24.03 ms (60 %), with `other` 3.62, `parse` 1.92, `pipe` 1.75,
`draw` 1.32 and `fetch` 0.96. The serial chain is now:
walker prepare -> each texture bind does `eng_mt_sync` (drain all chunks
since the last texture op) -> packet pushed -> worker records/submits ->
present. With the resolver on, the same point becomes an ordered group turn
between the chunk before and the chunk after the bind, which is what wedged.

**Is the MT design viable, and what single change would most move the live
crowded walker time?** Not in this form. The design's own premise was that
M3's `tex` wait was the blocker and M4's resolver would remove it; live, the
resolver-off arm already loses 22 us/draw to that wait and the resolver-on
arm does not produce a frame. The single change with the strongest evidence
is **ordered texture-upload records**: keep the cache decision and the byte
snapshot on the state thread, but record the upload in the worker's own
command buffer at the token's position instead of submitting it from a
separate `rsx-tex` thread and ordering it with a turn. That removes the
third thread from the submission path, which is where B's 14.5 ms and the
hang both live. It does not by itself explain B's black field, so the
per-worker recording state (targets / render pass / framebuffer / pipeline
cache) must also be fixed and re-checked visually before any timing claim.
If one more live run does not produce an identical frame first, MT should
stay parked.

## 4. What was not done

* **No second launch.** The hard cap allows a second launch only for a
  failure we fixed or for a validation arm; this was a hang in an MT arm,
  and there is no validation layer. The C-from-boot repro and the mixed
  A/B/A/C ordering that would separate "toggle" from "first use" were not
  run.
* **No full offline re-gate.** `build-mt` would not rebuild
  (`c690_native_cull.c` without `-mfma`). The changed paths were checked
  with `build-play`'s replay binary instead (section 1.2).
* **No TEX_DIRTY paranoid check and no user visual test.** Both are live
  checks; the run ended at the hang.
* **No user command for the zoomed-out replay.** Neither B nor C won, B has
  a visual diff, and C hangs. Nothing is default-on.

## 5. Reproduce

The measured run (one launch; the `/tmp` control file is managed by the
script):

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=walkermt4live METER=1 PIN=0 WCB=2 N=4 SEG=10 DROP=2 \
  VIDEODRIVER=x11 PITCH_THRESH=35 LIMIT=1100 SETTLE=10 REPORT=0 \
  ARMS='A=PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=1|B=PS3RECOMP_WALKER_MT=1;PS3RECOMP_WALKER_MT_WORKERS=3;PS3RECOMP_WALKER_MT_TEX_RESOLVE=0|C=PS3RECOMP_WALKER_MT=1;PS3RECOMP_WALKER_MT_WORKERS=3;PS3RECOMP_WALKER_MT_TEX_RESOLVE=1' \
  BOOT_ARM='PS3RECOMP_WALKER_MT=0;PS3RECOMP_WALKER_MT_WORKERS=0;PS3RECOMP_WALKER_MT_TEX_RESOLVE=1' \
  ./ab_live.sh
```

Offline hash check used in section 1.2:

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_WALKER_MT=1 PS3RECOMP_WALKER_MT_WORKERS=3 \
PS3RECOMP_WALKER_MT_TEX_RESOLVE=0 REPLAY_NULL_HASH=1 \
REPLAY_NULL_VSRANGE=1 \
  ./build-play/ps3recomp_sdk/replay_rsx_engine ./meter_frame \
    --frames 5 --warmup 2
```

## 6. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The
ordered-record shape, copied borrowed payloads and sync-call drain are
reused from the MIT inFAMOUS 2 port (`rsx_be_thread.c`,
`bq_direct_begin`); the header and the packet block carry the required
notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

No RPCS3 or other GPL emulator source was read or cited.
