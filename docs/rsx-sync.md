# RSX sync: what the guest's GPU-done wait really is, and the team-select green box

Date: 2026-11-05. Branch `exp/rsx-sync` (`ps3recomp-sync`, off `exp/live-ab`),
build `recomp-work/build-sync`. **No game was launched for this document.**
Sources: the EBOOT in Ghidra (md5 `a58eb36f...`), the runtime sources, and the
user logs `play_20261105_121417.log` / `play_20261105_122342.log`
(`grep -a`; the logs carry binary bytes). No RPCS3 or other GPL emulator source
was read or cited.

This closes the three questions the green-box mission left open:

1. which GPU-done wait the game uses before it frees/reuses a movie plane;
2. when that wait becomes visible **in our runtime** relative to the walker
   reading the plane's bytes;
3. how far the walker can lag, and what other bugs belong to this class.

The fix is behind `PS3RECOMP_RSX_SYNC_FIX` (**default 0**); see §5.

## TL;DR

> **2026-11-05 evening:** this fix did not remove the green flash (live). The real cause was `UI_Movie_Close` clearing the planes with no GPU wait; see [bink-video.md §8](bink-video.md). The ordering analysis below still holds for other write-after-pending-draw cases.

| question | answer |
|---|---|
| the game's GPU-done wait | `Gfx_WaitFrameRef` (`0x00063b98`), called by `UI_Movie_Open` (`0x0097fff0`) via `Gfx_WaitLastFrameRef` (`0x00062e0c`) **on the first-open path only** |
| what it polls | the GCM control `ref` word (`control + 8`) against the `SET_REFERENCE` (NV406E method `0x0050`) token `(&DAT_01245058)[frame & 3]`, on lwcond `DAT_01245068` with a 9000 us `Cond_Wait`; the token is written by `Gfx_WriteRefToken` (`0x0006309c`) at `Gfx_FrameEnd` |
| per-frame gate | `Gfx_WaitPrevFrameRef` (`0x000655cc`) waits **N-1**: one render frame in flight (already documented in [`cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md)) |
| the reuse path | `UI_Movie_Open`'s `+0x1d7 != 0` branch clears the existing planes **without** `Gfx_WaitFrameRef`; `Asset_Release` (`0x004b8678`) has a 2-bit in-flight test (`owner+0x14 & 3`) but no GPU label |
| does our drain read the bytes? | **yes, synchronously** on the walker thread: `eng_texture_upload_work` (`rsx_draw_engine.c:2886`) reads guest memory at `:2939` and decodes at `:2956`; the async rsx-tex resolver is disabled (`eng_tex_resolver_on()` returns 0, `:7510`), and even its enqueue path snapshots the bytes on the state thread (`:7553`) |
| does our label run early? | **no**: `gcm_ref_push_at` is called after the draw methods (`cellGcmSys.c:2850`) and `gcm_ref_publish_one(0)` at the end of the unlocked pass (`:4050`) |
| then what is the green? | a plane write that lands **after the game's wait covers the frame** but **before the walker binds the pending draw**. The 12:14:43 pair is direct: `[bink-ready]` reads `0x1594BD80` with 93 non-zero samples at the draw, and `[tex-zero]` uploads the same address all-zero in the same second |
| walker lag | **<= 1 render frame** by design (the N-1 ref gate). Pitch/meter drain 16-40 ms against a 29-37 ms guest frame; team-select drain < 2 ms. The "2 s" in the log is the time until the **next draw names the plane**, not a 2 s read lag |
| fix | hold the guest write until one drain pass that **started after** the write has finished, then charge the write. Off by default; one cached int when off |

## 1. The guest's GPU-done wait

### 1.1 The per-frame gate (N-1)

`Gfx_FrameEnd` (`0x00066530`) appends `Gfx_WriteRefToken` (`0x0006309c`) to the
frame's command stream. The token is an NV406E `SET_REFERENCE` (`0x40050`,
method `0x0050`) carrying `DAT_01246044 + 1`, and it is stored in the
four-entry array:

```c
uVar15 = Gfx_WriteRefToken(...);
(&DAT_01245058)[*(uint *)(param_1 + 0x4bc) & 3] = uVar15;
```

Before the next frame is submitted, `Gfx_WaitPrevFrameRef` (`0x000655cc`) spins
and then `Cond_Wait`s (9000 us) until `*(int *)(DAT_01123b00 + 8)` (our
`GCM_CONTROL_GUEST_ADDR + 8`) equals the **previous** frame's token:

```c
piVar1 = &DAT_01245058 + (DAT_011203d4 & 3);   /* frame N-1 */
while (*(int *)(DAT_01123b00 + 8) != *piVar1)
    Cond_Wait(&DAT_01245068, 9000);
```

That is the one-render-frame-in-flight gate measured in
[`cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md) (19-21 ms
of PPU wait per frame).

### 1.2 The resource-release wait (last frame, first-open path only)

`UI_Movie_Open` calls, in order and only when `ctx+0x1d7 == 0`:

```c
Gfx_WaitLastFrameSubmit();   /* 0x000629e8 -> Gfx_WaitFrameSubmit(0x00065b9c) */
Gfx_WaitLastFrameRef();      /* 0x00062e0c -> Gfx_WaitFrameRef(0x00063b98)   */
/* then Asset_Release() on the eight old plane handles, and
   RenderTarget_Create() for the new planes at the same addresses */
```

`Gfx_WaitFrameRef` with `param_2 = 0` turns into the **last completed frame**
id `DAT_011203cc` and waits for its `SET_REFERENCE` token:

```c
if (uVar1 < param_2) {
    iVar2 = (&DAT_01245058)[param_2 & 3];
    Mutex_LockOwned(&DAT_01245068, 0);
    while (DAT_011203d4 == param_2 && ctrl->ref != iVar2 && iVar3 < 0x70)
        Cond_Wait(&DAT_01245068, 9000);
    Mutex_UnlockOwned(&DAT_01245068);
}
```

It also waits up to ~1 s (0x1389 x 200 us) for `DAT_011203cc` to catch up
first. So the first open of a movie really does wait until the RSX has reached
the end of the last submitted frame before it frees the old planes.

`Asset_Release` (`0x004b8678`) adds a software in-flight test:

```c
if (*(int *)(a + 0x50) == 0 ||
    ((*(uint *)(*(int *)(a + 0x50) + 0x14) & 3) == 0))
    /* unlink (Asset_Unlink, 0x004b838c) and free */
else if (flags & 0x1000)
    /* deferred list (0x004bb524) */
```

The `& 3` is a small owner-side reference/in-flight count, not a GPU label;
the GPU label in this title is the `SET_REFERENCE` token above.

### 1.3 The gap: the reuse path

The other branch of `UI_Movie_Open` (`ctx+0x1d7 != 0`, same movie re-opened at
the same dimensions) checks the sizes, then falls through to the shared clear
loop (`Texture_ClearLevelById` on the existing planes). It does **not** call
`Gfx_WaitFrameRef` or `Gfx_WaitLastFrameSubmit`. The fast team switch uses this
path: the planes are reused and zeroed while the RSX may still hold the
previous frame's draw, and the game relies on the per-frame N-1 gate (one
render frame in flight) plus the movie's own double buffer rather than on a
fresh fence.

