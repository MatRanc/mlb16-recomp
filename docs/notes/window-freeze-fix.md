# Window freezes under native Wayland: the present thread never frees a slot

Date: 2026-11-02. Branch `play/multiscene` (ps3recomp worktree
`ps3recomp-play`), built as `recomp-work/build-play`. Player log
`recomp-work/play_20261102_115243.log`, diagnosis
`/home/m/Downloads/window-freeze-diagnosis.md`.

## Symptom and what the counters say

The window stood still for 15-50 s at a time while the game kept running
(rumble, `[flip]` counter), and moving the mouse made it live again. The
`PS3RECOMP_VK_PRESENT_STATS=1` lines show that the SDL event pump was healthy
during every freeze (`max pump gap` 4-15 ms with `PS3RECOMP_PUMP_MS=8`) while
`blits 0, shown 0, dropped ~65 no-slot` every second:

```
11:53:25  blits 77, shown 75, dropped 0 no-slot + 0 not-ready
11:53:26  blits 0,  shown 0,  dropped 71 no-slot + 0 not-ready
...
11:53:41  blits 0,  shown 0,  dropped 76 no-slot + 0 not-ready
11:53:42  blits 6,  shown 8,  dropped 61 no-slot + 1 not-ready, longest vkQueuePresentKHR 16945 ms
```

The session as a whole: **219 frozen seconds out of 457** (a frozen second is
one where `shown == 0` while the guest flip counter advanced), **15,059 no-slot
drops**, only **87 not-ready** drops, longest `vkQueuePresentKHR` **49.2 s**.

## Why there is "no free image" (from the code)

"no-slot" is not an acquire failure. `vk_present_window` first reserves one of
`n-1 = 2` present slots for a 3-image swapchain
(`rsx_vulkan_backend.c:1982`, `:2210`); if neither is free it increments
`noslot` and returns **before** `vkAcquireNextImageKHR` isever called
(`rsx_vulkan_backend.c:2211`).

A slot stays busy from that reserve until the present thread finishes with it.
The present thread (default `PS3RECOMP_VK_PRESENT_THREAD=1`) waits for the
window blit fence and calls `vkQueuePresentKHR`, then clears the slot
(`rsx_vulkan_backend.c:2088-2114`). Therefore:

- one present that never returns from `vkQueuePresentKHR` holds one slot;
- the next frame takes the second slot and queues behind it;
- the third frame finds both slots busy and is dropped no-slot. Every later
  flip is dropped the same way until the stuck present returns.
- `q_max` only records a present when it *returns*. A permanent stall therefore
  shows `longest vkQueuePresentKHR 0 ms` while `no-slot` climbs; when the mouse
  frees it, the next line shows the real 8-49 s. Both shapes are in the logs.

Why the present blocks is driver/compositor, not this repo: on this machine
the window is SDL video driver `wayland` with a **MAILBOX** swapchain, NVIDIA
615.71.09 and GNOME/mutter. A MAILBOX present keeps each committed buffer until
the presentation engine releases it; if the compositor does not produce a frame
for that surface, `vkQueuePresentKHR` does not return. Mouse motion makes mutter
produce a frame, which matches "moving the mouse unfreezes it". This part is
inferred from the behaviour; the slot accounting above is proven by the code
and counters.

## What each knob changes

| Knob | Effect | Why it can/cannot help here |
|---|---|---|
| `PS3RECOMP_VK_PRESENT_MODE=fifo\|mailbox` | Selects the swapchain mode at creation; MAILBOX is preferred when the surface offers it (`rsx_vulkan_backend.c:1916-1933`). | FIFO paces presents to the display and does not use the mailbox buffer-replacement path. It caps the window at refresh (60 Hz) and can add up to one frame of latency. This is the knob that removed the failure. |
| `PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS=n` | Makes `vkAcquireNextImageKHR` wait up to *n* ms instead of trying once (`:2051-2058`, `:2214`). | Only changes the **not-ready** class: it lets the WSI dispatch its events and wait for an image. It cannot help a **no-slot** freeze, because the acquire is never reached (`:2211`). It also blocks the frame-clock thread for up to *n* ms while holding `present_mu`, so keep it small (2-8 ms). |
| `SDL_VIDEODRIVER=x11` (Xwayland) | Uses the X11/Xwayland WSI instead of the native Wayland one. On this machine Xwayland offered **only FIFO**, so it changed the WSI and the mode together. | Avoids the native-Wayland WSI path. Costs native fractional scaling and an extra Xwayland hop. Freeze-free in the one run, but not separable from FIFO because the mode also became FIFO. |
| Swapchain image count | The code asks for `max(3, minImageCount)` (`:1912`) and exposes `n-1` slots (`:1982`). There is no env knob. | More images give more slots and delay exhaustion by `n-1` frames, but they cannot unblock a present that is already stuck; a 75-82 s stall would consume any count. The 3-image/2-slot choice is why two stuck presents are enough to stop the window. |

`__GL_SYNC_TO_VBLANK=0` (exported by `run.sh` and `play_windowed.sh`) is an
NVIDIA OpenGL variable; the Vulkan driver does not read it and it has no effect
on this path.

## A/B runs

