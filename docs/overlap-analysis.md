# The overlap lever: when the RSX ref becomes visible, and why the PPU waits

Date: 2026-11-02. Analysis only. One traced pitch run (`measure_fast.sh`, the normal measurement env:
`VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`), **35.01 fps over the 10 s counter**; trace window 12.05 s,
416 flips (34.53 fps), 510 NV406E_SET_REFERENCE writes, 396 PPU ref-wait episodes. New trace-only
events in `ps3recomp-probe` `mlb16-work` (off unless `PS3RECOMP_TRACE` is set) record the walker
reaching each fence and every guest-visible ref write.

## TL;DR

**The ref is not released late by a Vulkan submit, a GPU fence, a flip, `VK_FRAME_PIPELINE`, or a
deferred write-back.** The walker queues each fence at method dispatch and the runtime writes the ref
register at the end of that same drain pass. Measured, over all 510 fences:

| delay | p50 | p90 | mean | max |
|---|---:|---:|---:|---:|
| walker reaches `SET_REFERENCE` -> ref register written (`gcm.ref_set` -> `gcm.ref_pub`) | **3 µs** | 4 µs | 3 µs | 20 µs |
| ref written -> PPU main reads it (per 396 wait episodes) | **0.57 ms** | 1.01 ms | 0.97 ms | 22.9 ms* |
| PPU wait episode | **21.4 ms** | 30.8 ms | **20.7 ms** | 92.2 ms |
| — of which before the walker reaches the fence | 21.1 ms | 29.5 ms | **19.7 ms** | 91.5 ms |

\* the max is a frame where the PPU was not in the wait when the value appeared; the p90 is 1.0 ms.

In **396/396** wait episodes the target fence had *not* been written when the wait began. The PPU is
therefore waiting for the walker to **reach** the fence, not for a late publication: 19.7 of the
20.7 ms is walker time-to-fence. During that time the walker is inside a drain pass about **53 %** of
the wall clock (p50 72 %, p90 93 %) and parked/starved for the rest. This reproduces the
[critical-path](ppu-critical-path.md) split (11.6 ms drain, 8.1 ms starve) with the publication
question now answered directly.

The fence is the **last sync word before the frame's park**: `pub - (end of the drain pass)` is
0.000 ms at p50. So the proposed "publish at walker-dispatch time" is a **no-op** for this title:
dispatch already happens at the end of the pass. There is at most ~0.6–1 ms of headroom, and that is
the PPU's own wake latency, not the runtime.

The game already overlaps the PPU append with the walker: main builds frame N during the walker's
drain of N−1, and only the submission tail (the ref wait) is serial. There is no further append work
to overlap. Breaking that wait needs either an earlier guest fence or a two-frame guest pipeline —
both guest-code changes. Adding runtime ring buffers does not change it (section 4).

## Method

One traced pitch run, same trigger method as [`ppu-critical-path.md`](ppu-critical-path.md)
(`/tmp/opencode/ppupath/watch.py` creates the trigger when the pitch reference matches):

```
# 10 s counter: 35.01 fps, 350 flips. Trace window: 12.05 s, 416 flips, 34.53 fps.
EXTRA="PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/opencode/overlap/trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/opencode/ppupath/go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=16 PS3RECOMP_TRACE_FLUSH_MB=8" \
  recomp-work/ab-tools/measure_fast.sh overlap_traced.log
```

Instrumentation (trace-only, `ps3_trace_on()` gated, off by default), on `ps3recomp-probe`
`mlb16-work`:

* `gcm.ref_set` — `a = get (IO offset), b = value`, emitted at the top of `gcm_ref_push_at`
  (`libs/video/cellGcmSys.c:1799`), i.e. the instant the walker decodes the fence. This covers the
  normal dispatch (`cellGcmSys.c:2168`) and the resync rescue (`cellGcmSys.c:1691`).
* `gcm.ref_pub` — `a = value, b = source (0 = end of drain pass, 1 = read-driven poll)`, emitted at
  the guest-visible write (`libs/video/cellGcmSys.c:1831`, write at `:1830`).
* Event ids and descriptors: `include/ps3emu/ps3_trace.h:118-119`, `:173-174`.

The analysis scripts are in `/tmp/opencode/overlap/` (`analyse_ref.py`, `episodes.py`); the trace is
245 MB and stays in `/tmp`.

## 1. Guest side

### 1.1 The wait in `FUN_000655cc`

