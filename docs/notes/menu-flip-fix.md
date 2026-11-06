# Menu flip fix: complete the display tick from our own request state, age-bounded

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Implements the proposal in
[`menu-34ms.md`](menu-34ms.md) behind the live key
`PS3RECOMP_FLIP_TICK_OWN_STATE` (**default off in code**). Revision 2 adds the
FIFO fall-through, a selectable age bound, the analysis of the user's two menu
recordings, and one three-arm pitch-meter A/B. The user's own menu test
confirmed the fix (`=1`: ~60 fps, from ~42).

Follows [`menu-34ms.md`](menu-34ms.md), [`meter-p50-pin.md`](meter-p50-pin.md),
[`walker-cuts-2.md`](walker-cuts-2.md), [`frame-pacing.md`](frame-pacing.md)
and [`targets-and-backlog.md`](../targets-and-backlog.md).

## TL;DR

* **Implemented, default off.** With `PS3RECOMP_FLIP_TICK_OWN_STATE=1`,
  `cellGcmTickFlip` completes an outstanding **direct** flip using our own
  `s_flip_direct_pending` flag and the request timestamp, not the guest's
  `s_flip_status` word. It completes only while the request is younger than
  the guest's first 17 ms wait (`GCM_FLIP_TICK_AGE_FIRST_US = 17000`). With the
  key off, the old status gate runs unchanged.
* **Revision 2 fix: a FIFO-reached flip still completes.** The own-state test
  only judges a direct request. When it does not take a direct flip, the gate
  now falls through to the FIFO-reached half (`gcm_flip_fifo_take`) under the
  old status gate. A flip queued in the command stream has no direct-pending
  bit and was otherwise never completed by the tick while the key was on. The
  fall-through never consumes the direct flag, so it cannot bypass the age
  bound.
* **The bound is selectable:** key value **1 = 17,000 us** (the guest's first
  `Cond_Wait`), value **2 = 33,366 us** (`DAT_011203f8 << 1`, the whole
  two-wait deadline), any other non-zero value behaves as 2. The user's menu
  test used 1.
* **Unit-tested.** `test_gcm_tick_in_drain` covers the FIFO fall-through, both
  bounds, no double completion, and no completion past the guest's abandon
  point. All 30 video fixtures pass.
* **User's menu recordings (the number this mission is for).** The 34 ms class
  falls from **20.8/s to 2.55/s** (-88 %); frame rate goes **42.10 -> 59.44
  fps**, ticks/flip **1.485 -> 1.052**. It is not literally zero: 257 frames
  (4.3 % of flips) still take two timeouts over the 101 s recording, when the
  tick lands after the 17 ms bound. The new distribution is 17-20 ms
  **3.44/s**, 20-32 ms **2.65/s**, 32-36 ms **2.55/s** (the rest are fast
  frames under the 17 ms recorder threshold).
