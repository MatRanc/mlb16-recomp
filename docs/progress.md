# Progress log

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26).

## 2026-10-28 (early morning): job lists on several SPUs, ~6 → ~12-13.6 fps

With batching on, the frame trace (SPURS workers now record busy spans) showed one SPU busy nonstop for ~180 ms of a
~208 ms frame while the others idled. The WWS job manager's two main workloads ran on one SPU at a time, by turns.

**The cause was a missing API, not the scheduler's speed.** MLB calls `cellSpursRequestIdleSpu(spurs, wid, 7)` for
every workload. The SPURS kernel lets an SPU take a workload while its contention is below ready count + that idle
count; WWS uses it to spread one job list over several SPUs, each grabbing the next job. Our runtime's version was a
stub that kept nothing, so with a ready count of 1 every list got exactly one SPU.

The inFAMOUS 2 port's notes and SPURS kernel had the answer (it is the same job manager). Now implemented on branch
`mlb16-work`: the job lists run on three or four SPUs at once.

| | fps in play (30-210 s after the first pitch) |
|---|---|
| Before (batching on) | 5.4-8.0 over six runs |
| With idle SPUs honoured | 11.93, 13.59 |

**Also learned:** vertex decode is not where the RSX walker's time goes. SIMD decode is 2× faster in isolation and
bit-exact, but the walker's CPU per draw didn't move (60.4 vs 60.5 µs), and the walker sleeps most of the frame.

**Next:** the inFAMOUS 2 port's other SPU work, ported where MLB needs it:
- the lifter keeping SPU registers in locals (`--local-regs`), `--merge-chunks` and `--const-bi`;
- SIMD forms of the integer SPU helpers;
- cheaper DMA fences.

## 2026-10-28 (overnight): the frame is a waiting chain; draw batching +20%

**Correction to the entry below.** The CPU-vs-CPU game plays out differently every run, so it wasn't the same
at-bat each time, and fps swings ±20% with what's on screen. Changes are now judged by alternating A/B runs: a
closed-loop menu driver that recognises each screen, fps over 30-210 s after the first pitch, one game at a time.

**Where the time goes**, now measured on the running game:
- Sampled with gdb, plus the runtime's `PS3_FTRACE` timeline (SPURS workers now record busy spans).
- **Nothing is CPU-bound.** The process uses about 18% of the CPU. One SPU worker is ~60% busy, the RSX walker about
  half, and the main thread ~10%.
- **The frame is a chain of waits:**
  - The main thread waits (Gfx_FrameEnd) for the RSX walker to finish the previous frame.
  - The walker spent ~45% of its time waiting for the GPU after each draw.
  - The SPU mesh jobs wait on the walker for ring space, which shows as bursts of ~8 ms on, 8 ms off.
