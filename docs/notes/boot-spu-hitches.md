# Boot SPU hitches, the 2 s replay hitch, and the missing 3D logo

Date: 2026-11-03 (play test 20:26). **Static work only**: no game was launched
and nothing in `ps3recomp-play` was edited. Sources are the EBOOT in Ghidra,
`recomp-work/play_20261103_202606.log` with its MangoHud CSV and 2 s
thumbnails, `recomp-work/play_20261103_202606/hitches.pstl` parsed with
`mlb16-recomp/tools/hitch_table.py` (thread names read from the PSTL name
chunks), the runtime sources in `ps3recomp-play` (read only), and
`hitch-census.md`, `tick-and-resync.md`, `team-select-and-logos.md`,
`meter-pacing.md`. No GPL emulator source was read.

Run: `play_20261103_202606` (20:26:06-20:30:30, `build-play`, WCB=2,
MangoHud, 2 s thumbnails), launched with
`MLB16_CULL_BYPASS=1 PS3RECOMP_AUDIO_DIAG=1 GCM_PARK_GRACE_MS=600000`.
The timeline threshold is 250 ms, so `hitches.pstl` contains 19 hitches;
the census below uses all of them (the 14 boot hitches, the 5.5 s match load,
the 2.05 s replay hitch, and three later ones).

## TL;DR

| # | Question | Answer |
|---|---|---|
| 1 | 14 start-screen hitches of 0.28-0.95 s at 21-34 s | All are the **`Wws_Job/DXTJob` SPURS workload** (wid 0, maxC=4) running on image 6 (the shared WWS policy module) on 4 of 6 lanes, plus a little `nextsynthJobLst` (wid 3, maxC=1). The guest main PPU thread polls `sys_timer_usleep(200)` in a job-chain completion loop (`FUN_0006e8e0`, LR `0x0006E92C`) 2,674-3,803 times per hitch. The SPU work is **pure lifted compute**: no `spu.interp`, no mailbox/tag/event waits, no overlay cost, and host CPU load is only 26-36 %. The title caps DXT at 4 lanes, so two lanes idle while the 4 busy lanes run for the whole frame. Cheapest fix: let the workload use 6 lanes (HLE override for maxContention); probe is one start-screen run with that override. |
| 2 | 2,050 ms hitch at 216.9 s | An instant-replay / depth-of-field camera frame. Inside it: `DXTJob` 917 ms average-lane (4 lanes; busiest lane 1.41 s), `nextsynthJobLst` 421 ms lane, `LibMP3` 337 ms lane, `MeshJobs1/0` 173/96 ms lane, `c690.job` 182 ms, 96 pipeline creates / 192 shader translations (73 ms), 44 surfaces (24 ms), 20 texture uploads (14 ms), 4 reads (0.1 ms). The main PPU thread polls `usleep` 1,527 ms (6,645 calls); the walker waits 1.74 s for the producer. It is the same DXT + first-use shader mix as the boot, in a replay-camera scene. |
| 3 | 3-D logo still missing with `GCM_PARK_GRACE_MS=600000` | **The park/resync theory is disproved for this run.** The log has **zero** `FIFO resync` lines and `hitches.pstl` has **zero** `gcm.resync` events; the 16:37 run (default 10 s grace) has zero too. A 600 s grace cannot fire in a 4.5 min session anyway. Team select is otherwise healthy (1,296 walker reports, max wait 38.8 ms, 55 draws/frame, no c690, no I/O). The case is now "logo draw not submitted / not visible for another reason", and the next probe is a FIFO dump of one team-select screen plus a state bisect. |
| 4 | Throw-power meter fills slower | Per guest update (per frame), not per wall-clock. The meter position is computed from the delivery/game value each update (`FUN_007041e4` maps it to a fixed 64-pixel scale) and the title has no vblank/wall-clock pacing of its own (no `cellGcmSetFlipMode` call, no `cellGcmGetVBlankCount` import, dead 16,683 us deadline). At ~30 fps it therefore fills at roughly half the 60 fps rate. Probe: the title's own `PITCH METER PositionPct` print per frame. |

