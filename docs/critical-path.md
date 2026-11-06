# Critical path: what limits the frame (master doc)

This is the single source of truth for **where frame time goes** and **what we work on next**. Every number here
names its scene, date, build and source. When a new measurement disagrees with a line here, update this doc in the
same commit and move the old line to "Replaced claims". Detailed write-ups live in [`notes/`](notes/); the running
logs are [`progress.md`](progress.md) and [`targets-and-backlog.md`](targets-and-backlog.md).

Last updated 2026-11-06.

## 1. The verdict so far (in one paragraph)

**Crowded scene, measured end to end in one run (2026-11-05 23:16:51, section 3a): the frame is the RSX walker.**
45.7 ms = walker drain 38.1 ms (83 %) + walker waiting for the PPU main thread 7.6 ms (17 %); the SPU mesh jobs cost
the walker ~0 ms and the SPU lanes are ~28 % busy. Main is blocked 34.7 of its 45.7 ms on the walker. Inside the drain:
texture work 10.6, parse/in-drain waits 9.2, draw recording 5.6, other 3.7, vertex fetch 3.3, pipelines 2.7 ms.
At the pitch meter the walker drain and the SPU producer are back to back (section 2), so there both must shrink.
**Direction (per the rule set before the run): renderer / walker drain. Confirmed live: the parked walker-diet + WCB switches double the crowded replay view, 15 -> 31-34 fps (section 3b).**

## 2. Pitch meter (the automated benchmark scene)

On-screen ~34-38 fps on our build; RPCS3 on this PC: 46-48 fps (SPU 40-50 %, RSX thread 4-6 %).

| fact | value | source |
|---|---|---|
| main thread run / blocked per frame | 9.3 ms run (p50 13.4), **19.7 ms blocked** on the ref cond `0x1245090` (`Gfx_WaitPrevFrameRef` 0x000655cc) | 2026-11-02 trace, one run, [`notes/ppu-critical-path.md`](notes/ppu-critical-path.md) |
| walker frame split | 29.0 ms = **drain 17.3** + producer wait 11.8 (PPU append/park 6.2, resume lag 2.8, **c690 SPU 2.7**, queueing 0.1) | same trace (`analyse_walker_wait.py`); a second run matched |
| main thread split (counter) | run 13.0, **RSX ref wait 15.2**, flip 0, **SPU 0.0**, other 0.8 ms/frame | 2026-11-05 `PS3RECOMP_SPU_CRIT`, build-sync, 1 run, [`notes/spu-codegen-v2.md`](notes/spu-codegen-v2.md) v4 |
| removing all draws (`EXP_NO_DRAW`) | ~0 fps change: the walker's wait moves from drawing to the **SPU mesh producer** (16.8 ms producer wait, 954 SPU PUTs in one 13.8 ms ref wait) | [`notes/meter-pacing.md`](notes/meter-pacing.md) |
| SPU c690 job 1.11x faster (FTZ) | fps flat (34.6 vs 35.1, CI +-2.9; repeat 35.6 vs 35.7) | v3 + v4 A/B, 2 runs |

**Reading:** at the meter the walker's drawing and the SPU mesh producer are about the same length and sit back to
back on the path, so speeding up *only one* of them shows ~0 fps. That is the root of the "renderer won't help" /
"SPU won't help" back-and-forth: both statements were measured, and both are true *for one change alone*. The meter
is a poor scene for judging single changes.

## 2b. Pitch meter, real frames, TICK_IN_DRAIN on (2026-11-06 10:12, build-depth = play build, one 14 s trace)

`captures/meter_trace.json`; `analyse_ppu_path.py --main-tid 8` (main is unnamed in a triggered trace) and
`analyse_walker_wait.py`, outputs in `captures/meter_analysis/`. 391 frames, 27.96 fps; real frames = flips
(`real_fps.py`: 28.6 / 28.9 real fps in the two 20 s arms), so per-flip numbers are per real frame here.

| per frame (mean ms) | | check |
|---|---:|---|
| frame (flip to flip) | **35.76** (p50 35.54, p90 38.62) | |
| ppu main running / blocked on the walker ref | **20.86** / 14.90 | sums to 35.76 |
| walker drain / put_wait | **22.38** / 13.44 | sums to 35.82 |
| put_wait split: caught up with PPU 7.15, resume lag 5.77, c690 before PUT 0.46, park 0.06 | 13.44 | 100 % |
| SPU lanes 0-4 (c690 allowed, MAXCONT 5) | **89 % busy** (c690 71 %, other 18 %) | c690 126.9 lane-ms/frame |
| SPU lane 5 (no c690) | 33 % busy | |

