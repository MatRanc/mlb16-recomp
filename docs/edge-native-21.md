# c690 0x21 whole-command native: bit-exact offline, at parity; the one live A/B attempt wedged (pre-fix)

Date: 2026-11-05. Branch `exp/native-21` in the `ps3recomp-n21` worktree (from
`exp/live-ab`), build `recomp-work/build-n21`. Code pushed to `fork` only;
`PS3RECOMP_C690_NATIVE_21` stays **default 0**. Two pitch-meter launches (the
cap): launch 1 was the corpus attempt (it exposed a missing link anchor and
produced no pairs), launch 2 captured the corpus and ran the A/B. The A/B's
key-on arm wedged at the first switch. The captured inputs then found the bug;
the fixed build is bit-exact offline but was **not live-re-run** because the
cap was reached.

This follows [`c690-emit-ring.md`](c690-emit-ring.md) (0x21 is pure compute),
[`edge-native-2a.md`](edge-native-2a.md) (whole commands, not leaves),
[`edge-native-step5.md`](edge-native-step5.md),
[`spu-codegen-audit.md`](spu-codegen-audit.md) and
[`edge-exactness.md`](edge-exactness.md).

## TL;DR

* **The whole 0x21 command is now one flattened native body**, generated from
  the lifted module by `tools/c690_ref/gen_native_21_stage.py`
  (`runtime/spu/c690_native_21_stage.inc`, 80 blocks): the 0x13080 dispatcher,
  the 0x13140/0x131A0 selector bodies, the `brsl` call graph and the
  transform/pack/normalize tails reachable from the format tables. Tail
  transfers are `goto`s; `brsl` calls keep the lifted `ctx->gpr` marshalling so
  the caller-visible state is exact.
* **Offline it is bit-exact on real meter inputs.** 78 command entries were
  captured at the pitch meter. On the 76 that the lifted reference itself can
  replay, the native stage matches the full 256 KiB LS (**0 differing bytes**),
  **all 128 GPRs (0 differing words)** and the return PC/status/stop code. The
  other 2 hit a computed target the lifted resolver cannot resolve either
  (`0x018B7`, `0x0A0AF`); the native stage hands those to the lifted resolver.
* **What the meter actually runs is not the `0xBB80` kernel.** The 78 entries
  are **11 selector 1 (0x13140)** and **67 selector 7 (0x131A0)** (14 % / 86 %,
  matching the 15.7 / 84.3 % call split). Selector 7 is the
  `0xA3F8 -> 0xA8A0` transform/normalize chain; selector 1 in these entries is
  the `0x13250 -> 0xA3C0 -> 0xA410` path. The `0xBB80`/`0xBB98` path into the
  `0xC/D` format pipelines only appeared when we forced `r6` bit 4 in a
  synthetic state, so the "sel 1 = 0xBB80 large batch" reading in
  [`c690-emit-ring.md`](c690-emit-ring.md) is not what these meter entries do.
* **It is not faster.** With warm, back-to-back arms (the fair comparison),
  200 replays per pair over the 76 pairs: **31.44 us lifted vs 31.42 us
  native, ratio 0.998**. An alternating lifted/native microbench showed 2.5-3x,
  but that comparison thrashes the I-cache between the two huge bodies and
  inflates the lifted side; the back-to-back number is the one to trust. The
  result matches [`spu-codegen-audit.md`](spu-codegen-audit.md): the emitted
  instruction count, not the call marshalling, is the gap.
* **The live B arm wedged.** Run `ab_live_native21ab_20261105_060006`
  (N=5 x 10 s, key off/on) captured 96 entries during the settle, ran one clean
  A segment, then stopped flipping 0.2 s after the first switch to B. Replaying
  the captured inputs found the pre-fix build wrote 2 wrong bytes in the ring
  word at `0x1F0A2..A3` (the `[r80+0xA0]` emit word) because the flatten had
  dropped the `ctx->gpr` stores/reloads across `brsl`. Keeping them makes the
  body bit-exact (the gate above). There was no third launch to re-validate.
* **Decision:** the native 0x21 is exact but at parity offline, so it does not
  pay and **0x21 should not go to the GPU** on this evidence. Keep the key off.
  A live soak of the fixed build needs one launch if the user wants it; the
  remaining p50 lever is the codegen backend, not another per-command rewrite.

## 1. What the command is, dynamically

`0x13080` computes an 8-entry selector from the command payload and branches
through the table at LS `0x1311C` (index 0 -> `0x13178`, 1 -> `0x13140`,
3 -> `0x131F8`, 7 -> `0x131A0`, others stop). The bodies call one of the format
setup routines (`brsl`):

