# Walker phase profile, one-frame capture and real-engine replay

Date: 2026-11-02. Branch `feat/walker-prof` in `ps3recomp-probe` (off `mlb16-work`,
not merged). One pitch window measured with the profiler and the capture both
enabled; the capture itself is one frame and stays in `recomp-work/`.

## What was added

`PS3RECOMP_WALKER_PROF=1` times the real `sink_end` path and reports per frame:

| phase | what it covers |
|---|---|
| `walker` | the whole `gcm_rsx_process_fifo_unlocked` pass (summed over the frame) |
| `parse` | `walker - engine`: decode, control words, `put`/`get` bookkeeping |
| `fetch` | `dc_fetch` (vertex fetch + remap build), plus the deferred CPU conversion |
| `tex` | `sink_bind_textures` + `sink_bind_vertex_textures` (texture cache check, content hash, decode/upload, sampler) |
| `pipe` | `eng_pipeline_get` (guest program analysis, key, cache) + `eng_debug_fp` |
| `bind` | `g.be->bind_targets/pipeline/vs/ps/textures/vertex_textures`, viewport, scissor, stencil, bounds, bias |
| `draw` | `g.be->draw` / `g.be->draw_fill` (on Vulkan: `vk_eng_draw_from`) |
| `other` | `engine - (fetch+tex+pipe+bind+draw)`: group setup, constants, index build, bookkeeping |

It is off by default and costs one cached compare per drain when off. A report is
`[walker-prof] report=N flips=F draws=... walker=...ms ...`; a report can span more
than one flip, so every value is divided by the flips in it. Everything is per
frame, which is what the pitch window averages.

`PS3RECOMP_WALKER_CAPTURE=<prefix>` images one frame at a flip boundary. Create
`<prefix>.trigger`; the next flip arms the capture and the next drain starts it:

- `<prefix>.fifo` — the frame's method stream in execution order (RSXF header +
  big-endian words), with a `0xFEADxxxx` flip word at the end;
- `<prefix>.state` — `rsx_wcap_state`: regs, transform program, constants, the
  BEGIN_END accumulator and the inline stream, plus the frame size and IO window;
- `<prefix>.mem` — every guest page the frame's draws read, keyed by
  `(location, offset)` and expanded to 4 KB pages (one entry per page);
- `<prefix>.missing` — pages that could not be imaged (none in this run).

The capture images RSX local memory too. The title demand-commits local pages on
first access, so every page the frame read is already backed; the capture commits
any remaining page of a recorded span before reading it. The files are game data:
they live in `recomp-work/walker_frame.*` and are never committed.

`replay_rsx_engine <prefix>` replays that capture through the real
`rsx_draw_engine.c` with a null backend (`tools/replay_null_draw_backend.c`).
The walker, dispatcher, vertex fetch/remap, texture decode/hash, pipeline key and
bind traffic are the shipping code; only Vulkan command recording is absent. The
existing `replay_rsx` and its dispatcher-state hash are untouched:
`./build/replay_rsx kauffman_pitch.bin --frames 5 --verify` still reports
**0x85fc2ff5a1655c26, VERIFIED**.

## Pitch numbers

Run: `recomp-work` `measure6` flow, `PS3RECOMP_WALKER_PROF=1
PS3RECOMP_WALKER_CAPTURE=...`, pitch window 06:48:12–06:48:27 (**537 reports,
15.9 s**). `pitch-wait fps: 37.07` (1 s buckets mean 37.01, stddev 1.94). The
capture frame is 120,618 words, 27,449 pages (107.2 MB), 668 draws, 0 read misses.

Median per frame over the pitch window (500 draws/frame):

| phase | ms/frame | us/draw | share |
|---|---:|---:|---:|
| walker | 17.188 | 34.4 | 100 % |
| engine | 15.912 | 31.8 | 92.6 % |
| parse | 1.212 | 2.4 | 7.1 % |
| fetch | 3.558 | 7.1 | 20.7 % |
| tex | 3.233 | 6.5 | 18.8 % |
| pipe | 1.856 | 3.7 | 10.8 % |
| bind | 0.190 | 0.4 | 1.1 % |
| draw | 4.777 | 9.6 | 27.8 % |
| other | 1.816 | 3.6 | 10.6 % |

The walker drain is 17.2 ms; the earlier critical-path run measured 17.25 ms, so
this build is consistent.