Reading: main, walker and the SPU lanes are all near the frame. The c690 lanes are 89 % busy, so with today's SPU
code speed and 5 c690 lanes the meter cannot pass ~31.8 ms (~31.4 fps) whatever the PPU or walker do: at most
~3.5 fps is left outside the SPU. Levers: (1) lifted SPU code speed (c690 bodies; FTZ was the first step),
(2) ~~the sixth lane~~ already tried: `SPURS_LANE6=1` wedged the guest in ~10 s; five mesh instances are a guest
protocol, not a runtime limit (`notes/c690-dispatch-cuts.md`), (3) main 20.9 ms running (PPU code)
and main/walker serialization (each waits on the other ~7-15 ms).

**Meter trace with approximate float on (2026-11-06 11:42, build-sync, `captures/meter_trace2.json`, main tid 7):**
frame 34.26 ms (29.2 fps) = main running 20.61 + blocked 13.65; walker drain 22.26 + put_wait 12.04 (caught up with
PPU 7.79, resume lag 3.91, c690 0.26). c690 lane time 126.9 -> 116.9 lane-ms/frame (-8 %); lanes 0-4 still 87-90 %
busy, of which "other" ~19 % is mostly the PM between jobs, not other workloads.

**SPU lane CPU with approx on** (perf 10 s, 6 `spurs pm` threads, C690_STATS=0; call graph 5 s): c690 job code 50 %,
runtime overhead ~21 %. Of that, ~8.3 % is the WWS PM between jobs: memmove 3.3 % (two per-job DMA GETs at PM
0x1F6C / 0x2030 via mfc_do_transfer_fast), `spu_indirect_branch_slow` 2.25 % (PM 0x0A60 / 0x2CD0), lockline 1.4 %,
lookup 0.7 %, drain_call 0.6 %. Inside the c690 jobs, dispatch is ~2.3 % (lookup 1.45 %, drain 0.8 %), so bigger
lifted regions ("block size") can remove at most ~2.3 %; the per-job PM switch is the larger lever.

**Walker phases at the meter, live (flip-weighted, 761 flips, wake + approx on):** walker 25.7 ms per flip = engine 22.6
(fetch 7.4, other 5.5, draw 5.3, pipe 2.8, tex 1.2, bind 0.4) + parse 3.1.

**Meter critical chain (approx trace, 408 frames, every frame sums to the flip interval within 1 us;
`captures/meter_analysis2/chain/chain.md`):** main runs from the ref release to its put 6.22 + MainMultiTask
semaphore 1.24 + walker wake 0.63 + **walker drain 21.73** + **walker resume lag 3.85** (4 ms idle quantum) + PUT
wait 0.25 + small 0.34 = 34.26 ms. c690 production (0.25), DisplayFlip (0.06) and main's post-put running (hidden
under the drain, then 11.9 ms slack) are off the chain. Replayed upper bounds: lag removed -2.33 ms; drain -10 %
alone -1.05 (half absorbed by lag), with lag removed -3.37; main 2 ms less before the put ~-1.9. The +1.6 fps of
approx float cannot come through the chain (c690 -8 %); likely host-core contention relief (untested). Gap: the
FIFO put time is not traced (main/walker handoff bounded 0-4 ms).

**Meter chain with the current best config (approx + wake + REMAP_FLAT, 2026-11-06 13:43, `captures/meter_trace3.json`,
428 frames, every frame sums within 1 us; scripts `captures/meter_analysis3/chain/`):** frame 32.62 ms (traced) =
main running to put 6.34 + MainMultiTask semaphore 1.34 + **walker drain 20.55** + **walker waits for c690 PUT 2.06**
(was 0.25) + resume lag 1.13 (was 3.85: wake works) + walker wake/gap 0.89 + small 0.30. Shrinking the drain now
exposes SPU mesh production behind it, which is why REMAP_FLAT (-2.8 ms walker) gave only +0.5 fps and the memo
(SPU) alone gave 0: drain and c690 production have to shrink together.