| selector | body | observed meter path |
|---:|---|---|
| 1 | `0x13140` | `r6 & 16 == 0`: `0x13250 -> 0xA3C0 -> 0xA410`; set: `0xBB80 -> 0xBBB0 -> 0xBCB4 -> ...` |
| 7 | `0x131A0` | `r6 & 16 == 0`: `0x13288 -> 0xA3F8 -> 0xA8A0`; set: `0xBB98 -> 0xBBB0 -> ...` |

The `0xA410` / `0xA8A0` bodies dispatch through format tables at LS
`0x143D0..0x144F0` into the transform/normalize family (`0xA730`, `0xA798`,
`0xA818`, `0xA9B0`, `0xABB8`, `0xAD18`, `0xAF98`, `0xB238`, `0xB268`,
`0xB488`, `0xB830`, `0xB958`, `0xBA10`, `0xBB20`) and emit the ring word at
`[r80+0xA0]`. The `0xBB80`/`0xBB98` path enters the `0xC0F8..0xD080` format
pipeline (including the already-native `0xD080` cull) and is only taken by the
`r6` bit-4 formats; the meter entries we captured do not take it.

The 76 captured entries, replayed lifted, split **11 x 0x13140 / 67 x
0x131A0**; the aggregate lifted cost is 31.44 us/call over the sample (the live
cumulative `[c690-stats]` 0x21 average in the same run was 71.64 us/call,
including the larger boot/menu jobs).

## 2. What was built

* `tools/c690_ref/gen_native_21_stage.py` parses the lifted module and emits
  `runtime/spu/c690_native_21_stage.inc`:
  - `sr0..sr127` are initialised once and stay live; per-block prologue loads
    are dropped, tail `SPU_DIRECT` becomes `goto`;
  - `brsl` brackets become a return-id stack + `goto`; the `ctx->gpr` stores
    before the call and the reloads after it are **kept**, so the caller sees
    exactly the lifted marshalling; `SPU_RET` pops the stack or ends the
    command;
  - register-computed branches become switches over the flattened addresses;
    an unknown target goes to `L_LIFT_DISPATCH`, which publishes `pc` and
    calls the lifted resolver;
  - stop paths reproduce `spu_stop`.
* `runtime/spu/c690_native_21.c/.h`: the live key, counters, the entry hook and
  the `PS3RECOMP_C690_21_CAPTURE` corpus spool (full LS+GPR entry pairs,
  trigger-file gated).
* `tools/c690_ref/patch_native_21.py` (idempotent) renames the generated
  `0x13080` to `_body` and inserts a wrapper. The hooks are **weak**, so the
  shared `recomp-work/spu_lifted` tree still links in the play/gpu builds and
  they take the lifted body. The 0x2A hooks are untouched.