- **RPCS3 on the same PC** holds about 43 fps in the same scene, spreading SPU work over six threads.
- The SPU compute per frame is similar between the two, so faster SPU code (a lifter flag `--ib-known-targets`
  that turns the hot loops' computed branches into direct ones) gave no measurable gain.

**Draw batching.** `VK_DRAW_BATCH=1`, ps3recomp's opt-in mode written for God of War: Ascension, records 100-200
draws per GPU submission instead of waiting after each one:

| | fps (runs) |
|---|---|
| Batching off | 5.41, 5.53 (4.50, 5.00, 6.24 before the GPU clock lock) |
| Batching on | 6.25, 7.96 (6.01, 6.17 before) |

It is now on in `tools/run.sh`.

**The GPU sat in its idle clock** (210-270 MHz on a 3060 Ti). The bursty submit-and-wait pattern never ramps it up,
and PowerMizer "prefer maximum performance" didn't stick. `nvidia-smi -lgc 1400,1980` (root; reset with `-rgc`) pins
it for testing.

**Smaller fixes** on branch `mlb16-work`:
- Vertex fetch skips duplicate-removal hashing for sequential draws and fills attributes that have no array.
- `VK_EB_STATS=1` prints why GPU batches end.

**Next:**
- Asynchronous GPU submission, so the walker records while the GPU executes. The first step publishes RSX labels only
  after their GPU work completes; today the game can see a label before the work behind it ran.
- Then SIMD vertex decode and a cache for static geometry; the walker re-decodes every vertex every draw.
- Then why the SPU rings wait on the walker.

## 2026-10-27 (end of day): SPU speed, 3.4 → 6.4 fps

Measured as the average flip rate over 60 s of play (same at-bat each time, `GCM_FLIPCOUNT=1`).

| Change | fps |
|---|---|
| Start (exact SPU float, scalar local-store access) | 3.4 |
| `PS3RECOMP_SPU_FAST_FLOAT=ON` | 4.6 |
| + `PS3RECOMP_SPU_FAST_LS=ON` | 6.4 |

Both are CMake options on branch `mlb16-work` that affect only the lifted SPU sources (configure with
`-DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON`):

- **Fast float:** ps3recomp's x86 path emulates SPU single precision bit-exactly: each op in f64x4 with lane checks
  (~30 instructions), plus a scalar fix-up for tiny/out-of-range lanes that the vertex code hits often. The option
  uses native SSE/FMA3, keeps the SPU's denormal flush, and differs only in rounding (nearest vs truncate) and
  overflow (infinity vs saturate). It is within 1 ulp on 800k random lanes and ~6× faster per fma, like an
  emulator's default "approximate" SPU float mode.
- **Fast local store:** each 16-byte local-store access was four byte-swapped scalar moves plus a diagnostics-flag test
  and cold call. Now it's one vector load/store and a `pshufb`, with no diagnostics in lifted code. The hottest mesh
  block went from 827 to 478 x86 instructions.

Where the time goes now: one SPU worker at ~86% running the mesh jobs (the game runs that list on one SPU at a time,
as on hardware), a second at ~50%, the RSX walker thread at ~40% (CPU vertex conversion).

**Next lever, register caching (prototype in `tools/spu_regcache.py`):** the lifted code keeps SPU registers in
`ctx->gpr[]`, and every local-store store goes through a char pointer that may alias it, so the compiler reloads
registers after each store. The prototype rewrites each lifted function to use C locals (load at entry, write back
before exits/calls/channel ops, reload after calls).
- Applied to all 639 functions of the mesh job module, the job went wrong (an SPU return to LS `0x22`). The likely
  culprit is the interrupt path, which snapshots/restores `ctx->gpr` and can unwind host frames.
- Applied only to the 402 functions without calls or channel ops, the game plays correctly but no faster (6.0 fps).
  The hot loop (`0x7c18`) contains a call, so it wasn't covered.
- Next: make calls inside hot blocks safe for caching (or move the caching into the lifter itself, with interrupt
  points as explicit write-back sites), then measure again.

Also open: render-target writeback ("Write Color Buffers") for the dark players and crowd sprites, the RSX walker's CPU
vertex conversion, and the PPU side (same `ctx->gpr` pattern).

## 2026-10-27 (later): magenta fixed; where it stands against RPCS3

- **Magenta surfaces:** a RenderDoc capture of our own Vulkan output (layer mode,
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` with `SDL_VIDEODRIVER=x11`, since the layer has no Wayland support) showed the
  player shader sampling texture unit 5, which the game leaves disabled. Our backend binds a 1×1 magenta placeholder
  to such units, and the shader adds it as a tint. `VK_DUMMY_BLACK=1` (branch `mlb16-work`, set in `tools/run.sh`)
  makes the placeholder transparent black. Dirt, crowd, field and walls now look right.
- **Players and crowd sprites are dark/blank:** the same problem as RPCS3 without "Write Color Buffers". MLB 16 builds
  some textures by rendering them and reading the render target back from the CPU/SPU (the generated faces in
  `HeadFaceCacheFile`, the crowd sprite sheets). Our renderer doesn't write render targets back to guest memory, so
  those reads see black. Next to fix. Then delete `HeadFaceCacheFile`, which was built blank.
- **Speed: about 3.3 fps in a game** (flip counter, no RenderDoc). The two busiest threads are SPU workers running the
  mesh jobs (`jobmod_c690c2f7b09a`, skinning/geometry); the PPU main thread is at ~30% and the GPU is nearly idle.
  So the lifted SPU code is the bottleneck: one real SPU runs these jobs well within a frame, our translation is many
  times slower.
- **How this differs from an emulator, honestly:** the CPU side (PPU and SPU) is compiled ahead of time, but the GPU
  side is still an emulated RSX (command walker, translated shaders, texture formats), the same job RPCS3 does. And
  ps3recomp's RSX layer is much younger than RPCS3's, so graphics bugs RPCS3 fixed years ago show up here. The
  speed advantage can only appear once the SPU code generation is fast. Right now it's the opposite: RPCS3's SPU JIT
  (LLVM, with years of tuning) beats ps3recomp's straightforward SPU-to-C translation by a wide margin.

## 2026-10-27 (late night): in the game, pitches being thrown

**The match-load deadlock is fixed and the game plays:** the dugout, the pre-game intro, Kauffman Stadium, Volquez
pitching to Curtis Granderson, with the count and the score bug updating. Geometry is correct. Many surfaces
(uniforms, skin, infield dirt, part of the crowd) come out solid magenta, a renderer problem still to look at.

The cause was ps3recomp's SPURS scheduling, in three parts:

1. **Max contention dropped.** `_cellSpursWorkloadAttributeInitialize` takes `maxContention` as its ninth argument,
   on the guest stack, so the generic adapter never saw it and stored 1. The mesh lists ask for 5 and the DXT list
   for 4. Fixed with a context handler that reads the stack (branch `spurs-workload-maxcontention`).
2. **The ready count was consumed.** The scheduler decremented a workload's ready count each time it gave it an SPU.
   In the SDK the ready count is a level: the kernel runs up to `min(ready, maxContention)` SPUs and the workload
   lowers it itself. The WWS policy module does exactly that, setting it to 0 when its list is empty and back to 1
   when it sees more work. Consumed, a kick could be used up by a dispatch that found no jobs yet, and nothing
   dispatched the list again. `SPURS_READY_LEVEL=1` keeps the level.
3. **Idle probes ran workloads nobody had kicked.** To cover for (2), the scheduler periodically dispatched idle
   workloads. One such probe ran mesh jobs that the command-building threads had already queued, *before* the PPU
   reset the mesh rings and kicked the list. The reset then threw away the ring space those jobs had used, the next
   job was given the same space again, and it waited forever for the GPU to free it. `SPURS_NO_IDLE_PROBE=1`
   schedules only kicked workloads, as real SPURS does.

(2) and (3) are opt-in switches on branch `mlb16-work` for now, and `tools/run.sh` sets them. They change SPURS
scheduling for every title, so they need testing on other ps3recomp games before going upstream.

How it was found: a temporary override that forced the stuck wait through after 2 s got the game into the match
(with garbled geometry), which proved the wait was the only blocker. Then gdb watchpoints on the ring allocation
pointers and on the ready-count bytes showed the order of events.

## 2026-10-27 (night): how the mesh rings work, and where the deadlock really is

The stall at the pitching matchup isn't the game waiting for a pick (X on the matchup, START on the lineups):
flips stop and the main thread never leaves the job-list wait, so pad input is never read. Pressing X there changes
nothing.

How MLB 16 feeds geometry to the GPU (worked out from walker traces, gdb watchpoints and Ghidra):

- **Five mesh rings** of 2 MB each, holding **vertex/index data, not commands**: EA `0x11900000`, `0x11b00000`,
  `0x11d00000`, `0x11f00000` and `0x12100000` (IO `0x00900000`–`0x012fffff`), with RSX labels 72–76
  (`0x480`–`0x4c0`). Their state lives in the render object at `0x1246680`: `+0x200` holds
  `{base, end, alloc pointer, label address}` per ring (one 128-byte line).
- **Per-mesh slots in the command stream.** The PPU leaves a 0x90-byte slot per mesh job in its big command ring
  (IO `0x013xxxxx`–`0x014xxxxx`): a self-JUMP plus `0x40880100` (a non-incrementing NOP skipping 34 words). The job
  fills it by DMA when it finishes: vertex/index array offsets into its ring, the draw, and a release of its ring's
  label with the segment's end address. The GPU stops at the first unfilled slot, so it consumes ring data as jobs
  complete.
- **Frame setup** (`0x70694` → `0x6ed24` → `0x6f6b0`): wait for the command-building worker threads (`0x70894`),
  wait for the previous mesh job list (`0x9fb898`, list "MeshJobs0"), then set each ring's start for the new frame
  and kick the next list. Frame end is `0x66530` (links the frame into the libgcm ring, flips, paces on the
  frame counters).
- **Each ring has one SPU writer.** Watchpoints on the alloc pointers show ring 72 only written by one SPU worker,
  ring 75 by another, ring 76 by a third. Plus the PPU's frame setup. No lost updates.

The deadlock: the GPU parks at the first unfilled slot. That slot's job waits for ring space (`jobmod_c690c2f7b09a`,
`0xe4d0`–`0xe5c0`): it compares the ring's label (the GPU's read position) against the range it wants and retries
forever. In every run the waiting job wants a range the GPU hasn't released yet, often right after wrapping its ring.
Open question: why it wraps so early. In one run, ring 76's writer wrapped at `0x12137d00`, 0x37D00 bytes into a 2 MB
ring. The frame setup also once set ring 76's start to `0x12105180` rather than the base. Next: trace the per-frame
start/end values `0x6f6b0` hands out (`+0x18`, `+0x58`, and the single-ring branch at `+0x280`/`+0x290`) and how the
job derives its ring end. An RPCS3 memory dump of the same structure at the same screen would settle what "normal"
looks like.

Side findings:
- The walker's 50 ms park recovery is no longer needed for this game; running with it effectively off
  (`GCM_PARK_GRACE_MS=100000000`) still boots, and parks are always patched within 4–47 ms (`[parkfix]` log).
- `HeadFaceCacheFile` is the generated-face cache from the RPCS3 white-faces fix. Faces built natively before RSX
  render-target readback works may be cached blank; delete it once a match loads.
- Temporary diagnostics on branch `mlb16-work` (not for upstream), all under `MLB_SEMA_TRACE=1`: label releases with
  stream position, CALL/JUMP into the rings, park patches, and SPU PUTs into the command ring (`[ringput]`).

## 2026-10-27 (evening): Bink spin fixed, match load reaches the pitching matchup

- **`cellFsFtruncate` never worked:** it resolved to `libs/filesystem/cellFs.c`, which looks fds up in its own table,
  one that the port's `cellFsOpen` (`runtime/ppu/ppu_fs.cpp`) never fills, so every call returned EBADF.
  - The game sizes `HeadFaceCacheFile` (in the install folder) to 314,584,064 bytes before a match, and
    `ArenaLogoCache` to 9 MB. Implemented next to `cellFsOpen` (branch `cellfs-ftruncate`).
  - With it, the match load gets past the starting lineups to the **pitching matchup** screen.
- **The Bink SPU spin (99% of a core) is fixed:**
  - The task retried `cellSpursEventFlagWait` forever, because each call failed at once with `0x80410911` (null
    pointer): its event flag address was 0.
  - The task DMAs its setup block from r4, the taskset argument. `_cellSpursTasksetAttributeInitialize` dropped
    `argTaskset` and `cellSpursCreateTasksetWithAttribute` created the taskset with args 0.
  - Carrying it through (branch `spurs-taskset-args`) gives r4 = `0x01476E80`; the Bink thread now sleeps in its
    wait like on hardware. That's a core back.
- **The match-load stall is a real deadlock**, not a slow first cache build: with the SPU watchdog at 600 s it never
  moves. Three parties wait on each other:
  - **PPU:** the main thread spins in `0x9fb898`, the WWS job manager's wait for job list "MeshJobs0". Two SPU lanes'
    progress counters are stuck.
  - **SPU:** two mesh jobs (`jobmod_c690c2f7b09a`, loop at `0xe510`–`0xe550`) wait for ring space. Each DMA-reads an
    RSX label holding the GPU's read position in its ring (labels 72, 75 and 76; rings at EA `0x11900000`,
    `0x11f00000` and `0x12100000`). It retries while that position is inside the region it wants to overwrite.
  - **GPU:** our command walker has drained everything up to a self-JUMP "park" at IO `0x0138ef10`. The GPU waits
    there for the PPU to write more; the PPU waits for the jobs.
  - The only label release in that frame is label 75 = `0x11f02080`, inside the region its job wants. So that ring's
    previous output was never marked as read by the GPU.
- **Walker oddities found on the way** (not yet shown to cause the deadlock):
  - The game extends its big ring by leaving a self-JUMP at the write head. After 50 ms (`GCM_PARK_GRACE_MS`) the
    walker gives up on the park and jumps to `put`; a real RSX waits.
    - With the grace effectively infinite the game still boots and gets to the same stall.
    - The selected team's **logo now shows on team select**. It was probably in commands the early jump skipped.
  - At about 2.5 minutes in, the walker reads data as commands at IO `0x2d4e80`. The frame `CALL`s a vertex-program
    upload at `0x2d4ef0`; the walker was 0x70 bytes before it.
    - It reads an upload argument (`0x709d302a`) as a CALL to an unmapped address, and drops the rest of that pass.
    - Possibly a block rewritten while the walker, which drains on 4 ms ticks, was still behind it.
  - New-style CALL targets are masked with the old JUMP mask (`0x1ffffffc`) and new-style JUMPs (`w & 3 == 1`)
    aren't decoded at all. This game seems to use only old-style JUMPs and new-style CALLs into low IO, so neither
    has bitten here yet.
- **Ghidra:** the recompilation addresses are now named in the project (`Gfx_DisplayFlip`, `Gfx_FlipHandler`,
  `Mutex_LockOwned`/`Mutex_UnlockOwned`, `Cond_Wait`, `File_Duplicate`, `File_Open`, `File_AioCallback`,
  `MemPool_Acquire`, `MemPool_SharedMemContAnim_LazyInit`, `MemContainer_Carve`).
- **Next:** find which commands should advance labels 72/75/76 and why the walker never reaches them: trace every
  `0x1d6c`/`0x1d74` release value per frame, and the `CALL`s into the mesh rings (IO `0x0090xxxx`, `0x00f0xxxx`,
  `0x0110xxxx`).

## 2026-10-27 (afternoon): driving it headless; match load gets to the lineups

- **Scripted input:** `PAD_FILE=<file>` presses whatever button mask is written to the file (`0x4000` CROSS,
  `0x0008` START, `0x0040` DOWN; `"0x0040 3"` holds it for 3 reports). With a frame dump every 10 frames, the menus
  can be driven without a person: title → START → "Online disabled" OK → Play Now → Offline Rosters → team select →
  stadium select (a still image of Kauffman) → game options → starting lineups.
- **`-O2 -march=native` build** (`recomp-work/build-o2`, 16.5 min at `-j4`): same menu frame rate as `-O1`, 421
  frames in 20 s each (about 21 fps). The menus are paced at one frame per three 60 Hz vblanks, so they don't show CPU
  speed.
- **"3D TV detected":** `cellVideoOutGetResolutionAvailability` reported the 3D modes (ids `0x80`+) available, so
  the game asked to enable 3D and, on yes, rendered over-under. Fixed on branch `cellvideoout-no-3d` (PR draft 5).
- **The match-load NULL call** came from the game's memory containers:
  - `0x14fc4` carves an N MB container out of the default one (id `0xFFFFFFFF` at `0x0111ffc4`): get its size,
    destroy it, recreate it smaller, create its own.
  - ps3recomp's `sys_memory_container_get_size` rejected the default id, took its arguments in the wrong order and
    wrote `{total, used}` instead of `{total, available}`. `sys_memory_allocate_from_container` (syscall 350) wasn't
    registered.
  - So the "SharedMemContAnim" pool at `0x0134a464` (built lazily in `0x99c45c`) never got memory, and `0x8c5508`
    called a method on a NULL buffer.
  - Fixed on branch `sys-memory-containers` (PR draft 4). Both pools now allocate (25 MB in container 1, 7 MB in
    container 3), and the NULL call is gone.
- **Next stall:** about 30 s into the lineups screen, an SPU job on the job manager (local store `0xe510`) re-reads
  RSX label 76 (`0x200004c0`; labels start at `0x20000000`) until the runtime halts it. Something that should write
  that label, probably a semaphore release in the GPU command stream, isn't writing it.

- **Label 76, first look:** a temporary trace in our runtime branch (`MLB_SEMA_TRACE=1`, branch `mlb16-work`) shows
  the GPU command stream *does* release it. The game cycles through five labels, `0x480`, `0x490`, `0x4a0`, `0x4b0`
  and `0x4c0`, with `NV4097_SET_SEMAPHORE_OFFSET` (`0x1d6c`) and texture-read releases (`0x1d74`), on subchannel 0,
  with no semaphore context DMA set (so they land in the label window). The waiting job is
  `jobmod_c690c2f7b09a` (a geometry ring producer: DMA-reads the label at `0xe510`, compares it against its ring
  bounds at `0xe550`).
  - Next: compare the values released against what the job waits for. Candidates: byte order (`0x1d74` is written
    verbatim, `0x1d70` byte-swapped), the walker running behind the job's own output, or releases in
    SPU-built command blocks.
  - Driving there takes about 4 minutes with the `PAD_FILE` script above.

## 2026-10-27 (evening, playtest): menus playable, match load crawls

Played with a PS4 controller through SDL, in a window:
- **Menus work.** Experience level, main menu and moving between screens, at about 20 fps (109 new frames in about
  5 s, read from the frame counter at `0x011203d8`).
- **Button presses sometimes need two taps.** `PAD_ALWAYS_CHANGE=1` (every read returns a full report) didn't fix it.
  The controller is sampled every 4 ms, so short taps aren't the cause.
- **Menu boxes flicker** (only the boxes, not the whole screen): to capture with the renderer's pass dumps.
- **SELECT (Quick Menu) froze it.** This is online code, which also needed our server in RPCS3. Skipped for now;
  offline first.
- **Loading an exhibition match crawled.** The crowd job workload (`CrowdJobList`) started and stadium data loaded,
  then reads dropped to about 17 KB/s with no new frames. The main thread was busy in game code (`0x5499a0`,
  `0x643200`…) while the job module `255e2961` kept dropping into the SPU interpreter.
  - A gdb breakpoint on `spu_interp_run_until` caught 7,293 interpreter entries in 25 s, mostly at `0xd7a0`,
    `0xd9d0`, `0xd810` and `0xd878`: functions reached only through function pointers, so the lift never had them as
    entry points.
  - The 28 addresses are now passed to `lift_jobmods.py` (`LBP_JOBMOD_EXTRA_FUNCS`, in `tools/lift_spu.sh`). The
    job modules gained functions, e.g. `7ac07dde` went from 241 to 630. Test pending.
- **CPU use:** the Bink SPU program spins at 99% of a core in its wait loop even with no movie, and a job manager
  thread runs at about 84%.
- **Match load still stalls with the entry points added** (interpreter entries are now only the job manager's own
  self-modifying code at `0xa68`). At the start of each match load the main thread calls a NULL object:
  - `0x8c5508` writes the game's "FE data block" (`HDDBlk`) to the HDD cache. It takes a buffer from a 3-slot pool
    at `0x0134a464` via `0x98c72c`, which hands one out only if the slot's current owner says it's done (virtual
    `+0xc`).
  - All three were busy, so no buffer came back. The caller doesn't check, and calls a virtual method on 0 (call at
    `0x8c5638`, logged as `[null-call] near lr=0x8C563C`).
  - Next: read the pool's owners (slots `+0x18`, `+0x24`, `+0x30`) at that moment to see what's still holding them.
- `cellFsAioWrite`'s completion callback (`0x79c2c`) only forwards to the request's handler, which the game sets up
  before submitting, so completing synchronously looks safe here.

- The build now lives in `recomp-work/build` (lifted source in `recomp-work/recompiled`), not the session's scratch
  folder.

## 2026-10-27 (later still): first menu

- **The autosave-screen stall** was the game's file copy into its `/dev_hdd1` cache (`0x776d8`, a "Duplicate"
  routine called from the file open at `0x78ca4`). It writes 64 KB chunks with `cellFsAioWrite` and polls a status
  word set by the completion callback (`0x79c2c`). `cellFsAioWrite` (`0x4CEF342E`) wasn't implemented in ps3recomp,
  so nothing was written and the callback never ran. Implemented on our branch `cellfs-aio-write` (upstream PR
  candidate 3), modeled on its `cellFsAioRead`.
- **Result:** about 500 async writes during boot, then the game's first interactive screen, **"Choose your
  experience level" (Veteran / Novice)**, drawn correctly through Vulkan with the team background behind it. This
  is the first-time setup a new save sees before the main menu.
- The Bink thread still spins at 99% of a core; whether the intro movie plays isn't known yet.
- **New unresolved imports, reached for the first time:** `0xB20F87B3`, `0xCE4374F6`, `0xDBF22BD1`, plus
  `cellHttpInitCookie` and `cellSpursAttributeEnableSystemWorkload` from before.

## 2026-10-27 (late night): flip loop fixed; stuck on the autosave notice

- **The 100,000 flips a second** came from the flip handler running twice per flip. `cellGcmSetFlipImmediate` calls
  it at request time (a compatibility "early edge" in ps3recomp) and the pump calls it again at completion. MLB's
  handler advances the displayed-frame counter (`0x011203dc`) each time, so it overtook the rendered-frame counter
  (`0x011203d8`). `DisplayFlip` (`0x692dc`) then saw a frame ready forever and flipped non-stop. Upstream's
  `GCM_FLIPCB_ONTICK=1` drops the early call. With it, about 35 flips a second, and the two counters stay a few
  frames apart.
- **Where it stops now:** the screen stays on the autosave notice (cover art) from about 20 s on, flipping buffer 0
  only, while the game keeps loading in the background. It opens `MLB_INTRO.BIK` early, probably to preload.
  - The main thread polls an event flag with an ~11.6 ms timeout (`0x4c7ae0` → `0x78ca4` → `0x785d8` → `0x776d8`).
  - The Bink SPU program (`BinkSPU.spu.elf`, task created with a SPURS event flag, `direction=2`, SPU to PPU) spins
    in its wait-for-work loop (`0x3870` → `0x3438` → `0x3448`) at 99% of a core.
  - Next: check ps3recomp's SPURS task event flag and queue path between the two (`docs/spurs-task-signal-wake.md`
    upstream).
- **Upstream PR branches** are in the fork (`MatRanc/ps3recomp`: `template-vulkan-backend`, `gcm-handler-thread`),
  with the draft descriptions in `PR_DRAFTS.md` on its `pr-drafts` branch. Nothing is opened yet.

## 2026-10-27 (night): boot freeze fixed, display stops updating

**The freeze was a leaked lock, not Bink.** The game usually hung on its first screens, at random, before ever
opening the intro movie; one early run got through by luck. Stacks of the stuck process (`eu-stack`, function names
are PS3 addresses) and its guest memory (`gdb`, reading `vm_base + address`) showed:
- `DisplayFlip` (thread entry `0x692dc`) flips each frame, then waits on a condition for the flip handler
  (`0x691b0`, registered with `cellGcmSetFlipHandler`) to clear a "flip pending" flag at `0x011203e0`.
- MLB's lock wrappers (`0x9a2da8` lock, `0x9a2e78` unlock) record the owner's thread ID at `struct+8` and only
  unlock when the caller's ID matches.
- ps3recomp delivers vblank/flip handlers at whatever firmware call a guest thread happens to make, and the callback
  inherits that thread's ID (`ppu_guest_call`). The flip handler ran nested inside `DisplayFlip`'s own call, while
  `DisplayFlip` held the lock at `0x012450d8`. It re-entered the lock (same ID), and its unlock cleared the owner
  record. `DisplayFlip`'s own unlock then saw an owner of 0 and was skipped, so the lock leaked (owner 4,
  recursion 1). The display engine and a sound thread blocked on it forever, and with the handler stuck inside the
  pump, no more vblank/flip callbacks were delivered.
- On a PS3 these handlers run on GCM's own interrupt thread.

**Fix** (our ps3recomp branch `gcm-handler-thread`, upstream PR candidate): `GCM_HANDLER_THREAD=1` delivers the
handlers only on a dedicated host thread with its own identity. With it, 3 of 3 runs get past the boot screens,
open `MLB_INTRO.BIK`, and go on loading user data, crowd sound banks, profiles and the logo cache.

**Next problem:** the presented picture stays on the autosave notice (cover art) from about 20 s on, while the game
keeps running. `DisplayFlip` now calls `cellGcmSetFlipImmediate(0)` about 100,000 times a second, so its flip loop no
longer paces itself. The movie may be playing unseen.

Also unresolved, not blocking yet: `0x4cef342e`, `cellSpursAttributeEnableSystemWorkload` and `cellHttpInitCookie`.

## 2026-10-27 (evening): first picture through Vulkan

- **Packages:** Vulkan needs `vulkan-headers`, `glslang-devel` and `spirv-tools-devel` (Fedora). Build with
  `-DPS3RECOMP_RSX_VULKAN=ON`. With glslang found, CMake reports "guest programs available": the game's own RSX
  shaders are translated (HLSL, then SPIR-V).
- **Template gap (upstream fix candidate):** `templates/project/main.cpp` picks D3D12, Metal or the headless
  renderer and never Vulkan, even in a Vulkan build. The runtime's own host tool (`runtime/host/host_posix.c`) does
  pick it. Adding a `PS3RECOMP_RSX_VULKAN` case with `rsx_vulkan_backend_init/present/pump_messages` fixes it.
- **Run:** `PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1`. The renderer picks the RTX 3060 Ti, opens a
  1280×720 Wayland window, and the game draws its **legal and licensing screen correctly**: text, logos and the
  animated red and blue background. `PS3RECOMP_VK_DUMP=<file>.ppm PS3RECOMP_VK_DUMP_EVERY=60` saves frames.
- **Next blocker:** it freezes right after opening the intro movie, `PS3/UI/Movies/intro/MLB_INTRO.BIK`. Bink's
  SPU program is lifted, but playback hangs.
- `PATCH_GD.WAD` fails to open from the data install folder (`BCUS01089_R`); it may be found in the update folder
  instead, not checked yet.

## 2026-10-27 (later): SPU job manager running, data install done

- **Policy module:** the WWS job manager's SPURS policy module is 12,032 raw bytes at `0x105bb80` (file offset
  `0x104bb80`) that SPURS loads at local store `0xA00`. Wrapped as an SPU ELF at that base
  (`wrap_spu_elf.py --base 0xA00`) and lifted with `build_spu_workloads.py`, its text segment registers under the
  fingerprint the runtime looks up (`0x75BFE68AE96D7577`).
  - The runtime's `SPU_DUMP_MISS=<dir>` setting wrote the missing programs out exactly as fingerprinted, which is
    how both were found.
  - The same job manager runs LittleBigPlanet, whose fixes are already upstream.
- **Job modules:** `lift_jobmods.py --engine-elf` finds and lifts the 7 job modules linked into the EBOOT. One has
  308 undecoded words, the same count as the MP3 decoder program, so they're probably its data tables.
- **Build:** with the 13 SPU programs (`-DSPU_DIR=`), the rebuild takes 38 s. All of this is in `tools/lift_spu.sh`.
- **Results:**
  - All three job workloads resolve and run.
  - The game's 9.5 GB first-boot data install completes in about 50 s.
  - The game then creates more workloads and its seven sound streaming threads. Job loads stall and resume through
    SPU DMA lists and interrupts, and the job manager's self-modifying code is handed to the SPU interpreter.
- **Open problems:**
  - The job manager sometimes spins at local store `0x2534`, repeating one DMA transfer until the runtime's
    watchdog halts that SPU (256 identical transfers). LBP's notes say this policy module gets code patched into it
    by SPURS at load time (holes at `0xAEE`–`0x11B0`, `executeStage` at `0x14f4`); `SPURS_PM_DUMP` checks for that.
    - Checked with `SPURS_PM_DUMP=1`: the same holes as LBP's (1,728 of 1,730 bytes zero, `executeStage` empty).
      No game code writes the main-memory copy in 60 s. Upstream has no fix for LBP either.
    - That may be expected: the job manager writes code into its own local store (the runtime logs these as
      `spu-smc` and runs them through the SPU interpreter). That would fill the holes on the SPU side, where a probe
      of main memory can't see them.
    - The spin re-reads the same 128 bytes of the workload's data (data + `0x80`) 256 times, which looks like
      polling an empty job queue rather than a crash. The game keeps re-running those workloads afterwards. Still to
      confirm.
  - One guest write to NULL+0x48 from `0x7b988`.
  - The headless null renderer shows nothing yet; the Vulkan backend is next.

## 2026-10-27: first boot

**Lifting the PPU code** (`tools/probe.sh`):
- `ppu_loader.py` finds 15,790 functions from the OPD table and 433 firmware imports across 32 libraries.
- `ppu_lifter.py` lifts 43,501 functions to about 6.5M lines of C++ in 11 chunks, in about 1 minute.
- **Pass `--code-end 0xf2c634`.** Without it the lifter treats the read-only data after the code (1.5 MB, up to
  `0x10a43e8`) as code: 1.93M undecodable words and 954 MB of output. Ghidra's section table shows the last
  executable section ends at `0xf2c634`.
- With it, 6,847 words stay undecoded. They are float constants (1000.0, 32767.0, 10000.0…) and zero padding
  between functions, not unsupported instructions.

**Building** (ps3recomp's `templates/project`, clang, `-j6`):
- No compile errors. 10 min 55 s wall time, about 1.5 GB of RAM per chunk.
- Result: a 205 MB native Linux executable.
- The runtime itself needs `SDL2-devel` (Fedora: `sudo dnf install SDL2-devel`) and builds in seconds.

**Running** (headless null renderer, no SPU programs lifted yet):
- The game's own code runs natively. It sets up memory, SPURS, GCM and sysutil, starts its threads ("Job Manager
  Event Handler", "IMFE PPU Thread"…), draws its first frames and flips buffers, creates its trophy context, finds
  a PS4 controller through SDL, and reads `MLBPS3_SYS.WAD` from the disc.
- First stop: the game's "not enough system storage (5,120 MB)" dialog. The runtime resolves `/dev_hdd0/game` under
  the same root as the disc, which was a read-only ISO mount. Fix: a writable root folder holding a `PS3_GAME`
  link to the disc and `game/BCUS01089` (the v01.10 update).
- With that, the game offers its optional data install, the runtime auto-answers yes, and the game starts
  installing ("Installing Game Data… File 1/6").

**Missing so far:**
- The WWS job manager's SPURS policy module (12,032 bytes at `0x105bb80`) isn't lifted, so the game's SPU job
  workloads (`Wws_Job`: `DXTJob`, `MeshJobs0`, `MeshJobs1`) never run. ps3recomp's `lift_jobmods.py` targets this
  job format (`c0dec0de` modules).
- The Bink SPU program (110,560 bytes) isn't lifted yet, so video won't play.
- Two unresolved firmware imports showed up early in the boot.

**SPU programs:**
- The EBOOT embeds 5 SPU ELF images: two WWS job checks, an MP3 decoder, surround audio and Bink.
- It also holds 8 `c0dec0de` job modules (4 inside those ELFs), including the ICE mesh program.
- RPCS3 showed 25+ SPU programs in use, so the rest come from the game's data files.
- All 5 ELF images lift with `spu_lifter.py --auto-functions` with no unsupported instructions.

**Firmware coverage** (approximate, from ps3recomp's handler table vs our 433 imports):
- 165 have a real implementation, 71 a thin one, 46 return a constant, 32 run inside the runtime itself, and 119
  are missing. Missing imports return CELL_OK without doing anything.
- Gaps for online play:
  - `cellHttp` is registered under made-up names, so its handlers never match the game's imports (37 of 48 missing).
  - `cellSsl`/`cellHttps` have no TLS.
  - NP auth, ID lookup and signaling are missing or stubbed.
- ps3recomp's own docs recommend Sony's `libsre` (lifted from your firmware) over its built-in SPURS, which has hung
  most SPURS-heavy titles.