**Stats counter and SPU headers (2026-11-06 14:00-14:45, meter, approx + wake + REMAP_FLAT, 4 alternating launches x 2 x
15 s each, real fps):** with `PS3RECOMP_C690_STATS=1` the play-build headers give 32.2-32.5 and the current
ps3recomp headers 29.9-30.6; with stats **off**, both give **34.5-35.0**. So the diagnostic stats counter costs ~2.3
real fps (much more with the current headers, whose `SPU_DIRECT` calls `c690_stats_on_direct`); it is now default 0
in play_windowed.sh and ab_live.sh. The header difference itself is neutral live (offline -24 % on 0xFDA0 does not
show), so the lifted code keeps including the `recomp-work/runtime` (ps3recomp-native) copy;
`ab-tools/spu_include_fix.sh apply|revert` switches it.

## 3a. Crowded replay view, one trace that adds up (2026-11-05 23:16:51-23:17:05)

Replay camera, zoomed out over the field and stands, build-sync, `PS3RECOMP_TRACE` 14 s window, 2,481,785 events,
0 dropped; MangoHud showed 21 fps / 47 ms. Analysis in `recomp-work/captures/crowd_analysis/`
(`analyse_ppu_path.py --main-tid 7`, `analyse_walker_wait.py`).

| per frame (mean) | ms | check |
|---|---:|---|
| frame (flip to flip), 283 frames, 21.9 fps | **45.7** | = MangoHud 47 ms |
| main running / blocked on walker ref | 11.0 / **34.7** | sums to 45.7 |
| walker drain / waiting for a producer | **38.1** / 7.6 | sums to 45.7 |
| producer wait split: PPU append 4.1, PPU park 3.5, **c690 SPU 0.0**, resume 0.0 | 7.6 | 100 % attributed |
| while main waits: walker draining / waiting | 31.0 / 3.6 | |
| SPU lanes busy (6 lanes) | 28 % mean (c690 16 %) | jobs 441/frame |
| walker `[walker-prof]` (flip-weighted) | walker 35.5 = engine 26.3 (tex 10.6, draw 5.6, other 3.7, fetch 3.3, pipe 2.7, bind 0.3) + parse 9.2 | trace drain span 35.3 |

Not split yet: how much of the 38 ms drain is walker CPU vs blocking on the GPU inside the drain (one-shot readbacks).
An earlier, different run had the walker ~50 % on-CPU; `PS3RECOMP_WALKER_WAITS=1` (build-depth) measures it directly.

## 3b. Renderer switches, live A/B in the same replay view (2026-11-05 23:27 and 23:32, build-depth)

Two user-held runs, switches flipped every 15 s through `PS3RECOMP_CTL`; on-screen fps from MangoHud (first 2 s of
each window dropped). Visuals: no difference seen by the user at any switch point.

| arm (windows) | on-screen fps | typical frame |
|---|---|---|
| all off (6) | 13.9-16.0 | 71-87 ms |
| WCB_PREFETCH + WCB_STREAM (2) | 20.0, 21.9 | 57, 25 ms |
| VK_PASS_KEEP + VK_VTX_PRE + VK_DRAW_STATE ("diet", 2) | **30.4, 28.2** | 19, 27 ms |
| REMAP_DIRECT + REMAP_FAST (2) | 12.7, 16.4 | 86, 71 ms (no gain) |
| all seven (6) | **30.5-33.9** | 15-21 ms |

The walker-diet trio carries most of the gain; WCB adds ~3 fps on top; REMAP adds nothing here. p90 frames stay
~60 ms with everything on: the remaining spikes are the next target. Since 2026-11-05 the five keys (diet trio +
WCB_PREFETCH/STREAM) are default-on in play_windowed.sh (mlb16-recomp d0afeac) on build-depth; the user played with
them and saw no visual regression. Not yet re-checked with them on: team select (green box).

## 3c. Mound visit, renderer keys on (2026-11-06 02:15, build-depth, automated route)

Route (Claude-driven, user-authorized): ab_live.sh METER=0 parks on the first-pitch screen; then START, RIGHT
(Bullpen panel), X, DOWN (Mound Visit), X. Env = play_windowed's five renderer keys on, PS3RECOMP_TIMELINE=0.
Log: recomp-work/ab_live_mound_20261106_021158.{log,csv}.

| Measure (02:15:12 onward, ~40 s, 1 run) | value |
|---|---|
| MangoHud screen frames | 29.8 fps; median 35.4 ms, p90 49.8, p99 60.5, max 108 ms; 16 % of time in frames > 50 ms |
| walker per guest flip (flip-weighted, 369 reports / 1,086 flips) | busy 25.0 ms + waiting on main 18.3 ms |
| inside the busy 25.0 | engine 20.9 (fetch 6.8, draw 4.8, other 4.2, pipe 2.9, tex 1.9, bind 0.3) + parse 4.2 |
| draws per flip | 796 |