`FUN_000655cc` (0x000655cc) is called by the put writer `FUN_00065760` (0x00065760) at
**0x00065798**, before `FUN_00065760` writes the new put at **0x00065af8**. Its loop is:

```c
piVar1 = &DAT_01245058 + (DAT_011203d4 & 3);       /* 0x000655f0 */
if (*piVar1 != 0) {
  iVar2 = *piVar1;                                  /* expected ref */
  Mutex_LockOwned(&DAT_01245068, 0);                /* 0x0006562c */
  while (*(int *)(DAT_01123b00 + 8) != iVar2) {     /* 0x00065644..0x00065650 */
    ... DAT_01246662 / DAT_01244e00 reset the retry counter ...
    if (0x15b3 < iVar3) break;                      /* 5555 counted iterations */
    Cond_Wait(&DAT_01245068, 9000);                 /* 0x00065690, lwcond 0x01245090 */
  }
  Mutex_UnlockOwned(&DAT_01245068);
  ...
}
```

* `DAT_01123B00` is the GCM control-register pointer stored once by `FUN_0006de94` (0x0006de94)
  from `cellGcmGetControlRegister`; `+0` put, `+4` get, `+8` **ref**.
* The expected value is the 4-entry global array `DAT_01245058 + (DAT_011203d4 & 3)`; it is **not** a
  field of a ring struct. The slot index is the previously submitted frame's counter.
* The wait is a **depth-1 submission throttle**: wait for the previous frame's fence, then write this
  frame's put. It is not a display-flip wait (that is `Gfx_DisplayFlip` 0x000692dc / `DAT_01245020`).
* The lwcond is `0x01245090` (mutex object `0x01245068`, lwmutex `0x01245078`). It is signalled by
  the GCM user handler `.opd.FUN_00069138` (0x00069138, installed by `cellGcmSetUserHandler`), which
  runs when the walker executes the user-command block (`0x4E000/0xCAFEBABE/0x4EB00`) that follows
  the fence. Each 9000 µs `Cond_Wait` returns after ~1.06 ms in this runtime (the trace sees
  ~7,600 × 1.06 ms spans for ~400 logical waits).

The retry cap (`0x15b3` = 5555) only trips if neither `DAT_01246662` nor `DAT_01244e00` is set; at
the pitch the wait resets the counter and is effectively unbounded (normal behaviour, not hit).

### 1.2 Command rings and sizes

The game uses **one continuous command ring plus one small frame-tail ring**, not an array of
rotating buffers:

| ring | struct EA | base | size | notes |
|---|---|---|---|---|
| main pushbuffer | `0x01246390` | `[0x0111FFA8] + 0x1300000` | `0x280000` (usable `0x27FC00`, field `0x0124658C`) | `begin/end/cursor` at `+0x0/+0x4/+0x8`, `lastput/put/end/size` at `+0x1EC/+0x1F0/+0x1F8/+0x1FC` |
| mesh / frame-tail ring | `0x01244E08` | `[*(0x01476000)+4] - 0x3FFC` (tail of the cellGcm 64 KB FIFO) | `0x4000` (field `0x01245004`) | same field layout |
| cellGcm init FIFO | — | from `_cellGcmInitBody` | **`0x10000`** | `FUN_0006de94` 0x0006dea4/0x0006deb4; IO region total `0x1580000` |

Initialisation: `FUN_00067734` 0x00067810 -> `FUN_0006de94` -> `_cellGcmInitBody(ctx, 0x10000, …)`;
`FUN_00067734` also prints the split `GCMInit 0x10000, Mesh processing 0xA00000, Main pushbuffer
0x280000, IO Allocator 0x8F0000` (total `0x1580000`). The mesh ring is the last 16 KB of the 64 KB
FIFO. Only **2 display buffers** are registered (`cellGcmSetDisplayBuffer` in `FUN_00064b68`), and the
flip throttle allows 3 outstanding flips (`DAT_01120410 = 2`, `Gfx_FrameEnd` 0x00067368).

At frame end the two rings are cross-linked: a JUMP in the mesh ring targets the main ring's current
put (0x00066A74), and a JUMP in the main ring targets the mesh ring's new put (0x00066AE0). The
submitted `put` is the **mesh ring's** put (`FUN_00065760` computes it from `DAT_01244E10`).

### 1.3 The ref values