## 0. What was recorded

19 hitches, 19.0 s total, in a 264 s run. The per-hitch table from
`hitch_table.py`:

| t (s) | ms | dominant | cause ms | detail |
|---:|---:|---|---:|---|
| 21.52 | 891.5 | `spu.run` | 605.9 | |
| 22.52 | 693.0 | `spu.run` | 455.1 | |
| 23.32 | 842.6 | `spu.run` | 393.0 | |
| 24.28 | 701.7 | `spu.run` | 454.7 | |
| 25.05 | 885.2 | `spu.run` | 584.2 | |
| 26.01 | 953.5 | `spu.run` | 406.6 | |
| 27.03 | 732.8 | `spu.run` | 444.0 | |
| 27.83 | 671.4 | `spu.run` | 433.5 | |
| 29.09 | 276.3 | `spu.run` | 130.7 | |
| 30.05 | 325.3 | `spu.run` | 209.5 | |
| 30.49 | 835.9 | `spu.run` | 413.1 | |
| 31.48 | 923.0 | `spu.run` | 443.3 | |
| 32.55 | 940.0 | `spu.run` | 577.4 | |
| 33.59 | 462.6 | `spu.run` | 305.3 | |
| 134.53 | 5551.5 | `rsx.drain` | 3756.0 | shader 3,063 ms |
| 151.46 | 649.2 | `rsx.drain` | 644.3 | shader 337 ms |
| 216.94 | 2050.3 | `spu.run` | 917.5 | shader 73 ms |
| 257.74 | 302.9 | `spu.run` | 119.5 | |
| 259.47 | 306.8 | `c690.job` | 138.9 | |

The 14 boot hitches are 21.52-33.59 s (sum **10,134.8 ms**), i.e. just after
the start screen appeared (~20:26:20; the frames are 20:26:27.5-20:26:39.6).
The 5.55 s match load is at 134.5 s and is the known shader-translation case
(the shader cache was emptied by the review fix, `docs/notes/shader-disk-cache.md`);
it is not re-analysed here. The 216.9 s hitch is section 2.

Thread ids in the PSTL are host-thread slots in order of first use. Parsing
the name chunks gives the mapping used below: 2-7 `spurs pm` (the six lanes),
8 the boot guest main PPU thread (its name predates trace init), 18
`ppu GEM PPU Thread`, 22-27 the DataStream threads, and so on.

## 1. The 14 start-screen hitches

### 1.1 Which SPU workloads run

Every SPU span in the 14 hitches is **image id 6** on the six `spurs pm`
tracks. Image 6 is the WWS policy module, not one workload per wid: the log
shows every workload resolved to the same module (`wid=0..6 PM resolved
fp=0x75BFE68AE96D7577 image=6`, log lines 55, 80, 81, 503, 512, 15263,
15668). The workload names and their contention limits are in the AddWorkload
lines:

| wid | name | minC | maxC | boot lane-time |
|---:|---|---:|---:|---:|
| 0 | `Wws_Job/DXTJob` | 1 | **4** | **33.5 s** |
| 1 | `Wws_Job/MeshJobs0` | 1 | 5 | 0 (not run in these frames) |
| 2 | `Wws_Job/MeshJobs1` | 1 | 5 | 0 |
| 3 | `Wws_Job/nextsynthJobLst` | 1 | 1 | 1.60 s |
| 4 | `Wws_Job/LibMP3 JobList` | 1 | 1 | 0 (10.8 ms in the last hitch) |

The separate SPURS taskset (image 5, `MkJob`/job-manager task 0) is asleep in
`WAIT_SIGNAL` throughout (`[spu_workload] task 0 ... sleeping 13s in
WAIT_SIGNAL`); it is not the producer here. There are **no `spu.job` spans**
in these frames: the work runs inside the image-6 policy module.