Read: the user's ~80 ms mound-visit frames (before the keys) are ~35 ms median now. Unlike the crowded replay view
(3a), the walker here waits on the PPU main thread for 42 % of each flip, so main is on the path too; a trace is
needed to split it. Texture work, the crowd view's top row, is small here; vertex fetch is the top engine row.
The one-shot walker capture taken at 02:15:11 recorded only a frame tail (65 words): the capture was bounded at the
PPU's flip request, not where the walker reaches the flip. Fixed in ps3recomp `exp/wcapfix` a159ed55 (fork); the
04:31:56 capture holds a whole frame: `recomp-work/captures/mound/m2_cap` (163,846 words, 0 replay misses, 768
draws, replay 31.8 ms/frame offline).

**Trace (2026-11-06 04:31:39, 13.98 s, 298 frames, build-sync, keys on; `captures/mound/m2_trace.json`,
`analyse_ppu_path.py --main-tid 8`, main is "host thread 8" in this trace).** Both chains add up to the frame:

| per frame, mean (p50 / p90) ms | |
|---|---|
| frame (flip to flip) | **46.9** (36.6 / 71.1) |
| ppu main = blocked on the walker ref + running | 31.0 + 16.0 = 47.0 |
| walker = drain + waiting for work (put_wait) | 28.7 + 18.3 = 47.0 |
| put_wait split: resume lag after the PUT / caught up with PPU put / c690 SPU / park patched | **11.4** / 5.6 / 0.9 / 0.3 |

Read: the walker is the chain main waits on (main blocked 31 of 47 ms), and the walker itself is idle 18 ms a frame,
11.4 of it *after* its work was already PUT. At the pitch meter that resume lag was ~2.8 ms and the wake fix
(`PS3RECOMP_WALKER_WAKE`, docs/notes/walker-wake.md) only moved the wait onto the SPU producer; here the SPU
producer costs 0.9 ms, so the wake fix is retested live in this scene (below). ~~SPU stays off the path.~~
**Corrected the same night (see next paragraph): in this scene SPU c690 production sets the pace.**

**Walker wake, live A/B in the mound visit (2026-11-06 04:41-04:44, build-sync, 6+6 paired 10 s segments,
`ab_live_mound_wake_20261106_043853`):** off 26.0 / on 26.0 fps, paired diff -0.03 +/- 0.41 fps, CI [-0.43, 0.38].
No gain here either: removing the post-PUT resume lag does not shorten the frame, so something else paces the walker.

**Dependency analysis of the same trace (2026-11-06, `captures/mound/analysis/resume_lag.md`, 297 frames):**
the 11.4 ms "resume lag" is real waiting but harmless: 100 % of it overlaps c690 SPU jobs still running (66 % ends on
the 4 ms idle timeout, 31 % on a game-thread gcm_finish kick). The analyser's "c690 0.9 ms" counts only the time
before the covering PUT; the jobs for the next slots are already running behind it. **The frame is bounded by c690
SPU production:** frame = 4.27 + 0.258 x c690 lane-ms (r = 0.95), 165 ms of c690 work per frame on 5 lanes that
average 4.06 busy. Accounting: 40.03 ms with >= 1 lane busy + 6.72 ms walker tail with no lane busy = 46.75 vs
46.83 ms measured. Frames are bimodal: 168 light (32.4 ms, lanes saturated) and 129 heavy (65.7 ms, 233 ms of c690
work, 61 % of wall time). Levers, ranked: c690 job cost (-10 % ~ -4.3 ms/frame); lane occupancy 4.06 -> 5 (upper
bound ~7.5 ms; why lanes idle not yet shown); walker drain CPU (-20 % ~ -1.3 ms tail only).
**So the direction differs by scene:** crowded replay view = walker (3a); pitch meter = walker and SPU back to back
(2); mound visit = SPU c690. A walker-only change cannot show here; an SPU change can.

Replay CPU profile of `m2_cap` (`captures/mound/analysis/drain/drain_profile.md`; offline replay is ~16-17 ms warm,
cache hits higher than live): top walker hot spots are rsx_vertex_remap_build ~4.9 ms/frame (per-draw remap of
134k refs through a hash table; cache on index-buffer address+generation est. -3 to -4.5 ms), big-endian f32 vertex
decode ~2.0 ms (SIMD byte swap est. -1 to -1.9 ms), texture-content and pipeline-key hashing ~3 ms (est. -0.8 to
-1.4 ms). These shorten the crowded view and the meter, not the mound visit.

