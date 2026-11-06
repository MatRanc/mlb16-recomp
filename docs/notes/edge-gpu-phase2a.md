# c690 Edge on the GPU — phase 2a (contradiction, captured formats, 0x2A kernel, verdict)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two automated pitch-meter launches
(`ab_live.sh`, `TAG=fncap21` and `fncap21b`, governor `performance`) plus
offline static analysis, the lifted c690 module, the captured LS/GPR images,
and a headless Vulkan compute kernel. Nothing was wired into the runtime and
nothing is default-on.

This follows [`edge-gpu-phase1.md`](edge-gpu-phase1.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690_function_map.md`](c690_function_map.md),
[`targets-and-backlog.md`](../targets-and-backlog.md) and
[`pin-and-burst-path.md`](pin-and-burst-path.md).

## TL;DR

* **The phase-2a plan as written is wrong and is stopped.** A GPU 0x21 that
  leaves its result in VRAM removes **0 ms** of SPU critical-lane time: the
  same job's 0x2A assembler reads **100 % of all four** planar arrays 0x21
  produces (normal, position, colour, uv0), not just positions. The 0x2A
  stage must therefore also run on the GPU (the 0x25 cull is position-only and
  can go with it). The GPU pass must cover the whole geometric tail.
* **Evidence, not inference.** Two real pitch-meter captures, the lifted
  functions run on those images, and a line-level read/write trace:
  - 0x25 cull reads **only** its position array (235/235 lines in
    `0x204C0..0x21370`; 0 lines in the normal array at `0x1F6C0`).
  - 0x26 cull reads a 640-line position block; only the first `0xB0` bytes of
    the next array (`0x21ED0`), none of the third (`0x24610`).
  - 0x2A assembler reads **200/200, 200/200, 200/200, 199/199** lines of
    `0x1F6B0` (normal), `0x20330` (position), `0x20FB0` (colour) and
    `0x21C30` (uv0): every packed value, every vertex.
* **What is actually removable.** 0x21 is 42.9 % of the c690 wall
  (`~10.5 ms` of the 24.39 ms critical lane; selector 1 `~6.1 ms`, selector 7
  `~4.4 ms`), 0x2A is 15.7 % (`~3.83 ms`) and the cull 12.7 % (`~3.10 ms`).
  A full tail (0x21 + cull + 0x2A) removes `~17.4 ms` of the lane, i.e. a
  burst of `35.0 -> ~17.6 ms`. The smallest honest pass (0x21 + 0x2A) removes
  `~14.3 ms` (`-> ~20.7 ms`). 0x21 alone removes `0`.
* **Built and proved one tail kernel.** The 0x2A assembler is a GPU compute
  kernel (`c690_2a_assembler.comp`) and is **bit-exact** against the lifted
  SPU run on two real captures and on a synthetic colour stress image
  (0 differing bytes, max float error 0, max u8 error 0). Its unit test is
  `test_c690_2a_shader.c`. The 0x21 transform kernel is **not** signed off:
  the pitch job's real 0x13140 input is the A730 format-conversion/table path,
  not the validated 12/12/11 stream; a stage-corpus is needed first (2b).
* **Verdict: KILL phase 2a as scoped.** Do not wire the walker, do not default
  anything on. Go to 2b only with the full-tail plan in §6.

## 1. The contradiction, resolved

### 1.1 What 0x21 writes

The captured pitch job is a real `0x13080` dispatch on a real game image
(`/tmp/opencode/fncap21b/0x13140`, input GPRs `r3=0x1F158`, `r80=0x1F000`,
`r6=0x00000001`). It takes the **A730** sub-branch (`r6 & 16 == 0`) and writes
**364 16-byte records** at `0x1F750..0x20E00`. The job's own source table at
LS `0x1F260` holds two entries (`0x1F750`, `0x20E50`), and the descriptor at
`0x1F2B0` repeats them.

Other jobs in the same runs use the BB80 branch: the first `0xBED0`/`0xC018`
entries were captured at `08:58:51`, i.e. the BB80/`BED0/BF00/C018` body also
runs at the pitch. `0xBB80` itself is a direct call, so the `SPU_FN_CAPTURE`
hook (which sits on the table-dispatch lookup) cannot capture it; `0xBED0` and
`0xC018` are reached by `bi` and are captured.

### 1.2 What the cull reads

Method: run the captured `0x13520` (opcode 0x25) and `0x13518` (opcode 0x26)
entries through the lifted module with a trace on `spu_ls_read128` /
`spu_ls_write128`; classify the touched 16-byte LS lines against the job's
own array table.

| job | array table at `cmd+0x2B0` | cull read lines | conclusion |
|---|---|---|---|
| 0x25 (`0x13520`) | `0x204C0` (position: `-153.8, 0, -30.3`), `0x1F6C0` (normal: `0,1,0,0.33`) | **235/235** in `0x204C0..0x21370`; **0** in `0x1F6C0..0x204C0` | position array only |
| 0x26 (`0x13518`) | `0x1F790` (position), `0x21ED0` (normal: `0.144,0.013,-0.989,-1`), `0x24610` | 640 contiguous lines `0x1F790..0x21F80`; includes the first `0xB0` at `0x21ED0`; **0** of `0x24610` | position block + an 11-record (0xB0) normal slice |

So a "cheap position-only transform for cull" split is technically possible
for 0x25, but 0x26 already touches a normal slice, and neither split removes
the full transform/pack work because of 0x2A below.

### 1.3 What the assembler reads

Same method on the captured `0x10F78` (opcode 0x2A) entry. The assembler's
source table at LS `0x1F260` is exactly the four planar arrays:

```
0x1F260:  0001F6B0  00020330  00020FB0  00021C30
          normal    position  colour    uv0
```

Read lines (16 bytes each) over the lifted run:

| array | range | lines read / present |
|---|---|---:|
| normal | `0x1F6B0..0x20330` | **200 / 200** |
| position | `0x20330..0x20FB0` | **200 / 200** |
| colour | `0x20FB0..0x21C30` | **200 / 200** |
| uv0 | `0x21C30..0x228A0` | **199 / 199** |
| output | `0x2F870..` 193 x 36-byte records | 441 lines written |

**Answer:** the cull reads positions (0x25) and positions plus a small normal
slice (0x26); the assembler reads the **complete** 0x21 output for all four
attributes. A GPU 0x21 whose output stays in VRAM would force the SPU to read
back every array before 0x2A, or leave 0x2A unable to run. A position-only
split does not change that: 0x21's normals/uv/colour still have to exist for
0x2A.

### 1.4 SPU time that would actually be removed

Frame shape from [`pin-and-burst-path.md`](pin-and-burst-path.md) /
[`frame-pacing-floor.md`](frame-pacing-floor.md): 429 jobs/frame, five lanes,
critical-lane busy **24.39 ms**, inter-job gaps 10.61 ms, burst wall 35.00 ms,
frame p50 34.15 ms. Shares from [`c690-emit-ring.md`](c690-emit-ring.md) /
[`edge-native-step5.md`](edge-native-step5.md).

| pass | stages | critical-lane time removed | burst wall (model) |
|---|---|---:|---:|
| 2a as written (0x21 only, result in VRAM) | — (SPU must still produce it for 0x2A) | **0 ms** | 35.0 ms |
| 0x21 with per-job readback | + 4 arrays x ~3.2 KB + one fence/job | negative (adds 2.8+ ms) | > 35 ms |
| 0x21 + 0x2A | 10.5 + 3.83 | **~14.3 ms** | ~20.7 ms |
| 0x21 + 0x25/0x26 + 0x2A (full tail) | 10.5 + 3.10 + 3.83 | **~17.4 ms** | ~17.6 ms |

Per selector: 0x21 sel 1 is 58.2 % of 0x21 = **6.1 ms** of the lane; sel 7 is
41.5 % = **4.4 ms**. Neither is removable alone.

**Re-plan.** Phase 2a must cover the **whole geometric tail**. The only useful
split is "SPU keeps ring allocation, the command hole and the fallback; the
GPU produces the final 36-byte records and the kept-index list; the walker
binds both". That is exactly phase-1 option (i), not the 0x21-only spike.

## 2. Capture

Two runs, both `ab-tools/ab_live.sh`, build `build-play`, at the pitch meter
(adds are not part of the standard play configuration):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
TAG=fncap21b N=1 SEG=2 DROP=0 SETTLE=8 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 PITCH_THRESH=35.0 LIMIT=420 BOOT=40 ARMS='A=' \
  SPU_FN_CAPTURE=0x13520,0x13518,0x13510,0x13504,0x13140,0x131A0,0x10F78,0xBB80,0xBB98,0xBED0,0xC018:/tmp/opencode/fncap21b \
  ./ab-tools/ab_live.sh
```

The first run (`fncap21`, log `ab_live_fncap21_20261104_085124.log`) used
`0x13140,0xBB80,0xBED0,0xC018,0xA730,0xB830,0x132E0,0xD080,0x10F78,0x131A0,0xBB98,0xA3E0` (the two direct-call entries were not captured); the second
(`fncap21b`, log `ab_live_fncap21b_20261104_085643.log`) added the cull and
assembler entries. Both confirmed the pitch meter (`meter_*.png`) and the
capture fired at `08:53:33-35` / `08:58:50-51`, before the meter press.

Captured (never committed; copied to `recomp-work/captures/fncap21/`):
`0x13140` (A730 path, real pitch job), `0x131A0` (selector 7, A3F8 path),
`0x10F78` (assembler), `0x13520`/`0x13518` (cull 0x25/0x26), `0x0A730`,
`0x0B830`, `0xBED0`, `0xC018`.

`0xBB80`/`0xBB98` directories are absent because those are direct calls; the
capability is proved by the `0xBED0`/`0xC018` captures (only reachable through
`0xBB80 -> 0xBBB0 -> 0xBCB4`). Forcing bit 4 of `r6` on the real `0x13140`
input also runs the BB80 body on real source arrays (`outa_real_bb`), but that
is a hybrid input class, not a validated game format.

## 3. GPU kernel: 0x2A assembler (built, exact)

Because the contradiction moves 2a to the tail, the first kernel built is the
stage that forces it: the 0x2A assembler. It is a gather + format pack per
vertex:

```
[UV0.xy f32][COLOR0 u8x4][POSITION.xyz f32][NORMAL.xyz f32]   36 bytes
```

The kernel (`tools/replay/c690_2a_assembler.comp`) reads the four planar arrays
from a storage buffer holding the LS image and writes the records at
`0x2F870`. The colour conversion is the SPU rule, recovered from the lifted
`0x7D40` chain: `+0.5` bias, truncate toward zero, saturate to `0..255`.

Test (`tools/replay/test_c690_2a_shader.c`) runs the captured `0x10F78`
through the lifted module in-process (reference) and through the Vulkan kernel,
then diffs all `193 x 36 = 6948` bytes:

| input | differing bytes | max float error | max u8 error | result |
|---|---:|---:|---:|---|
| real capture `fncap21b/0x10F78` | **0** | 0 | 0 | PASS, bit-exact |
| real capture `fncap21/0x10F78` | **0** | 0 | 0 | PASS, bit-exact |
| synthetic colours (`0,1.4,1.6,2.5,127.6,255.9,300,-1,...`) | **0** | 0 | 0 | PASS, bit-exact |

That is exact / 0 LSB over every packed value, well inside the phase-1 gate
(`<= 1 LSB on <= 0.1 %`). The harness exits non-zero on any byte, so it is the
unit test for the diff.

Timing on the RTX 3060 Ti, same host:

| operation | time |
|---|---:|
| lifted `0x10F78` (50 iters, no trace) | **14.5-19.5 us/call** |
| GPU single dispatch + submit + wait | 72-174 us |
| GPU batched, N=64 in one submission | **10.7-14.2 us/dispatch** |
| GPU batched, N=429 in one submission | 13.3-28.7 us/dispatch |
| Vulkan transport, batched N=64 (empty kernel, `test_vk_sync`) | 2.00 us p50 / 2.16 us mean |

The assembler is **overhead-bound**. It is a correct building block, but moving
only 0x2A cannot move the frame: 14.5 us/call x 429 / 5 lanes = ~1.24 ms/lane,
and the GPU batched burst costs more than the compute it replaces.

**0x21 kernel: not signed off.** The real `0x13140` at the pitch is the A730
format-conversion/table path (jump tables at LS `0x143D0..0x14480`, per-batch
format constants, `shufb`/`fma` conversion), not the 12/12/11 MSB-first
position stream that the existing validated `c690_vert.comp` covers
(`tools/replay/test_c690_shader.c`). Its input class was not in the corpus, and
the BB80 job's `0x13140` input cannot be captured by the current hook. A
bit-exact 0x21 shader built from a forced/hybrid input would repeat phase 1's
mistake. No diff is claimed for 0x21; this is the first 2b task.

## 4. A/B

Not run. The phase-1 kill criterion is applied to the contradiction result:
the spike removes **0 ms** of SPU time, so a meter A/B would only measure
noise, and §4 says to stop after step 2 when real SPU time is not removed. The
two launches were capture-only; the 3-launch cap was not reached (2/3).

No runtime key was added, no walker binding was written, no `spu_context`
field was added (the ABI check passes:
`spu-context ABI check: shared field offsets agree`), and nothing is on by
default.

## 5. Verdict

**KILL phase 2a as scoped.** The 0x21-only GPU pass cannot remove SPU time
because 0x2A consumes the complete 0x21 output. Wiring the walker or running
the A/B would not test the intended change.

The premise "Edge on the GPU" still holds; the unit of work is wrong. The
smallest viable GPU pass is **0x21 + 0x25/0x26 + 0x2A** (full tail), expected
to remove `~17.4 ms` of the 24.39 ms critical lane and take the burst from
35.0 ms to about 17.6 ms. That is the only version worth a walker binding and
an A/B.

## 6. Phase-2b plan

1. **Stage-level corpus for the real 0x21 formats.** Capture `0x13140` and
   `0x131A0` input+output with the format word (`r6`) and job id, at the meter,
   for both the A730/A3E0 and BB80/BB98 branches. The current hook captures
   only first dispatch of table-dispatched entries; add a direct-call probe for
   `0xBB80`/`0xBB98` or capture at `0x13140` entry with a call-site tag.
2. **0x21 kernel exactness gate** over that corpus: positions, normals, uv and
   colour; RZ/FTZ and `frsqest`/`fi` emulation for the normalize path; accept
   `<= 1 LSB` on `<= 0.1 %` and zero cull changes.
3. **Cull and index.** 0x25 is position-only; 0x26 needs the position block plus
   the small normal slice. Port `0xD080` with the existing union rule so the
   eye holes stay closed (the union keeps the original list untouched and adds
   the 2x-grid keeps). 0x27 index copy is a small kernel.
4. **Keep the 0x2A kernel** (already exact).
5. **One GPU submission per burst.** GPU-resident vertex/index ring mirroring
   the five mesh rings; SPU keeps ring allocation, the self-JUMP hole and a
   per-layout fallback. The walker binds the GPU buffer for the ring EAs and
   waits on the slot park; no per-job fence and no payload readback.
6. **Gate.** Burst wall `<= 28 ms` and p50 `<= 30 ms` with fallback `<= 5 %` of
   c690 jobs; stop otherwise. The 21 ms target still needs the gap/dispatch
   work, not just the tail.

Risks: format coverage (A730/BB80 jump tables and per-batch constants), GPU
RNE vs SPU RZ/FTZ on the packed normals, borderline cull triangles, and the
slot-ordering contract. Each has an offline gate in the corpus before a live
binding.

## 7. Reproduction (offline)

```sh
# references and kernel gate
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I ps3recomp-play/runtime/spu -I ps3recomp-play/include \
  -I mlb16-recomp/tools/replay \
  -o /tmp/opencode/gpu2a/test_c690_2a_shader \
  mlb16-recomp/tools/replay/test_c690_2a_shader.c \
  /tmp/opencode/c690gpu/reg_c690.c \
  recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  ps3recomp-play/runtime/spu/job_c690_native.c -l:libvulkan.so.1 -lm

/tmp/opencode/gpu2a/test_c690_2a_shader \
  /tmp/opencode/fncap21b/0x10F78 \
  mlb16-recomp/tools/replay/c690_2a_assembler.comp \
  /tmp/opencode/gpu2a/c690_2a.spv

# transport numbers
mlb16-recomp/tools/replay/test_vk_sync
```

The LS/GPR capture format is produced by `SPU_FN_CAPTURE` (see
`docs/notes/edge-gpu-phase1.md` §6); the read/write trace harness used for §1 is a
private research build of the lifted module with a hook in
`spu_ls_read128`/`spu_ls_write128`.

## 8. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our
runtime/walker code and our own captures. The stage/corpus/differential method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2
PC port (MIT, ThiagoFraga761 and contributors), design reference only. No
RPCS3 or other GPL emulator source was read or cited.