The DXT workload accounts for 33.5 s of lane time in a 10.13 s wall window,
and the `nextsynthJobLst` workload 1.6 s. On four lanes 33.5 s is 8.4 s of
wall, i.e. ~83 % of the hitch time; the rest is the per-frame handoff and the
synth job.

Representative per-hitch lane use (ms; T2-T7 are the six lanes):

| t | frame | DXT lane-ms | synth lane-ms | lanes |
|---:|---:|---:|---:|---|
| 21.5 | 892 | 3,487 | 150 | T5 885, T2 872, T3 870, T4 868, T6 141 |
| 26.0 | 954 | 2,300 | 141 | T2 940, T4 903, T3 319, T5 240, T6 37 |
| 32.5 | 940 | 3,322 | 143 | T5 933, T3 882, T4 805, T2 743, T6 101 |
| 33.6 | 463 | 1,753 | 69 | T3 456, T2 455, T5 427, T4 421, T6 69, T7 3 |

Four lanes run for essentially the whole frame; T6 is mostly idle and T7 is
not used until the last hitch. That is **the title's own `maxC=4` for DXTJob**,
not our scheduler losing lanes: `spurs_pm_fast_can_rerun` refuses to exceed
`maxcont` read from the guest workload (`libs/spurs/cellSpurs.c:1830-1850`).

### 1.2 What the PPU waits on

The main PPU thread (track `host thread 8`; "host thread 8" is the boot
thread whose track name predates trace init) does nothing but short sleeps:

| hitch | `ppu.usleep` | calls | guest LR |
|---:|---:|---:|---|
| 21.5 | 888.4 ms | 3,548 | `0x0006E92C` |
| 26.0 | 949.4 ms | 3,803 | `0x0006E92C` |
| 32.5 | 936.7 ms | 3,748 | `0x0006E92C` |

The LR resolves to `FUN_0006e8e0` (`EBOOT.elf:0x0006e8e0`), which is the
title's own job-chain completion loop:

```c
FUN_009fbd30(local);                          /* snapshot the chain state */
while (FUN_009fad08(local_c, 3, local[0]) == 0)   /* chain 3 not done */
    syscall_sys_timer_usleep(200);
FUN_009fc018(...);
```

`FUN_009fad08` reads the per-lane job counters and returns whether the
requested count has completed. So the PPU is **polling at 200 us**, not
blocked on a condition or on I/O; the long `ppu.lwcond` spans on the other
tracks in the same window are idle threads whose spans start before and end
after the frame and are clipped to the window, not a producer wait.

No other work explains the frame:

| work in the 14 hitches | value |
|---|---|
| `spu.interp` | 0 |
| `spu.mbox_wait` / `tag_wait` / `event_wait` / `out_mbox_wait` | 0 |
| `spu.overlay_verify` | ~1 ms per hitch |
| `spu.overlay_load` | zero-duration (no module streamed in) |
| `fs.read` in the hitches | 0-3 calls, <0.1 ms |
| `rsx.drain` | 0.6-0.7 ms |
| MangoHud `cpu_load`, 21-34 s | mean 26.3 %, max 35.8 % |

### 1.3 Why each takes so long here, and what the PS3 would overlap

The 4-lane run is **compute-bound recompiled SPU code**:

* It is not interpreter fallback: there are no `spu.interp` spans at all in
  these frames.
* It is not DMA, channel or tag stalls: all SPU wait categories are zero, and
  `spu.policy ≈ spu.run` (3,505 vs 3,505 ms at 25.05 s), i.e. the lanes are
  executing, not waiting.
* It is not host contention: only four of the six lanes are runnable, and the
  whole machine averages 26 % CPU.
* It is not overlay loading: `spu.overlay_verify` is ~1 ms per frame and
  `spu.overlay_load` is zero.