**SPU FTZ, live A/B in the mound visit (2026-11-06 04:58-05:01, build-sync, 6+6 paired 10 s segments,
`ab_live_mound_ftz_*`):** off 25.4 / on **27.8 fps**, paired diff +2.47 +/- 1.50 fps, 95 % CI [+0.96, +3.97]; on won
all 6 pairs. First live gain from an SPU change, as predicted by the SPU-bound reading above. FTZ is exact on the
corpus (docs/notes/spu-codegen-v2.md, 4/4 pairs, LS+GPR diff 0). Not in build-depth (play build): it needs the
port/stable merge.

**Why the SPU lanes idle (same trace, `captures/mound/analysis/lanes/lane_idle.md`):** 36.6 lane-ms idle per frame
(4.12 of 5 lanes busy). 79 % (28.9 lane-ms) is the fixed per-job head/tail of the lifted policy module, ~35 us x
~820 jobs/frame, the same cost c690-lane-gaps.md found at the meter. Starvation, wake latency, end-of-frame tail and
ramp-up together are 6 %; dependency waits 7.7 %. No lane/SMT contention signal (inter-job gaps get shorter with
more busy lanes); pinning was neutral before. Heavy frames idle more only because they run 2x the jobs. So both
SPU levers are the speed of lifted SPU code: c690 job bodies (FTZ +2.5 fps live) and the PM's per-job code
(halving it est. -2.9 ms/frame). Already tried and neutral: SPURS_READY_WAKE; already on: native dispatch +
FAST_RESCHED=64.

## 3. Crowded scenes (driven by the user)

| fact | value | source |
|---|---|---|
| on-screen frame rate | **12-18 fps, typical frame 65-91 ms**, 72-96 % of time in frames >= 66 ms, spikes to 190 ms | 2026-11-05 22:54:49-22:55:34, MangoHud CSV, `recomp-work/play_20261105_225313` |
| game's own flip rate, same window | 20 flips/s (400 in 20 s), of which 16/s reach the screen (320 blits, all shown) | same log, `[flip]` and `[RSX vulkan] window` |
| main thread split (per guest flip, ~4.4 ref wakes per flip) | **RSX ref wait ~37 ms**, run ~13, **SPU ~0.7**, other ~0.1 | same log, `[spu-crit]` (per-wake values x 4.4) |
| walker on-CPU vs runnable-wait | on-CPU 501 ms/s, run-queue wait 2 ms/s; SPU lanes 121-479 ms/s; main 189 ms/s; 2.9 of 12 CPUs | earlier 2026-11-05 run, `tools/thread_sched.py` |
| walker blocking inside a drain | one-shot GPU waits 15-40 ms per heavy frame, mostly `eng:wcb_copy_surface` 14.9 ms + flush 6.1 (guest CPU reads of render targets) | earlier 2026-11-05 run, [`notes/oneshot-waits.md`](notes/oneshot-waits.md), [`notes/walker-waits.md`](notes/walker-waits.md) |
| `WCB_PREFETCH=1 STREAM=1` | one-shot waits 23 -> 15 ms, flips 27.5 -> 32.8/s (rough, 6 reports), no visible change | same session, live keys, 1 run |
| walker CPU hotspots | NVIDIA driver 25 %, `rsx_vertex_remap_build` 12.6 %, memmove 6.7 %, `eng_pipeline_get` 6.3 % | perf, crowded, earlier 2026-11-05 |
| depth-as-texture fix | walker 149 -> 82 ms per heavy frame | live, 2026-11-05 |

**Follow-ups 2026-11-06 morning (build-sync, exp/remapkey):**
- *Vertex byte swap is already SIMD.* The drain profile's "scalar BE f32 decode" was wrong: `rsx_loop_f32_all` compiles
  to one `vpshufb` per attribute (objdump). ~2 ns per attribute slot, bound by the random source loads; no cheap lever.
