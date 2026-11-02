# Multi-scene windowed play: does c690/SPU load matter outside the pitch?

Date: 2026-11-02. Branch `play/multiscene` (ps3recomp-probe), built in
`recomp-work/build-play`. This session is for the **user** to play scenes in the
windowed view; nothing here runs the game automatically.

The question: the pitch screen is walker/PPU-bound and the earlier trace showed
c690 is not the bottleneck there (`docs/walker-spu-wait.md`). Is c690/SPU load
heavier in ball-in-play, replays/cutscenes or crowd shots, i.e. is the shelved
Design B worth doing for those scenes?

## Build

`play/multiscene` is `mlb16-work` plus `feat/walker-cuts`, `feat/walker-wake`,
`fix/window-pump` and `feat/wcb-async`. The checkout is a worktree at
`ps3recomp-play`, so the normal `ps3recomp-probe` checkout stays on
`mlb16-work`.

```sh
cd /home/m/Documents/mlb16-online
cmake -B recomp-work/build-play -S ps3recomp-play/templates/project -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/lib64/ccache/clang \
  -DCMAKE_CXX_COMPILER=/usr/lib64/ccache/clang++ \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -march=native" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -march=native" \
  -DPS3RECOMP_DIR=$PWD/ps3recomp-play \
  -DRECOMP_DIR=$PWD/recomp-work/recompiled \
  -DSPU_DIR=$PWD/recomp-work/spu_lifted \
  -DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON \
  -DPS3RECOMP_SPU_WRCH_INLINE=ON -DPS3RECOMP_FFMPEG_RUNTIME=OFF \
  -DPS3RECOMP_RSX_VULKAN=ON -DPS3RECOMP_BUILD_TESTS=ON
ninja -C recomp-work/build-play MyGameRecomp
```

`build-o2` is untouched. The unit tests the branches added are built with
`-DPS3RECOMP_BUILD_TESTS=ON` and run from `build-play/ps3recomp_sdk`
(`ctest -R color_readback_guest`; `tools/test_gcm_walker_wake.py`; the walker
fast checks live in the same `test_color_readback_guest` binary).

## Run

```sh
cd /home/m/Documents/mlb16-online
recomp-work/play_windowed.sh
```

That is the command to type. It runs in the foreground; close the window (or
Ctrl-C) to end it. The script uses `build-play/MyGameRecomp` and the normal
`run.sh` environment (real textures, real controller, SPURS settings,
`VK_DRAW_BATCH=1`, `VK_FRAME_PIPELINE=1`) plus:

| variable | why |
|---|---|
| `PS3RECOMP_VK_WINDOW=1` | windowed |
| `GCM_FLIPCOUNT=1` | `[flip]` lines; all fps comes from these |
| `PS3RECOMP_C690_STATS=1` | c690 jobs/s, per-worker job counts, job wall |
| `PS3RECOMP_WALKER_PROF=1` | walker own CPU and per-phase time |
| `PS3RECOMP_VK_PRESENT_STATS=1` | pump gap, shown/dropped, present stall |
| `PS3RECOMP_PUMP_MS=8` | pump inside the catch-up loop (freeze knob, already on) |
| `PS3RECOMP_VK_DUMP` + `_EVERY=30` | the thumbnail source (the game's own frame) |

Arguments:

```sh
recomp-work/play_windowed.sh wcb      # + PS3RECOMP_RSX_WCB=3, _STATS=60
recomp-work/play_windowed.sh nohud    # no MangoHud (if it breaks the window)
recomp-work/play_windowed.sh wcb nohud
```

`wcb` is async render-target readback: correct faces/crowd/jerseys at close to
baseline fps, instead of the ~11 fps of `WCB=1`. Default is off so the CPU/SPU
numbers are not mixed up with readback work.

Output, under `recomp-work/`:

* `play_<timestamp>.log` — the game log (timestamped). `[flip]` lines are the
  fps truth; `[walker-prof]`, `[c690-stats]`, `[RSX vulkan] window:` are the
  diagnostics.
* `play_<timestamp>/thumb_<epoch-ms>.png` — a 320×180 frame every ~2 s.
* `play_<timestamp>/MyGameRecomp_*.csv` — MangoHud CSV (fps, frametime,
  cpu/gpu load, per-core load).
* `play_<timestamp>/meta.txt` — start epoch and the arguments.

MangoHud shows fps + frametime graph, CPU/GPU load and per-core load. It is
opt-out (`nohud`) because an overlay or its Vulkan layer can be the thing that
breaks the window or the device on this machine.

## What to do in-game (about 1 minute each)

The contact sheet is keyed by time, so a rough note of when you started each
scene is enough. Suggested order:

1. **Boot / menu** (~1 min). Let it load; this also proves the window and the
   HUD are alive. Note the first `window: SDL video driver '...'` line.
2. **Pitch / at-bat (baseline)** (~1 min). Let the pitcher work; stand still.
   This should reproduce the known pitch numbers (~35–38 fps).
3. **Ball in play / fielding** (~1 min). Put a ball in play and field it. This
   is the main question: more c690 jobs, or the same?
4. **Replay / cutscene** (~1 min). Trigger an instant replay or let an inning
   end. Camera moves and post-processing change here.
5. **Crowd / dugout shot** (~1 min). Between innings, a home run, or the
   pre-game pan. Crowd sprites are a c690/WCB workload, so this is the other
   candidate for Design B.

Tip: to make labelling easy, say the scene name out loud or write the wall
clock time down when you switch. The sheet shows `HH:MM:SS` for every 2 s
window.

## What to look for

Run the analyser while or after playing:

```sh
python3 mlb16-recomp/tools/analyse_play.py recomp-work/play_<timestamp>.log
```

It prints one row per 2 s window and writes
`recomp-work/play_<timestamp>_sheet.html`: fps, c690 jobs/s, mean per-lane busy
%, walker CPU, wait, top walker phases, present shown/dropped/gap, and the
nearest thumbnail. Rows are amber when c690 lanes are ≥85 % busy, wait is ≥40 %
of the frame, or frames were dropped / the pump gap was >100 ms.

The Design B question:

* **c690 lane busy %** is the throughput signal. The pitch run was ~59 % busy
  over six lanes, ~42 % c690, with the sixth lane idle. A scene where the mean
  is **near 100 %** (or every lane is ≥90 %) has no SPU headroom; that is where
  Design B can pay.
* **c690 wall per frame** (printed as the work-to-wait ratio; from
  `[c690-stats] wall_us ... (sum=)` divided by the flips in the window) is the
  total SPU work per frame. If it is comparable to or larger than the frame
  time, c690 is a large part of the frame.
* **walker wait** is `frame time − [walker-prof] walker=` when no
  `PS3RECOMP_TRACE` timeline is recorded. A big wait with saturated c690 lanes
  is the "Design B matters here" case. The exact split of that wait into c690
  vs PPU/cadence needs the trace + `tools/analyse_walker_wait.py`; this session
  is the screening pass that says where that trace is worth recording.
* **present** columns: `gap` is the largest gap between event pumps and
  `drop` counts frames lost for no slot / not-ready. A growing drop count or a
  gap of hundreds of ms while the picture stands still is the freeze from
  `docs/window-freeze-diagnosis.md`.

Then plot the frametime for the whole session:

```sh
python3 mlb16-recomp/tools/frametime_plot.py \
  recomp-work/play_<timestamp>/MyGameRecomp_*.csv
```

It prints average fps, 1 % / 0.1 % lows, p50/p99 frametime and the count of
spikes >50 ms, and writes a PNG with 16.7 ms (60 fps) and 33.3 ms (30 fps)
guides.

## Quick feel-test of the two levers

Both are off by default; export them in front of the same command. Play the
same tough scene (ball in play or a replay) with and without:

```sh
PS3RECOMP_WALKER_FAST=1 recomp-work/play_windowed.sh
PS3RECOMP_WALKER_WAKE=1 recomp-work/play_windowed.sh
```

* `WALKER_FAST` caches the vertex fetch/remap plan and the pipeline input memo
  (`docs/walker-cuts.md`). Expect a few ms off the walker in draw-heavy scenes.
* `WALKER_WAKE` wakes the FIFO walker as soon as an SPU PUT fills the parked
  ring slot, instead of waiting for the idle timeout (`docs/walker-wake.md`).
  Expect lower wait, not more throughput.

They compose: `PS3RECOMP_WALKER_FAST=1 PS3RECOMP_WALKER_WAKE=1 ...`.

## If the window freezes

The freeze diagnosis is in `docs/window-freeze-diagnosis.md` in the
ps3recomp checkout. The play script already sets `PS3RECOMP_PUMP_MS=8`. Read
the `[RSX vulkan] window:` lines during the freeze, then try, one at a time:

```sh
PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS=4 recomp-work/play_windowed.sh   # WSI holds images (not-ready climbs)
PS3RECOMP_VK_PRESENT_MODE=mailbox recomp-work/play_windowed.sh   # present thread waits (no-slot climbs)
PS3RECOMP_VK_PRESENT_MODE=fifo recomp-work/play_windowed.sh
SDL_VIDEODRIVER=x11 recomp-work/play_windowed.sh                 # Xwayland instead of native Wayland
SDL_VIDEODRIVER=wayland recomp-work/play_windowed.sh
```

What the line means: a large **max pump gap** is pump starvation (the
`PUMP_MS=8` knob targets it); climbing **no-slot** with a long
`vkQueuePresentKHR` means the present thread is stuck in the WSI; climbing
**not-ready** with `shown=0` means the WSI is holding every image. If blits and
shown track the flip rate while the window is frozen, the frames reach the
compositor and only a different driver/present mode helps. If MangoHud itself
is the suspect, rerun with `nohud`.

## Files

* `recomp-work/play_windowed.sh` (not committed) and
  `mlb16-recomp/tools/play_windowed.sh` (same script).
* `mlb16-recomp/tools/analyse_play.py` — per-window analysis + contact sheet.
* `mlb16-recomp/tools/frametime_plot.py` — MangoHud CSV frametime plot.
* `ps3recomp-play` — the `play/multiscene` worktree; `recomp-work/build-play`
  is its build.