The only remaining cost is the lifted/recompiled SPU instruction rate on the
four permitted lanes. On a real PS3 the same `DXTJob` also uses at most four
SPEs (the guest sets `maxC=4`), but each SPE runs its native code; the PPU's
200 us poll would still be there but would see the chain complete in a small
fraction of the wall time. The PS3 overlap is the PPU poll, the RSX and the
audio running on separate units while the four SPEs decompress; it is the
per-SPE speed, not extra parallelism, that we lack.

### 1.4 Cheapest fix and probe

**Fix (cheap):** raise the DXTJob contention limit from 4 to 6 in the SPURS
HLE. Two lanes are idle through every boot hitch, and the workload is already
split into 355 short policy quanta across the 14 frames, so it should
partition further. A single env knob in `cellSpursAddWorkload*`
(`PS3RECOMP_SPURS_MAXC=6`, applied to wid 0 or to all workloads) is enough for
the experiment and needs no guest patch. Expected ceiling: the 4 saturated
lanes are ~8.4 s of the 10.1 s; six lanes could take that toward ~5.6 s
(about -33 % on the boot hitches) if the job scales.

Do **not** ship it as an unconditional change: `maxContention` is guest
policy and the title may rely on it (it is also 5 for the mesh workloads and 1
for synth/MP3). Make it an opt-in override, measure, and keep it only if the
asset results are identical.

**Probe:** one `play_windowed.sh` launch at the start screen with the override
on. Measure the 14 hitch durations and the per-lane `spu.run`/`spu.policy`
split from `hitches.pstl`; compare T2-T7 saturation and the wid-0 lane-ms.
Also check the log for asset errors and that no `spu.mbox_wait`/`tag_wait`
appears (if the extra lanes contend on a shared lock, those waits will show).
The same run should confirm the DXT job is divisible: its 355 policy quanta
across 14 frames are many small jobs, not one monolithic one.

An alternative (more expensive) fix is to cache the decoded frontend textures
across runs, like the shader cache; that removes the work instead of widening
it. That is a larger change and should follow the 6-lane experiment.

## 2. The 2,050 ms in-game hitch at 216.9 s (20:29:41.4-43.4)

The 2 s thumbnails on either side (`...782711`, `...784801`) show the replay
camera with the `RECORDING` and `DEPTH OF FIELD` HUD, so the frame is the
title's switch into an instant-replay/DOF camera, not the pitch or the match
load.

Inside the 2,050.3 ms window:

| work | value |
|---|---|
| `DXTJob` (wid 0) | 5,504.7 lane-ms = **917.5 ms** average-lane; busiest lane **1,409 ms**, lanes T2-T5 1,096-1,409 ms, T6 251 ms, T7 26 ms |
| `nextsynthJobLst` (wid 3) | 421.5 ms lane (3,201 quanta) |
| `LibMP3 JobList` (wid 5) | 337.2 ms lane (3,188 quanta) |
| `MeshJobs1` / `MeshJobs0` (wid 2/1) | 173.5 / 96.3 ms lane |
| `c690.job` | 2,420 jobs, 181.8 ms lane |
| `shader.compile` | 192 spans, **72.7 ms** |
| `pipe.create` | 96, 2.5 ms |
| `surface.create` | 44, 24.4 ms |
| `tex.upload` | 20, 13.9 ms |
| `fs.read` | 4, 0.1 ms (on a sound thread) |
| `rsx.drain` | 308.1 ms (581 passes) |
| walker `rsx.put_wait` / `rsx.idle_wait` | 1,743 / 1,595 ms (overlapping waits for the producer) |
| main PPU `ppu.usleep` | 1,527.5 ms over 6,645 calls (LR `0x6E92C`, the same chain-3 poll) |
| main PPU `gcm.finish` | 50.1 ms |
| GEM PPU event queue | 133.4 ms |