There is **one global monotonic counter, `DAT_01246044`** (initialised to 1 in `FUN_00067734`), not a
value per ring. `FUN_0006309c` increments it and emits `0x00040050` = `NV406E_SET_REFERENCE`, count 1,
value, into the current ring (0x00063100–0x0006310C), and stores the value in
`DAT_01245058[ctx+0x4bc & 3]` (`Gfx_FrameEnd` 0x0006703C for the flip path, 0x0006726C for the kick
path). A second, inline emission in the non-flip/region branch (0x000668A4–0x000668C4) writes into the
main ring; that value is not stored in the wait array.

The trace shows the two kinds: 255 fences at low IO offsets (the mesh-tail fence, e.g. `0xD28C`,
`0xD338`, `0xD3E4`) and 255 at high main-pushbuffer offsets (e.g. `0x141F0E0`, `0x14640D4`). The PPU's
wait targets were 250 low and 146 high over the window. Both are emitted immediately before the
walker's park, which is why publication is effectively immediate (section 2).

## 2. Runtime side: every path that makes the ref visible

### 2.1 `NV406E_SET_REFERENCE` (method `0x0050`)

1. **Dispatch.** `rsx_process_method` sets `g_rsx_last_reference = data`
   (`libs/video/rsx_commands.c:965`). The walker's `gcm_3d_method` then calls
   `gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff)` (`cellGcmSys.c:2168`). This only **queues**;
   it does not write guest memory.
2. **Queue.** `s_ref_q` / `s_ref_qhead` / `s_ref_qtail`, 4096 entries (`cellGcmSys.c:1521-1523`,
   push at `1795-1807`). Overflow drops the oldest (liveness, never at the pitch: 510 fences in 12 s).
3. **Guest-visible write.** `gcm_ref_publish_one` writes `GCM_CONTROL_GUEST_ADDR + 8`
   (`cellGcmSys.c:1825-1831`) — a plain `vm_write32`, **no host/GPU fence**, no flip dependency.
   Callers:
   * end of every drain pass — `cellGcmSys.c:3194` (always runs, including when the pass stopped on a
     park, a flip word, a semaphore acquire or a resync);
   * the read-driven poll — `cellGcm_ref_on_poll` (`cellGcmSys.c:1895-1912`), called from `vm_read32`
     of the ref register (`runtime/ppu/ppu_loader.cpp:1758-1764`), paced to one publication per
     ~200 µs (`cellGcmSys.c:1901-1909`) and default-on (`GCM_REFPOLL=0` disables).
4. **Pacing.** At most one queued fence per drain pass. That could delay intermediate values if many
   were queued, but at the pitch the waited fence is the last command before the pass ends, so the
   pass tail is the write. The poll path adds a second, faster publisher while the PPU spins; a PPU
   sleeping in `sys_lwcond_wait` is woken by the user handler and reads the ref on each ~1.06 ms wake,
   which also runs the poll publisher.

### 2.2 Labels and semaphores — written at dispatch

Unlike the ref, these are not queued and are not delayed to the pass end:

* `NV406E_SEMAPHORE_OFFSET/RELEASE` (`0x64/0x6C`): the walker writes
  `GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFF)` at dispatch (`cellGcmSys.c:2721`, write `:2751`),
  after publishing `get` (`:2745-2746`).
* `NV4097` back-end/texture-read releases (`0x1D6C/0x1D70/0x1D74`): `rsx_process_method` writes the
  label immediately (`libs/video/rsx_commands.c:525`, `:545`, `:555`; `0x1D70` with the required
  byte swap). The resync rescue replays the same writes (`cellGcmSys.c:1725-1732`).
* `cellGcmSetFlipCommandWithWaitLabel` writes its label from the PPU immediately
  (`cellGcmSys.c:3511-3513`).

So a guest polling a label sees it at walker dispatch; only the ref is batched, and in this title the
batching costs ~3 µs.

### 2.3 Vulkan / present / `VK_FRAME_PIPELINE` — not on the ref path

* The ref write is a guest-memory store; it has no dependency on `vkQueueSubmit`, `vkWaitForFences`,
  or the present fence.
* `VK_FRAME_PIPELINE=1` (used by `measure_fast.sh`) batches GPU work into 3 sets, submits at present
  without waiting, and waits only before reusing a set (`libs/video/rsx_vulkan_backend.c:3961-3992`,
  `:4022-4027`, `:4051-4063`). It does not touch `s_ref_q`.
