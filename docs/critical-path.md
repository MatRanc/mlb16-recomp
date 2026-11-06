# Critical path: what limits the frame (master doc)

This is the single source of truth for **where frame time goes** and **what we work on next**. Every number here
names its scene, date, build and source. When a new measurement disagrees with a line here, update this doc in the
same commit and move the old line to "Replaced claims". Detailed write-ups live in [`notes/`](notes/); the running
logs are [`progress.md`](progress.md) and [`targets-and-backlog.md`](targets-and-backlog.md).

Last updated 2026-11-05.

## 1. The verdict so far (in one paragraph)

In both measured scenes the PPU main thread spends most of each frame **waiting for the RSX walker** (the host thread
that turns the PS3's GPU command stream into Vulkan), and **almost no time waiting on SPU work**. What is *not* known
is what the walker itself is doing during the crowded frame: its own counters explain only a fraction of it. So
"the renderer is the bottleneck" is half proven: we know *who* main waits on, not *why* that thread is slow. Section
5 is the one measurement that closes this.

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

## 3. Crowded scenes (driven by the user)

| fact | value | source |
|---|---|---|
| on-screen frame rate | **12-18 fps, typical frame 65-91 ms**, 72-96 % of time in frames >= 66 ms, spikes to 190 ms | 2026-11-05 22:54:49-22:55:34, MangoHud CSV, `recomp-work/play_20261105_225313` |
| game's own flip rate, same window | 20 flips/s (400 in 20 s), of which 16/s reach the screen (320 blits, all shown) | same log, `[flip]` and `[RSX vulkan] window` |
| main thread split (per guest flip, ~4.4 ref wakes per flip) | **RSX ref wait ~37 ms**, run ~13, **SPU ~0.7**, other ~0.1 | same log, `[spu-crit]` (per-wake values x 4.4) |
| walker's own counters, same window | busy 4.67 s + idle 0.51 s of a **20 s** window (11.7 ms busy per flip) | same log, `[walker-prof]` summed |
| walker on-CPU vs runnable-wait | on-CPU 501 ms/s, run-queue wait 2 ms/s; SPU lanes 121-479 ms/s; main 189 ms/s; 2.9 of 12 CPUs | earlier 2026-11-05 run, `tools/thread_sched.py` |
| walker blocking inside a drain | one-shot GPU waits 15-40 ms per heavy frame, mostly `eng:wcb_copy_surface` 14.9 ms + flush 6.1 (guest CPU reads of render targets) | earlier 2026-11-05 run, [`notes/oneshot-waits.md`](notes/oneshot-waits.md), [`notes/walker-waits.md`](notes/walker-waits.md) |
| `WCB_PREFETCH=1 STREAM=1` | one-shot waits 23 -> 15 ms, flips 27.5 -> 32.8/s (rough, 6 reports), no visible change | same session, live keys, 1 run |
| walker CPU hotspots | NVIDIA driver 25 %, `rsx_vertex_remap_build` 12.6 %, memmove 6.7 %, `eng_pipeline_get` 6.3 % | perf, crowded, earlier 2026-11-05 |
| depth-as-texture fix | walker 149 -> 82 ms per heavy frame | live, 2026-11-05 |

## 4. Open conflicts (must be explained, not ignored)

1. **Guest flips vs screen frames (explained 2026-11-05):** in 22:54:50-22:55:10 the game issued **400 flips** but the
   Vulkan backend made only **320 blits** and showed all 320 (`[RSX vulkan] window` lines: `shown` = `blits`,
   `dropped 0`); MangoHud counted 313. So nothing is lost at present: ~20 % of guest flips never become a blit
   (likely several flips handled inside one walker drain, only the last one presented; not yet confirmed in code).
   **Use MangoHud / `shown` for fps.** `[flip]` counts game flip requests, not frames on screen.
2. **Walker time does not add up:** in the crowd window its counters cover ~5 of 20 s, yet main waits on it ~37 ms
   per flip. Earlier `thread_sched` showed it ~50 % on-CPU. The missing time is either uncounted walker work, GPU
   waits, or waits for the SPU mesh producer / PPU (as at the meter). Different runs and builds; never measured
   together.
3. **SPU share in crowds:** main waits ~0.7 ms on SPU, but at the meter the walker waited 2.7 ms (with draws) to
   16.8 ms (without) on the SPU producer. Whether the walker waits on SPU output in crowds is unmeasured.

## 5. Next measurement (one run, user-driven, crowded)

The only tool that has ever made a frame add up to 100 % is the 2026-11-02 trace (all threads, one timeline). Run it
once in a crowd, nothing else changed:

```
cd ~/Documents/mlb16-online/recomp-work
BUILD=build-sync PS3RECOMP_SPU_CRIT=1 PS3RECOMP_C690_STATS=1 \
PS3RECOMP_TRACE=$PWD/captures/crowd_trace.json PS3RECOMP_TRACE_TRIGGER=$PWD/captures/crowd_go \
PS3RECOMP_TRACE_SECS=14 PS3RECOMP_TRACE_RING_MB=16 PS3RECOMP_TRACE_FLUSH_MB=8 ./play_windowed.sh
# in a second terminal, once you are in the crowd:  touch ~/Documents/mlb16-online/recomp-work/captures/crowd_go
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
| "Crowd runs at ~20 fps / 50 ms" | orchestrator, from guest `[flip]` | the screen shows 12-18 fps, 65-91 ms typical (MangoHud) |
| "SPU codegen is 99 % of the work" | direction set 2026-11-05 evening on the first claim above | on hold until section 5 is measured |
| "Lifted SPU is slow because of register spills" | spu-regs / audits | float-semantics emulation is ~2/3 of hot SPU time; spills ~19 % ([`notes/spu-codegen-v2.md`](notes/spu-codegen-v2.md)) |

## 7. Built and parked (switch, default, measured effect)

| switch | default | effect |
|---|---|---|
| `PS3RECOMP_SPU_FTZ` | 0 | c690 job 1.11x, lane time -7 %, fps flat at meter; bit-exact |
| `PS3RECOMP_RSX_WCB_PREFETCH` + `STREAM` | 0 | crowd one-shot waits 23 -> 15 ms (rough) |
| `PS3RECOMP_VK_PASS_KEEP` / `VK_VTX_PRE` / `VK_DRAW_STATE` | 0 | offline walker 43-46 -> 38 ms, byte-identical frame |
| `PS3RECOMP_REMAP_DIRECT` + `REMAP_FAST` | 0 | offline walker -4 ms |
| `PS3RECOMP_BINK_CLOSE_KEEP` | 1 | fixes the green box (shipped) |
| `PS3RECOMP_C690_STATS` | 1 in play_windowed | ~5 % of SPU-thread CPU; set 0 for A/B |

Dead ends (measured, do not retry): SPU register tweaks, inlining callees, cheaper `frsqest`, exact lazy sign-of-zero,
WWS native ticket claim alone, RSX_SYNC_FIX, frames-in-flight (design shelved).