Runs are `build-play/MyGameRecomp`, windowed, MangoHud CSV logging,
`PS3RECOMP_VK_PRESENT_STATS=1`, no mouse/keyboard input. "frozen s" counts
stats seconds with `shown=0` while `[flip]` advanced (whole run). The planned
`measure_fast.sh` harness (scripted pad, `VK_DUMMY_BLACK=1`, nav to pitch) was
tried first and did **not** reproduce the stall (two runs, 0 frozen seconds),
so the freeze A/B was run with `play_windowed.sh` sitting at the boot/menu
attract, which is where the player's fits started. Real textures were used in
all freeze runs; the pitch-fps run used the same and reached the pitch.

| run | driver / mode | wall s | frozen s | no-slot | not-ready | q_max | presents/s | pitch flips/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| player session 11:52 | wayland / MAILBOX | 457 | **219** | 15059 | 87 | 49231 ms | 28.6 | - |
| harness base 12:07 (`VK_DUMMY_BLACK=1`) | wayland / MAILBOX | 146 | 0 | 9 | 35 | 5 ms | 57.5 | - |
| harness realtex 12:11 | wayland / MAILBOX | 229 | 0 | 29 | 110 | 6 ms | 57.2 | **35.44** |
| `play_windowed` 12:18 | wayland / MAILBOX | 171 | **138** | 8220 | 1 | 6 ms | 9.1 | - |
| `play_windowed` + pad 12:22 | wayland / MAILBOX | 104 | **82** | 5161 | 3 | 11 ms | 12.4 | - |
| `play_windowed` no thumbnail 12:25 | wayland / MAILBOX | 104 | 0 | 1 | 0 | 5 ms | 64.2 | - |
| `play_windowed` 12:29 | wayland / MAILBOX | 139 | 0 | 2 | 1 | 5 ms | 60.2 | - |
| **(c) FIFO 12:33** | wayland / **FIFO** | 147 | **0** | 1 | 835 | 13 ms | 55.9 | - |
| **(b) Xwayland 12:35** | **x11** / FIFO | 149 | **0** | 4 | 14 | 12 ms | 57.2 | - |
| **(c) FIFO repeat 12:38** | wayland / **FIFO** | 149 | **1** | 2 | 214 | 5 ms | 57.5 | - |

Reading the table:

- The default MAILBOX configuration is **intermittent**: it froze hard in two
  of four play-style runs (and in the player's session) and not at all in the
  other two, which is why one clean run proves nothing by itself. In the two
  failures the window never recovered on its own, because nothing presented a
  new compositor frame.
- FIFO never showed the failure mode: at most **1 frozen second** and **1-2
  no-slot drops** across two runs, against 5k-15k drops when MAILBOX failed.
  Its `not-ready` drops (214-835) are the normal display-paced acquire, not
  drops of the kind that froze the window.
- Xwayland negotiated FIFO too and was clean. It is kept documented as a
  fallback, not made the default, because it changes the video driver.
- The acquire-timeout arm was not run: the failure is `no-slot`, and `no-slot`
  returns before the acquire, so `ACQUIRE_TIMEOUT_MS` cannot affect it. Making
  the present wait instead of dropping would only move the stall to the
  walker.
- Pitch fps was measured once, on the MAILBOX harness: **35.44 flips/s**.
  The play-window runs sit at the menu and cannot report pitch fps. Present
  mode does not change the guest flip rate; it only changes how fast the
  window is updated (FIFO is at ~55 presents/s, above the guest's ~35 fps), so
  the window is not the reason game fps differs.

MangoHud CSV (`tools/frametime_plot.py`, after the avg-fps fix): the player
session reads **28.37** avg presents/s, 1 % low 0.50, 0.1 % low 0.06 over
461.4 s. The failing `play_windowed` CSVs cover only 28.2 s and 17.2 s of a
171 s / 104 s run: MangoHud logs one row per window present, so a present
stall stops the log. The PRESENT_STATS lines, not the CSV, measure a freeze.

## Fix

`tools/play_windowed.sh` now sets, before the game:

```sh
export VK_DUMMY_BLACK=1
export PS3RECOMP_VK_PRESENT_MODE="${PS3RECOMP_VK_PRESENT_MODE:-fifo}"
```

- `VK_DUMMY_BLACK=1`: disabled texture units read black instead of magenta, so
  the picture is not pink; `measure_fast.sh` already did this.
- `PRESENT_MODE=fifo`: default only. `PS3RECOMP_VK_PRESENT_MODE=mailbox` in the
  environment restores the old mode. Xwayland remains available with
  `SDL_VIDEODRIVER=x11` if FIFO ever regresses.

No ps3recomp code change was needed: both knobs already exist on
`play/multiscene`. Nothing was pushed to `fork`.

## Reproducing / checking

```sh
# The freeze, if it happens, shows as blits=0 / shown=0 / no-slot climbing
# in the window: lines. No mouse input during the run.
PS3RECOMP_VK_PRESENT_STATS=1 recomp-work/play_windowed.sh
grep 'window:' recomp-work/play_<ts>.log

# After the run: freezes and pitch fps
python3 tools/frametime_plot.py recomp-work/play_<ts>/MyGameRecomp_*.csv
```

Caveat: the failure is intermittent. A clean short run does not prove a fix;
look for the no-slot storms (thousands of drops) and multi-second `shown=0`
runs, and repeat the run.