* Default per-draw `vk_submit_and_wait` (`rsx_vulkan_backend.c:678-689`, `:4506-4507`) is bypassed
  when `VK_DRAW_BATCH=1`. `rsx_draw_engine_flush_dirty_to_guest` is a no-op unless
  `PS3RECOMP_RSX_WCB` is set (`rsx_draw_engine.c:3727-3751`), so the fence path does not read back.
* A GPU wait can still **delay reaching** a later fence if it is inside the same drain pass (set-reuse
  `vk_ebs_swap`, batch-boundary flush). That shows up as a long drain pass, not as a late ref write.

### 2.4 Measured: the fence is the last word, publication is immediate

* Across all 510 fences: `set -> pub` p50 **3 µs**, p90 4 µs, max 20 µs.
* `pub - end of the covering drain pass`: p50 0.000 ms, p90 0.002 ms, max 0.012 ms — the fence is
  decoded at the pass boundary.
* Across the 396 PPU wait episodes:
  * target fence already written before the wait began: **0/396**;
  * time from wait start to the walker reaching the fence: p50 21.1 ms, mean 19.7 ms;
  * set -> pub: mean 3 µs;
  * pub -> PPU reads it: p50 0.57 ms, mean 0.97 ms (the ~1.06 ms lwcond/scheduling cadence);
  * walker in a drain pass during the pre-publication wait: mean 52.6 %, p50 71.9 %, p90 92.7 %.

Example (`trace` window, times relative): fence `0x26EE` is reached at 68,139 µs, published at
68,157 µs, the same drain pass ends at 68,159 µs; the PPU's wait for it started ~20 ms earlier and it
wakes within ~1 ms. The 20 ms is the walker draining and starving, not the ref write.

## 3. Hardware / RPCS3 comparison

On real hardware (and in any emulator that models the command processor), an
`NV406E_SET_REFERENCE` updates the ref register **when the RSX command processor reaches that
command**, not when the frame's draws finish rendering and not at flip. Our runtime does the same at
the semantic level: the value is captured at dispatch. It then writes the register at the end of the
host drain pass (`cellGcmSys.c:3194`). For this title that is a ~3 µs difference because the guest
puts the fence immediately before the park that ends the pass. **The publication model is therefore
not the difference from RPCS3.**

The difference is time-to-fence. Our walker is the RSX: it fetches vertices, decodes textures, builds
pipelines and records Vulkan commands synchronously, ~17 ms of CPU per frame (the drawprof in this
run: 9.7–10.1 µs/draw × ~500 draws ≈ 4.9 ms just recording draws), and it also stalls on SPU/c690
producer parks. RPCS3's command processing reaches the same fence in less wall time on this PC; that
is consistent with its ~43 fps, and it does not require a different ref-publication rule. (RPCS3
source was not consulted.)

If a future fence were **not** the last word of a pass, publishing at dispatch would be safe with
respect to guest memory: the walker has already read every vertex/index/texture byte referenced before
the fence as it dispatched those draws. It would *not* guarantee GPU rendering completion — but neither
does the hardware command-processor fence, and the guest uses it as a command-stream milestone
(`cellGcmFinish` semantics). The one caveat is any deferred guest-memory reader: `PS3RECOMP_RSX_WCB`
read-back does run `submit_and_wait` and must stay ordered before such a publication (it is off by
default and not used by this title at the pitch).

## 4. More ring buffers / deeper pipelining without guest changes?

**No.** The game does not rotate among N command buffers; it reuses one main ring and one mesh-tail
ring, and `FUN_000655cc` is an explicit **one-frame-in-flight throttle** (`DAT_01245058[previous
slot]`). A runtime that owns more buffers cannot make the guest skip the wait: the guest still calls
`FUN_000655cc` and compares the single global ref before writing the next put. The 4-entry
`0x01245018/20/58` arrays are histories (4 slots of `id & 3`), not 4 buffers. The 2.5 MB main ring is
not full at the pitch; size is not the limit.

