# Tiled logo wipe: Phase 0 (still present, short) and the missing replay frame

Date: 2026-11-06. Branch `exp/wipe` (off `exp/crowd`, tip `d002ccdf`) in
`ps3recomp-sync`, build `recomp-work/build-sync`. Two automated launches (the
mission cap). No RPCS3 or other GPL emulator source was read or cited.

## TL;DR

* **It still happens on the current build (2026-11-06), but not as a persistent
  overlay.** On the normal pitch-meter route the batter intro is preceded by an
  expanding tile/mosaic rectangle over the stadium flyover. It covers the screen
  for about **2.7 s** (dense frame dumps `f_186`-`f_196`, 00:37:42.1-00:37:44.9)
  and resolves to the clean batter close-up (`f_198`, 00:37:45.0).
* **The raw FIFO of the transition is captured** (`captures/wipe/wipe1.bin`,
  723 MB, 189,410,045 words, 1,679 flip frames), plus 427 dense frame dumps
  (`captures/wipe/wipe1/live/`). The wipe is around raw frame ~640-700.
* **The one-shot walker capture missed the wipe.** It fired on the
  "DEFENSIVE LINEUP" banner at 00:37:25 because the live detector matched an
  orange bar, not the transition. The captured frame is the lineup screen
  (`captures/wipe/wipe1_cap.*`, 330.7 MB `.mem`, 155,749 words), so there is no
  `.state`/`.mem` for the wipe frame and no replayed before/after image.
* **No fix is claimed.** The offline replay of frames after the captured one
  stops at the first inter-frame sync the live guest would have serviced, and a
  state snapshot alone does not carry the engine's pipelines/surfaces, so a
  synthetic wipe frame renders garbage. The existing unproven hypothesis in
  [`rsx-sync.md`](rsx-sync.md) (a tiled surface written between bind and
  sample) still needs the live walker capture of the wipe frame.
* No new key was added, so no critical-path section 7 row. The live command for
  the user is in section 5.

## 1. Phase 0: the current frame sequence

`ab_live.sh` on `BUILD=build-sync`, one pitch-meter route, `SCENE_SHOTS=0`,
`PS3RECOMP_VK_DUMP_EVERY=3`, plus a run-local watcher (`captures/wipe/watch_run2.py`)
that copied every distinct `frame.ppm` to `captures/wipe/wipe1/live/`.

The batter-intro transition, 00:37:42.1-00:37:45.3 (epoch ms in the names):

| dense frame | time | what it shows |
|---|---|---|
| `f_184` | 42.0 s | normal stadium flyover, no overlay |
| `f_186` | 42.12 s | small checkerboard rectangle enters at the top-left |
| `f_188` | 42.46 s | the rectangle is a left band |
| `f_190` | 42.63 s | large rectangle over sky/stands; scene visible through the tile pattern |
| `f_194` | 43.32 s | tiles cover most of the frame, batter/crowd visible through them |
| `f_196` | 44.85 s | thin boundary at the right edge, batter close-up |
| `f_198`/`f_200` | 45.0/45.3 s | clean batter close-up |

This is a short animated wipe, not a persistent overlay. It matches the
2026-11-02 sample (`ab_live_wcb_attr_20261102_191439/scene_1793665113991.png`):
that run was stuck before the pitch and the same tiled transition was caught
mid-animation. Whether the tile content itself is correct cannot be judged from
our own frames; the visual is the outgoing/incoming scene mixed through a tile
pattern, so a still always looks like "tiles over the scene".

## 2. The capture

One run, `TAG=wipe1`, 00:34:45-00:38:23:

* FIFO: `PS3RECOMP_DUMP_FIFO=captures/wipe/wipe1.bin`, `ARM=1`,
  `FRAMES=2500`; the trigger was written to `PS3RECOMP_CTL` at the
  "cinematics skipped" anchor. 1,679 flip frames, 189,410,045 words, 723 MB.
* Dense frames: 427 unique PPMs, 1.2 GB (`captures/wipe/wipe1/live/`).
* Walker capture: `PS3RECOMP_WALKER_CAPTURE=captures/wipe/wipe1_cap`.
  The watcher fired the one-shot trigger on the first frame with a strong
  orange bar in the bottom centre. That was the **defensive-lineup banner**
  (raw frame 183, 00:37:25), not the wipe. The captured frame is byte-identical
  to raw frame 183 (`wipe1_cap.fifo` body = `wipe1.bin` words
  21,974,161-22,129,908), which confirms the capture path but not the wipe.