- *Remap memo hit rate, live at the meter* (`PS3RECOMP_REMAP_DIAG=1`, 1 run, 117 reports of 20k draws): the
  content-keyed memo hits **46 %** of refs; another **21 %** miss only because the pattern is shifted by a constant
  vertex/base offset; 33 % are new each time. `PS3RECOMP_REMAP_SHIFT=1` keys the memo relative to the first ref:
  offline m2_cap byte-identical (off / cache / shift), fetch_plan 5.08 / 2.2 / 2.56 ms on an all-hit replay.
  **Live meter A/B (6+6 paired 10 s, REMAP_CACHE=1 + FTZ + diet/WCB keys): off 39.45 / on 39.84 fps, diff +0.39,
  95 % CI [-0.58, +1.36]: neutral.** Parked default-off (`exp/remapkey` 0e1ebc92, fork).
- *SPU lane profile at the meter* (perf, 8 s, 6 `spurs pm` threads): c690 job code 46 %, `PS3RECOMP_C690_STATS`
  instrumentation **13 %**, indirect-branch lookups 7.6 % (dispatch cache already on), memcpy 3.8 %.
- *C690_STATS off in the mound visit: no gain* (2 launches x 4+4 x 10 s, same keys + FTZ): on 27.8 / 28.9 fps,
  off 27.2 / 27.7 fps; same-config arms differ by ~1 fps. The 13 % lane CPU is not on the frame's path.
  **Weak result:** two launches, and the mound visit varies between launches (user: sometimes 3 players, sometimes
  ~7). Only paired arms inside one launch compare like with like; record the player count (`n5_mound_visit.png`).

**Real frames vs flips (2026-11-06, review finding confirmed live).** The title re-flips the buffer already on
screen when its flip handler has not fired within two vblanks (~34.1 ms); MangoHud and `[flip]` count those as
frames. `[flip]` lines now carry `buf=` and `real=` (buffer changes; ps3recomp `exp/realframes` d74346b5) and
`recomp-work/ab-tools/real_fps.py <log>` scores an A/B on real frames (34 ms timing classifier for older logs; it
undercounts, prefer `real=`). **All fps numbers above this line are flip rates and overstate real frames.**

| A/B (meter unless noted) | flips/s A -> B | real fps A -> B | real diff |
|---|---|---|---|
| `PS3RECOMP_TICK_IN_DRAIN` 0 -> 1, live 2026-11-06 09:48, build-sync = port/stable + counter, play keys, 6+6 x 10 s | 36.98 -> 29.08 | **25.11 -> 29.06** | **+3.95 +/- 0.26** |
| `PS3RECOMP_SPU_FLOAT_APPROX` 0 -> 1 (opt-in approximate SPU float, `exp/spu-approx` 72269fce), meter live 2026-11-06 11:25, build-sync, play keys + TICK_IN_DRAIN + FTZ, 6+6 x 10 s | 29.30 -> 30.93 | **29.30 -> 30.93** | **+1.63 +/- 0.22** |
| `PS3RECOMP_WALKER_WAKE` 0 -> 1 (resume the walker at the covering PUT), meter live 2026-11-06 12:51, build-sync, approx on, 6+6 x 10 s | 30.73 -> 31.55 | **30.73 -> 31.55** | **+0.82 +/- 0.25** |
| `REMAP_CACHE`+`REMAP_SHIFT` 0 -> 1 with walker wake on, meter live 2026-11-06 12:57 | 31.37 -> 30.78 | 31.37 -> 30.78 | -0.58 +/- 0.43 (small loss; keep off) |
| `PS3RECOMP_REMAP_DIRECT` 0 -> 1 with walker wake on, meter live 2026-11-06 13:04 | 31.77 -> 32.02 | 31.77 -> 32.02 | +0.25 +/- 0.54 (neutral) |
| `PS3RECOMP_REMAP_FLAT` 0 -> 1 (`exp/drain-cuts` 3fcb5ade; offline meter_cap -2.5 ms, byte-identical on 4 captures), wake + approx on, meter live 2026-11-06 13:28 | 31.59 -> 32.10 (walker 29.1 -> 26.3 ms/flip) | 31.59 -> 32.10 | +0.51 +/- 0.56 (walker -2.8 ms, frame barely moves: another limit binds near 32 fps) |
| `SPU_CODE_DMA_MEMO` 0 -> 1 again, with wake + REMAP_FLAT + approx on, meter live 2026-11-06 13:34 | 31.47 -> 31.42 | 31.47 -> 31.42 | -0.05 +/- 0.27 (neutral) |
| `SPU_CODE_DMA_MEMO` 0 -> 1 (skip unchanged per-job code GETs), meter live 2026-11-06 12:27, approx on; prior live audit (`SPU_CODE_DMA_MEMO_AUDIT=1`, `exp/pm-overhead`): 7.11 M O(1) skips, 0 mismatches | 30.80 -> 30.50 | 30.80 -> 30.50 | -0.30 +/- 0.32 (neutral) |
| same approx switch, **mound visit** live 2026-11-06 11:36, build-sync, 4 players, 6+6 x 10 s | 21.20 -> 22.27 | **21.20 -> 22.27** | **+1.07 +/- 0.16** |
| same, **mound visit** live 2026-11-06 10:03, build-depth, ~7 players, 6+6 x 10 s | 23.72 -> 16.26 | 15.52 -> 16.26 | +0.74 +/- 1.14 (neutral) |
| same, re-scored 2026-11-04 `pacingfloor` (timing classifier, 4 pairs) | 33.4 -> 25.5 | 21.7 -> 24.7 | +3.0 +/- 2.6 |
| SPU FTZ, mound (timing classifier) | 25.5 -> 27.8 | 17.9 -> 18.9 | +1.0 +/- 2.0 |
| REMAP_SHIFT (timing classifier) | 39.4 -> 39.8 | 22.3 -> 22.8 | +0.5 +/- 0.6 |
| walker wake, mound (timing classifier) | 26.2 -> 26.1 | 16.7 -> 17.1 | +0.4 +/- 0.7 |

