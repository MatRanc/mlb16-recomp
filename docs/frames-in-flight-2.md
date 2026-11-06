# Two frames in flight: what the guest gate really protects, and what it would take

Date: 2026-11-05. Phase 1 (design) only: static analysis of the EBOOT in Ghidra (md5 `a58eb36f...`), the runtime
sources in `ps3recomp-depth` (`exp/oneshot2`) and the existing docs. **Zero game launches, zero builds, no code
written.** No RPCS3 or other GPL emulator source was read or cited. "GUESS" marks anything not read from code or a
measurement; "UNVERIFIED" marks a lead I did not close.

Reads: [`rsx-sync.md`](rsx-sync.md), [`cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md),
[`overlap-analysis.md`](overlap-analysis.md) (ring table), [`ppu-critical-path.md`](ppu-critical-path.md),
[`frame-timeline.md`](frame-timeline.md), [`frame-timeline-2.md`](frame-timeline-2.md),
[`c690-serialisation.md`](c690-serialisation.md), [`bink-video.md`](bink-video.md) §8,
[`walker-multithread-design.md`](walker-multithread-design.md), [`progress.md`](progress.md) (2026-10-27 mesh rings).

## TL;DR

| question | answer |
|---|---|
| Is the N-1 gate one site? | **No.** The wait target is one call (`0x00065798` -> `Gfx_WaitPrevFrameRef`), but the gate is **an equality test on `ctrl->ref`** at six sites, and the same function **advances the pushbuffer free-limit** (`FUN_00060904`). Moving the target to N-2 without those fixes hangs (equality never matches once the RSX is past the token) or lets the PPU overwrite unread commands. |
| Is it safe if only the pushbuffer is doubled? | **No.** The five c690 mesh rings are reset to their base **every frame** (`Mesh_SetRingStartsAndKick`, mode 0). Their protection is a "GPU read position is inside the range I want" check, which is correct for one frame behind and **wrong for two** (N+2 would overwrite unread N+1 vertex data). They need a third copy (or three parity sets). |
| Is the gate the thing that serialises us? | **Only partly.** Chain per frame: gate wake -> PPU tail of `Gfx_FrameEnd` -> put -> walker. The c690 burst, the PPU build and the walker already overlap (the burst for N+1 starts when N-1 finishes). What the gate costs is the **start-of-frame put-wait** (measured 9.8-15.6 ms on a 86 ms crowded frame, `c690-serialisation.md`) plus the wake/tail (~4-6 ms at the meter). Estimated ceiling for any decoupling = that share, **not** 2x (see §5). |
| Verdict | The premise "let the guest run one frame further ahead" is **a high-risk change with a small, unmeasured payoff**. Phase 2 was **not started**. §6 gives the cheaper measurement that decides it, and §4 the full patch list if the user still wants it. |

## 1. The gate, with evidence (Ghidra)

### 1.1 Who waits, who advances

`Gfx_FrameEnd(N)` (`0x00066530`): closes frame N's stream (jump into the tail ring, `SET_REFERENCE` token via
`Gfx_WriteRefToken` stored in `DAT_01245058[N & 3]`, user-handler words `0x4e000/0x4eb00`), then either

* **sync path** (the one used at the pitch): `FUN_00065760` (`0x00065760`), which calls `Gfx_WaitPrevFrameRef` at
  `0x00065798`, then stores `DAT_011203d4 = DAT_011203cc` (**the last kicked frame id**), shifts the ring limit
  words (`ctx[0x7b] = ctx[0x7c]; ctx[0x7c] = ctx->cur`; `DAT_01244ff4 = DAT_01244ff8; DAT_01244ff8 = DAT_01244e10`)
  and writes `put` (`0x00065af8`); or
* **async path** (`bVar20`: flags `&0x10`, flip frame, `DAT_01245e60 != 0`): the main thread skips the wait and
  signals the Async Kick thread, which makes the same call. `DAT_01245e60` has **no static writer** (one read at
  `0x00067498`), so it is 0 in the shipped config. The next `Gfx_WaitFrameSubmit(...,1)` (`0x00065b9c`) waits for it.

`Gfx_WaitPrevFrameRef` (`0x000655cc`), in order:

1. `tok = DAT_01245058[DAT_011203d4 & 3]` (the token of the **last kicked** frame; `d4` is still N-1 when frame N
   enters `FUN_00065760`).
2. `while (ctrl->ref != tok) Cond_Wait(lwcond 0x1245090, 9000us)` - **equality**, bounded by a 5555-iteration counter.
3. `FUN_00060904(DAT_011203d0, ctx[0x7c] - 4)` and `FUN_00060904(&DAT_01244e08, DAT_01244ff8 - 4)` - **the only
   callers of `FUN_00060904`**. It moves the "free limit" (`ctx[1]` / `ctx[0x7d]`) of the main ring (struct
   `0x01246390`, `0x280000` bytes) and of the tail ring (`0x01244e08`, `0x4000` bytes) up to the end of the frame the
   RSX just finished. This is the only place where pushbuffer space is returned to the writer.
4. Clears every `DAT_01245020[i*0x10]` (the display-flip token table) that equals `tok`, then `DAT_01245058[..] = 0`.

The pushbuffer buffer-full callback (`OPD 0x010B2CF8` -> `0x00068ec0`) **also calls `Gfx_WaitPrevFrameRef`**, then
rewinds the main ring to its start (`DAT_01246398 = DAT_01246580`). So the gate function has two jobs: frame
throttle and ring-space reclaim. Both assume "the last kicked frame is fully consumed".

### 1.2 The equality compare appears at six sites

Readers of `*(DAT_01123b00 + 8)` (xrefs): `Gfx_WaitPrevFrameRef` `0x00065648`; `Gfx_WaitFrameRef` `0x00063c18` and
`0x00063cd0`; `Gfx_DisplayFlip` `0x0006939c` (`entry == ref`, break when equal, and the table entry is cleared by
step 4 above); `Gfx_FrameEnd` `0x0006752c` (decides sync vs async kick: `ref == tok(d4)`) and `0x0006761c`
(`while (ref != iVar12) usleep(200)`).

All of them are correct today **because the RSX cannot get past the last kicked token** (frame N+1 is not visible to
it until the gate has passed). With two frames in flight the RSX can sit on token N or N+1 while a waiter wants
N-1: the equality is never true, and the wait runs until the 5555-iteration cap (~50 s). Every site needs a signed
`(int)(ref - tok) >= 0` test (tokens come from one monotonic counter `DAT_01246044`, so the difference is small).
Step 4 (clearing `DAT_01245020`) is the guard that keeps `Gfx_DisplayFlip` from hanging in the current design; it
only clears the entry equal to the waited token, so under N-2 the flip of frame N-1 is cleared one kick late unless
`Gfx_DisplayFlip` also uses `>=`.

### 1.3 Other per-frame state the gate sequences

| state | where | copies / index | what an N-2 gate breaks |
|---|---|---|---|
| ref token array | `DAT_01245058` | 4, `frame & 3` | nothing: 4 >= 3 frames. Slots must not be zeroed early (step 4 zeroes only the waited slot, fine). |
| flip token table | `DAT_01245020`, 0x10 stride | 4, `flipSubmitted & 3` | nothing structurally; needs the `>=` fix above. Flip queue depth is already 3 (`DAT_01120410 = 2`). |
| display buffers | `param_1+0x4b8 ^= 1` in `FUN_000638c0` | **2** (VRAM `0x10000`, `0x890000`) | hazard on real hardware (render into the buffer being scanned out). **Not a hazard for us**: the walker executes the stream in order and presents by blit at the flip word. |
| main pushbuffer | ring `0x01246390`, `0x280000` | 1 ring; limit advanced by `FUN_00060904` | needs the limit to point at the end of N-2, not N-1, i.e. `ctx[0x7b]` and `DAT_01244ff4` (the shifted copies kept by `FUN_00065760`). **Capacity**: the ring then holds N-1, N and the frame being built = 3 frames. One frame is 0.44 MB (`meter_frame`) to ~0.5 MB (`walker_frame`, 668 draws); a crowded 1,450-draw frame is GUESS ~1.0-1.2 MB. 3 x 1.2 > 2.5 MB: the buffer-full callback fires (safe, it waits and rewinds, but it stalls the PPU on a full drain). **Unmeasured.** |
| tail ring | `0x01244e08`, `0x4000` | 1 ring, same limit pair | 16 KB holds ~1 frame of jumps/tokens/flip words (each frame is `~0x100` bytes GUESS). Fine, but must use `DAT_01244ff4`. |
| mesh job lists | "MeshJobs0/1", `param_1+0x500 + idx*0x30 + 0x378`, `idx = 1 - (frame & 1)` (`FUN_0006f51c`, called from `FUN_000638c0`) | **2** | SPU-side only. List N+2 is kicked after list N completed (`Mesh_WaitPrevJobList`, `FUN_0006f998`). Safe under N-2 as long as the PPU still waits for list completion before reusing the idx. |
| per-parity mesh work area | `param_1+0x180/0x184/0x188` (`FUN_0006f0f8`, halves by parity) | **2** | PPU/SPU only. Same argument as the job lists. |
| **mesh vertex rings** | five rings, EA `0x11900000`..`0x122fffff` (2 MB each), state `0x1246680 + 0x200 + 16*i` = `{base, end, alloc, label addr}`; labels 72..76 (`0x480..0x4c0`) | **1 per lane, reset to base every frame** (`Mesh_SetRingStartsAndKick` mode 0; bases are written once in `FUN_0006f0f8` and never rotated) | **the hard one**, §2. |
| GPU timing/report buffers | `DAT_011204f8`, `0x1a20` bytes, `(frame+1) % 3` | 3 | read two frames later; fine for N-2 (done two frames ago). |
| frame-end label reset | `FUN_0006f838` (`0x41d6c`/`0x41d74` pairs) emitted at the start of `Gfx_FrameEnd` | in-stream | resets each ring's label to its base **when the RSX reaches it**. |
| dynamic PPU geometry / constants / UI textures | not enumerated (`src_main` draws read IO memory) | unknown (GUESS: parity or ring) | **the unknown**, §3. |

## 2. The mesh rings are the single-buffered state

From `Mesh_SetRingStartsAndKick` (`0x0006f6b0`) and `FUN_0006f0f8` (init):

* mode 0 (the five-ring mode, `DAT_0124ad80` lanes): per ring `base = *(mesh + 0x18 + 4i)`; `alloc = base`;
  `end = base + size`; `label = &labels[i]`. Written every frame, always from the same base.
* modes 1 and 2 exist: mode 1 is one ring, **mode 2 is two halves by frame parity** (`+0x18`/`+0x1c`, selected by
  `+0x438`). The game was built with a parity-split option, but the shipped PS3 path runs mode 0 (UNVERIFIED: the mode
  argument is passed from `FUN_00067734` at `0x00068480`; I did not read its source).

The SPU job's ring-space wait (lifted c690 module `jobmod_c690c2f7b09a_eF5B8`, `0xE4D0`-`0xE5C0`, DMA-read of the
label at `0xE510`) is, from `progress.md` (2026-10-27) and the `clgt` ladder in the lifted code, "retry while the
GPU's read position (the label) is inside the range I want to overwrite". That is a correct circular-ring free check
for one frame behind. With the ring reset to base every frame it is **not** enough for two:

```
RSX reading N (label L inside N's data), N+1 fully produced in [base, P1) with P1 <= L,
PPU sets up N+2 (reset to base): job wants [base, y), y <= L  -> label not inside range -> proceeds
-> overwrites N+1's unread vertices.
```

N+1's data lies *behind* the reader's N position only because both frames start at the same base. This is exactly the
kind of failure the earlier `(id+1)&3` pipeline experiment hit (`ppu-critical-path.md` §5: "corrupted geometry").
Today the N-1 gate makes it impossible: setup(N+2) runs after kick(N+1), which needs N complete.

So **N-2 needs three parity sets of the mesh rings** (3 x 10 MB IO, or 3 x 2 MB / lane split), 15 more labels
(`0x4d0..`), the walker's snapshot/binding ranges (`0x11900000-0x122fffff` is hard-coded in the runtime's c690 GPU/2A
and walker-MT code, GUESS: grep needed) and a way to choose the set per frame. A one-frame improvement of the rings
alone (two sets, keep the N-1 gate) is safe and is the §6 option B.

## 3. What is not enumerated (and why that matters)

The bink-video §8 lesson is: if the guest writes memory every frame and the walker reads it a frame late, the walker
sees the new bytes. That is true **today** (lag <= 1 frame) for anything not double-buffered; the double buffers in
the table (`& 1`, mesh job lists, display buffers) were sized for lag <= 1. With lag 2 every `& 1` resource that the
RSX reads is a hazard: frame N+2 reuses N's copy while the RSX may still be on N.

I could not enumerate the RSX-read, PPU/SPU-written per-frame data statically within reasonable effort:
dynamic vertex/index buffers for the PPU ("src_main") draws, constant uploads (these are inline in the pushbuffer,
fine), streamed/dynamic textures (UI, crowd/ad boards, face generation), cube probes (RSX-written). Only the
`Asset_Release` in-flight counter (`owner+0x14 & 3`, a 2-bit count) hints the engine tracks up to 3 frames of asset
lifetime (GUESS). **This is the part that cannot be proven safe offline.** It can be found empirically with a hazard
detector (§6), not by reading code.

## 4. The minimal patch set if N-2 is built anyway

Guest (generated-code patch script like `tools/patch_bink_ready.py`, inert unless `PS3RECOMP_FRAMES_IN_FLIGHT=2`):

1. `0x00065798` in `FUN_00065760`: call a new `WaitPrevFrameRef2` that waits for the token of the frame **before** the
   last kicked one (`DAT_01245058[(d4 - 1) & 3]`), using a signed compare, zeroes that slot, clears matching
   `DAT_01245020` entries, and calls `FUN_00060904(ctx, ctx[0x7b] - 4)` and `FUN_00060904(tail, DAT_01244ff4 - 4)`.
   The original `Gfx_WaitPrevFrameRef` stays for the buffer-full callback (it still needs "ring drained" before it
   rewinds).
2. Signed compares at `Gfx_DisplayFlip` `0x0006939c`, `Gfx_WaitFrameRef` `0x00063c18/0x00063cd0`,
   `Gfx_FrameEnd` `0x0006752c/0x0006761c`.
3. Three parity sets for the mesh rings (and their labels) in `Mesh_SetRingStartsAndKick`, `FUN_0006f0f8` (sizes) and
   `FUN_0006f838` (label reset values); IO space for 20 MB more.
4. Whatever §3 finds.

Runtime (`ps3recomp-depth`):

* publishing `ref` stays as is (end of pass + poll publisher, `cellGcmSys.c:2322/2493/2511`), but the walker will now
  routinely hold 2 frames of backlog: the "one flip per drain unless `gcm_fifo_backlog >= 0x10000`" rule
  (`cellGcmSys.c:3344`) decides how much of the queue is drained per tick; the present/frame clock then paces on the
  backlog, which needs a look (`present-stalls.md`, `flip-deadline.md`: the display tick shares the walker thread).
* `VK_EB_SETS = 3` (`rsx_vulkan_backend.c:199`) is already a 3-deep frame pipeline: no change, but `ebs_swap` waits
  become the pacing point if the GPU is the slow part.
* WCB / texture-dirty: render targets read back by the guest are protected by `eng_mt_sync` and the page-seq
  hooks, which are stream-order based and independent of the gate. The walker lagging two frames instead of one means
  the guest CPU can read a WCB target two frames late: any guest reader of a render target (cube probes are GPU-only;
  face generation, shadow/depth readback via `eng_zdepth_store`) gets data one frame older. GUESS: harmless; test.

## 5. Expected gain, from the existing timelines

Let `W` be the walker drain, `B` the c690 burst, `P` the PPU build, `k` the start-of-frame put-wait.

* **Crowded replay** (`c690-serialisation.md`): the Edge batch is ~constant, ends +24-32 ms into the frame in every
  band, the walker then drains 62 ms of an 86 ms frame; `drain + put_wait = 100 %`, put-wait 9.8-15.6 ms **at the
  start** of the frame, drain 78-83 %. If the whole put-wait disappears: 86 -> ~70-76 ms, **+13-22 % fps**.
  The user's latest per-thread schedstat (walker on-CPU 50 %) implies a larger idle share than that older data
  (walker diet has cut the drain since); if that idle is all put-wait, the ceiling is up to 2x, but it is unattributed.
  `thread_sched.py` says nobody is saturated; it does not say *what each idle walker is waiting for*. The attribution
  tool (`analyse_walker_wait.py` a/d/e2/u, and `[walker-prof] wait=`) must be run on a crowded trace before building.
* **Pitch meter**: the frame is the c690 burst chain (`frame-timeline-2.md`: p50 34 ms ~ burst 36 ms wall; walker 19 ms
  is hidden). N-2 overlaps the PPU wake/tail (~4-6 ms) but not the burst: expected **0 to +10 %**. The 17/34 ms tick
  cliff (`live-ab-and-ceilings.md`, `flip-deadline.md`) can hide even that.

The roundtable already ranked this lever fifth for the same reason ("only after the producer is under ~20 ms").

## 6. Alternatives and the recommended next step

| option | what | safety | gain | cost |
|---|---|---|---|---|
| A. N-2 gate (§4) | guest patches, 3 mesh ring sets, hazard hunt | **unproven**; §3 unknown | §5 | high (patching generated TUs, ~2 GB each, 10+ MB IO) |
| B. Two parity sets of the mesh rings, keep N-1 | decouples c690 (N+1) from the reader of N; the game's own mode 2 is a parity split | safe by the same argument as the current gate (setup(N+1) runs after N-1 is done) | removes any reader-chasing stall of the burst (**not measured to exist**: the Edge batch ends +24-32 ms regardless of the walker length, `c690-serialisation.md`, which argues the throttle is not binding) | medium |
| C. Runtime-side: pipelined walker (parse + snapshot -> publish ref -> record on another thread) | the guest sees `ref` early because the commands and bulk data were copied; no guest patch | needs packet-owned snapshots of mesh rings, textures, pushbuffer; and the **holes** (SPU-patched self-JUMPs) can only be read live | same as A but guest-transparent | very high; this is `walker-multithread-design.md` (a), already rated high risk |
| D. Do nothing until the walker wait is attributed | 1 crowded trace with the existing tools | n/a | decides everything | small |

**Recommendation: D first.** One crowded live run (user-driven) with `PS3RECOMP_WALKER_WAITS=1 WALKER_PROF=1` and the
trace tools gives the split of the walker's idle: (a) c690 not ready / (d) running, (e2) resume cadence, (u) waiting for
the PPU's park patch, and PPU time between "token seen" and "put written". If `u + e2` (the kick path) is the majority,
A or C buys that much; if `a + d` (producer) dominates, B/C do not help and the lever is the c690 burst (roundtable item 2).
Two cheap diagnostics (a few lines each, behind env keys) would close the §2-§3 unknowns without any guest patch:

1. at each flip, log `{alloc - base}` for the five rings (guest read of `0x1246680 + 0x208 + 16*i`) and the main ring
   write head (`0x01246398`) minus the previous: **per-frame mesh and pushbuffer bytes**, which sizes A's ring needs;
2. count entries to the lifted c690 label-wait loop (`0xE530/0xE550`) with the existing `c690_stats_on_lookup` hook:
   **does the producer ever wait for ring space at all**.

## 7. Phase 2

Not started. Reason (per the mission rule "stop if the guest has hard single-buffered state"): the mesh rings are
single-buffered and reset every frame, the safe version of N-2 needs three parity sets plus six compare patches and an
unenumerated hazard hunt, and the payoff is unmeasured (§5). If the user wants A anyway, the order is: diagnostics 1-2
above (one run), then the signed-compare + limit patches behind `PS3RECOMP_FRAMES_IN_FLIGHT=2` as a **gain probe with
mesh corruption tolerated** (a single pitch run tells whether the overlap exists), then the ring sets.

## 8. Ghidra names worth keeping

| EA | name |
|---|---|
| `0x00060904` | `Gfx_RingAdvanceFreeLimit` (only caller: `Gfx_WaitPrevFrameRef`) |
| `0x00068ec0` | `Gfx_RingFullCallback` (OPD `0x010B2CF8`) |
| `0x0006f6b0` | `Mesh_SetRingStartsAndKick` |
| `0x0006f51c` | `Mesh_SelectParity` (job list idx, per-parity work area) |
| `0x0006f838` | `Mesh_EmitLabelReset` |
| `0x0006f0f8` | `Mesh_Init` (modes 0/1/2) |
| `0x00070694` | `Gfx_FrameBegin_WaitWorkersAndMesh` |
| `0x000638c0` | `Gfx_PostKickSetup` (parity flips, display buffer toggle) |

(Not applied in the Ghidra project; this mission did not rename anything.)
