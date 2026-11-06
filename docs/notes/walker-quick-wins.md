# Walker quick wins at the pitch meter: remap cache, fetch memo, pipeline memo, and the "[unknown]" samples

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play` (`Release`, `-O2 -DNDEBUG -march=native`). This is the
cheap end of the walker plan in [`walker-profile.md`](walker-profile.md): plan
items 4, 5 and 7's CPU half, plus an attribution of the 27.6 % `[unknown]`
walker samples in [`edge-native-step1.md`](edge-native-step1.md).

All three changes are behind their own default-off switch:
`PS3RECOMP_REMAP_CACHE`, `PS3RECOMP_FETCH_MEMO`, `PS3RECOMP_PIPE_MEMO`
(live keys `PS3_LIVE_REMAP_CACHE`, `PS3_LIVE_FETCH_MEMO`,
`PS3_LIVE_PIPE_MEMO`). `PS3RECOMP_WALKER_FAST=1` remains the umbrella and
still turns on the fetch and pipeline memos. Replay gates are unchanged.

## TL;DR

* **Only the pipeline memo is a live win.** `PS3RECOMP_PIPE_MEMO=1` cut the
  walker's `pipe` phase **2.107 -> 1.633 ms** (-0.47) at an **85.0 %** hit rate
  in a live meter A/B, and -0.84 ms offline. It is kept on in
  `play_windowed.sh`.
* **The fetch plan memo is a live loss and explains the old
  `PS3RECOMP_WALKER_FAST` regression.** Live it hits **5.4 %** (37,463 /
  695,926) because the reference sets change every frame; the misses pay the
  plan decode, the full reference-list key and the memo save. `fetch`
  **4.080 -> 6.493 ms** (+2.41) in the bundled arm. The one-frame offline
  replay hits 497/502 and therefore never shows this.
* **The remap cache is exact and a large offline win (-1.74 ms of 10.28 ms
  frame) but live-neutral.** It hits only **28.5-30.2 %** live; `fetch`
  +0.31 ms and fps +0.21 (CI -0.87..+0.45) in its own A/B. It stays available
  but off.
* **Offline, all three together: 10.283 -> 8.077 ms/frame (-2.21).** Both
  replay gates are bit-identical for every switch combination.
* **The 27.6 % `[unknown]` is not a JIT.** With full DSO attribution the
  walker is **80.4 % our own symbolized binary, 11.3 % libc**
  (memmove/syscalls) and **8.3 % libnvidia-glcore** (Vulkan driver), in a
  whole-run capture that includes boot and menus. The driver time is driven
  by per-draw recording in `vk_eng_draw_from`: `vkCmdBeginRenderPass` 14.5 %
  of driver time, `vkCmdDispatch` (vertex unpack) 12.8 %, descriptor
  alloc/update ~7 %, `vkCmdEndRenderPass` 4.1 %, `vkCmdBindPipeline` 5.7 %,
  `vkQueueSubmit` 2.3 % (table below).

## 1. Method

**Offline.** `replay_rsx_engine` on the meter capture
(`recomp-work/meter_frame`, 502 draws/frame), 240 frames after 20 warm-up,
`REPLAY_NULL_HASH=0` for timing. Three repeats per configuration; the table
reports the median of the per-frame medians and the min/max of the three
repeats. `PS3RECOMP_WALKER_PROF2=1` for the per-term split (it adds 11-20
`clock_gettime` calls per draw; identical in every arm). Exactness is the
shipping gate:

```sh
cd recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
# output hash a6fcc849341eba9b, gate ff2a220c28750bc3
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_frame --frames 5 --warmup 2
# output hash 416abee2c5bff47d, gate 3e6f6ad4ebb23ed6
```

**Live.** Two `ab-tools/ab_live.sh` meter launches, `METER=1 SKIP_CINE=1`,
`WCB=2`, `PIN=0`, x11, `PS3RECOMP_TEX_DIRTY=1` plus the four cheap-win
switches (`SPU_MFC_LOOKUP_CACHE`, `GCM_NOP_SKIP`, `GCM_REF_IMMEDIATE`,
`PS3RECOMP_RSX_TEX_PIN`), `PS3RECOMP_WALKER_PROF2=1`,
`PS3RECOMP_FLIP_LOG=1`, 6 x 10 s per arm with 2 s dropped at each switch.
Frame times below are per game flip from `[fliplog] fin` intervals inside the
measured windows, not the every-10th `[flip]` decimation; the resulting fps
matches the `[flip]` counter. The walker phases are the report's
`[walker-prof]` means over the same windows. Run 1: A off / B all three.
Run 2: A off / B remap cache only.

## 2. Item 1: `rsx_vertex_remap_build` (19.1 % of walker samples)

### Why it runs per draw

`rsx_vertex_remap_build` (`libs/video/rsx_vertex_compact.c:819`) collapses
repeated `(vertex_id, base_index)` references in first-use order. It is
enabled by default (`s_vtx_remap = 1`) and `dc_fetch` calls it for every
indexed group with more than one reference
(`libs/video/rsx_draw_engine.c:4910`). Its internal ascending fast path only
catches `draw_arrays` and index runs that are already strictly ascending; the
meter's meshes repeat vertices, so the function builds an open-addressing
table of `2 x count` slots on every draw of every frame.

The offline replay profile shows the loop directly (1,200-frame replay,
`perf record -F 999`, no call graph): `rsx_vertex_remap_build` is **20.3 %**
of all samples. `perf annotate` inside it:

| self | instruction | role |
|---:|---|---|
| 32.9 % | `cmpl %r13d,0xc(%rax,%r9)` / `jne` | generation tag probe, random slot read |
| 12.5 % | `movl %edx,0x8(%r8)` | store unique index |
| 8.2 % | `je` after the key compare | key hit |
| 3.8 % | `andl %ebp,%r12d` | table mask |
| ~15 % | two `imulq` hash mixes | splitmix finalizer |

It is a random-access table build: the cost is cache misses on the slot
array, not arithmetic. Per-draw caching (below) removes it.

### The cache

`PS3RECOMP_REMAP_CACHE=1` memoizes the collapse result keyed by the draw's
source reference list (`eng_refs_key`, one FNV-1a pass over the refs, shared
with the fetch key). An entry stores only the source count, unique count, the
unique prefix and the occurrence map; it holds no guest bytes. On a hit the
accumulator gets the same unique list and occurrence map that
`rsx_vertex_remap_build` would have produced, and the index writer reads the
same map. An uncollapsed hit leaves the already-built source refs in place; a
collapsed hit copies the stored prefix and map. Capacity is 1,024 entries (the
first 256-entry version thrashed at ~500 draw groups/frame: 18 hits and a
+1.1 ms regression).

Exactness: the key is the complete reference list (both words per reference),
so a different index array misses. The memo is independent of the fetch plan,
so a draw whose transform/buffer registers changed but whose references did
not still hits. Guest payloads are never cached. Unit test
`test_remap_cache` checks the first build, an identical-list hit, that the
remap memo does not enable the other memos, and that changed index bytes miss.

### Numbers

Offline, remap cache alone: frame **10.283 -> 8.542 ms** (-1.74) and
`fetch_plan` **2.734 -> 1.011 ms** (497/502 groups hit). Live it hits
**28.5 %** (run 1, inside the fetch+remap arm) and **30.2 %** (run 2, alone):
the live meshes' index/vertex sets genuinely change most frames, and the
remaining misses still pay the hash and the save. Live remap-only: `fetch`
3.961 -> 4.270 (+0.31), walker 17.68 -> 17.73, fps 34.93 -> 35.08. **Not a
live winner; left off.** The offline win is real but the one-frame replay
overstates it.

## 3. Item 2: fetch plan cache and a cheaper key (plan item 4)

The existing cut (`PS3RECOMP_WALKER_FAST`, `rsx_draw_engine.c:4867-4950`)
memoizes the prepared `rsx_vertex_fetch_plan` and its remap keyed by the
layout, all sixteen decoded VTXFMT descriptors/defaults, base/divider and the
complete reference list. This mission split it into `PS3RECOMP_FETCH_MEMO`
(and `PS3RECOMP_PIPE_MEMO`), and shares one `eng_refs_key` pass between the
fetch and remap memos instead of hashing the list twice.

**Why `WALKER_FAST` was ~2 fps slower live in `walker-cuts.md`.** It is not
measurement noise. In run 1 the fetch memo hit **5.4 %**: 37,463 hits vs
658,463 misses over the measured windows (arm B). Offline the same code hits
497/502 because the replay is one captured frame repeated. Live, the c690
producer rotates vertex buffers and index sets every frame, so the absolute
`base_offset`/reference set in the key changes and the memo misses almost
every draw. On a miss the fast path still pays:

* `rsx_vertex_fetch_plan_init` (decode 16 attributes and defaults),
* the FNV key over the layout, descriptors, base/divider and the full
  reference list,
* `eng_fetch_memo_find` (linear over 1,024 entries),
* `eng_fetch_memo_save` (copy the unique refs and occurrence map).

That is the opposite of the offline case, hence `fetch` 4.080 -> 6.493 ms
(+2.41) live while offline it is 2.734 -> 1.350 (-1.38). The cheaper key
(one shared hash instead of two) is correct but cannot fix a structural miss;
the memo would need to be keyed relative to the rotating buffer base, which is
a different change. **Left off**, and `play_windowed.sh` carries a comment so
it is not flipped on by name.

## 4. Item 3: pipeline memo (plan item 5)

`PS3RECOMP_PIPE_MEMO=1` uses the existing pipeline answer memo
(`eng_pipeline_get`): fingerprint of the vertex/fragment program bytes, the
pipeline-relevant registers, every enabled texture unit's raw words, the
input layout and the render state. A hit restores the pipeline handle,
`tex_used`, the fragment debug hash, the `vp_c` range and the buffered
fragment constant block without walking either program. This *is* the
program-analysis cache: on a hit `pipe_analyze`, `pipe_key` and `pipe_dir`
are all zero.

Live run 1 (arm B, with the fetch memo also on): pipeline memo hits
**85.0 %** (587,140 / 690,807), and the `pipe` phase falls **2.107 -> 1.633
ms** in the same run where `fetch` rises. The fetch memo does not touch the
`pipe` phase, so the -0.47 ms is the memo's own (offline it is -0.84 ms).
The memo's own cost is the fingerprint, measured in `pipe_lookup`
0.156 -> 0.764 ms offline; the net is what the phase shows. **Kept on.**

Exactness: the fingerprint includes the raw program bytes, so rewriting a
program in guest memory misses (covered by the existing unit test); the
render state and texture registers are in the fingerprint; the memo stores the
exact buffered constant block it answered with. Replay gates unchanged.

## 5. Item 4: what the 27.6 % `[unknown]` was

Run 1 recorded the whole game process with
`perf record -F 199 --call-graph=dwarf,2048` (431 MB, 176,181 samples). The
`perf report` call-graph pass did not finish on a file that size, so the
analysis used `perf script --max-stack 6` and symbolized the IPs from the
binary/`nm` tables. Of **21,697 walker samples** (covers boot, menus and the
A/B windows), by the innermost frame's DSO:

| DSO | share |
|---|---:|
| `MyGameRecomp` (our code, symbolized) | **80.4 %** |
| `libc.so.6` (memmove/malloc/syscalls) | **11.3 %** |
| `libnvidia-glcore.so.615.71.09` (Vulkan driver) | **8.3 %** |
| kernel / other | <0.1 % |

A driver frame appears somewhere in **22.9 %** of walker call chains; the
edge doc's 27.6 % "unknown" is therefore not a JIT or a missing symbol in our
build. It is mostly our own code plus the Vulkan driver and libc, and it was
unsymbolized because that profile used only the first IP of each sample. The
edge doc's steady-window share is higher than the 8.3 % above because the
whole-run capture includes boot/menu frames with less driver work.

The driver time is entered from per-draw recording in `vk_eng_draw_from`
(`rsx_vulkan_backend.c`) and the present/submit paths. Call sites of the
4,962 walker samples that contain a driver frame (36.8 % of chains were
truncated at depth 6 before reaching our frame):

| walker call site | Vulkan call | share of driver samples |
|---|---|---:|
| `vk_eng_draw_from+0x1103` | `vkCmdBeginRenderPass` | 14.5 % |
| `vk_eng_draw_from+0xe35` | `vkCmdDispatch` (compute vertex unpack) | 12.8 % |
| `vk_eng_draw_from+0x111a`, `+0xd2e` | `vkCmdBindPipeline` | 5.7 % |
| `vk_eng_write_descriptors_to+0x5ed` | `vkUpdateDescriptorSets` | 4.8 % |
| `vk_eng_draw_from+0x147b` | `vkCmdEndRenderPass` | 4.1 % |
| `vk_eng_draw_from+0x7f9` | `vkAllocateDescriptorSets` | 2.5 % |
| `vk_eng_draw_from+0x128b` | `vkCmdSetViewport` | 2.0 % |
| `vk_eng_draw_from+0x1385`, `+0xd61` | `vkCmdBindDescriptorSets` | 2.4 % |
| `vk_eng_draw_from+0x13f2` | `vkCmdDrawIndexed` | 1.3 % |
| `vk_eng_draw_from+0x13ad` | `vkCmdBindVertexBuffers` | 1.1 % |
| `vk_eng_draw_from+0x12c0` | `vkCmdSetStencilReference` | 1.0 % |
| `vk_eng_flush_async`, `vk_submit_and_wait` | `vkQueueSubmit` / submit+wait | ~3.3 % |
| `vk_eng_draw_from+0xe17` | `vkCmdPushConstants` | 0.2 % |

The same per-draw recording also appears in the `VK_DRAWPROF` split in
`walker-profile.md` (vtxgpu, `rp_begin`, copy, desc, barrier). Render-pass
begin/end and the compute unpack are the two largest; batching render passes
across draws of the same target set (plan item 3 there) attacks the top of
this list.

## 6. Offline A/B (3 repeats, 240 frames)

`REPLAY_NULL_HASH=0`, `--warmup 20`. Frame ms is the harness median; phase
ms are the `[walker-prof2]` medians over the last 200 measured frames.

| config | frame ms | fetch_plan | pipe total (lookup+analyze+key+dir) |
|---|---:|---:|---:|
| off | **10.283** (10.267-10.297) | 2.734 | 1.610 |
| `PIPE_MEMO` | **9.434** (9.411-9.443) | 2.729 | **0.764** |
| `FETCH_MEMO` | **8.878** (8.877-8.887) | **1.350** | 1.602 |
| `REMAP_CACHE` | **8.542** (8.517-8.573) | **1.011** | 1.609 |
| all three | **8.077** (8.060-8.111) | 1.370 | 0.759 |

All three: -2.21 ms/frame (-21.5 %). In "all" the fetch memo hits first, so
the remap memo is idle (0 uses); remap and fetch overlap on this capture.

## 7. Live meter A/B (two launches)

### Run 1: off vs fetch+pipe+remap

| per game flip | A off | B all three |
|---|---:|---:|
| fps (per-flip `fin`) | **33.87** | **33.19** |
| p50 ms | 34.00 | 33.96 |
| p90 ms | 40.42 | 41.59 |
| p99 ms | 46.37 | 48.17 |
| mean / stddev ms | 29.52 / 10.92 | 30.13 / 10.18 |
| intervals | 1,620 | 1,586 |
| walker drain ms | 18.81 | 20.15 |
| wait ms | 10.36 | 9.80 |
| fetch / pipe / draw / other / parse ms | 4.080 / 2.107 / 5.104 / 4.936 / 1.578 | 6.493 / 1.633 / 5.024 / 4.511 / 1.500 |
| hit rates fetch / remap / pipe | - | **5.4 % / 28.5 % / 85.0 %** |

Paired fps difference (report's 6 segments): A-B 0.57 +/- 0.99, 95 % CI
[-0.42, +1.55]. The bundle does not win: the pipeline memo's -0.47 ms is more
than cancelled by the fetch memo's +2.41 ms.

### Run 2: off vs remap cache only

| per game flip | A off | B remap |
|---|---:|---:|
| fps (per-flip `fin`) | **34.93** | **35.08** |
| p50 ms | 33.88 | 33.87 |
| p90 ms | 39.24 | 39.45 |
| p99 ms | 44.66 | 44.06 |
| mean / stddev ms | 28.63 / 10.62 | 28.51 / 10.43 |
| intervals | 1,671 | 1,675 |
| walker drain ms | 17.68 | 17.73 |
| wait ms | 10.89 | 10.57 |
| fetch / pipe / draw / other / parse ms | 3.961 / 2.067 / 4.965 / 4.175 / 1.541 | 4.270 / 2.019 / 4.911 / 4.083 / 1.499 |
| remap hit rate | - | **30.2 %** |

Paired difference A-B -0.21 +/- 0.69 fps (CI -0.87..+0.45): neutral. The
remap cache is exact but the live hit rate is too low for its hash/save cost.

### What is kept

`PS3RECOMP_PIPE_MEMO=1` is exported in both copies of `play_windowed.sh`
(`recomp-work/play_windowed.sh` and `mlb16-recomp/tools/play_windowed.sh`),
with a comment that `PS3RECOMP_FETCH_MEMO` / `PS3RECOMP_WALKER_FAST` is not
to be enabled live. `PS3RECOMP_REMAP_CACHE` stays available and off.

## 8. Reproduce

```sh
# offline A/B
cd recomp-work/build-play
for cfg in "" "PS3RECOMP_PIPE_MEMO=1" "PS3RECOMP_FETCH_MEMO=1" \
           "PS3RECOMP_REMAP_CACHE=1" \
           "PS3RECOMP_FETCH_MEMO=1 PS3RECOMP_PIPE_MEMO=1 PS3RECOMP_REMAP_CACHE=1"; do
  env $cfg PS3RECOMP_WALKER_PROF2=1 REPLAY_NULL_HASH=0 \
    ./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 240 --warmup 20 \
    2>&1 | grep walker-prof2