With TICK_IN_DRAIN=1 every flip is a new buffer (real = flips): the drain fires the flip tick, so the handler is no
longer late and the title stops sitting out the 34 ms timeout. It was rejected 2026-11-04 on the flip rate (-8 fps).

## 4. Open conflicts (must be explained, not ignored)

1. **Guest flips vs screen frames (explained 2026-11-05):** in 22:54:50-22:55:10 the game issued **400 flips** but the
   Vulkan backend made only **320 blits** and showed all 320 (`[RSX vulkan] window` lines: `shown` = `blits`,
   `dropped 0`); MangoHud counted 313. So nothing is lost at present: ~20 % of guest flips never become a blit
   (likely several flips handled inside one walker drain, only the last one presented; not yet confirmed in code).
   **Use MangoHud / `shown` for fps.** `[flip]` counts game flip requests, not frames on screen.
2. **Walker time (resolved 2026-11-05):** the "counters cover ~5 of 20 s" claim was an orchestrator arithmetic error:
   `[walker-prof]` values are per-flip averages and must be flip-weighted, not summed. Weighted, they give 35.5 ms per
   flip, matching the trace (section 3a).
3. **SPU share in crowds (resolved 2026-11-05):** in the 3a trace the walker waits 0.0 ms/frame on c690 output; its
   whole 7.6 ms producer wait is the PPU main thread. The SPU is off the crowded critical path in this scene.

## 5. Crowd trace recipe (used for 3a; reuse for any scene)

The only tool that has ever made a frame add up to 100 % is the 2026-11-02 trace (all threads, one timeline). Run it
once in a crowd, nothing else changed:

```
cd ~/Documents/mlb16-online/recomp-work
BUILD=build-sync PS3RECOMP_TIMELINE=0 PS3RECOMP_SPU_CRIT=1 PS3RECOMP_C690_STATS=1 \
PS3RECOMP_TRACE=$PWD/captures/crowd_trace.json PS3RECOMP_TRACE_TRIGGER=$PWD/captures/crowd_go \
PS3RECOMP_TRACE_SECS=14 PS3RECOMP_TRACE_RING_MB=32 PS3RECOMP_TRACE_FLUSH_MB=8 ./play_windowed.sh
# in a second terminal, once you are in the crowd:  touch ~/Documents/mlb16-online/recomp-work/captures/crowd_go
# PS3RECOMP_TIMELINE=0 matters: play_windowed.sh otherwise turns on the hitch recorder, which takes precedence over
# PS3RECOMP_TRACE and keeps only windows around >= 250 ms hitches (2026-11-05 23:14 run lost this way).
```

Analysis (offline): `tools/analyse_ppu_path.py` and `tools/analyse_walker_wait.py` on `captures/crowd_trace.json`,
cross-checked against MangoHud for the same 14 s. **Acceptance:** main + walker + SPU-producer + GPU-wait links sum
to the on-screen frame within 10 %. Only then is the direction picked.

