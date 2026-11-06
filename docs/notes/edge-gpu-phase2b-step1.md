# c690 Edge on the GPU — phase 2b step 1 (stage corpus, format keys, burst cost model)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two automated pitch-meter launches (`ab_live.sh`,
governor `performance`) plus offline analysis of the captured LS/GPR images,
the lifted c690 module, and a headless Vulkan compute kernel. Nothing was wired
into the walker and nothing is default-on.

This follows [`edge-gpu-phase1.md`](edge-gpu-phase1.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690_function_map.md`](c690_function_map.md) and
[`pin-and-burst-path.md`](pin-and-burst-path.md).

## TL;DR

* **The stage capture works and is cheap.** A new env-gated hook
  (`PS3RECOMP_C690_TAIL_CAPTURE=<dir>`) records IN/OUT pairs for the 0x21,
  0x25/0x26 and 0x2A commands at the c690 command boundary: full 256 KiB LS +
  all 128 GPRs + 40 meta words + a 16-byte-line write bitmap. It needs no edit
  to the generated lifted module (it rides the existing `c690_stats` dispatch
  hooks), including the 0xBB80/0xBB98 direct-call bodies. Its unit test is
  `tools/test_c690_tail_capture.py` (passes; covers off, key, cap, stride,
  trigger and the LS bitmap).
* **Three real 0x21 format classes were captured**, not one:
  sel1 `A730` (`0x13140`), sel7 `A3E0` (`0x131A0`, r6 bit 4 clear) and sel7
  `BB98` (`0x131A0`, r6 bit 4 set). The live run-level 0x21 split is
  `0x13140` 10.3 % of calls / 45.5 % of 0x21 time and `0x131A0` 89.7 % /
  54.2 % (`PS3RECOMP_C690_OP21`, 405,079 calls). The per-call A730 body is
  ~7x the sel7 body, so no single class is >50 % of **both** jobs and time.
  **Step 3 (one bit-exact 0x21 kernel) is therefore not triggered.**
* **The meter window was not captured.** The trigger used for the second run
  was `CAPTURE_PREFIX.trigger`, which the walker capture
  (`rsx_draw_engine.c:976-1000`) consumes and deletes; the tail hook polls
  every 200 ms and missed it. The run-level corpus is still real stage data,
  and the last A730 pairs (frame 8682) are at the run's pitch/meter end, but
  the exact meter-window class shares are **not** measured. This is the main
  gap for the phase-2b coverage/fallback number, not a GPU-cost gap.
* **One dispatch per burst is comfortably fast.** The exact 0x2A kernel, 429
  jobs in one `vkCmdDispatch` (device-local storage, RTX 3060 Ti), is
  **0.23-0.27 ms for the whole burst (0.54-0.62 us/job)**; host-visible
  storage is PCIe-bound at 12.6 ms (29.3 us/job). CPU record+submit is
  ~2-4 us. This is far under the ~5 ms kill threshold. **No kill signal for
  option C on GPU cost.**
* **Verdict: GO for the GPU tail on cost; the blocker is format coverage, not
  bandwidth.** Do not build one 0x21 kernel from this corpus: it would cover
  a minority of either jobs or time, and the corpus is not meter-gated. Next
  step is a meter-gated, non-consumed trigger and kernels per class (three
  observed), or a fallback-aware two-class split.

## 1. Capture hook

### 1.1 Design

Added to `runtime/spu/c690_corpus.c` / `.h` and driven from `c690_stats.c` at
the same command boundary that already feeds the opcode histogram:

```
PS3RECOMP_C690_TAIL_CAPTURE=<dir>   enable (off by default)
PS3RECOMP_C690_TAIL_PAIRS=N         max pairs per format key (default 8)
PS3RECOMP_C690_TAIL_STRIDE=N        sample every Nth stage open (default 1)
PS3RECOMP_C690_TAIL_TRIGGER=<path>  start when the file appears
```

For every top-level c690 command entry it closes the previous stage (recording
OUT) and opens the new one when the opcode is 0x21, 0x25, 0x26 or 0x2A
(recording IN). A 0x21 stage's key is refined at its sub-selector entry
(`0x13178/0x13140/0x131A0/0x131F8`) from `r6`: `(target_lsa << 8) | (r6 & 0x1f)`,
so selector and the A730/BB80 bit are both in the key. Because the capture is
at the command boundary, the 0xBB80/0xBB98 direct calls inside 0x13080 are
covered without a hook in the generated module.

`tail_<lane>.bin` uses the existing `C69STG01` spool header with two new
record types (IN/OUT). Each record carries 24 u32 of tail meta (stage LSA,
opcode, key, key target LSA, sequence, entry GPRs, eight command payload
words, entry/exit monotonic ns), a 2048-byte bitmap of the LS lines whose
bytes changed between IN and OUT (the write range), then the full LS and GPRs.
`tail_hist.txt` is rewritten every 2 s with the exact per-key call and ns
totals for the sampled window (including commands whose pairs were capped),
so the shares survive a killed process.

When the key is off, the only added cost is the predicted-not-taken
`g_c690_tail_on` test inside `c690_stats_on_lookup`/`on_direct`, which are
themselves only called when `PS3RECOMP_C690_STATS` is on. No `spu_context`
field was added.

### 1.2 Unit test

`tools/test_c690_tail_capture.py` compiles
`runtime/spu/tests/test_c690_tail_capture.c` with the real
`runtime/spu/c690_corpus.c`, simulates the 0x21 -> 0x25 -> 0x2A -> end command
boundaries and parses the spool. It checks:

* env unset: `g_c690_tail_on == 0`, no spool;
* IN/OUT LS, GPR and opcode for 0x21/0x25/0x2A;
* the 0x21 key from the sub-selector (`0x01314001` for sel1 A730);
* the LS write bitmap (exactly the one line each simulated stage changed);
* the N-per-key cap and the stride;
* the trigger file arming (absent vs present).

## 2. Corpus and format histogram

Two runs, both `ab_live.sh`, build `build-play`, at the pitch meter:

| run | log | result |
|---|---|---|
| 1 | `ab_live_fncap2b1_20261104_093922.log` | 96 pairs, 6 keys (49 MB), no trigger; sampled from boot |
| 2 | `ab_live_fncap2b1m_20261104_094533.log` | `PS3RECOMP_C690_OP21=1`; tail trigger missed, no pairs; full op21 split |

### 2.1 What run 1 captured

96 complete pairs = 16 per key (cap), from 5 lane spools:

| key | class | pairs | frame range | seq range |
|---|---|---:|---|---|
| `0x0000002A` | 0x2A assembler | 16 | 8128..8128 | 0..960 |
| `0x00000025` | 0x25 cull | 16 | 8128..8128 | 224..480 |
| `0x00000026` | 0x26 cull | 16 | 8130..8491 | 4416..23712 |
| `0x0131A007` | 0x21 sel7 A3E0 | 16 | 8128..8130 | 16..4240 |
| `0x0131A017` | 0x21 sel7 BB98 | 16 | 8130..8253 | 3952..10240 |
| `0x01314001` | 0x21 sel1 A730 | 16 | 8130..8682 | 3872..186768 |

The first five keys cap early in the load/pitch transition. The A730 key's
16th sample lands at seq 186,768 and frame 8682, i.e. the run's pitch/meter
end; the last three A730 pairs are the best meter candidates in the corpus.
The whole run is not meter-gated, so the sel7 shares above are not the meter's.

Per-class per-call times measured in this corpus (pre-meter mix):

| class | avg us/call |
|---|---:|
| 0x21 sel1 A730 | 189.5 |
| 0x21 sel7 A3E0 | 97.5 |
| 0x21 sel7 BB98 | 88.8 |
| 0x26 cull | 65.7 |
| 0x25 cull | 41.0 |
| 0x2A assembler | 80.5 |

### 2.2 Live 0x21 split (run 2, whole run)

`[c690-op21]` cumulative (405,079 calls, 36.52 s inclusive):

| sub-selector | calls | jobs % | time | time % | avg us |
|---|---:|---:|---:|---:|---:|
| `0x13140` (sel1; A730 in the corpus) | 41,656 | 10.3 | 16.63 s | 45.5 | 399.2 |
| `0x131A0` (sel7; A3E0/BB98) | 363,423 | 89.7 | 19.80 s | 54.2 | 54.5 |

The corpus's early sample rate (A3E0 capped at seq 4240, BB98 at 10240)
suggests sel7 is roughly 70/30 A3E0/BB98 in jobs; with the measured per-call
times that is ~63 % jobs / ~39 % time for A3E0 and ~27 % / ~15 % for BB98.
Combined with sel1 A730 (~10 % jobs / ~45.5 % time), **no class is a majority
of both jobs and time**. That is an estimate, not a meter measurement.

### 2.3 LS ranges read/written

Traced offline on one representative pair per class with the same
`spu_ls_read128`/`spu_ls_write128` line tracer used in phase 2a (research
build, not committed). 16-byte lines, contiguous runs:

| class | read ranges | write ranges |
|---|---|---|
| sel1 A730 | 31 runs: code/constants `0x110A0..`, `0x13E20..0x14350`, descriptor/table `0x1F0A0`; arrays; stack | `0x1F0A0`; `0x1F750..0x213D0` (four planar arrays); stack `0x3F4B0..0x3F820` |
| sel7 A3E0 | 32 runs, same code/table shape plus `0x0F720..` | `0x1F0A0`; `0x1F7C0..0x21B80`, `0x21BC0..0x23F80`, `0x23FC0..0x26380` (planar arrays); stack |
| sel7 BB98 | 18 runs, same shape plus `0x144B0..0x14530` constants | `0x1F0A0`; `0x1F780..0x21F00`, `0x21F40..0x246C0`, `0x24700..0x26E80`; stack |

The capture's own write bitmap matches these runs within a line; the small
difference is the OUT snapshot landing just after the command, before the next
handler body. The arrays are the whole tail's working set (normal, position,
colour, uv0), confirming phase 2a: the GPU pass must produce all four, not
just positions.

## 3. GPU cost model: one dispatch per burst

The exact 0x2A kernel (`c690_2a_batch.comp`, byte-exact on the validated
`fncap21b/0x10F78` capture) was run with **all 429 jobs in one
`vkCmdDispatch` / one submission**, on the RTX 3060 Ti:

| storage | J | GPU burst | GPU us/job | record us | submit us | exact |
|---|---:|---:|---:|---:|---:|---|
| device-local | 1 | 0.029 ms | 29.3 | 2.0 | 2.3 | YES |
| device-local | 64 | 0.031 ms | 0.49 | 1.7 | 2.2 | YES |
| device-local | 429 | **0.268 ms** | **0.624** | 3.1 | 3.2 | YES |
| host-visible | 429 | 12.576 ms | 29.3 | 3.6 | 4.3 | YES |

A second run over the 16 real 0x2A inputs (varied per-job array bases) in one
dispatch measured **0.231 ms / 0.539 us per job** device-local. Its per-job
*exactness* is not usable: the command-boundary OUT snapshot is after the
next command, and ~1.1 0x2A commands run per job, so a pair whose next command
is another 0x2A is not the output of its IN (only the phase2a single capture
and job 0 validated byte-exact). The cost number is unaffected; the reference
pairing is a corpus limitation (see §6).

Upload/init of 429 full LS images is 31 ms device-local (107 MiB in + 107 MiB
out). That is **not** a per-frame cost in the plan: the tail writes the
GPU-resident ring images, so only a small job table changes per burst.

**Whole-tail estimate.** 0x2A is 19.3 % of the command wall in run 2; 0x21 is
35.5 %, the culls 12.5 %. The 0x2A kernel is 0.23-0.27 ms for the burst. The
0x21 work is per-vertex FMA/shuffle math; 429 jobs x ~200 vertices is
~86k vertices, a few tens of MFLOP, sub-0.1 ms of FP32 on the 3060 Ti, plus
the cull/index kernels. Even allowing an order of magnitude for the 0x21
format branches, **the whole tail in one dispatch is well under the ~5 ms
kill threshold**; dispatch/transport (record+submit ~5-7 us) dominates.
No kill signal for option C on GPU cost.

## 4. 0x21 kernel: not built

Step 3 is conditional on *one dominant* 0x21 format class. The corpus shows
three classes and no class is >50 % of both jobs and time:

* sel1 A730: ~10 % of calls, ~45.5 % of 0x21 time, ~7x the per-call cost;
* sel7 A3E0: estimated ~63 % of calls, ~39 % of time;
* sel7 BB98: estimated ~27 % of calls, ~15 % of time.

A kernel for the largest time class (sel1 A730) would still leave ~55 % of
0x21 time (and ~90 % of jobs) on the SPU; a kernel for the largest job class
would leave ~45 % of time. Neither meets the phase-1 gate. Also, only the
last A730 pairs are at the meter, so building from this corpus would be the
same unvalidated-input mistake phase 2a stopped. **No 0x21 kernel was built
and no diff is claimed.**

## 5. Verdict

**GO on the GPU tail transport; do not build a single-class 0x21 kernel yet.**
The GPU cost is not the blocker: one dispatch per 429-job burst is ~0.3 ms
device-local, and the CPU record/submit is microseconds. The blocker is
format coverage: at least three real 0x21 classes run in the pitch workload,
with a ~7x per-call cost spread, and the exact meter-window split is still
unmeasured.

Phase 2b should proceed only with one of:

1. **All-class coverage**: kernels for sel1 A730, sel7 A3E0 and sel7 BB98,
   plus the culls and 0x2A; fallback for the rest. This is the only path that
   removes the ~17.4 ms tail.
2. **Two-class split** (A730 + the dominant sel7 path) only if a meter-gated
   histogram shows the missing class is under the 5 % fallback budget. The
   current data does not show that.

The 21 ms target still needs the gap/dispatch work; the tail alone models
35.0 -> ~17.6 ms.

## 6. What the corpus gives option D, and what to fix

* **Real IN/OUT for 0x21 and the culls is usable.** The 0x21 pairs are 16 each
  for the three classes (full LS + GPRs + key + write bitmap), enough for
  offline kernel development and differential tests. The cull pairs are 16
  each for 0x25/0x26.
* **The 0x2A command-boundary pairs are not reliable** when two 0x2A commands
  run back to back. 0x2A returns normally (`SPU_RET`), so the fix is a
  function-entry/exit wrapper for `0x10F78` (the `patch_cull_hooks.py`
  pattern) or recording the next command opcode at close. The validated
  `fncap21b/0x10F78` capture remains the exact 0x2A reference.
* **Meter gating failed because the trigger was consumed.** The walker
  capture deletes `CAPTURE_PREFIX.trigger`. A future run needs a separate
  unchanged trigger file (or `CAPTURE_PREFIX` unset) touched at the confirmed
  meter. With that, `tail_hist.txt` gives exact per-key call/time shares in
  the stable meter window.
* **Coverage/fallback arithmetic.** With three classes, handling any one
  leaves a fallback of ~27-90 % of 0x21 jobs; the phase-1 gate is 5 %.
  The fallback rate is therefore set by class coverage, not by the GPU.

## 7. Reproduction

```sh
# capture hook unit test
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_c690_tail_capture.py

# run 1 (captures; no meter trigger)
cd /home/m/Documents/mlb16-online/recomp-work
TAG=fncap2b1 N=1 SEG=2 DROP=0 SETTLE=8 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 PITCH_THRESH=35.0 LIMIT=420 BOOT=40 ARMS='A=' \
  PS3RECOMP_C690_TAIL_CAPTURE=/tmp/opencode/fncap2b1 \
  PS3RECOMP_C690_TAIL_PAIRS=16 PS3RECOMP_C690_TAIL_STRIDE=16 \
  ./ab-tools/ab_live.sh

# report / extract a spool
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/c690_tail_report.py /tmp/opencode/fncap2b1
python3 tools/c690_tail_report.py /tmp/opencode/fncap2b1 --extract /tmp/opencode/corpus21

# 0x2A burst cost model (device-local), validated single capture
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu \
  -I /home/m/Documents/mlb16-online/ps3recomp-play/include \
  -I /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay \
  -o /tmp/opencode/gpu2a/test_c690_2a_batch \
  /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/test_c690_2a_batch.c \
  /tmp/opencode/c690gpu/reg_c690.c \
  /home/m/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu/job_c690_native.c \
  -l:libvulkan.so.1 -lm
cd /tmp/opencode/gpu2a
./test_c690_2a_batch \
  /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/c690_2a_batch.comp \
  /tmp/opencode/gpu2a/c690_2a_batch.spv --jobs 429 --iters 9 --device-local \
  /tmp/opencode/fncap21b/0x10F78
```

## 8. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our
runtime/walker code and our own captures. The stage/corpus/differential method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2
PC port (MIT, ThiagoFraga761 and contributors), design reference only. No
RPCS3 or other GPL emulator source was read or cited.