* **Meter A/B (one launch, 3 arms, N=4 x 10 s, 2 s dropped).** 17 ms is
  neutral: fps 34.97 -> 36.22, p50 **34.15 -> 34.15**, p90 37.95 -> 37.68,
  p99 40.82 -> 40.66; two-timeout frames 0.324 -> **0.336 per frame** (paired
  differences mixed sign, +0.012 mean, inside the sampler's noise).
  **33,366 us is rejected:** p50 **36.05**, fps **29.09**, walker 10.43 ->
  14.69 ms. It removes deadline frames (0.121/frame) by completing in the
  second wait, but that is the `KEEP_STATUS` failure: the guest advances and
  waits on the next buffer's ref instead of taking its deadline branch.
* **Recommendation: keep value 1 (17 ms) as the menu-fix setting.** Default
  stays off in code; the play scripts/defaults are Claude's to set after
  review.

## 1. Change

### 1.1 Live key

`PS3_LIVE_FLIP_TICK_OWN_STATE` / `PS3RECOMP_FLIP_TICK_OWN_STATE`, at the end of
the live-ctl enum and name table (`ps3_live_ctl.h:92`, `ps3_live_ctl.c`).
Default 0 everywhere; `play_windowed.sh` does not set it. The value is read
live on every tick completion, so the control file can switch 0/1/2 mid-run.

### 1.2 Tick gate

`gcm_flip_complete_locked(GCM_FLIP_GATE_TICK)` (`cellGcmSys.c:1085`):

```c
const int own = gcm_live_get(PS3_LIVE_FLIP_TICK_OWN_STATE, ...);
if (own) {
    const unsigned long long age = gcm_us_now() - s_flip_req_us;
    const unsigned long long bound = own >= 2
        ? GCM_FLIP_TICK_AGE_DEADLINE_US : GCM_FLIP_TICK_AGE_FIRST_US;
    if (s_flip_direct_pending && s_flip_req_us &&
        age < bound && gcm_flip_direct_take())
        seq = s_flip_req_seq;
    else if (s_flip_status == CELL_GCM_FLIP_STATUS_WAITING &&
             gcm_flip_fifo_take())
        seq = s_flip_req_seq;
    break;
}
if (s_flip_status == CELL_GCM_FLIP_STATUS_WAITING &&
    gcm_flip_executed_take())          /* unchanged */
    seq = s_flip_req_seq;
```

* `s_flip_req_us` is stamped in `gcm_flip_request` under `FLIP_LOCK`
  (`cellGcmSys.c:4218`), next to the request sequence.
* `gcm_flip_direct_take()` (`cellGcmSys.c:203`) is the old direct-pending
  half of `gcm_flip_executed_take`, so the completion consumes the request
  (no double completion) and the FIFO-marker bit cannot be consumed by this
  branch.
* `gcm_flip_fifo_take()` (`cellGcmSys.c:218`) is the FIFO-marker half only.
  It is the revision-2 fall-through: `s_fifo_flips_reached !=
  s_fifo_flips_completed` is the in-FIFO flip the drain has executed, and it
  is completed under the same `WAITING` status test as the old gate. It never
  touches `s_flip_direct_pending`.
* `GCM_FLIP_TICK_AGE_FIRST_US` / `..._DEADLINE_US` (`cellGcmSys.c:238`) are
  the guest's own two numbers, not new tuning constants (section 2).
* Guest-visible behaviour is unchanged: the completion still only marks a
  pending flip (`GCM_PENDING_SET(2)`); `ppu_gcm_pump` still writes DONE only
  when the completed sequence is still the newest request, and the handler is
  still delivered in the same order on the GCM interrupt thread.
* The fixture-only environment fallback in `gcm_live_get` now parses a
  numeric value like `ps3_live_ctl.c`, so a fixture can test `=2`.

## 2. Why the 17 ms bound, and why not 33,366 us

`Gfx_DisplayFlip` waits for its handler and gives up after
`DAT_011203f8 << 1 = 2 x 16683 = 33,366 us`, but it does so in **two 17,000 us
`Cond_Wait`s**. A signal is still accepted during the second wait (up to the
deadline). The 17 ms bound matches the end of the *first* wait; the 33,366 us
bound matches the whole deadline.

The 33,366 us bound looks attractive: it would shorten the menu frames whose
tick lands at 17-33 ms (our on-recording still has 2.55 34 ms frames/s). At the
meter it is a clear loss:

| arm | what it completes | p50 | fps | walker |
|---|---|---:|---:|---:|
| 17 ms | tick inside the first wait | 34.15 | 36.22 | 8.91 |
| 33,366 us | tick inside the first **or second** wait | **36.05** | **29.09** | **14.69** |

The reason is the same one that made
`PS3RECOMP_PREPARE_FLIP_KEEP_STATUS` lose 4.3 fps. When the guest is signalled
during the second wait, `Gfx_FlipHandler` advances the consumer and hands over
the next buffer; the display thread then asks for the next buffer and blocks
on **its** ref, which the producer publishes only at ~34 ms. Without the
signal the guest takes its deadline branch at 34.1 ms and starts the next frame
immediately, which is *faster* than the hand-off. So a completion after the
first wait is on the wrong side of the title's pacing choice, even before the
33.4 ms abandon point.

`KEEP_STATUS=1` had **no** age bound, so its tick completed the flip whenever
the drain eventually ran: the beat lands p50 ~7.5 ms after the 33.4 ms
deadline (`flip-deadline.md`), i.e. the guest had already abandoned the frame.
That late handler advanced the consumer the guest was no longer waiting on and
desynced the hand-off: p50 34.15 -> 37.13, walker 17.75 -> 21.84, -4.3 fps.
The 33,366 us arm above isolates the earlier half of the same failure (a
completion inside the second wait): it removes deadline frames but still
costs 5.9 fps. The 17 ms bound suppresses both.

At the meter the 17 ms arm is effectively a no-op: the tick is usually starved
past 17 ms, so its completion is refused exactly as the clobbered-status gate
refused it. The +0.012 two-timeout frames/frame in this run is mixed-sign
across pairs and smaller than the first run's +0.047; see section 5.

## 3. Unit test

`libs/video/tests/test_gcm_tick_in_drain.c` (the existing GCM fixture run by
`tools/test_video_fixtures.py`), key off for all older cases and on for the
new block:

1. `request_direct()` -> `cellGcmSetPrepareFlip(NULL, 0)` (the title's
   prepare-ahead) writes `s_flip_status = DONE` -> `cellGcmTickFlip()` +
   `ppu_gcm_pump()` completes **once** and leaves DONE.
2. Immediate second tick: no double completion, no second handler call.
3. `s_flip_req_us = now - 20000`: with key 1 the request is **not** completed;
   the status stays WAITING and the direct-pending flag stays set.
4. `s_flip_req_us = now - 16999`: inside the 17 ms bound it completes once.
5. **FIFO fall-through:** a request with `s_flip_direct_pending = 0` and
   `s_fifo_flips_reached = s_fifo_flips_completed + 1` completes on the tick,
   advances the reached marker, and a second tick does not fire again.
6. **Key 2:** a 20 ms-old request (missed by the first bound) completes; a
   33,367 us-old request does **not** (past the guest's abandon point); a
   33,365 us-old request does.
7. **Key 1 again:** the same 20 ms request is not completed.
8. No direct request (stale timestamp, status WAITING): the tick completes
   nothing.

`python3 tools/test_video_fixtures.py`: **30/30 pass** (including
`test_gcm_tick_in_drain`). No `spu_context` field was added; the shared-field
ABI check is unaffected.

## 4. User menu recordings (key off vs key on)

Two recordings from the same session (user test, 2026-11-04):

* `recomp-work/captures/menus_own_off_20261104_202035.pstl` (29.66 s,
  key off, log `play_20261104_202035.log`)
* `recomp-work/captures/menus_own_on_20261104_202035.pstl` (100.62 s,
  key on, log `play_20261104_202118.log`)

Both used `PS3RECOMP_TIMELINE_HITCH_US=17000`, so the capture contains every
frame longer than 17 ms; faster frames are counted from the `[flip]` log, not
from the file.

| quantity | off | on (=1) |
|---|---:|---:|
| guest flips / log window | 1,239 / 29.43 s | 5,989 / 100.76 s |
| fps | **42.10** | **59.44** |
| vblank ticks / s | 62.52 | 62.51 |
| ticks / flip | **1.485** | **1.052** |
| capture hitches (>17 ms) | 662 | 871 |
| hitch rate / s | 22.32 | 8.66 |
| 17-20 ms / s | 1.35 | **3.44** |
| 20-32 ms / s | 0.07 | **2.65** |
| 32-36 ms / s | **20.81** | **2.55** |
| 36+ ms / s | 0.10 | 0.01 |

The 34 ms class is the two-timeout fallback: 617 of 662 off hitches, and 257
of 871 on hitches. **Rate falls 20.81 -> 2.55/s (-88 %)**; the remaining 257
are frames where the tick landed after the 17 ms bound, so the key refuses the
completion exactly as intended (the same producer-bound case as the meter).

The on recording's 20-32 ms class is **not** a second-wait completion: the
`gcm.flip_done` for those frames lands p50 12.27 ms after the request (inside
the first wait), and the remaining 7-15 ms is the next buffer's ref tail. The
17-20 ms class has `flip_done` at p50 16.50 ms (p90 16.80) followed by the
short tail. The 34 ms class has essentially no completion (1 of 257), which is
the signature of the two-timeout fallback.

Per-second counts (17-20 / 20-32 / 32-36) are in the analysis script below.
The steady sections of the on recording run 59-61 fps; the low windows are
screen changes (start -> main menu -> team select).

Reproduce:

```sh
cd /home/m/Documents/mlb16-online
python3 - <<'EOF'
import struct, collections
for p in ('recomp-work/captures/menus_own_off_20261104_202035.pstl',
          'recomp-work/captures/menus_own_on_20261104_202035.pstl'):
    recs = []
    with open(p, 'rb') as f:
        f.read(20)
        while True:
            b = f.read(12)
            if len(b) < 12: break
            kind, tid, n = struct.unpack('<III', b)
            if kind == 1: f.read(n + (-n) % 4)
            elif kind == 2:
                d = f.read(32 * n)
                for i in range(n):
                    recs.append(struct.unpack_from('<QQIIII', d, 32 * i))
            else: break
    h = [r for r in recs if r[4] == 56]
    c = collections.Counter()
    for r in h:
        d = r[2]
        c['17-20' if d < 20000 else '20-32' if d < 32000
          else '32-36' if d < 36000 else '36+'] += 1
    span = (max(r[1] for r in h) - min(r[0] for r in recs)) / 1e9
    print(p, 'span %.2fs' % span, {k: '%.2f/s' % (v / span)
                                    for k, v in sorted(c.items())})
EOF
```

## 5. Pitch-meter A/B (0 / 1 / 2)

One launch: `ab_live_menufix2_20261104_203012`, `METER=1 SKIP_CINE=1 PIN=0
WCB=2 VIDEODRIVER=x11 PITCH_THRESH=35`, `PS3_LWCOND_STATS=1`, `N=4 SEG=10
DROP=2`, arms rotated A/B/C, boot off:

* A = `PS3RECOMP_FLIP_TICK_OWN_STATE=0`
* B = `PS3RECOMP_FLIP_TICK_OWN_STATE=1` (17 ms)
* C = `PS3RECOMP_FLIP_TICK_OWN_STATE=2` (33,366 us)

Pitch meter confirmed 154 s after launch, first X only. `pin_ab.py`:

| per-flip | A off | B =1 (17 ms) | C =2 (33.4 ms) |
|---|---:|---:|---:|
| frames | 1,119 | 1,159 | 931 |
| fps | 34.97 | **36.22** | **29.09** |
| p50 ms | **34.15** | **34.15** | **36.05** |
| p90 ms | 37.95 | 37.68 | 38.89 |
| p99 ms | 40.82 | 40.66 | 41.73 |
| std ms | 10.86 | 11.14 | 10.12 |
| mean ms | 28.58 | 27.63 | 34.36 |
| walker ms | 10.43 | 8.91 | **14.69** |
| wait ms | 16.56 | 16.21 | 17.47 |

Per-segment p50: A 34.16 / 34.15 / 34.15 / 34.16; B 34.15 / 34.15 / 34.15 /
34.14; C 36.18 / 36.01 / 36.03 / 35.73. Paired fps differences: B-A +2.74,
-1.12, +0.38, +3.00 (mean +1.25, mixed sign); C-A -5.00, -7.62, -6.00, -4.88
(consistently negative).

Two-timeout frames (`[lwcond-stats] deadline_frames`, two consecutive 17 ms
flip-done timeouts), interpolated per 8 s measured window:

| arm | deadline frames / 32 s | per second | per frame |
|---|---:|---:|---:|
| A | 362 | 11.32 | 0.324 |
| B | 389 | 12.17 | **0.336** |
| C | 113 | 3.52 | **0.121** |

Paired B-A per-frame differences are +0.031, -0.010, -0.008, +0.035 (mean
+0.012). This is much smaller than the first run's +0.047 and has mixed sign,
so the 17 ms key is neutral at the meter within this sampler's resolution. C's
low count is not a win: it moves the completion into the second wait and loses
5.9 fps (section 2).

Window health: 0 no-slot / 0 not-ready drops; longest `vkQueuePresentKHR` 5 ms;
max pump gap 61 ms (one 1 s window, otherwise 34-42 ms). No hang, no crash.

Scene thumbnails (2 s shots, 640x360, mean abs luma diff 0-255) inside the
measured windows:

| pair | n | mean | min | max |
|---|---:|---:|---:|---:|
| A/A | 105 | 2.48 | 1.01 | 3.68 |
| B/B | 78 | 2.62 | 0.79 | 3.89 |
| C/C | 105 | 2.69 | 1.01 | 4.11 |
| A/B | 195 | 2.49 | 0.95 | 3.84 |
| A/C | 225 | 2.53 | 0.87 | 4.10 |
| B/C | 195 | 2.56 | 0.87 | 4.04 |

Every cross-arm pair is inside the A/A control, so no thumbnail regression.

## 6. Recommendation

* Use `PS3RECOMP_FLIP_TICK_OWN_STATE=1` (17 ms) as the menu fix.
  * It preserves the user-confirmed menu result: the direct path is unchanged
    from the revision-1 build they tested (42.10 -> 59.44 fps, 34 ms class
    -88 %).
  * At the meter p50 is unchanged, fps is +1.25 (mixed sign, treat as neutral),
    and the two-timeout change is +0.012/frame with mixed sign.
* Do **not** use 2 (33,366 us). It removes deadline frames but converts them
  into second-wait completions, which cost 5.9 fps and 4.3 ms of walker time:
  the `KEEP_STATUS` failure mode.
* The code default remains **off**. Claude sets the play default after review;
  this mission does not change `play_windowed.sh`.

## 7. Caveats

* One automated launch, one meter state. p50 is identical in every paired
  segment, which is stronger than the ~1 fps run-to-run drift; the B-A fps
  sign is mixed, so +1.25 is not a claim of a meter win.
* `deadline_frames` is a cumulative counter logged every 2 s; the per-arm
  split interpolates it at the window edges and is good to roughly +/-10 %,
  not to the frame. The per-pair signs are reported above.
* The menu recordings come from one session and one navigation pass; the off
  recording is 30 s, the on recording 101 s. Rates, not totals, are compared.
  The capture only holds frames > 17 ms, so the fast-frame share comes from
  the `[flip]` log (ticks/flip 1.485 -> 1.052), not the file.
* The own-state gate handles a **direct** flip on its own state and a
  **FIFO-reached** flip through the fall-through. This title uses the direct
  path; the FIFO case is unit-tested but not exercised live here.
* No capture or `recomp-work` file is committed.

## 8. Clean room

All of this is our own static analysis of the EBOOT (Ghidra), our own runtime
code, trace stream and logs. No RPCS3 or other GPL emulator source was read or
cited.
