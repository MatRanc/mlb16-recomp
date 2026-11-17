# Handoff: Item 4 (c690 SoA/output kernels) -> Item 2 (SPU dispatch, coherence, inline stores)

Date: 2026-11-17.

## 1. What is done and pushed (Item 4, first stage + output writers)

- **`MatRanc/ps3recomp` (`exp/soa-kernels`, `c3457bf5`):**
  - Native kernels behind live key `PS3RECOMP_C690_SOA` (`0` off, `1` native, `2` shadow compare):
    - Normal/tangent regeneration (`runtime/spu/c690_soa_kernels.inc`): `0xFA20` clear, `0xFA58` normals, `0xFCB8` normalize, `0xFDA0` tangents (2.5–15.9x on the `approx` twin, 0 differing bytes across all 3 twins).
    - Output writers (`runtime/spu/c690_output_kernels.inc`): `0x7998`, `0x8160` (joining `0x7C18` in `job_c690_native.c`).
  - Hook script `tools/c690_ref/patch_native_soa.py` and offline gate `tools/c690_ref/run_native_soa_test.sh`.
- **`MatRanc/mlb16-recomp` (`main`, `15f2e78`):**
  - `docs/notes/soa-kernels.md`, `docs/plan.md`, `SETUP.md` (NVIDIA vs `nouveau`/NVK troubleshooting note), and `tools/ab/shot.sh` (`.png` extension fix).
- **Live verification (`recomp-work/build-soa`, NVIDIA `620.70.06`, `approx` twin, play settings):**
  - Shadow compare (`=2`): 266,240 calls (normal/tangent) + 20,164,608 calls (with `0x7998/0x8160`) with **0 local-store byte mismatches** and **0 PC mismatches**.
  - Meter A/B (`=0` vs `=1`): `40.18 -> 40.73` fps (stage 1) and `39.72 -> 39.31` fps (with output writers), flat within run noise because SPU `c690` is hidden behind the ~18–20 ms RSX walker drain.
- **Left alone for later (after Items 2 & 3):** `0xF520`, `6D68`, `AF98`, `D2B8`/`D4A8`, `E750`.

## 2. Next: Roadmap Item 2

From `docs/plan.md` and `recomp-work/captures/pm_approx_top.txt`:

1. **SPU flat dispatch table:** replace indirect dispatch/lookup (`spu_lifted_lookup` 2.63%, `spu_indirect_branch_slow` 2.39%, `spu_indirect_branch` 1.16%, `spu_lifted_lookup_full` 1.12%, `spu_lookup` 0.77% — ~8.1% of SPU lane cycles, ~6.8% of process cycles) with a flat per-image table indexed by `(pc & SPU_LS_MASK) >> 2`.
2. **Coherence holders (`spu_coherency.c`):** coherence marks are never cleared today, so stores to any once-reserved line pay a `seq_cst` fence plus a list walk (`spu_lockline_lock` 1.67%, `spu_coh_notify_write` 0.77%, `spu_coh_is_reserved` 0.25%). Use per-line holders.
3. **Inline store cleanup:** compile the two diagnostic globals out of the inline store hot path.
- **Expected impact:** ~1–2 ms busy time (~40% on the critical chain), plus ~10 lane-ms/frame from the flat dispatch table.

## 3. Benchmarking notes

- Run `tools/ab/ab_live.sh` with `REF_DIR=$PWD/../../../recomp-work/ab-tools/ref`, `PITCH_THRESH=38`, and the `play_windowed.sh` env flags (`PS3RECOMP_CROWD_FIX=1`, `PS3RECOMP_SPU_FTZ=1`, `PS3RECOMP_SPU_FLOAT_APPROX=1`, `PS3RECOMP_WALKER_WAKE=1`, `PS3RECOMP_REMAP_FLAT=1`, `PS3RECOMP_ENG_PROG_MEMO=1`, `PS3RECOMP_ENG_SWZ_RT=1`, `PS3RECOMP_VK_DEPTH_BIAS=1`, `PS3RECOMP_VK_PASS_KEEP=1`, `PS3RECOMP_VK_VTX_PRE=1`, `PS3RECOMP_VK_DRAW_STATE=1`, `PS3RECOMP_RSX_WCB_PREFETCH=1`, `PS3RECOMP_RSX_WCB_STREAM=1`, `PS3RECOMP_C690_STATS=0`).
- Confirm `nvidia-smi` works before benchmarking (`nouveau`/NVK drops walker speed ~10x to ~10–12 fps).