Faking the ref earlier (or rotating buffers behind the guest's back) lets the PPU overwrite command
memory the walker has not read. That is exactly the failure of the earlier `MLB16_PIPELINE` `(id+1)&3`
experiment (geometry corruption / 185 flips in 689 s). A deeper pipeline is a **guest-code change**
(more buffers plus a ref scheme that permits two frames in flight), not a runtime one.

## 5. Options, with expected gain and safe experiments

All estimates use the measured 20.7 ms wait and the 29 ms frame (34.5 fps in-window, 35.0 on the
10 s counter). Single runs swing ±2 fps, so every experiment below is env-gated and needs several
alternating runs.

| # | option | expected saving | fps estimate | correctness risk | safe experiment |
|---|---|---|---:|---|---|
| A | **Publish the ref at `SET_REFERENCE` dispatch instead of pass end** | **≤0.02 ms** (measured 3 µs; the fence is already at the pass end) | 0 | low (the walker has read all earlier guest memory) | `PS3RECOMP_REF_IMMEDIATE=1` writing in `gcm_ref_push_at` under the pub lock; compare `gcm.ref_set`->`gcm.ref_pub` with the trace; 3 alternating runs. Expected: no change; do not spend a run on it unless the trace shows a mid-pass fence. |
| B | **Shorten walker time-to-fence: cut drain CPU** | up to the cut; the earlier cuts are 2.6 ms offline, so 0–2.6 ms | 35 → 35–38 | none (output gate verified) | existing `PS3RECOMP_WALKER_FAST=1`, 3 alternating A/B runs; also run `PS3RECOMP_WALKER_PROF=1` once to confirm the in-game drain drop. The 0.9 ms of removed wait plus ~2.6 ms of CPU is the realistic 1–3 ms range; the first A/B saw no gain, consistent with the walker then starving more. |
| C | **Shorten the starved half of the wait** (c690 earlier, wake-on-PUT) | `e2` 0.9 ms was measured removable but relocated; 0–2 ms | 35 → 35–37 | low–medium | `PS3RECOMP_WALKER_WAKE=1` alone (already off by default) and, separately, earlier c690 scheduling; 3 alternating runs. |
| D | **Deeper pipeline / more rings** | theoretical up to the 9.8 ms starved half (frame 29 → ~19 ms) | ~50 | **high** (would need to violate the depth-1 fence) | not safe without guest changes; do not run. |
| E | **Env A/B on the GPU path to see how much of the drain is recording vs set-reuse waits** | unknown, 0–3 ms | 35 → 35–38 | none | `VK_FRAME_PIPELINE=0` vs `1` (and `VK_DRAW_BATCH=0`) with 3 alternating runs; the drawprof in this run shows ~4.9 ms/frame of pure draw recording, so pipeline changes can only affect the rest. |

The honest ranking for the **overlap** lever specifically: A is a no-op; B and C are the same
walker-time-to-fence levers the earlier docs identified, not new overlap; D is a guest change. The
frame is `walker drain + walker producer wait`, and the PPU's 19.7 ms block is the walker's path to
the fence, already overlapped with the PPU's own 9.3 ms of append work.

## Caveats

* One run, one window (12.05 s, 416 flips). The trace window runs 34.53 fps against 35.01 on the
  10 s counter; the event cost is small (two instants per fence for 510 fences, two per publication).
* The `pub -> PPU reads it` max (22.9 ms) is a frame where the PPU was not waiting when the value
  appeared; the normal case is one 1.06 ms lwcond tick.
* "In a drain pass" is the walker's wall time in `rsx.drain`, which can include an internal Vulkan
  set-reuse wait; it is not pure CPU. The split "drain vs starve" is therefore a lower bound on the
  starved fraction.
* The waited fence is sometimes the main-ring inline ref and sometimes the mesh-tail ref (250/146);
  both are emitted immediately before a park, and both show the same ~3 µs set->pub.
* RPCS3 was not run or read for this analysis; the 43 fps comparison is the user's on this PC.

## Reproduce

```
# one traced pitch run (delete stale frame.ppm first; check the ISO is mounted)
rm -f recomp-work/frame.ppm /tmp/opencode/ppupath/go
python3 /tmp/opencode/ppupath/watch.py &         # creates the trigger at the pitch wait
EXTRA="PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1 \
  PS3RECOMP_TRACE=/tmp/opencode/overlap/trace.json \
  PS3RECOMP_TRACE_TRIGGER=/tmp/opencode/ppupath/go PS3RECOMP_TRACE_SECS=14 \
  PS3RECOMP_TRACE_RING_MB=16 PS3RECOMP_TRACE_FLUSH_MB=8" \
  recomp-work/ab-tools/measure_fast.sh overlap_traced.log

python3 /tmp/opencode/overlap/analyse_ref.py /tmp/opencode/overlap/trace.json
python3 /tmp/opencode/overlap/episodes.py   /tmp/opencode/overlap/trace.json
```