That is the premise the green-box mission started from, and it is true: the
only full fence is on the first-open path.

## 2. When the wait becomes visible in our runtime

### 2.1 The token is not early

* `cellGcm_rsx_process_fifo_unlocked` walks the ring in order. Draw methods
  are executed when the walker reaches them; `SET_REFERENCE` is handled in
  `gcm_3d_method` (`cellGcmSys.c:2849`), which calls
  `gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff)` (`:2850`). Every draw
  before the token in the stream has already been processed.
* `gcm_ref_publish_one(0)` (`cellGcmSys.c:4050`) writes `control+8` at the end
  of the unlocked pass. `GCM_REF_IMMEDIATE=1` only moves that write from the
  drain tail to the moment `0x0050` is executed; both are after the draws.
* `Gfx_WaitFrameRef` therefore means "the walker has parsed and executed every
  command through the end of that frame".

### 2.2 The drain reads texture bytes synchronously

The engine resolves a draw's textures inside the same pass, on the walker
thread:

* `sink_bind_textures` -> `eng_texture_slot` -> `eng_texture_upload_work`
  (`rsx_draw_engine.c:2886`);
* `eng_guest_ptr` (`:2939`) returns the live guest bytes and
  `rsx_texture_decode` (`:2956`) copies/decodes them into `g.tex_staging`,
  before the backend upload;
* the asynchronous rsx-tex resolver is a no-op: `eng_tex_resolver_on()`
  returns 0 (`:7510`), so `s_tex_async_draw` is always 0; even the enqueue path
  it would have used takes the byte snapshot on the state thread at the stream
  position (`:7553-7560`), not on the worker.

So a full pass that started after the write **has read** every pending draw's
texture bytes by the time it acknowledges. There is no label-before-read
window in the sense the mission asked about.

### 2.3 What is still racy

