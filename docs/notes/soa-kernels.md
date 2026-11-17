# c690 native kernels from the algorithm: normals and tangents

Date: 2026-11-17. ps3recomp branch `exp/soa-kernels` (worktree `ps3recomp-soa`, from `port/stable` 5726ebe3).
Key `PS3RECOMP_C690_SOA` (live, default 0; 1 = native, 2 = shadow compare). Verified offline and live in-game.

## What 0xFDA0 really is

`0xFDA0` was labelled "tree traversal / cull". It is not. Read from the SPU disassembly, `0x10190` regenerates
per-vertex normals and tangents, and its four leaves are compiler-generated **scalar** float code:

| LS | role | arguments |
|---|---|---|
| `0xFA20` | clear | count, dst |
| `0xFA58` | area-weighted face normals into vertices | pos, out, ntri, ntail, idx, scale |
| `0xFCB8` | normalize | count, buf |
| `0xFDA0` | area-weighted tangents `(dv2*e1 - dv1*e2)/det` into vertices | pos, uv, out, ntri, ntail, idx, scale |

Each scalar load or store is lifted as "load 16 bytes, byte-swap, rotate, compute, build an insert mask, shuffle,
write 16 bytes back" (about 750 host cycles per triangle in `0xFDA0`, with store-forwarding stalls). That is why
flattening the lifted code gave 1.0-1.5x: it rearranged the same instructions. In the meter profile
(`proc_meter.data`) this family is about 9.3 % of all process cycles (`0xFDA0` 5.7, `0xFA58` 2.75, `0xFCB8` 0.8).

## The kernels

`runtime/spu/c690_soa_kernels.inc`: the same float operations in the same order, with x, y, z in lanes 0..2 of one
register; triangles stay sequential, so repeated vertices and aliasing behave as before. Instantiated once per float
twin (base, ftz, approx) with that twin's helpers. Not reproduced: registers r2..r79 and the callee's dead stack frame.

Offline gate (`tools/c690_ref/run_native_soa_test.sh`, 4 captured `0xFDA0` stage images; the other three functions
are driven with arguments derived from the same images): **0 differing local-store bytes in all three twins.**

| function | approx twin, lifted -> native (us/call) | ratio |
|---|---|---|
| `0xFDA0` tangents | 129.2 -> 32.8 | 3.9x |
| `0xFA58` normals | 44.9 -> 2.8 | 15.9x |
| `0xFCB8` normalize | 8.6 -> 2.8 | 3.1x |
| `0xFA20` clear | 0.41 -> 0.16 | 2.5x |

One run, quiet machine, 2000 calls per arm back to back. ftz twin: 3.6x / 11.0x / 1.6x; base: 3.1x / 8.4x / 1.5x.
This passes the plan's "first stage at least 2x" gate.

## Live game verification (2026-11-17, `build-soa`)

- **Normal/tangent kernels (`0xFA20/FA58/FCB8/FDA0`):**
  - Shadow compare (`PS3RECOMP_C690_SOA=2`, `approx` twin): 266,240 live calls across boot, menus, and pregame with
    **0 mismatches** (`[c690-soa] shadow ... mismatch=0`).
  - Meter A/B (`ab_live.sh`, `N=4`, `SEG=10s`, play settings, NVIDIA 620.70.06, `soa_ab_nv`): `0` = `40.18 +/- 1.10` fps
    (`walker 19.23 +/- 3.32 ms`) vs `1` = `40.73 +/- 0.50` fps (`walker 16.99 +/- 2.65 ms`), paired diff
    `+0.55 +/- 1.48` fps (95% CI `[-0.93, +2.03]` fps).
- **Output writers added (`0x7998`, `0x8160`):**
  - Shadow compare (`PS3RECOMP_C690_SOA=2`, `approx` twin): 20,164,608 live calls with **0 local-store byte mismatches**
    and **0 PC mismatches** (2 isolated GPR-only diffs in `0x8160`).
  - Meter A/B (`soa_out_ab`, `N=4`, `SEG=10s`, play settings, NVIDIA 620.70.06): `0` = `39.72 +/- 1.43` fps
    (`walker 20.34 +/- 1.41 ms`) vs `1` = `39.31 +/- 0.42` fps (`walker 18.45 +/- 2.64 ms`), paired diff
    `0.42 +/- 1.49` fps (`A/A check: PASS`). Matches the critical-path estimate: at ~40 fps, SPU `c690` is hidden
    behind the ~18–20 ms walker drain, so this banks SPU headroom rather than immediate meter FPS.

## Not done

- Build dir `recomp-work/build-soa` (private lifted tree `recomp-work/spu_lifted_soa`); the shared `spu_lifted` is
  untouched. `tools/apply_patches.sh --soa` applies the hook (weak, so older runtimes still link).
- Only 4 stage captures, all from one scene. The whole-command replay over the 78 `0x21` captures (`CMD21=` in the
  gate script) passes in all three twins but at 1.00x: the `0x21` command does not reach these functions, so it
  proves nothing about them. Which command calls `0x10190` at the meter is not established.
- `0xFDA0` is latency-bound (2 sqrt + 4 div per triangle in one chain); batching triangles would roughly halve it
  again but needs an aliasing check between `out` and `pos`/`uv`.
- A native index-run loop for `0xF520` (1.6 % of cycles) was written and removed: its offline check did not pass
  (the test's drive of the lifted loop returned 0, undiagnosed).
- The other hot functions (`6D68`, `7C18`, `8160`, `7998`, `AF98`, `D2B8`/`D4A8`) are Sony's hand-vectorised Edge
  kernels, not scalar code. The output writers (`7C18`/`8160`/`7998`, about 6 %) do two 16-byte read-modify-writes per
  vertex field and look like the next candidate; the gain there is unmeasured.
