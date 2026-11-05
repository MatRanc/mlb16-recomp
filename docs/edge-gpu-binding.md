# c690 0x2A GPU ring binding and live key: offline gates pass, the live A/B crashes

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

## 7. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime/walker code, our own captures and Vulkan kernel. The stage/corpus/
differential method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern is
from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors), design
reference only. No RPCS3 or other GPL emulator source was read or cited.