So the 2,050 ms is not a shader hitch: shader translation is only 73 ms of
it. It is the DXT texture workload again (917 ms average-lane; on four lanes
that alone is ~1.4 s of wall), plus the small synth/MP3/mesh workloads that
come with the replay, while the main thread polls the same chain-3 completion
loop and the walker waits for the producer.

The shader cache was cold in this run: `[shader-cache] 896 stores, 0 hits,
896 misses` and `[rsx engine] 448 pipelines created by frame 11893` at
20:29:43. The 96 pipeline creates inside this frame are part of that first-use
burst (the cache was emptied by the review fix, `docs/notes/shader-disk-cache.md`),
which is why a replay camera that was not seen earlier pays it now. The
texture and surface creates point at the replay's own render targets and
textures, again first-use.

There is no file I/O and no missing asset in the window; the title simply has
to fetch/decompress a replay-sized batch and compile its new pipelines on the
same frame.

## 3. The missing 3-D team logo: the park/resync theory is wrong in this run

### 3.1 What the run says

The run's environment was `GCM_PARK_GRACE_MS=600000`. If the logo loss were
the jump-park resync, this run should have been the control that shows it.
It does not:

* `grep 'FIFO resync' play_20261103_202606.log` -> **0 matches**.
* `grep 'resync-fix: walking'` -> **0 matches**.
* `hitches.pstl` has **0** `gcm.resync` events (event 65) and only 2
  `rsx.park` events (both outside the logged resync paths).
* The 16:37 run (`play_20261103_163747.log`, default 10 s grace) also has
  **0** `FIFO resync` lines.

Every resync reason (`jump-park`, `stuck-get`, `circling`, unmapped
JUMP/CALL) goes through `gcm_fifo_resync_why`, which logs the first eight and
emits `PS3_TR_GCM_RESYNC`; neither happened. A 600 s grace also cannot elapse
in this 264 s session, so the knob could not have fired even if a park had
stood.

The earlier note that "the logo showed with the grace effectively infinite"
(`progress.md`, 10-27 evening) must have been a different build or a
coincidental view; the recent static-safe runs do not reproduce it. The
missing logo is **not** the FIFO jump-park resync.

### 3.2 What the team-select window does show

Team select is 20:27:46-20:28:16 (thumbnails `...660553`, `...668905`,
`...677254`, `...681409`; Mets away, Royals/Twins home). Over that window:

| metric | value |
|---|---|
| walker reports | 1,296 |
| max report gap | 39 ms |
| max walker wait | 38.8 ms |
| max walker time | 34.2 ms |
| draws per frame | mean 55.5, max 281 |
| pipelines / surfaces | 13 / 0 |
| `c690` lines | 0 |
| file opens | none until 20:28:11 (match-load cache/ROM opens) |

So the renderer is producing normally, at ~30 fps, with no dropped-frame or
I/O signature. The 2-D crests, the team cards and the middle `AT` panel are
present in the thumbnails; only the floating 3-D model is absent (the faint
large crest/script visible behind the right card is the background watermark,
not the 3-D logo). This is a **specific draw or state**, not a general frame,
texture, or file failure.

### 3.3 Next probe

1. **Confirm whether the guest submits the logo draw.** Capture the FIFO of
   one team-select screen and replay it offline:
   `PS3RECOMP_DUMP_FIFO=/tmp/teamselect.bin PS3RECOMP_DUMP_FIFO_FRAMES=60
   PS3RECOMP_DUMP_FIFO_SKIP=<frames to team select>`, then
   `tools/replay_rsx` on the dump. Search the draw list for the `logo3d_*`
   model's vertex range and its texture. If the draw is absent, the frontend
   never issues it (UI/model-loading path), and the walker is not the suspect.