**Decision rule (set before the run):**
* walker waits mostly on the SPU mesh producer -> SPU codegen (register-mapped backend) is the main project;
* walker mostly busy or blocked on GPU readbacks -> renderer (drain cost, WCB prefetch/stream default-on, then the
  walker-diet keys);
* both large -> both, starting with the bigger link.

## 6. Replaced claims (do not repeat these)

| claim | where it came from | why it is wrong |
|---|---|---|
| "At the meter the SPU work sets the pace" | orchestrator summary, 2026-11-05 | main waits 0 ms on SPU; walker drain is the longest link (section 2). SPU matters only *together* with the drain. |
| "Speeding up the renderer will not change anything" | orchestrator, from `EXP_NO_DRAW` = 0 fps | true at the meter for the drain alone; not a statement about crowds, where main waits ~37 ms per flip on the walker |
| "Walker counters cover only ~5 of 20 s" | orchestrator, summed per-flip averages | flip-weighted they match the trace (35.5 vs 35.3 ms) |
| "Crowd runs at ~20 fps / 50 ms" | orchestrator, from guest `[flip]` | the screen shows 12-18 fps, 65-91 ms typical (MangoHud) |
| "SPU codegen is 99 % of the work" | direction set 2026-11-05 evening on the first claim above | on hold until section 5 is measured |
| "TICK_IN_DRAIN loses 8 fps" | meter A/B 2026-11-04, MangoHud | the lost flips were timeout re-flips; real frames +3.95 fps (above) |
| "Vertex f32 decode is scalar; SIMD byte swap saves 1-1.9 ms" | drain_profile.md, 2026-11-06 | it is already SSE (`vpshufb`), see 3c follow-ups |
| "C690_STATS costs no fps" | mound A/B, 2 launches, 2026-11-06 morning | weak test (mound varies per launch); at the meter it costs ~2.3 real fps (4+4 launches) |
| "Lifted SPU is slow because of register spills" | spu-regs / audits | float-semantics emulation is ~2/3 of hot SPU time; spills ~19 % ([`notes/spu-codegen-v2.md`](notes/spu-codegen-v2.md)) |

## 7. Built and parked (switch, default, measured effect)

| switch | default | effect |
|---|---|---|
| `PS3RECOMP_SPU_FTZ` | 0 | c690 job 1.11x, lane time -7 %, fps flat at meter; **mound visit +2.5 fps (25.4 -> 27.8, CI +1.0..+4.0)**; bit-exact |
| `PS3RECOMP_RSX_WCB_PREFETCH` + `STREAM` | 0 | crowd one-shot waits 23 -> 15 ms (rough) |
| `PS3RECOMP_VK_PASS_KEEP` / `VK_VTX_PRE` / `VK_DRAW_STATE` | 0 | offline walker 43-46 -> 38 ms, byte-identical frame |
| `PS3RECOMP_REMAP_DIRECT` + `REMAP_FAST` | 0 | offline walker -4 ms |
| `PS3RECOMP_BINK_CLOSE_KEEP` | 1 | fixes the green box (shipped) |
| `PS3RECOMP_C690_STATS` | 1 in play_windowed | ~5 % of SPU-thread CPU; set 0 for A/B |
| `PS3RECOMP_CROWD_FIX` | 0 | crowds: split a BEGIN/END at each draw-type/vertex-state change (2,170-batch group -> 268 runs in the offline crowded replay); non-crowd captures byte-identical, key off byte-identical ([`notes/crowd.md`](notes/crowd.md)); **user-verified live 2026-11-06 00:2x: crowd looks right**, +2.6 ms walker offline (+267 draws) |
| `PS3RECOMP_REMAP_SHIFT` (+ `REMAP_CACHE`) | 0 | remap memo keyed relative to the first ref; byte-identical offline; live meter +0.39 fps, CI [-0.58, +1.36] |
| `PS3RECOMP_WIPE_FIX` | 0 | tiled logo wipe: the wipe draw samples a 512x256 Morton-format A8R8G8B8 render target whose guest bytes are our linear readback; probe both layouts on 8-texel neighbours and read the bytes as stored. Wipe replay checkerboard -> correct card; crowd and meter captures byte-identical on and off ([`notes/tiled-wipe.md`](notes/tiled-wipe.md)) |

Dead ends (measured, do not retry): SPU register tweaks, inlining callees, cheaper `frsqest`, exact lazy sign-of-zero,
WWS native ticket claim alone, RSX_SYNC_FIX, frames-in-flight (design shelved).