The guest write is a host store (PPU lifted `memset`, or an SPU MFC PUT through
`ps3_guest_write`) that lands at the instant the guest executes it. If the
game clears a plane that a **still-pending** draw names, and that draw is not
covered by the fence the game just took (the reuse path, or a draw submitted
after the clear in the same frame), the walker binds it after the clear and
`eng_texture_slot` sees the new `dirty_seq`, re-uploads the zero bytes, and the
draw samples green.

The 2026-11-05 12:14:43 pair is the direct evidence:

```
12:14:43 [bink-ready] ctx=0x155C11E0 tick=6 dec=2 set=0 st=2
         Y=@1594BD80:z0/m93 Cr=@1595BF00:z0/m108 Cb=@15960080:z0/m153 ...
12:14:43 [tex-zero] frame=2967 guest_flip=2774 loc=1 off=0x0494BD80
         ea=0x1594BD80 256x256 fmt=0xA1 pitch=256
```

The getter (i.e. the draw the game is about to submit) reads non-zero decoded
data; the walker uploads the same plane all-zero a moment later. The clear that
zeroed it is the `RenderTarget_Create` zero-fill (`FUN_004b8ae4` ->
`FUN_00a42468(dst, 0, size)`, the `[ww]` writes) plus the level clear in
`UI_Movie_Open`.

The 12:24:17 -> 12:24:19 "2 s" in the resume is **not** a 2 s walker lag: it is
the interval from the clear to the next draw that names the plane. The walker's
own submission lag is at most one frame (§3).

## 3. How far can the walker lag?

| scene | guest frame | walker drain | submission lag | texture-read lag |
|---|---:|---:|---:|---:|
| team select | ~33 ms (30 fps) | < 2 ms (logs: `walker=0.03-0.06 ms`, `wait=17-20 ms`) | <= 1 frame | <= 1 frame |
| pitch meter | 29-37 ms | 17-40 ms | <= 1 frame | <= 1 frame |
| crowded play | 29-37 ms | 17-40 ms | <= 1 frame | <= 1 frame |