2. **If the draw is present, bisect the state with the existing knobs on the
   same screen**, one at a time:
   `PS3RECOMP_ENG_TEXMEMO=0` (texture keying/memo), `PS3RECOMP_ENG_TEXHASH_FULL=1`
   (texture hash coverage), `PS3RECOMP_ENG_FSBIND_ALL=1` (fragment state
   binding), `PS3RECOMP_ENG_DEPTH_VIA_GUEST=1` (depth), and
   `GCM_SEMA_ACQUIRE=1` (draw waits for the mesh producer). Whichever restores
   the logo names the component.
3. If none does, the missing piece is a per-draw drop reason in the walker
   (primitive 0, zero vertices, cull, scissor, depth, missing pipeline /
   texture, index range). `rsx.draw_submit` only counts batches, so the
   current trace cannot say which draw disappeared.

Do not spend another run on `GCM_PARK_GRACE_MS`; it is ruled out for this
symptom.

## 4. Note: the throw-power meter fills slowly (low priority)

The meter value is advanced by the title's per-update (per guest frame)
gameplay logic, not by wall-clock time and not by the raw pad-report rate. The
pitch-meter display path is direct evidence: `FUN_007041e4`
(`EBOOT.elf:0x007041e4`) computes the displayed position as
`(1.0 - object+0xc) * 64.0` (constants at `0x00fb5770` = 1.0 and `0x00fb621c`
= 64.0) from the object's stored position, and compares it to fixed
pixel thresholds (perfect at 16/64); the position itself is produced each
update by `FUN_00be02e0` from the pitcher's delivery value (`param_1+0x29bc`)
through a fixed mapping, with no time argument. The title also never calls
`cellGcmSetFlipMode`, does not import `cellGcmGetVBlankCount`, and its vblank
handler's 16,683 us deadline has no reader (`meter-pacing.md`), so there is no
wall-clock/vblank pacing for the meter to fall back on. At our ~30 fps the
same per-frame increment therefore produces about half the fill rate of the
60 fps target; this is consistent with the user's observation, and the
per-frame pad poll is the same cadence, so "per poll" and "per frame" cannot
be separated by the pad rate alone. Probe (when convenient): enable the
title's own `PITCH METER PositionPct: ...` print (`FUN_00052b5c`) and log the
position once per frame at 30 fps and at a forced faster tick; the slope per
**frame** should be constant, the slope per **second** should scale with the
frame rate. Caveat: this was checked on the pitch meter; the fielding
"ThrowMeterButton" widget's increment was not located in the time available,
so the throw-specific meter is the same inference, not a second direct
reading.

## Caveats

* One play-test launch, no A/B. The boot workload attribution is from the
  timeline and the `AddWorkload` lines, not from a per-job name trace; the
  policy module runs the job code, so `spu.job` is empty by construction.
* The six-lane experiment is a proposal, not a measurement. The expected
  -33 % assumes the DXT job scales from 4 to 6 lanes; the job's granularity
  (355 quanta in 14 frames) suggests it will, but only the run can prove it.
* Per-lane sums from the PSTL are clipped to the hitch window; `spu.run` is
  six lanes, `spu.policy` is the enclosing module span, and they overlap.
  The 917 ms figure in section 2 is `spu.run` lane-time / 6, as in the census.
* The team-select 3-D logo is a visual symptom: the run proves it is not the
  walker's jump-park resync, not which draw is missing.
* The MangoHud CSV has only package-level `cpu_load`, not per-core; "not host
  contention" rests on that package average and on the idle-lane evidence.

## Reproduction

```sh
python3 mlb16-recomp/tools/hitch_table.py \
  recomp-work/play_20261103_202606/hitches.pstl --top 30
```

Thread names are in the PSTL name chunks; the analysis used the same parser
as `tools/hitch_table.py` plus a name-chunk read. The run's log lines cited
above are from `recomp-work/play_20261103_202606.log` (e.g. 43, 55, 64, 76,
498, 509 for the workloads; 30006-30007 for the replay shader cache). The
thumbnails are under `recomp-work/play_20261103_202606/`.
