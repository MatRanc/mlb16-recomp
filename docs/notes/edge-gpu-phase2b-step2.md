# c690 Edge on the GPU — phase 2b step 2 (meter-gated corpus, one-command 0x2A pairs, coverage)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two automated pitch-meter launches
(`ab_live.sh`, governor `performance`) plus offline replay of every captured
pair against the lifted c690 module. Nothing was wired into the walker and
nothing is default-on.

This follows [`edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md),
[`edge-gpu-phase2a.md`](edge-gpu-phase2a.md),
[`edge-gpu-phase1.md`](edge-gpu-phase1.md),
[`c690-emit-ring.md`](c690-emit-ring.md) and
[`c690_function_map.md`](c690_function_map.md).

## TL;DR

* **Both blockers from step 1 are fixed in the capture.**
  1. The tail hook has its own trigger file
     (`PS3RECOMP_C690_TAIL_TRIGGER`). `ab_live.sh` removes a stale copy at
     launch and touches it only after the pitch meter is confirmed; the hook
     only tests for it and never deletes it. It can no longer race the walker
     capture, which deletes `CAPTURE_PREFIX.trigger`.
  2. 0x2A is captured at the **function exit** of `0x10F78` (the
     `patch_cull_hooks.py` wrapper pattern). Every 0x2A pair is one command:
     **64/64 captured 0x2A pairs replay byte-exact** (0 differing LS bytes).
     The unit test covers the separate trigger, the function-exit pairing
     (two back-to-back 0x2A commands) and the non-consumed trigger.
* **Meter-window histogram is now measured, not estimated.** The trigger fired
  at the confirmed meter (`10:04:14`) and the last 10 s of the run produced
  395,784 tail commands / 23.242 s of tail time. By **time** the order is
  sel1 A730 `7.532 s` (32.4 %), 0x2A `5.359 s` (23.1 %), sel7 A3E0 `4.655 s`
  (20.0 %), 0x25 `3.060 s` (13.2 %), 0x26 `1.332 s` (5.7 %), sel7 BB98
  `1.304 s` (5.6 %). A730 is the largest 0x21 class by meter time, as
  expected; it is **55.8 % of 0x21 time / 16.9 % of 0x21 jobs**.
* **Every 0x2A and cull pair replays.** The second (wrapper-validated) run's
  64 pairs per key replay **byte-exact** for 0x2A, 0x25 and 0x26. The 0x21
  command-boundary corpus replays byte-exact on the output arrays for
  21/32 A730, 21/32 A3E0 and 18/32 BB98 pairs; the only difference is the
  single parser-owned count byte at `0x1F0F3` (the `+2` at `0x1F0F0`). The
  other 11/11/14 pairs are boundary-contaminated: their next 0x21 was a
  direct dispatch the resolver never saw, so one pair spans two commands.
  They are excluded from kernel input.
* **A730 kernel: not built, and no diff is claimed.** The meter corpus gives
  clean real inputs and outputs, but the A730 command is not a compact
  formula: it writes four planar arrays of 142 x 16-byte records
  (`0x1F750..0x21AD0`, 9088 bytes) through a heavily unrolled
  transform/format chain (matrix/palette data at `0x27270`, packed streams at
  `0x26570`/`0x2BF00`, jump tables and `shufb`/`fma` conversion). A
  function-entry wrapper on the sub-selector `0x13140` captures only its
  prefix (58 us) because the heavy body is reached through the trampoline;
  the command-boundary pair is the only full-command pair. Porting that chain
  bit-exactly is a separate multi-day job, so step 3 did **not** pass and
  step 4 was not attempted.
* **Coverage gate.** At the meter, the 5 % job-fallback budget is met by
  covering every class except BB98 (4.52 % of tail jobs, 5.61 % of tail
  time). The honest target is all six keys plus the culls; the A730 saving is
  ~5.6 ms of the 24.39 ms critical lane, A3E0 ~3.5 ms, BB98 ~1.0 ms,
  0x2A ~4.0 ms, 0x25 ~2.3 ms, 0x26 ~1.0 ms (see §6).

## 1. Capture gate

### 1.1 Separate, non-consuming trigger

`ab_live.sh` now takes `TAIL_TRIGGER=${PS3RECOMP_C690_TAIL_TRIGGER:-}`:

* at launch it removes a stale `TAIL_TRIGGER` (only that file) and refuses to
  run if it equals `$CAPTURE_PREFIX.trigger`;
* both the walker capture and the tail capture are armed in the
  meter-confirmed block, but each touches its own file;
* `c690_tail_armed()` in `runtime/spu/c690_corpus.c` only `access()`es the
  file and latches `s_tail_trigger_seen`; it never removes or truncates it.

The first step-2 run (`fncap2b2`) confirmed the fix: the log shows
`[ab_live] c690 tail capture armed at the meter
(/tmp/opencode/fncap2b2.trigger)` immediately followed by
`[c690-tail] trigger seen; sampling`, and the trigger file still existed
after the process exited.

### 1.2 0x2A at the function exit

`tools/c690_ref/patch_cull_hooks.py` now also wraps LS `0x10F78` (0x2A) in
`*_body` + `c690_corpus_tail_2a_enter/exit`. The entry closes any open
command (its true exit is this entry); the exit closes the pair only when the
handler returns to its captured caller link, so an interrupt trampoline
return leaves the pair open and the next command boundary closes it at the
real exit. The same script now also wraps the cull body `0x132E0`
(0x25/0x26, op from `r4`) and the 0x21 sub-selector bodies `0x13140` /
`0x131A0`; those extra wrappers are capture scaffolding and are off by
default.

`tools/test_c690_tail_capture.py` (builds the real `c690_corpus.c`) now also
checks:

* two back-to-back 0x2A commands through their own entry/exit wrappers give
  two pairs, each containing only its own write;
* the trigger file is still present after the run (the hook does not consume
  it);
* the existing off/key/cap/stride/bitmap cases still pass.

With the key off, the only added cost is the predicted-not-taken
`g_c690_tail_on` test in the wrappers. No `spu_context` field was added; the
ABI check still reports `shared field offsets agree`.

## 2. Meter capture

Both runs are `ab_live.sh`, build `build-play`, at the pitch meter
(`METER=1 SKIP_CINE=1`, one X, ~8 s settle):

| run | tag / log | captures | result |
|---|---|---|---|
| 1 | `fncap2b2` `ab_live_fncap2b2_20261104_100138.log` | command-boundary pairs, 32 per key | meter-gated histogram; full 0x21 command pairs |
| 2 | `fncap2b2w` `ab_live_fncap2b2w_20261104_101400.log` | function-wrapper pairs, 64 per key | 0x2A/cull pairs replay exact; 0x21 wrapper is prefix-only |

Run 1 is the histogram/narrative corpus. Run 2 exists to validate the
function-exit fix and the exact 0x2A/cull pairing; it cost the second launch
and is reported as such.

### 2.1 Meter-window class histogram (run 1)

`tail_hist.txt`, written from the trigger to process exit (10.0 s of meter,
395,784 commands, 23,242.0 ms of tail command time):

| key | class | calls | jobs % | time ms | time % | avg us |
|---|---|---:|---:|---:|---:|---:|
| `0x01314001` | 0x21 sel1 A730 | 18,559 | 4.69 | 7,532.00 | 32.41 | 405.84 |
| `0x0000002A` | 0x2A assembler | 175,179 | 44.26 | 5,358.67 | 23.06 | 30.59 |
| `0x0131A007` | 0x21 sel7 A3E0 | 73,466 | 18.56 | 4,654.77 | 20.03 | 63.36 |
| `0x00000025` | 0x25 cull | 82,078 | 20.74 | 3,060.02 | 13.17 | 37.28 |
| `0x00000026` | 0x26 cull | 28,593 | 7.22 | 1,332.38 | 5.73 | 46.60 |
| `0x0131A017` | 0x21 sel7 BB98 | 17,909 | 4.52 | 1,304.22 | 5.61 | 72.82 |

0x21 totals 109,934 calls / 13,491.0 ms (58.05 % of tail time). Within
0x21: A730 16.9 % of jobs / 55.8 % of time, A3E0 66.8 % / 34.5 %, BB98
16.3 % / 9.7 %. The command-level split is stable across the 2 s windows
(the `[c690-op21]` cumulative line at the same run is 0x13140 49.1 % of
0x21 calls / ~392 us each; the tail histogram's A730 average is 405.8 us
including the command prefix).

### 2.2 Run 2 (wrapper validation)

The same meter gate, 64 pairs per key, 384 pairs / 196 MB. The function-level
histogram is **not** a command histogram: the 0x13140 wrapper measures only
its 58 us prefix because the heavy body is reached through
`spu_indirect_branch`, so run 2's A730 time (1.07 s) must not be compared with
run 1's 7.53 s. Use run 1 for time, run 2 for 0x2A/cull pairing.

## 3. Offline replay gate

`tools/replay/test_c690_tail_replay.c` reads a `C69STG01` spool, runs the
captured handler on the IN LS+GPRs with the real lifted module and a minimal
drain/channel model, and diffs the whole 256 KiB LS against OUT.

Run 2 (function wrappers), 64 pairs per key:

| class | pairs | exact | diff bytes |
|---|---:|---:|---:|
| 0x2A | 64 | **64** | 0 |
| 0x25 | 64 | **64** | 0 |
| 0x26 | 64 | **64** | 0 |

Run 1 (command boundary), 32 pairs per key, with the command-level 0x21 drain
enabled:

| class | pairs | replay with only the parser word | boundary-contaminated |
|---|---:|---:|---:|
| 0x21 sel1 A730 | 32 | 21 | 11 |
| 0x21 sel7 A3E0 | 32 | 21 | 11 |
| 0x21 sel7 BB98 | 32 | 18 | 14 |
| 0x25 / 0x26 | 64 | 64 (one parser byte) | 0 |
| 0x2A | 32 | 32 | 0 |

The parser-owned difference is exactly byte `0x1F0F3` (the `+2` in the count
word at `0x1F0F0`): the boundary OUT includes the parser's post-command
update, which is not part of the command body. On the clean 0x21 pairs the
four output arrays match byte-for-byte. The contaminated pairs are the ones
whose next 0x21 entry was a direct known-target dispatch; their OUT includes
the next command and they are not valid kernel input/output.

This is the gate for the kernel work: only the clean pairs (21 A730, 21 A3E0,
18 BB98) are used, and only the command-body bytes are compared.

## 4. 0x21 A730 kernel: attempted and blocked

**What the corpus shows.** A clean A730 pair is a real meter command: IN at
LS `0x13080` with `r3=0x1F000`, `r4=0x21012500`, `r6=0x21012500`,
`cmd[0..7]=0x261D0,0x26260,0x263A0,0x26570,0x27270,0x4DD0,0x3460,0x1A20`;
OUT writes exactly four planar arrays of 142 x 16-byte records,
`0x1F750..0x21AD0` (9088 bytes), plus the stack. The four arrays hold
position-like float4 records (`w == z` on the first three arrays); the
packed streams at `0x26570` (weights/indices, 0.02-0.99 and complements),
`0x27270` (a sequence of 3x4 matrices, translations ~6.03/3.07) and
`0x2BF00` (large screen-space coordinates) are all read by the path.

**Why no kernel.** The handler `0x13080` is only a sub-selector dispatcher;
the `0x13140` body computes a target and leaves it on the trampoline
(`g_spu_trampoline_fn = spu_indirect_branch`). The heavy work is the
`0xA730`/`0xA798`/`0xA808`/`0xA8A0`/`0xA940` -> `0xB830`/`0xBB20` family: a
heavily unrolled 4-lane transform/format chain (`fm`, `fma`, `shufb`,
`rotqby`, per-batch format constants and jump tables), not a compact
matrix times vertex formula. A function-entry wrapper on `0x13140` captures
the 58 us prefix only; wrapping the actual body would require editing the
generated module at the same dynamic target the trampoline selects, and the
input/output contract would still be command-level, not sub-function-level.

The honest offline experiments we ran:

* the four output arrays are not an in-place affine transform of the
  destination (the 4x4 solve on the first vertices is singular);
* the first output record is `(-0.2468, 6.6509, 2.8358, 2.8358)` and the
  matrix/palette at `0x27270` translates by `(-0.064, 6.028, 3.074)`, so the
  data is consistent with a palette transform of a packed stream, but the
  stream decode is the part that is not a small fixed formula;
* a one-dispatch stand-in was therefore not built. **No kernel, no diff and
  no burst number are claimed for A730.**

Step 3's acceptance gate (`<= 1 LSB on <= 0.1 %` of packed values) is not
met because there is no kernel to diff. Step 4 (next class) is conditional on
step 3 and was not attempted.

The exact 0x2A kernel from phase 2a remains the only signed-off class kernel:
429 jobs in one `vkCmdDispatch`, device-local, **0.23-0.27 ms per burst
(0.54-0.62 us/job)**, bit-exact on the validated `fncap21b/0x10F78` capture.
That number is unchanged and is the cost floor for any class kernel.

## 5. What this corpus does give the next attempt

* Real, meter-gated IN/OUT for **0x2A and both culls**, 64 pairs per key,
  all replay byte-exact. These can be used as-is for GPU kernel development.
* Real, meter-gated **command-level 0x21** pairs: 21 A730, 21 A3E0, 18 BB98
  valid pairs (plus 36 contaminated pairs that must be excluded). The valid
  pairs have the complete four-array output and a clean IN at `0x13080`.
* A predictor for a proper 0x21 function capture: wrap the **dynamic
  trampoline target** (the sub-selector's `ctx->pc` after `0x13140`), not
  `0x13140` itself, or capture the command at `0x13080` entry and at the
  parser's next command **body** entry (not boundary) so the parser count
  word is excluded. That is the next capture change, not a runtime change.

## 6. Coverage plan: what must be on the GPU for a 5 % fallback

The 5 % gate is on c690 **jobs**. Using run 1's meter histogram:

| class | jobs % | time % | if left on SPU |
|---|---:|---:|---|
| 0x2A | 44.26 | 23.06 | fallback > 5 % alone |
| 0x25 + 0x26 | 27.96 | 18.90 | fallback > 5 % alone |
| A3E0 | 18.56 | 20.03 | fallback > 5 % alone |
| A730 | 4.69 | 32.41 | 4.69 % jobs / 32.4 % time |
| BB98 | 4.52 | 5.61 | 4.52 % jobs / 5.6 % time |

Because the four larger groups each exceed 5 % by themselves, the fallback
budget is met only by this **minimal set**:

```
0x2A + 0x25/0x26 + A3E0 + A730        fallback = BB98 = 4.52 % jobs / 5.61 % time
```

The safer set is all six keys plus the culls (fallback 0 %), because BB98 in a
different meter scene can exceed 5 % and the culls are needed for the 0x2A
input anyway. The 0x21 result of A730 cannot be consumed by 0x2A unless the
culls and the other 0x21 classes that feed the same command list also move,
so a one-class A730 kernel does not by itself reduce the critical lane.

Projected critical-lane saving, scaling the meter time shares to the measured
full-tail budget of 17.4 ms (phase 2a: 0x21 10.5 ms, 0x2A 3.83 ms, cull
3.10 ms):

| class | meter time share | projected lane saving |
|---|---:|---:|
| A730 | 32.41 % | ~5.6 ms |
| A3E0 | 20.03 % | ~3.5 ms |
| BB98 | 5.61 % | ~1.0 ms |
| 0x2A | 23.06 % | ~4.0 ms |
| 0x25 | 13.17 % | ~2.3 ms |
| 0x26 | 5.73 % | ~1.0 ms |

Full coverage (all six + culls) models the 17.4 ms tail removal; the minimal
5 % set leaves BB98 (~1.0 ms) and any unscene class on the SPU, so it is a
fallback gate, not the performance target. The 21 ms burst target still needs
the gap/dispatch work from phase 1/2a; the tail alone models 35.0 -> ~17.6 ms.

## 7. Reproduction

```sh
# capture + pairing unit test
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_c690_tail_capture.py

# patch the generated module (0x2A exit, cull body, optional 0x21 bodies)
python3 tools/c690_ref/patch_cull_hooks.py

# one meter run, separate trigger (run 1, command boundary corpus)
cd /home/m/Documents/mlb16-online/recomp-work
TAG=fncap2b2 N=1 SEG=2 DROP=0 SETTLE=8 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 PITCH_THRESH=35.0 LIMIT=420 BOOT=40 ARMS='A=' \
  PS3RECOMP_C690_TAIL_CAPTURE=/tmp/opencode/fncap2b2 \
  PS3RECOMP_C690_TAIL_PAIRS=32 PS3RECOMP_C690_TAIL_STRIDE=1 \
  PS3RECOMP_C690_TAIL_TRIGGER=/tmp/opencode/fncap2b2.trigger \
  PS3RECOMP_C690_OP21=1 ./ab-tools/ab_live.sh

# histogram
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/c690_tail_report.py /tmp/opencode/fncap2b2

# replay every pair (command-level 0x21 pairs need FF_DRAIN_21=1)
clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
  -I ps3recomp-play/runtime/spu -I ps3recomp-play/include \
  -I ps3recomp-play/libs/video \
  -o /tmp/opencode/test_c690_tail_replay \
  mlb16-recomp/tools/replay/test_c690_tail_replay.c \
  mlb16-recomp/tools/replay/reg_10f78.c \
  recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
  ps3recomp-play/runtime/spu/c690_corpus.c \
  ps3recomp-play/runtime/spu/c690_cull_fix.c \
  ps3recomp-play/runtime/spu/c690_native_cull.c \
  ps3recomp-play/runtime/spu/c690_native_screen.c \
  ps3recomp-play/runtime/spu/job_c690_native.c -lm
FF_DRAIN_21=1 /tmp/opencode/test_c690_tail_replay /tmp/opencode/fncap2b2
/tmp/opencode/test_c690_tail_replay /tmp/opencode/fncap2b2w
```

## 8. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our
runtime/walker code and our own captures. The stage/corpus/differential method
follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL
code was copied. The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2
PC port (MIT, ThiagoFraga761 and contributors), design reference only. No
RPCS3 or other GPL emulator source was read or cited.
