# c690 0x2A GPU ring binding and live key: heap crash found and fixed; overlay measured

Date: 2026-11-05. Branch `exp/edge-gpu` in the `ps3recomp-gpu` worktree (merged
`exp/live-ab` first), build `recomp-work/build-gpu`. **Two pitch-meter launches
(the cap), both crashed on the key-on arm; no A/B numbers.** Offline: the
32-byte kernel gate, a new compact-arena mirror of the live packing, the
resolver transparency gate through `replay_rsx_engine`, unit tests, the 30
video fixtures and the ABI check.

This implements the §4 binding from [`edge-gpu-2a-wiring.md`](edge-gpu-2a-wiring.md)
for the class decoded in [`edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md), following
[`edge-gpu-plumbing.md`](edge-gpu-plumbing.md) and the policy in
[`edge-exactness.md`](edge-exactness.md).

> **Update 2026-11-05 (later): the live crash is found and fixed.** The lane
> key was `ctx->spu_id % 16`, but the c690 jobs run through
> `spu_run_spurs_job`, which creates every lane's context with `spu_id 0`; all
> five lanes shared one export slot. Lane state is now thread-local, the live
> key is atomic, and the seed-fallback/queue lifetimes are fixed. A new
> concurrent corpus harness reproduces the old heap corruption under ASan and
> is clean under ASan/UBSan and TSan after the fix. **One live pitch-meter A/B
> ran without a crash; no speed gain, as designed (the SPU still packs).** The
> path's own cost is ~6.0 ms/frame (submit+fence). See §7.
>
> **Update 2026-11-05 (async hand-off, §8).** The compute now runs on a
> dedicated queue with a timeline semaphore; the walker never waits (submit
> cost 0.061 ms/frame, wait 0). The frame still loses ~7.6 ms at p90 to the
> compute queue's scheduling, so the next step is to record the dispatches
> into the frame's own command buffer, not another queue submit. See §8.

## TL;DR

* **The binding exists and is default off.** Live key
  `PS3RECOMP_C690_GPU_2A` (0 = off). The SPU lane exports the four planar
  source arrays and the per-record offset list for a 0x2A command that matches
  the 32-byte A/B contract; the renderer runs the offline `c690_2a_32b` Vulkan
  kernel on its device/queue (one command buffer, one submit, one fence wait
  per drain); the draw engine's guest resolver serves the GPU-written range
  for the mesh-ring span. Any other class is not exported.
* **The lifted handler is unchanged, so the overlay is safe.** The first
  design suppressed the record stores (`+0` dropped, `+16` keeping only
  `+28..31`); an offline trace of the real lifted body showed that it
  **reads back the records it writes**: after the correct record was stored,
  the handler stored the pre-body bytes back over it. Suppression therefore
  changes the handler's own output and cannot be restored exactly. The safe
  first stage is an overlay: the SPU writes exactly what it writes today, and
  the resolver returns the GPU bytes for the covered range. **The pack is not
  skipped yet**; the next stage must replace the per-stream pack
  (`0x10C70`/`0x109C0`), not the stores.
* **Offline gates pass.** Resolver transparency: the exact lifted bytes served
  through the resolver give the *same* draw/state hash as the same bytes in
  guest memory (`44f962cef2a40ae3`, gate `7463a05e53fffdb7`), and the
  unpatched baseline differs (`a6fcc849341eba9b`). The GPU bytes through the
  resolver give `903b37427417553a`; the difference is the approved
  `<= 1 LSB` packed-field drift (positions are exact). The compact-arena
  packing that the live backend uses is byte-for-policy identical on both
  formats (51/51 kernel gate; two compact pairs re-run here).
* **The live A/B is blocked.** Launch 1 (03:11) and launch 2 (03:17) both
  aborted (SIGABRT, glibc `double free or corruption`) within ~0.1-1 s of the
  first `PS3RECOMP_C690_GPU_2A=1` switch, after the compute pipeline was
  created. The control file and the arm switch are the only changes; the A
  arms ran normally. The module/export path is clean under ASan (6 threads,
  1.2 M jobs, 0 errors); the engine probe and the Vulkan prepare are the
  remaining suspects. The mission cap (2 launches) and the repeat-failure rule
  stop the live work here. **No fps, p50/p90/p99, c690-wall or sync number is
  claimed for the key-on arm.**
* **What the offline work already implies for the plan.** The GPU compute
  itself is the measured 0.23-0.27 ms per 429-job burst device-local (phase
  2b step 1); the per-job export/resolve path adds the copies and one fence.
  Because the SPU pack is still running, this stage can only *cost* time. The
  next stage pays off only if the pack is replaced; that is a
  `0x10C70`/`0x109C0` job, not another binding change.

## 1. What was built

### 1.1 Live key

`libs/video/ps3_live_ctl.h/.c`: `PS3_LIVE_C690_GPU_2A` /
`"PS3RECOMP_C690_GPU_2A"`, appended at the end, default 0. It is a control-file
key, so the A/B switches arms without a rebuild. The key is read by
`c690_gpu_2a_enabled()` at every 0x2A entry and cached in `g_c690_gpu_2a_on`
for the PUT hook.

### 1.2 SPU export (`runtime/spu/c690_gpu_2a.c/.h`)

The generated 0x10F78 wrapper (added by `tools/c690_ref/patch_gpu2a.py`) is
now:

```c
void ..._func_00010F78(spu_context* ctx) {
        c690_gpu_2a_enter(ctx);
        ... the capture wrapper and the lifted body ...
        c690_gpu_2a_exit(ctx);
}
```

* **enter** saves `r3` and the entry output cursor (`LS[r3+0xD0]`).
* **exit** reads the post-handler cursor, rejects a non-multiple-of-32 delta,
  a count over 8192, or a range outside the 256 KiB local store, then
  classifies the output as format A (u16 byte-offset list at `cmd[0]`) or
  format B (identity `k*16`) by comparing each record's 12-byte position
  prefix with `array0[offs[k]]`. On a match it copies:
  * the four source arrays (pos, nrm, tan, uv from `r3+0x260`) with
    `extent = max(offs)+16` (or `count*16` for B), and
  * the exact lifted record bytes (`count*32`, including the SPU-written
    `+28..31` attribute word),
  and queues an export for the lane.
* **PUT** (`spu_dma.h`, fast and slow PUT paths) matches any DMA PUT that
  overlaps `[out_base, end)` against the lane's exports. The submit PUT can be
  split at the 0x4000 DMA chunk size, so one overlapping point extrapolates
  the record EA on the linear ring (`ea_rec = put_ea + out_base - put_lsa`).
  The export becomes a `c690_gpu_2a_job` and is handed to the renderer sink.
* Per-class fallback counters: `[c690-gpu2a] enter= class= fallback= nosink=
  put= unmatched= records= fallback_records=` (every ~2 s). `class/enter` is
  the command coverage; `records/(records+fallback_records)` is the record
  coverage.

Nothing is allocated or written when the key is off (`enter` returns before
touching the lane state). The handler's output, the ring PUT and guest memory
are byte-for-byte the lifted path.

### 1.3 Renderer compute (`libs/video/rsx_vulkan_backend.c`)

`vk_c690_sink` (SPU lane thread) queues the job and arms a pending resolver
range. `vk_c690_prepare` (walker, called from the first draw that resolves a
pending range) does one drain:

1. resets the range map,
2. copies each job's arrays, offsets and full-record seed into persistent
   host-visible arenas (32 MiB in / 4 MiB offsets / 32 MiB out; slices aligned
   to `minStorageBufferOffsetAlignment`, min 256),
3. binds one descriptor set (three `STORAGE_BUFFER_DYNAMIC` bindings) with
   dynamic offsets and pushes `{pos,nrm,tan,uv,out,count}` (the arrays are
   compact: `0, extent, 2*extent, 3*extent`; `out_base = 0`),
4. dispatches `ceil(count/64)` groups per job into one command buffer,
5. submits once and waits one fence (the "draw that reads the range waits"),
6. installs each job's output range at `(ea, count*32)` in the engine
   resolver; if the compute is unavailable it installs the exact SPU seed
   instead.

The arenas and the device-local compute pipeline are created lazily on the
first job (`[RSX vulkan] c690 gpu 2a: compute pipeline ready`). The kernel is
the proven `tools/replay/c690_2a_32b.comp`, checked in as
`libs/video/vulkan/c690_2a_32b.comp` + `.spv.h`. A per-2 s
`[c690-gpu2a-gpu]` line reports prepares/jobs/records/sync_us.

### 1.4 Walker resolver (`libs/video/rsx_draw_engine.c/.h`)

A seqlock range map (128 entries) is consulted at the top of
`eng_guest_ptr_raw`, so it covers both the live VM path and the replay
`cap_read` path. A read fully inside a range returns the GPU bytes; a read
that straddles it returns a merged buffer (guest bytes with the overlap
overlaid), cached per `(location, offset, len)` so a cached fetch plan keeps a
stable pointer. Range installs are `add_ea` (live: the requested RSX offset is
resolved with `cellGcmResolveLocated`) and `add_offset` (replay/synthetic).
The resolver is dormant when no range is installed (one load + test).

The `PS3RECOMP_C690_GPU_2A` key only changes the vertex *source* for a
covered range; the index list, cull, textures, WCB and PPU paths are
untouched. No guest memory is patched.

### 1.5 Module wrapper

`tools/c690_ref/patch_gpu2a.py` is idempotent, composes with the existing
`patch_cull_hooks.py --capture` wrapper, and was applied to
`recomp-work/captures/gpu2a/spu_capture/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`
(the build-gpu SPU_DIR; game data, not committed).

## 2. Why the SPU record stores are not suppressed

The first implementation (the literal §4 wording) hooked `spu_ls_write128`
through a per-context window, dropped the record `+0` store and wrote only
`+28..31` of the `+16` store. It was exact in a synthetic body (400/400
records: 0..27 unchanged, 28..31 exact, non-record bytes exact), but the real
lifted body is not a pure writer. Tracing the actual 32-byte pair
(`seq 453000`, 400 records) recorded:

```
[sav0] v4=3dc5284c   <- the correct position
[sav0] v4=fbc7e07d   <- the pre-body bytes, stored back afterwards
```

The handler reads the record bytes it wrote (the later stores carry the
pre-body value once the first store was suppressed). Suppressing the stores
therefore changes the body's own result, and the exact lifted bytes cannot be
reconstructed from the suppressed run. The implementation was changed to the
overlay above; guest memory keeps the lifted output, and the resolver supplies
the GPU bytes. **Skipping the pack needs a native `0x10C70`/`0x109C0`
replacement** (the per-stream record assembler), which is the next stage.

## 3. Offline gates

All commands and the raw output are in `/tmp/opencode/gpu2a/final_gates.txt`.

| gate | result |
|---|---|
| 32-byte kernel, `fncap2b2w3` | **51/51 pairs, 20,312 records**; positions 0 differing bytes; normal/tangent/uv max 1 LSB; SPU attr 0; 0 writes outside 0..27; ring position/attr 0, packed 1,151 bytes <= 1 LSB; PASS |
| compact-arena mirror (the live packing), A pair 453000 | 400 records; positions 0; packed max 1 LSB; attr 0; outside 0; ring position/attr 0; PASS |
| compact-arena mirror, B pair 1293000 | 576 records; positions 0; packed max 1 LSB; attr 0; outside 0; ring position/attr 0; PASS |
| resolver transparency, meter frame | guest baseline `a6fcc849341eba9b`; guest SPU bytes and resolver SPU bytes both `44f962cef2a40ae3` (gate `7463a05e53fffdb7`); **MATCH**, 22,353 reads 0 miss |
| resolver GPU bytes, same frame | `903b37427417553a` (gate `20ac1b0f52b40723`); differs only by the approved packed drift (positions exact, per the kernel gate) |
| unit test `test_c690_gpu_2a` | PASS: format A, format B, exact fallback, ring-EA mapping |
| `tools/test_video_fixtures.py` | **30/30 pass** |
| `check_spu_context_abi.sh` | shared field offsets agree (the appended fields were removed; no ABI change) |
| ASan stress of the export/PUT path | 6 threads, 1,199,937 jobs, 0 errors |

The kernel gate is the byte-level policy proof; the resolver gate is the
binding proof. A single "key on vs key off" hash cannot be identical for the
32-byte class because the kernel is policy-exact, not byte-exact: the
null-backend digest hashes converted vertex bytes. The gate therefore uses the
lifted bytes through the resolver (identical hash) and the kernel gate for the
GPU bytes. This is the approved exactness policy.

## 4. The live A/B attempts

Both runs used `BUILD=build-gpu`, `METER=1 SKIP_CINE=1 WCB=2 PIN=0`,
`VIDEODRIVER=x11`, `PITCH_THRESH=35`, one X, governor `performance`, arms
`A=PS3RECOMP_C690_GPU_2A=0` / `B=PS3RECOMP_C690_GPU_2A=1`.

| run | log | result |
|---|---|---|
| 1 | `ab_live_c690gpu2a_20261105_0309...` | crashed 0.1 s after the first B switch (`double free or corruption (out)`); core `1466901` |
| 2 | `ab_live_c690gpu2b_20261105_031502` | crashed 0.1 s after the first B switch (`double free or corruption (!prev)`), immediately after `[RSX vulkan] c690 gpu 2a: compute pipeline ready`; core `1471362` |

After run 1 the export was bounded (`out_base + delta <= 256 KiB`), the
resolver seqlock was made torn-read safe and the pending marker was added to
the sink; run 2 still crashed, so the fix did not address the cause. The
module/export path was then ASan-stressed clean; the remaining suspects are
the backend prepare (Vulkan/arena) and the engine merge/probe. The coredumps
are truncated by the huge guest mapping, so the aborting thread's stack is not
readable; the readable SPU-lane stacks show the 0x2A body, not the export.

Per the mission's repeat-failure rule and the 2-launch cap, no third launch was
made. The key is default off; `build-play` and the shared lifted module are
untouched.

## 5. What this implies for the GPU plan

* **The binding is no longer the blocker for measuring a GPU Edge stage; the
  live integration is.** The kernel, the compact packing, the resolver and the
  exact fallback are proven offline. The crash is in the live renderer hand-off
  and must be isolated before any stage is timed.
* **This stage cannot pay off by itself.** The SPU still runs the pack, and the
  export copies (`4*extent + count*32 + count*2` bytes per job) plus one fence
  are added. The only honest offline estimate for the compute is the phase-2b
  0.23-0.27 ms per 429-job burst; the export copies are tens of KiB per job.
  At the 31.2 % class share the potential *saving* is ~1.07 ms of the 24.39 ms
  lane, so even a perfect skip is small at this coverage.
* **Each further GPU stage pays only after the pack skip.** 0x21 sel7
  (4.74 ms), the culls (3.03 ms) and the rest of 0x2A (2.36 ms) are larger,
  but they all ride the same binding and the same resolver. The measured
  overhead of this path — once it runs — is the number that decides whether
  the tail is worth porting.
* **The next stage is not another kernel.** It is (a) isolate and fix the live
  crash, then (b) replace the per-stream pack (`0x10C70`/`0x109C0`) for the
  A/B class so the SPU stops computing the records, then re-run the §6 A/B.
  The projected gain at full 0x2A coverage is the existing 3.43 ms lane slice;
  the binding makes it measurable.

## 6. Reproduction (offline)

```sh
cd /home/m/Documents/mlb16-online/mlb16-recomp

# 32-byte kernel gate (51 pairs / 20,312 records)
python3 tools/replay/test_c690_2a_32b.py \
  --spool ../recomp-work/captures/gpu2a/fncap2b2w3

# compact-arena mirror of the live packing (A and B pairs)
clang -O2 -march=native -o /tmp/test_c690_2a_compact \
  tools/replay/test_c690_2a_compact.c -l:libvulkan.so.1 -lm
/tmp/test_c690_2a_compact /tmp/opencode/gpu2a_32b/pair_453000.job \
  tools/replay/c690_2a_32b.comp /tmp/opencode/gpu2a_32b/c690_2a_32b.spv
/tmp/test_c690_2a_compact /tmp/opencode/gpu2a_32b/pair_1293000.job \
  tools/replay/c690_2a_32b.comp /tmp/opencode/gpu2a_32b/c690_2a_32b.spv

# resolver transparency (replay_rsx_engine + PS3RECOMP_GPU_RING)
python3 tools/replay/test_c690_2a_binding.py --seq 453000

cd ../ps3recomp-gpu
python3 tools/test_c690_gpu_2a   # built from tests/test_c690_gpu_2a.c
python3 tools/test_video_fixtures.py
SDK=$PWD ../recomp-work/ab-tools/check_spu_context_abi.sh
```

The live run (do not repeat before a fix):

```sh
cd ../recomp-work/ab-tools
TAG=c690gpu2c BUILD=build-gpu N=5 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 \
  PIN=0 VIDEODRIVER=x11 REPORT=0 LIMIT=420 SETTLE=10 PITCH_THRESH=35 \
  ARMS='A=PS3RECOMP_C690_GPU_2A=0|B=PS3RECOMP_C690_GPU_2A=1' \
  BOOT_ARM='PS3RECOMP_C690_GPU_2A=0' ./ab_live.sh
```

First isolate the crash with the compute disabled but the export on (a debug
key that makes the sink drop the job and leaves the resolver empty), then with
the resolve-only path. Only then re-time.

## 7. Live crash isolated and fixed (2026-11-05, later)

### 7.1 The aborting thread and the failing free

The run-2 core (`1471362`, still in `coredumpctl`) is the RSX FIFO thread, not
an SPU lane:

```
frame_clock
 cellGcm_rsx_process_fifo
  rsx_draw_engine_methods -> sink_end -> sink_end_work
   rsx_vertex_fetch_plan_prepare -> eng_guest_ptr -> eng_guest_ptr_raw
    eng_gpu_range_probe -> vk_c690_prepare
     vk_c690_job_free   (0xc666577 = the instruction after free(j->offs))
```

So the `double free or corruption` is prepare freeing a job buffer that had
already been freed or written after free. It is not a Vulkan lifetime problem:
the compute pipeline was created on this same drain.

### 7.2 Why: every lane had `spu_id = 0`

The export state was keyed on `ctx->spu_id % 16`. The c690 mesh jobs do **not**
run as lv2 SPU threads on this port: `spurs_pm_worker` calls
`spu_workload_dispatch_job` -> `spu_run_spurs_job`, which builds a stack-local
`spu_context` with `spu_context_init(&ctx, 0)`. The run-1 log ends with:

```
[c690-stats] worker=0 spu=0 jobs=104427
[c690-stats] worker=1 spu=0 jobs=103668
...
[c690-stats] worker=4 spu=0 jobs=104503
```

All five concurrent lanes therefore shared `s_lane[0]`. `enter`/`exit`/`put`
(r3, cursor, `exp_next`) and `export_push`'s free ran from five threads on one
slot: one lane could overwrite or free a buffer another lane had already
queued, and prepare then freed it a second time. That is exactly the live
symptom, and it explains why the ASan stress of the module path was clean: it
did not model five contexts with the same id.

### 7.3 The fix

1. **Lane state is per host thread** (`SPU_THREAD_LOCAL` in
   `runtime/spu/c690_gpu_2a.c`). A lane is one host thread here: the job
   runner and the lifted thread run the whole job, including the submit
   epilogue PUT, synchronously on the caller. No id dependence, no lock.
2. `g_c690_gpu_2a_on` is read and written with relaxed `__atomic`
   load/store everywhere (TSan flagged the atomic store vs the plain reads in
   `exit`/`put`/`spu_dma.h`).
3. **Seed-fallback lifetime.** The init-failure / resolve-only paths installed
   `j->seed` in the resolver and freed it immediately, leaving the range
   pointing at freed heap. Seeds are now retained until the next
   `gpu_range_clear()` (the drain that replaces the map); past the retain cap
   the range is not installed and the draw reads the identical guest copy.
4. **Queue ownership.** A job requeued by prepare clears its pointers in the
   detached array; the EndCommandBuffer failure path sweeps the jobs it did
   not reach; release frees queued/retained jobs after clearing the ranges.
5. **Debug isolation values** (env, default 0) for the next crash hunt:
   `PS3RECOMP_C690_GPU_2A_DBG=1` export on / GPU off (sink queues, prepare
   drops, resolver empty -> guest bytes), `=2` resolve only (exact SPU seed
   installed as a resolver range, no Vulkan object is created).

### 7.4 Concurrent corpus harness

`ps3recomp-gpu/tests/test_c690_gpu_2a_race.c`,
`tools/run_c690_gpu_2a_race.sh`: five lane threads, **all with `spu_id 0`**,
drive the real `runtime/spu/c690_gpu_2a.c` over the captured
`fncap2b2w3` C69STG01 tail pairs (192 pairs; synthetic 32-byte A jobs when the
spool is absent), while a walker thread drains a mirror of
`vk_c690_sink`/`vk_c690_prepare`'s ownership and reads every installed range.

| build | result |
|---|---|
| old `c690_gpu_2a.c` (HEAD) + ASan | **heap-use-after-free**, drain `memcpy` of a seed another lane's export still owned — the live bug |
| fixed + ASan/UBSan | PASS (`sunk=2040`, `leftover=0`) |
| fixed + TSan | PASS, no race reports |
| `test_c690_gpu_2a` | PASS (format A/B/fallback) |
| `tools/test_video_fixtures.py` | **30/30** |
| `check_spu_context_abi.sh` | shared offsets agree (no `spu_context` change) |

### 7.5 Live pitch-meter A/B (one launch, cap respected)

`BUILD=build-gpu`, `METER=1 SKIP_CINE=1 WCB=2 PIN=0`, `VIDEODRIVER=x11`,
`PITCH_THRESH=35`, `N=5 SEG=10 DROP=2`, one X, governor `performance`,
`PS3RECOMP_C690_GPU_2A=0` until the first switch. **No crash**, normal exit;
the key switched 9 times (`[live-ctl] ... PS3RECOMP_C690_GPU_2A=1/0`). Log
`ab_live_c690gpufix_20261105_035535.log`.

| arm | frames | fps | p50 | p90 | p99 | std | walker ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| A (key off) | 1215 | 30.38 | 34.23 | 43.90 | 48.24 | 10.91 | 13.78 |
| B (key on) | 917 | 22.93 | 34.16 | 68.60 | 72.98 | 22.55 | 25.89 |

The B arm is **not** a win, and is not supposed to be: the SPU still packs,
and the overlay adds a submit + blocking fence per drain. The number that
matters is the path's own cost, from `[c690-gpu2a-gpu]` and `[c690-gpu2a]`:

| quantity | value |
|---|---|
| prepares / jobs / records (B) | 3,042 / 162,849 / 61.2 M |
| jobs, records per prepare | 53.5, 20,132 |
| prepares, records per B-active frame | 2.47, 49,779 |
| submit+fence per prepare | **2.41 ms** |
| submit+fence per frame | **5.96 ms** over all B-active frames (8.0 ms over the post-drop windows) |
| GPU compute per prepare (estimated) | ~0.03 ms (phase-2b: 0.23-0.27 ms per 429-job burst, same kernel shape) |
| GPU compute per frame (estimated) | ~0.08 ms |

So ~98 % of the measured cost is queue submit/fence serialisation: every drain
submits on the render queue and then blocks the walker on a fence, behind the
frame's own graphics work. During B, `class/enter` = 162,342/586,050 = 27.7 %
of all 0x2A entries (the same order as the 31.2 % of sampled 0x2A pairs); the
remaining entries are the unclassified classes and the early-reject cursor
cases.

### 7.6 What a real gain requires, and the estimate

The doc's next stage is unchanged: **replace the per-stream pack
(`0x10C70`/`0x109C0`) so the SPU stops writing the records**, then the GPU
output is the only source and the packing time disappears. The measurement
above adds a second requirement: **the per-drain blocking fence has to go**.
At 2.41 ms per submit+fence and ~2.5 drains/frame, the current overlay spends
~6 ms/frame to save at most 1.07 ms (31.2 % of the 3.43 ms 0x2A lane slice).
Even a perfect pack skip cannot beat that while the walker waits on the
fence. The compute itself is cheap (~0.08 ms/frame); the binding should use
one timeline semaphore per burst and let the walker wait only at a draw that
actually reads the range, or run the compute after the SPU submit and before
the walker drain.

With the pack replaced and the fence pipelined, the honest saving is the
0x2A lane slice at the covered share: ~1.07 ms at the current 31.2 %
(51 pairs), ~2.3 ms at the 68.2 % now classified offline
([`edge-gpu-2a-classes.md`](edge-gpu-2a-classes.md)), and 3.43 ms at full 0x2A
coverage. That is a p50 of ~34.15 -> ~32-33 ms at best, with the 0x21 sel7
(4.74 ms) and the culls (3.03 ms) needed on the same asynchronous binding to
reach the 5-7 ms Edge target. The overlay's measured cost is the reason to do
the pack replacement and the async hand-off **together**, not as separate
stages.

## 8. Asynchronous hand-off: dedicated compute queue + timeline semaphore (2026-11-05, later)

Branch `exp/edge-gpu` in `ps3recomp-gpu`, build `recomp-work/build-gpu`.
**Two pitch-meter launches (the mission cap).** Run 1 exposed why a CPU wait
on the compute batch is not cheap; run 2 measured the fixed hand-off. The
binding stays default off (`PS3RECOMP_C690_GPU_2A=0`).

### 8.1 What changed from §7

* **A compute queue of its own.** `vk_find_compute_queue` picks a compute-only
  queue family (family 2 on the RTX 3060 Ti). The c690 command pool and all
  compute submits use it; the walker/present queues are untouched. If the
  device has no compute-only family, the walker's queue is used.
* **A timeline semaphore instead of a CPU fence.** The instance is created as
  Vulkan 1.2 when the loader supports it (a 1.0 instance silently reports
  `timelineSemaphore = false`; that is why run 1 logged "fence fallback"). The
  `VK_KHR_timeline_semaphore` extension + feature are enabled, the compute
  submission signals a value, and the driver is polled
  (`vkGetSemaphoreCounterValue`) at drain boundaries. No `vkWaitForFences` in
  the hot path; `vkWaitSemaphores` is used only at shutdown.
* **Submit at the drain boundary.** `rsx_wprof_drain_begin()` now calls a
  renderer kick hook: all jobs queued since the last drain become one dispatch
  set on the compute queue, submitted without waiting. Jobs that arrive
  mid-drain are batched by the existing prepare hook. The old 64-job cap is
  gone; only a full arena requeues a job (with its pending marker re-armed).
* **No blocking on arena reuse.** If the previous batch is still in flight,
  the new batch is put back on the queue for a later drain instead of waiting.
  The arena is single-buffered, so this is the only ordering the compute needs.
* **The resolver's CPU view never blocks.** A range now carries the GPU bytes,
  the exact SPU seed bytes, and the timeline value that produces them. The
  probe serves the **seed** until the cached "done" value reaches the range's
  value, then the GPU bytes. This is the explicit choice the gate asks for:
  a CPU reader either gets the seed or (once the hand-off has completed) the
  GPU result; it never stalls. In this stage the per-drain range clear means
  the live draw path takes the seed in practice; the GPU result is computed
  and available, not yet consumed.
* **One memory barrier, validation-clean.** The compute command buffer ends
  with a shader-write -> compute/transfer/host barrier. The first version used
  a `VERTEX_SHADER` destination stage, which is invalid on a compute-only
  queue (VUID-vkCmdPipelineBarrier-dstStageMask-06462); the offline validation
  arm found it and the mask is now compute/transfer/host only.
* **Debug values unchanged.** `PS3RECOMP_C690_GPU_2A_DBG=1` (export on, GPU
  off) and `=2` (resolve only, exact seed) keep their old meaning. The stats
  line now reports `submits/submit_us/wait_us` and prints every 2 s (the old
  code compared a nanosecond clock against 2,000,000, printing every 2 ms).

### 8.2 Offline gates

| gate | result |
|---|---|
| concurrent race harness (`tools/run_c690_gpu_2a_race.sh`) | ASan/UBSan and TSan clean, 192 captured pairs, 5 lanes with `spu_id 0` |
| `test_c690_gpu_2a` | PASS (format A, format B, exact fallback) |
| `tools/test_video_fixtures.py` | **30/30** (one pass; `test_gcm_tick_in_drain` is timing-flaky under load) |
| `check_spu_context_abi.sh` | shared field offsets agree (no `spu_context` change) |
| replay gate, `meter_frame --frames 64` | `a6fcc849341eba9b`, gate `ff2a220c28750bc3` (unchanged) |
| offline Vulkan validation arm (`tools/c690_vk_timeline_validate.c`) | 200 submit/poll/wait rounds, **0 validation errors** |

The live A/B command below was the second launch; with the 2-launch cap there
was no third launch for a game-run validation arm. The offline harness uses
the same device/queue/timeline/pipeline-barrier sequence and is the validation
arm required by the mission; it is not the in-game integration.

### 8.3 The two launches

Both used `BUILD=build-gpu`, `METER=1 SKIP_CINE=1 WCB=2 PIN=0`,
`VIDEODRIVER=x11`, `PITCH_THRESH=35`, `N=5 SEG=10 DROP=2`, one X, governor
`performance`, `A=PS3RECOMP_C690_GPU_2A=0` / `B=PS3RECOMP_C690_GPU_2A=1`.

**Run 1** (`ab_live_c690gpu2d_20261105_041941`) used a 1.0 instance, so the
timeline feature query returned false and the binding fell back to a fence;
it also still waited at the read. The wait cost **2.48 ms per batch** (4.36 s
over 1,758 batches) and the frame collapsed:

| arm | frames | fps | p50 | p90 | p99 | walker ms |
|---|---:|---:|---:|---:|---:|---:|
| A | 1,133 | 28.32 | 34.20 | 57.20 | 70.74 | 13.42 |
| B | 636 | 15.90 | 74.07 | 89.40 | 102.79 | 35.87 |

**Run 2** (`ab_live_c690gpu2e_20261105_043438`): timeline semaphore on,
compute submitted at drain start, no CPU wait:

| arm | frames | fps | p50 | p90 | p99 | std | walker ms | wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 1,485 | 37.12 | 34.15 | 38.78 | 42.57 | 11.41 | 8.46 | 15.98 |
| B | 1,104 | 27.60 | 34.65 | 46.43 | 50.88 | 32.30 | 15.14 | 18.96 |

Thumbnails: the meter shots are clean in both arms (B brightness 102.0 +/- 1.0
against A's scene-dependent 96.8; no black/garbage frame, consecutive
thumbnail difference 0.75 in B).

### 8.4 Where the remaining overhead is

From the run-2 counters:

| quantity | value |
|---|---:|
| prepares / submits | 4,525 / 4,525 (all key-on) |
| jobs / records | 232,588 / 87,467,740 |
| jobs / records per prepare | 51.4 / 19,330 |
| submits per B-active frame | 4.10 |
| **submit per submit** | **14.8 us** |
| **submit per frame** | **0.061 ms** |
| **CPU wait per frame** | **0** |
| GPU compute per frame (phase-2b model) | ~0.12 ms (79,228 records/frame) |

So the hand-off itself (submit + wait) is **~0.07 ms/frame**, under the 0.5 ms
target. The frame level still moves the wrong way: p50 +0.50 ms, p90 +7.65 ms,
p99 +8.31 ms, fps -25.6 %. With 4.1 compute-queue submits per frame, the tail
is about **1.9 ms of lost graphics time per submit**: a queue-family switch,
not the fence and not the arithmetic (87 M records is ~0.12 ms/frame at the
phase-2b rate). Run 1 shows the same constant from the other side: waiting on
a compute-queue batch costs as much as the old render-queue fence because the
graphics queue is saturated and the compute queue is scheduled in coarse
slices.

This is the mission's main negative result: **removing the fence does not make
the hand-off free while the compute runs on a separately submitted queue.** The
next step is to record the dispatches into the frame's own command buffer
before the first draw that reads the range (a pipeline barrier on the render
queue, no separate submit), which removes the queue switch. That only pays
once the draw consumes the GPU bytes directly; while the SPU still packs, the
CPU takes the exact seed and the in-stream compute is pure overhead.

### 8.5 What is left before a real GPU stage can win

1. **Pack replacement** (`exp/native-2a`, `ps3recomp-n2a`): stop the SPU
   writing the records so the seed fallback disappears. Then the draw must
   either read the GPU range directly (preferred; no CPU wait) or wait, which
   reintroduces the cost measured in run 1.
2. **In-stream compute or GPU-direct consumption** as above: one submit per
   frame, no compute-queue context switch. The validation-safe barrier on the
   render queue may include the vertex stage.
3. **Class coverage**: the live binding exports only the A/B 32-byte class
   (27.7 % of 0x2A entries this run: 232,588/840,713). The offline kernels
   cover 131/192 = 68.2 % of meter pairs; adding the other six classes is the
   u32 entry-index list + a class selector + the extra stream bases. The 31
   first-in-job runtime pairs and the 19 no-position outputs stay fallback.
4. **0x21 sel7 next** (4.74 ms burst), then the culls (3.03 ms), on the same
   asynchronous binding. The 0x2A saving is unchanged: ~1.07 ms at 31.2 %
   coverage, ~2.3 ms at 68.2 %, 3.43 ms at full 0x2A — and it only appears
   once the pack is skipped and the hand-off costs under ~0.5 ms/frame.

### 8.6 Reproduction

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-gpu

# offline race harness (ASan/UBSan, then TSan)
tools/run_c690_gpu_2a_race.sh

# unit test / fixtures / ABI / replay gate
../recomp-work/build-gpu/ps3recomp_sdk/test_c690_gpu_2a
python3 tools/test_video_fixtures.py
SDK=$PWD ../recomp-work/ab-tools/check_spu_context_abi.sh
cd ../recomp-work/build-gpu
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10

# validation arm
cd ../../ps3recomp-gpu
cc -O1 -Ilibs/video/vulkan -o /tmp/vk_c690_timeline_validate \
  tools/c690_vk_timeline_validate.c -ldl
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation /tmp/vk_c690_timeline_validate

# live A/B (already run twice; do not repeat before a fix)
cd ../recomp-work/ab-tools
TAG=c690gpu2e BUILD=build-gpu N=5 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 \
  PIN=0 VIDEODRIVER=x11 REPORT=0 LIMIT=420 SETTLE=10 PITCH_THRESH=35 \
  ARMS='A=PS3RECOMP_C690_GPU_2A=0|B=PS3RECOMP_C690_GPU_2A=1' \
  BOOT_ARM='PS3RECOMP_C690_GPU_2A=0' ./ab_live.sh
```

## 9. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code, our own captures and Vulkan kernel. The stage/corpus/
differential method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern is
from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors), design
reference only. No RPCS3 or other GPL emulator source was read or cited.
