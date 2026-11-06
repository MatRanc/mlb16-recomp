# Native DXT, step 1: job identification stopped on a code-attribution gap

Date: 2026-11-03. Static/offline work only: no game was launched and nothing
outside the `ps3recomp-dxt` worktree was edited. Sources: the EBOOT in Ghidra,
the lifted SPU tree in `recomp-work/spu_lifted` (read-only), and the existing
run `recomp-work/play_20261103_202606` (log + `hitches.pstl`). No GPL emulator
source was read.

**Result: no native encoder was written.** The DXT workload and its PPU-side
job submission are confirmed, but the phase-1 rule ("if the job is not what the
analysis said, stop and report") applies to the *code* attribution: no lifted
job module can be shown to be the DXT kernel, and there is no captured DXT
input/output corpus to make a bit-exact port provable offline. The rest of this
note records what is established and the one capture that would unblock it.

## TL;DR

| item | finding |
|---|---|
| DXT workload is real and hot | `spu.policy` spans (a=image, **b=guest wid**) give wid 0: **355 quanta / 33,988 ms** in the 18-36 s boot hitches and **143 / 4,479 ms** in the 216-219 s replay window. These match `boot-spu-hitches.md` (33.5 s, 355 quanta; 917 ms average-lane) exactly. |
| PPU-side submission | `FUN_0006e78c` -> `FUN_0006e650` creates the `Wws_Job/DXTJob` workload; `GpDxtCompressor` (`FUN_00a566e8`) fills a **0x1080-byte WWS job source** and a **0x30-byte parameter block**. Inputs: `0x500AA93`, `0x500AAE4`, `0x500AA1B`; outputs: `0x600AAE4`/`0x700AAE4`/`0x800AAE4` = DXT1/DXT3/DXT5. |
| Job-medium | `cellSpursAddWorkloadWithAttribute` is given the shared PM image at `0x0105BB80` (12032 bytes, `wwsjob_pm`, image 6). The per-job code is streamed separately into the job code buffer. |
| Code attribution | **No `spu.overlay_load` event falls inside a wid-0 policy span.** The jobmod overlay loads in the boot window (`jobmod_7ac`, `jobmod_255e`) land under wid 3 (nextsynth) accounting; their lifted code is FMA/normal-map/vertex math, not DXT. The only modules with 565/1/3 color code are `jobmod_96de` (loaded under the wid-5 / MP3-Crowd spans) and the **unlifted base-0 module at `0x010A1700`** inside the fourth embedded SPU ELF. |
| Consequence | A bit-exact encoder cannot be written from the current material without guessing which kernel is DXT. The next step is one **offline-readable capture** of the DXT job source and the code-DMA source EA (section 5). |

## 1. Confirming the workload (not the code)

`ps3recomp-play/include/ps3emu/ps3_trace.h` documents `PS3_TR_SPU_POLICY` as
`a = image id, b = workload id`, and `libs/spurs/cellSpurs.c:1903` emits it
with `slot->wid`. Parsing `hitches.pstl` with that meaning gives, for the
boot (18-36 s) and the 2 s replay (216-219 s) windows:

| wid | workload (from the AddWorkload log) | boot | replay |
|---:|---|---|---|
| 0 | `Wws_Job/DXTJob` | **355 spans, 33,988 ms** | **143, 4,479 ms** |
| 1 | `Wws_Job/MeshJobs0` | - | 916, 96.3 ms |
| 2 | `Wws_Job/MeshJobs1` | - | 1,522, 173.5 ms |
| 3 | `Wws_Job/nextsynthJobLst` | 12,100, 1,597 ms | 3,201, 421.5 ms |
| 5 | `Wws_Job/LibMP3 JobList` (trace wid 5) | - | 3,188, 337.2 ms |
| 6 | `Wws_Job/KinematicsJobs` | - | 4, 3.1 ms |

The wid-0 numbers reproduce `boot-spu-hitches.md` to the quantum and to the
millisecond, so the "DXT is the boot/replay cost" premise is correct. The
earlier note's claim that the *same image 6* runs the code is also correct at
the context level: `spu.run` reports `a=6` throughout, because image 6 is the
primary image and the streamed job code is tracked separately.

## 2. What the PPU submits (established)

`FUN_0006e650` (`EBOOT.elf:0x0006e650`, called from `FUN_0006e78c`):

* allocates 0x1080 bytes (0x80 header + 0x1000 job source) and names the job
  list `DXTJob`;
* adds the cellSpurs workload with the shared PM image `0x0105BB80`
  (size 0x2EC0 = 12,032) and **maxContention 4** (the log's `maxC=4`).

`GpDxtCompressor::CompressImage` (`FUN_00a566e8`, `0x00a566e8`) then builds the
job source. The builder calls are `FUN_00da7f14/7f28/7f68/7fc4/8018/8044/8098/
80dc/812c`; they encode 40-bit WWS commands: DMA-list entries, tag wait
`0x6|0xf`, a store command `0xa|…`, and a `memcpy` of the parameter block.

The 0x30-byte parameter block is written at `r1+0x70` and copied to the job
source at `0x00a56a3c`:

| off | field |
|---:|---|
| 0x00 | source EA (`r19` = `param_3`) |
| 0x04 | input width (`r26` = `param_5`; assert `< 2048`) |
| 0x08 | input height (`r23` = `param_6`) |
| 0x0c | unused |
| 0x10 | channel swizzle word 0: `0x03000102` / `0x01000203` / `0x03020100` |
| 0x14 | swizzle word 1: `0x07040506` / `0x05040607` / `0x07060504` |
| 0x18 | swizzle word 2: `0x0b08090a` / `0x09080a0b` / `0x0b0a0908` |
| 0x1c | swizzle word 3: `0x0f0c0d0e` / `0x0d0c0e0f` / `0x0f0e0d0c` |
| 0x20 | mip / allocation argument (`r18`) |
| 0x24 | **output code: 1 = DXT1, 3 = DXT3, 5 = DXT5** |
| 0x28 | stack argument 0x1a4 |
| 0x2c | stack argument 0x1ac |

The three swizzle sets correspond to the three accepted source formats
(`0x500AA93`, `0x500AAE4`, `0x500AA1B`); the output formats are
`0x600AAE4` (DXT1), `0x700AAE4` (DXT3), `0x800AAE4` (DXT5). The code asserts
input width < 2048, both dimensions divisible by `2 << mipCount`, and a
scratch pointer for multi-mip jobs; the DMA input buffer is limited to 32 KiB
per transfer (`inputDmaSize < 1024*kInputBuffer`).

This is all the layout evidence needed for a native replacement *if* the kernel
side is settled: the encoder input is raw 8-bit RGBA/BGRA (channel order from
the swizzle table), the output is DXT1/3/5, both are host-endian in LS, and
the block loop is the caller's DMA unit.

## 3. Which lifted module is the kernel? Not resolvable from this tree

The seven lifted job modules (registered as verified streamed regions, image
ids 200-206) are:

| image | EA | sig (first 8 bytes) | image size | entry | lifted content |
|---:|---|---|---:|---:|---|
| 200 | `0x0105EE00` | `42f21202433fb882` | 0x00DAD0 | 0x80 | 56 KB, FMA + `frest`/`frsqest`, normal/vertex math |
| 201 | `0x0106D380` | `42e64802425a3682` | 0x00B3F0 | 0x80 | 46 KB, same family |
| 202 | `0x01079180` | `42894e02427cde82` | 0x019430 | 0x11A88 | 103 KB, 130 `mpyu`, 0x5556 |
| 203 | `0x01093280` | `420a7c02424eae82` | 0x00D740 | 0x9850 | 55 KB, geometry |
| 204 | `0x01161300` | `420f2e024285de82` | 0x010860 | 0xF5B8 | c690 tree/cull |
| 205 | `0x0117EF40` | `43626602427cca82` | 0x000CB0 | 0x0AD8 | 3.2 KB, 565->888, 0x5556, 2-bit ops |
| 206 | `0x0117FC40` | `43b8600242551282` | 0x0049C0 | 0x45C8 | 19 KB, geometry |

Correlating `spu.overlay_load` (EA + image id) with the enclosing
`spu.policy` span gives the following (all 19 hitches, lane-ms):

| workload | modules loaded inside its policy spans |
|---|---|
| wid 0 DXT | **none** (no overlay load) |
| wid 1/2 Mesh0/1 | `c690` (204) |
| wid 3 nextsynth | `7ac` (200) + `255e` (201) |
| wid 5 (MP3 per the replay timing) | `96de` (205) |
| wid 6 Kinematics | `2387` (206) |

There is also an **unlifted eighth** `C0DEC0DE` module embedded in the engine
ELF at `0x010A1700` (sig `42465602431a9282422d77024363a382`, image 0x2C30,
entry 0x30, load base **0** rather than 0x4000). `lift_jobmods.py
--engine-elf` skips it because it checks for load base 0x4000. A one-off lift
of it (base 0, offset 0x30) shows a valid WWS job skeleton plus 565/`0x5556`
color interpolation and shufb masks `0x0303..0x1D1D` — the only other
DXT-shaped code in the tree. It is a *candidate*, not a proof: it is the
base-0 program of the fourth embedded SPU ELF, not a module the PM streams
into the 0x4000 job buffer.

Why this blocks phase 1:

* `spu.run` reports image 6 for every lane, so the trace's primary id cannot
  name the streamed kernel (the code is tracked in `resident_ovl`).
* The DXT workload's wid-0 spans contain **zero** registered overlay loads, so
  the kernel is either the PM image (which has no DXT math: `wwsjob_pm` has no
  0x5556, no 565 expansion, no 2-bit palette loop) or an unregistered/unlifted
  module.
* The modules that *do* have DXT-shaped color math (`96de`, the base-0 module,
  and to a lesser extent `c566`) are attributed by the trace to other
  workloads or are not lifted at all.
* There is no captured DXT input/output corpus on disk (the only corpus is
  c690 stage pairs), so no candidate kernel can be validated bit-exactly
  offline.

Guessing among these would violate the bit-exact requirement: the output feeds
the game's own texture caches, and "close" DXT endpoints/indices are a
different cache entry.

## 4. What a native encoder would need (design only)

Once the kernel is named, the replacement follows the c690/Edge pattern:

1. **Hook at the job level**, not the PM: intercept the WWS job dispatch for
   wid 0, read the 0x30-byte parameter block from the captured job source
   (source EA, width, height, swizzle, mip, outfmt), and call the native
   encoder for the job's DMA units. Leave the PM, ring allocation and the
   output PUTs lifted.
2. **Layout guard** from the parameter block: width/height/mip/outfmt and the
   swizzle word identity. Any unknown tuple falls back to the lifted module
   and increments a counter; the c690 switch is the precedent
   (`PS3RECOMP_C690_NATIVE_CULL`, default off).
3. **Default-off switch** `PS3RECOMP_NATIVE_DXT` (env + live control key),
   inert when unset: no allocation, no registration, no code path change.
4. **Corpus capture for the live check**: log the job source, the code-DMA
   source EA, the module's 16-byte header, and the input/output LS ranges for
   every sampled DXT job. That single capture resolves the attribution gap and
   supplies the differential corpus at the same time.

The expected saving is the wid-0 lane time: 33.99 s in the 14 boot hitches and
4.48 s in the 2.05 s replay. On four lanes the boot number is ~8.5 s of the
10.1 s hitch window; a native encoder at the c690/Edge order of speedup would
remove most of that, subject to the hand-off floor measured for c690
(`boot-spu-hitches.md` §1.4 and `edge-native-step2.md`).

## 5. The one capture that unblocks step 2

Offline, and without a new launch, the question is already answerable *if* the
next run records:

* `SPURS_PM_TRACE=1` (or the runtime's `[spurs-pm] DMA` trace with the
  "suppressed from here" limit raised) across the first 40 s, and
* one one-shot hook that, when `slot->wid == 0`, copies (a) the 0x1080 job
  source, (b) the source EA of every `spu_overlay_note_get` in the job's
  window, and (c) the 16-byte header at the streamed LS base.

The DXT job's code-DMA EA plus its 16-byte signature then names the module;
if it is `0x010A1700` or an SDATA copy, the tree needs that module lifted and
registered before any encoder work starts. Until then, step 2 (bit-exact
encoder + harness) is not started.

## 6. What was not done

* No native encoder; no SSE/AVX2 code; no harness.
* No hook and no switch were added to the live runtime.
* No corpus was captured (phase 1 forbids game launches, and none exists on
  disk for DXT).
* No code changes in `ps3recomp-play`, `build-play`, or `recomp-work`.

Attribution: the differential/corpus method is the one used for NHL Legacy
Native (MIT, (c) 2026 HeckYouMan); no NHL or GPL emulator code was copied.