### Profiler overhead

Measured offline on the same engine and capture, 3× 200 frames each:

| run | median frame |
|---|---:|
| profiler off (`PS3RECOMP_WALKER_PROF=0`) | 14.06 / 14.09 / 13.09 ms |
| profiler on | 14.91 / 14.02 / 13.95 ms |

That is inside run-to-run noise (≤0.5 ms on a 14 ms drain, <1 fps). On the pitch
run the fps was 37.07 with the profiler on; the nearest no-profiler baseline in
`progress.md` is 34.58 fps on an older build, so the in-run overhead is not
distinguishable from build-to-build variation. The capture adds one frame's dump
(~0.5 s) once and is otherwise off.

## Offline replay vs the in-game profile

`replay_rsx_engine recomp-work/walker_frame --frames 200 --warmup 10`:
median **14.0–14.9 ms** for 668 backend draws (691 `sink_end` groups), 0 read
misses. Per group, warm (medians after warmup) against the pitch window:

| phase | in-game us/draw (500 groups) | replay us/draw (691 groups) | note |
|---|---:|---:|---|
| parse | 2.42 | 1.33 | less contention on one thread |
| fetch | 7.12 | 6.06 | representative |
| tex | 6.47 | **0.20** | static frame: textures never change between loops |
| pipe | 3.71 | 2.99 | representative |
| bind | 0.38 | 0.05 | null backend does no host work |
| draw | 9.55 | 8.57 | null `draw_fill` still does the CPU vertex conversion |
| other | 3.63 | 2.40 | less bookkeeping |
| **engine** | **31.82** | **20.22** | |

The first replay frame (cold texture cache) is the other extreme: tex 18.28 ms
(26.4 us/draw) and pipe 9.85 ms, so the static frame brackets the in-game texture
cost rather than reproducing it. The capture holds the bytes each draw read, but
it is one frame: crowd/UI/animated texture updates between frames are absent, and
the learned texture memo makes repeated binds nearly free. Offline replay is
therefore a good A/B for fetch/remap and pipeline work and **not** a valid A/B for
the texture-check cut.

The replay also excludes Vulkan recording and the walker producer wait, which is
why its 21.5 us/draw is below the in-game 34.4 us/draw even before the texture
difference.

## Cuts (not implemented)

From the pitch profile, the three largest walker-CPU blocks after Vulkan
recording are:

1. **Vertex fetch/remap (`fetch`): 3.56 ms/frame, 7.1 us/draw.** Rebuild the
   fetch plan and remap only when the attribute layout or the reference set
   changes; repeated groups currently redo both. Expected **1.4–2.1 ms/frame
   (40–60 %)**.
2. **Texture check/hash (`tex`): 3.23 ms/frame, 6.5 us/draw.** Replace the
   per-frame sparse+full FNV revalidation with a page-dirty generation check
   (and skip decode when the generation is unchanged). Expected **1.9–2.6 ms/frame
   (60–80 %)**, but see the replay caveat: this cut cannot be A/B'd offline from
   a single static frame.
3. **Pipeline lookup (`pipe`): 1.86 ms/frame, 3.7 us/draw.** Memoize the key and
   the guest-program input analysis while the program registers and the relevant
   render state are unchanged. Expected **0.6–0.9 ms/frame (30–50 %)**.

Together: **3.9–5.6 ms/frame** off a 17.2 ms walker drain, i.e. roughly 15–20 % of
the ~27 ms pitch frame, or ~37 → ~42–44 fps if the producer wait does not absorb
it. The single largest phase is still `draw` (Vulkan recording) at 4.78 ms; that
is the batching/frame-pipelining path, not one of these walker cuts.

## Reproduce

```sh
# ps3recomp-probe, feat/walker-prof
cmake -S . -B build -G Ninja -DPS3RECOMP_BUILD_TESTS=ON && ninja -C build replay_rsx replay_rsx_engine
./build/replay_rsx kauffman_pitch.bin --frames 5 --verify          # 0x85fc2ff5a1655c26
./build/replay_rsx_engine recomp-work/walker_frame --frames 200 --warmup 10

# game: recomp-work/ab-tools/measure6 flow with
#   PS3RECOMP_WALKER_PROF=1 PS3RECOMP_WALKER_CAPTURE=$PWD/walker_frame
# then `touch walker_frame.trigger` at the pitch.
```