* `runtime/spu/spu_dispatch_mt.c` carries a strong `__attribute__((used))`
  anchor, because a weak reference alone does not extract the runtime archive
  member (launch 1's failure).
* `PS3RECOMP_C690_NATIVE_21` is a live control-file key
  (`libs/video/ps3_live_ctl.*`, appended at the end, default 0). No
  `spu_context` field was added.

## 3. Offline gates

The corpus is `recomp-work/captures/n21` (78 entries, meter, settle window).
The exit half of that capture is not usable: the wrapper recorded it after the
lifted dispatcher returned a pending indirect branch, so the gate re-runs the
lifted `0x13080` on each entry to produce the reference.

| gate | result |
|---|---|
| corpus pairs, lifted reference replays | 76 / 78 (2 abort in the lifted resolver itself) |
| native vs lifted, full LS | **0 differing bytes on 76/76** |
| native vs lifted, all 128 GPRs | **0 differing words on 76/76** |
| native vs lifted, PC/status/stop | **exact on 76/76** |
| native fallbacks | 2 pairs -> `L_LIFT_DISPATCH` (unknown computed target) |
| `test_c690_native_21` (new) | **PASS** (wiring, key off fallback, trivial path, pc/trampoline) |
| `tools/test_video_fixtures.py` | 32/32 on clean runs; `test_gcm_tick_in_drain` is the known intermittent timing fixture (it failed twice under load, passed on the other runs) |
| `check_spu_context_abi.sh` | **PASS** (no `spu_context` change) |

Reproduce:

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-n21
python3 tools/c690_ref/gen_native_21_stage.py \
  ../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  runtime/spu/c690_native_21_stage.inc
python3 tools/c690_ref/patch_native_21.py
ninja -C ../recomp-work/build-n21 MyGameRecomp test_c690_native_21
tools/c690_ref/run_native_21_test.sh ../recomp-work/captures/n21 200
../recomp-work/build-n21/ps3recomp_sdk/test_c690_native_21
SDK=$PWD ../recomp-work/ab-tools/check_spu_context_abi.sh
python3 tools/test_video_fixtures.py
```

## 4. Offline cost

`tools/c690_ref/run_native_21_test.sh <dir> 200` runs all 200 lifted replays
warm, then all 200 native replays warm, per pair (same inputs, same process,
same flags as the game: `-O2 -march=native -mavx2 -mfma -ffp-contract=off
-DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS`):

| metric | value |
|---|---:|
| pairs with timing | 76 |
| mean lifted | **31.44 us/call** |
| mean native | **31.42 us/call** |
| aggregate ratio | **0.998** |

An alternating lifted/native microbenchmark (one pair at a time, 200-400
iterations) reports 2.5-3x for some pairs, but it alternates two multi-MB bodies
in the I-cache and inflates the lifted arm; it is not the number to use. The
back-to-back arms are at parity. This is the same finding as the earlier
per-function gates (0.99-1.53x): the compiler already emits the same
AVX2/FMA/shuffle mix, and the 0x21 cost is instruction count, not the call
boundaries.

## 5. Live A/B

Run `ab_live_native21ab_20261105_060006`, `BUILD=build-n21`,
`METER=1 SKIP_CINE=1 WCB=2 PIN=0`, `VIDEODRIVER=x11`, `PITCH_THRESH=35`,
`SETTLE=20`, `N=5` x 10 s, 2 s dropped, arms
`A=PS3RECOMP_C690_NATIVE_21=0` / `B=...=1`, plus the 0x21 capture trigger.

| phase | result |
|---|---|
| capture | 96 requested, 78 written during the 20 s settle; 96/96 `[c690-cap21] DONE` lines from the lanes |
| A segment | clean 10 s (flips continuing) |
| B segment 1 | **wedged**: `STOP: no flips during segment 1 (B)` ~0.2 s after the switch |
| performance numbers | none (the run exits on the hang guard) |

The pre-fix build differed from the lifted output by 1-2 bytes in the ring
word at `0x1F0A2..A3` on every captured input (151 bytes over 76 pairs), plus
scratch GPRs. The ring word is what the walker consumes, so the mismatch
corrupted the emitted command stream and stopped the flips. Root cause: the
first generator dropped the `ctx->gpr` stores/reloads at `brsl` returns, so a
register the callee did not store leaked its new value into the ring word.
Keeping the lifted marshalling (the shipped generator) removes the difference
(0 bytes / 0 GPRs on 76/76). The cap (2 launches) was already spent, so the
fixed build has not been live-run.

## 6. What this means

* **Do not enable the key and do not move 0x21 to the GPU.** The whole-command
  native is exact but at parity; a GPU hand-off would add the 5.96 ms/frame
  binding cost measured in [`edge-gpu-binding.md`](edge-gpu-binding.md) to save
  nothing. The 2A precedent (a partial native at 1.5 %) and this whole-command
  result both say the meter c690 compute is at the compiler's limit.
* **The remaining edge** is the emitted host instruction count: the
  [`spu-codegen-audit.md`](spu-codegen-audit.md) estimate of 1.2-1.4x for a
  register-mapped backend is the realistic 0x21 lever, not another native
  command.
* **If the user wants the fixed exact version live:** one pitch-meter A/B
  (same command) is the only outstanding check. It should show no regression;
  it will not show a speed win.

## 7. Deviations and caveats

* The 0x21 capture's exit half is recorded before the pending dispatcher
  branch, so the gate re-runs the lifted reference instead of comparing to the
  captured exit. The entries are valid; only the exits are not.
* Launch 1 was lost to the missing archive anchor (the native/capture objects
  were not linked). Launch 2 is the A/B above.
* Two of the 78 entries hit computed targets the lifted resolver cannot
  resolve from an LS-only capture; they are counted as fallbacks, not native
  coverage.
* The 0x21 dynamic split in this doc (11/67) is from the captured entries at
  the pitch meter; the earlier `sel1 = 0xBB80` mapping is a static reading that
  these entries do not take.

## 8. Clean room

Own static analysis of our own EBOOT (Ghidra), the lifted c690 module, our own
runtime and captures. The flatten/corpus/differential method follows NHL
Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was
copied. No RPCS3 or other GPL emulator source was read or cited.