done

# live A/B (one launch, arms through PS3RECOMP_CTL)
cd ../../recomp-work
PS3RECOMP_TEX_DIRTY=1 SPU_MFC_LOOKUP_CACHE=1 GCM_NOP_SKIP=1 GCM_REF_IMMEDIATE=1 \
PS3RECOMP_RSX_TEX_PIN=1 PS3RECOMP_WALKER_PROF2=1 PS3RECOMP_FLIP_LOG=1 \
PS3RECOMP_FETCH_MEMO=0 PS3RECOMP_PIPE_MEMO=0 PS3RECOMP_REMAP_CACHE=0 \
ARM_A=$'PS3RECOMP_FETCH_MEMO=0\nPS3RECOMP_PIPE_MEMO=0\nPS3RECOMP_REMAP_CACHE=0' \
ARM_B=$'PS3RECOMP_FETCH_MEMO=1\nPS3RECOMP_PIPE_MEMO=1\nPS3RECOMP_REMAP_CACHE=1' \
N=6 SEG=10 DROP=2 TAG=walkq METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
./ab-tools/ab_live.sh
```

Unit tests: `libs/video/tests/test_rsx_draw_engine.c` gains
`test_remap_cache`; all 291 checks pass (compiled standalone with the engine,
the compact vertex helper, the dispatcher and the texture/VP/FP helpers).

## Caveats

* **The offline replay is one static frame.** It cannot model the live
  reference-set churn, which is exactly why the fetch memo looks like a win
  offline and is a loss live. Treat the offline deltas for `FETCH_MEMO` and
  `REMAP_CACHE` as upper bounds, not predictions.
* **Run 1's perf capture is whole-run** (boot + menus + A/B), not a 20 s
  steady window; the driver share of walker samples is therefore lower than
  the edge-step1 steady-window figure. Run 2's perf capture failed (the
  watcher attached to the wrong wrapper PID), so the call-site table is from
  run 1 only.
* `perf report` with a call graph did not complete on the 431 MB file; the
  numbers above come from `perf script` plus `nm`, which is the same IP-level
  data.
* Live A/B windows are 6 x 8 s measured per arm after a 2 s drop. The title's
  meter still alternates short and long frames (stddev ~10 ms), so p50 moves
  less than the 0.4-1.3 ms walker deltas; the walker phase table is the
  higher-resolution measurement.