Lesson for the next capture: the orange-bar detector is not specific enough.
The wipe follows the lineup screen and the "THE SHOW 16" logo/stadium flyover
by roughly 17 s; trigger on the *transition* (e.g. the first frame whose
top-left corner gains a high-frequency tile block) or, better, fire the walker
capture when the FIFO itself shows the tile draws.

## 3. Offline replay attempts (and why there is no proof image yet)

`replay_rsx_engine` expects a `PS3RECOMP_WALKER_CAPTURE` prefix
(`.fifo`/`.state`/`.mem`), not the raw FIFO dump.

1. **One-frame synthetic splice.** The raw dump is a linear method stream; the
   lineup capture frame was located exactly (frame 183), so a `.fifo` was
   sliced from that frame through any later flip marker with the captured
   `.state`/`.mem`. A single frame replays (412 draws, 0 misses for the lineup
   frame), but a multi-frame slice parks at the first inter-frame sync
   (`s_fifo_getoff` never reaches `stream_end`); the image stays at an early
   frame. Raising the pass cap and processing one flip-delimited segment at a
   time lets the loop advance (draws/frame ~3,714) but the presented target is
   still the old lineup background: the visible scene is composited through
   guest-memory 2D/blit paths the replay tool does not present.
2. **State snapshot.** `rsx_wcap_state` carries registers, VP/FP constants and
   inline data, not the engine's surfaces/pipelines/texture cache. Replaying the
   next frame with a saved state produced magenta/cyan garbage, so this is not a
   substitute for a live walker capture.
3. **Guest display buffer.** The live present blits a guest-memory display
   buffer. Dumping it in the replay gives noise; the composite that fills it
   runs in the walker/2D path, not in the engine's surface list.

The diagnostic switches added to `tools/replay_rsx_engine_core.c` on
`exp/wipe` (`PS3RECOMP_REPLAY_SPLIT`, `_STOP_FRAME`, `_SAVE_STATE[_DIR]`,
`_GUEST_DUMP`, `_MAX_PASSES`) are what these experiments used; they change no
shipping path.

## 4. Root-cause candidate (not proven)

[`rsx-sync.md`](rsx-sync.md) already lists "the tiled wipe/logo transition
could be this class (a tiled surface written between the bind and the sample)"
as an unproven candidate. The dense frames are consistent with that shape: the
tile pattern changes every frame, so a bind that samples a surface the guest is
still writing (or one frame behind) would show exactly this kind of mixed,
stale tile content. The existing `PS3RECOMP_RSX_SYNC_FIX` implements the
ordering fix for that class (default 0); it is a frame-time dead end, but that
does not rule it out for this visual.

The FIFO state needed to confirm it (the wipe draw's bound texture offset,
format/swizzle/remap and wrap modes) is in `wipe1.bin`, but without the wipe
frame's `.state`/`.mem` the draw cannot be replayed and the hypothesis cannot
be closed. This is the missing capture, not a new theory.

## 5. Live command for the user

One launch, windowed. Drive to the pitch screen; when the batter intro starts
(the tile wipe), run the trigger line in a second terminal. `PS3RECOMP_CTL`
must be the explicit path `/tmp/wipe.ctl`.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
rm -f /tmp/wipe.ctl /tmp/wipe_cap.trigger
BUILD=build-sync \
PS3RECOMP_CTL=/tmp/wipe.ctl \
PS3RECOMP_WALKER_CAPTURE=/tmp/wipe_cap \
PS3RECOMP_DUMP_FIFO=/tmp/wipe.bin PS3RECOMP_DUMP_FIFO_ARM=1 PS3RECOMP_DUMP_FIFO_FRAMES=300 \
PS3RECOMP_RSX_SYNC_FIX=0 \
./play_windowed.sh
```

At the wipe (in another terminal):

```sh
echo 'PS3RECOMP_DUMP_FIFO_TRIGGER=1' > /tmp/wipe.ctl; touch /tmp/wipe_cap.trigger
```

Then repeat once with `PS3RECOMP_RSX_SYNC_FIX=1` and compare the wipe visually.
If the sync key is the cause, the tile overlay should change or disappear; if
it does not, the next mission has a replayable wipe frame to work from.

## 6. Clean room

Only this repository's runtime, capture format, replay tool and the game's own
FIFO were used. No RPCS3 source, diff or fork was read or cited.