The guest's own N-1 gate is the bound: frame N's put cannot be written until
the walker has reached frame N-1's `SET_REFERENCE`. The walker can therefore
be at most one submitted frame behind the PPU (and the PPU can be in the next
frame while the walker drains the previous one). The team-select numbers are
the 2026-11-05 logs (`[walker-prof] ... walker=0.036ms wait=19.342ms`); the
meter numbers are [`walker-prof.md`](walker-prof.md) and
[`cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md).

Other bugs in this class (same "guest write overtakes a pending read"):

* one-frame wrong/zero textures on UI transitions (this report);
* a render target reused for the next frame while the previous frame's readback
  or sampled view is pending (the WCB/`eng_surface` path already serialises
  these with `eng_mt_sync`, which is why it is not on the critical path);
* SPU decode (Bink, DXT) writing the next frame into a plane while a delayed
  draw still samples the current one: the same shape, a different producer;
* the tiled wipe/logo transition could be this class (a tiled surface written
  between the bind and the sample), but that is **not proven** here and needs
  its own capture.

## 4. The fix

Behind `PS3RECOMP_RSX_SYNC_FIX` (env, **default 0**; cached on first use):

* `rsx_wcb.c` notices a write to a page a cached texture covers, **before** it
  is charged into `page_seq` (`rsx_texdirty_on_fault`, `td_guest_write_hook`),
  and calls a callback installed by `cellGcmInit`
  (`rsx_texdirty_set_sync_request`);
* the callback (`cellGcm_texdirty_sync_request`, `cellGcmSys.c:1348`) bumps
  `s_td_sync_req` and waits until a drain pass that **started after** the bump
  has finished;
* `cellGcm_rsx_process_fifo` snapshots `s_td_sync_req` on entry
  (`cellGcmSys.c:2742`), sets `t_in_gcm_drain`, runs the pass, and on exit
  stores the snapshot into `s_td_sync_ack` (`:2759`). Requests that arrive
  during a pass wait for the next one;
* because the write has not been charged while the guest waits, the pass sees
  the **old** `dirty_seq` and the pending draws sample the old bytes; then the
  store lands, `seq` moves, and the next bind re-uploads the new bytes.

Deadlock safety: the wait happens **outside** `wcb_lock` (the drain takes it)
and the callback returns immediately when the current thread is the walker
inside a pass (`cellGcm_texdirty_in_drain`, checked by `td_in_drain()` before
the lock in `rsx_wcb.c:1274`). `s_gcm_fifo_lock` is never held by a waiter.
The wait is async-signal-safe (atomics, `clock_gettime`, `sched_yield`,
`write(2)`; no stdio). When the key is 0 the cost is one cached `int` per
guest write (and one per texture-page fault); when the key is 1 it is one
`wcb_lock` range scan per write to a covered range.

Bound: 100 ms. The measured drain is 16-40 ms at the meter and < 2 ms at team
select, so 100 ms is two to six passes; a pass that is compiling a shader
(0.25-2 s, [`present-stalls.md`](present-stalls.md)) is abandoned rather than
stalling the switch for its whole length. A timeout keeps the old order for
that write and is logged at most eight times.

Cost: a team switch pays at most one drain lag on the PPU thread (<= one frame;
< 2 ms at team select, 16-40 ms worst case at the meter). The pitch/meter
scene has no movie-plane clears, so its steady-state fps cost is expected to be
~0; the key-off build is byte-identical to `exp/live-ab` in this path. The
live A/B below measures it.

## 5. Offline verification

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-sync
python3 tools/test_video_fixtures.py
```

`test_rsx_draw_engine` gains four checks: a covered PPU store requests the
sync before the write is charged; the write sequence is still the pre-store
value at the request and moves after it; the store lands afterwards; a covered
host write through `ps3_guest_write` (the SPU MFC PUT / HLE memset path)
requests it too, and a write outside every cached texture does not. The suite
passed **34/34** on the recorded run. `test_gcm_tick_in_drain` is the
pre-existing ~50 % timing flake: it fails the same way on unmodified
`exp/live-ab`, and a rerun passes (`build-sync` and `build-play` both measured
4-5 failures in 8 standalone runs under load).

The full game binary builds with `build-play`'s options copied into
`recomp-work/build-sync` (`-O2 -DNDEBUG -march=native`, Vulkan, 14 SPU images,
`-mfma` on the SPU TUs).

## 6. User check (~2 minutes)

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# key ON
BUILD=build-sync PS3RECOMP_RSX_SYNC_FIX=1 PS3RECOMP_TEX_ZERO_LOG=1 \
  ../mlb16-recomp/tools/play_windowed.sh

# key OFF (same scene)
BUILD=build-sync PS3RECOMP_RSX_SYNC_FIX=0 PS3RECOMP_TEX_ZERO_LOG=1 \
  ../mlb16-recomp/tools/play_windowed.sh
```

Reach team select and scroll the team list quickly with the d-pad, then close
the window and compare:

```sh
L=$(ls -t play_*.log | head -1)
grep -a -c 'tex-zero.*0x1594\|tex-zero.*0x1595\|tex-zero.*0x1596\|tex-zero.*0x1597' "$L"
grep -a '\[tex-zero\]' "$L" | head
grep -a '\[rsx-sync\]' "$L" | head
```

What to look for:

* green box gone or visibly shorter with the key on: keep it on; report the
  `[tex-zero]` counts for both arms;
* green unchanged: the zero is being drawn **before** the decoder writes the
  plane (the guest's four-tick gate, [`bink-video.md`](bink-video.md) §7), not
  a stale read; the next step is the decode, not this wait;
* `[rsx-sync] drain did not acknowledge ... in 100 ms` at every switch: the
  drain is slower than the bound; report the lines (do not raise the bound
  without the fps cost).

## 7. Ghidra renames made for this mission

| Old | New | EA |
|---|---|---|
| `FUN_00063b98` | `Gfx_WaitFrameRef` | `0x00063b98` |
| `FUN_00065b9c` | `Gfx_WaitFrameSubmit` | `0x00065b9c` |
| `FUN_0006309c` | `Gfx_WriteRefToken` | `0x0006309c` |
| `FUN_00062e0c` | `Gfx_WaitLastFrameRef` | `0x00062e0c` |
| `FUN_000629e8` | `Gfx_WaitLastFrameSubmit` | `0x000629e8` |
| `FUN_000655cc` | `Gfx_WaitPrevFrameRef` | `0x000655cc` |
| `FUN_004b8678` | `Asset_Release` | `0x004b8678` |

## 8. Caveats

* No live trace of the reuse-path flag (`ctx+0x1d7`) was taken; the branch is
  read statically. The fix does not depend on which branch ran: it delays any
  write to a covered texture page.
* The 12:14:43 pair proves "guest bytes are non-zero at the draw and the walker
  uploads zero later"; it does not by itself prove that the zeroing write was
  the **reuse clear** rather than a decode/fill. The `[ww]` correlation at
  12:24:17 (RenderTarget_Create zero-fill) and the 12:14:43 timing make the
  clear the only writer seen; a `PS3RECOMP_RSX_SYNC_FIX` A/B settles it.
* The fix serialises one write with one drain pass; it does not add a fence to
  the guest or change the guest's code.
