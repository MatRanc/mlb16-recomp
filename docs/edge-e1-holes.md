# Edge E1: the fused native 0x25/0x26 holes pass (caller modelled, one command pass)

Date: 2026-11-04. Worktree `ps3recomp-e1`, branch `exp/edge-e1-holes`
(created from `exp/live-ab` `230acd9f`). Own build dir
`recomp-work/build-e1` (Release, `-O2 -DNDEBUG -march=native`, same options
as `build-play`; lifted SPU sources copied and re-patched privately to
`recomp-work/spu_lifted-e1`). **Offline only: zero game launches in this
mission.** No file under `ps3recomp-play`, `recomp-work/build-play` or
`build-mt` was touched.

This follows [`edge-holes-fix.md`](edge-holes-fix.md),
[`cull-bypass-ab.md`](cull-bypass-ab.md),
[`edge-native-step4.md`](edge-native-step4.md) and
[`edge-native-step5.md`](edge-native-step5.md). The user's finding stands:
`MLB16_CULL_BYPASS=3` removes the eye holes at **-7.6 fps**; the lifted union
cost **-4.8 fps**; both are diagnostics only.

## TL;DR

* **The 0x132E0 caller is modelled.** It passes `r3` = cull context, `r4` = a
  16-byte stack slot whose **word 0 is the candidate triangle count** and
  **word 2 is the kept triangle count**, and `r5` = the end of the index
  buffer. After `0xD080` returns it writes
  `+0x114 = 3 * slot[2]` (kept index count), `+0x118`, `+0xA0`. The old step-4
  blocker was exactly this write; the fused pass now leaves it untouched and
  merges the sample-only triangles *after* it.
* **One 0x132E0 body pass.** The wrapper calls the body once (mode 3 in
  `patch_cull_hooks.py`); `c690_cull_fix_begin()` returns 3 without taking the
  real-context LS/GPR snapshot, the `0xD080` hook runs the **exact game grid
  on the real context** and the **sample grid on a private copy**, and
  `c690_cull_fix_end()` appends the sample-only triangles after the caller's
  count write, then bumps `+0x114`. No second command body run and no real-LS
  restore/run/restore cycle.
* **Gate on 128 captured pairs (all PASS):** 0 removed/reordered triangles vs
  the SPU on every pair, 0 added on this corpus, full LS+GPR bit-identical to
  the lifted union, and the fused kept count `+0x114` equals the dumped list
  length, a multiple of 3, and the union's count on every pair. Buffer
  pointer/end never move.
* **Cost, gcc `-O2` offline gate per 0x132E0 call:** lifted 22.2-24.9 us,
  native exact 18.9-19.9 us, **fused holes 39.7-41.9 us**, lifted union driver
  116.2-119.9 us.
  The fused pass is **~2.7-3.5x cheaper than the union** and ~1.5-1.9x the
  lifted cull. Three repeats: 24.9 / 18.9 / 39.7 / 116.3 us (medians).
* **Meter estimate: near-neutral, not yet measured.** 0x25 (8.6 % of the
  meter c690 wall) runs the native stage and is *faster* than the lifted
  cull; 0x26 (4.1 %) pays the extra sample evaluation. The union was -4.8 fps
  while costing ~4.7-5.4x the lifted cull; the fused pass is at ~1.6-1.9x, so
  the expected meter delta is about **-0.5 to 0 fps**, i.e. inside the A/B noise.
  The live A/B is the next mission's job (§5).
* **Nothing is enabled by default.** `PS3RECOMP_C690_HOLES_NATIVE` is a new
  live key, 0 in every script. `PS3RECOMP_C690_CULL_FIX`,
  `PS3RECOMP_C690_NATIVE_SCREEN` and `MLB16_CULL_BYPASS` keep their old
  defaults. No `spu_context` field was added (ABI unchanged); only the live
  key enum was appended.

## 1. What the 0x132E0 caller does around 0xD080

`0x132E0` (cull body) is called by both `0x13520` (0x25) and `0x13518`
(0x26); the handler sets `r4 = 0` (0x25) or `r4 = 1` (0x26) immediately
before the call. The body then builds the `0xD080` arguments:

| argument | value at `0xD080` entry |
|---|---|
| `r3` | `+0x110` = u16 index buffer pointer (candidates in, kept list out) |
| `r4` | stack slot `sp+0x20`; **word 0 = candidate triangle count**, word 2 written by `0xD080` = **kept triangle count** |
| `r5` | `+0x1E0` = end of the index buffer / start of the next allocation |
| `r6`, `r7` | `+0x1F0`, `+0x240` (screen-transform/aux descriptors) |

After `0xD080` returns the body does:

```
ptr35 = ls[ctx+0x114]                  // still the candidate index count
kept  = rotqbyi(ls[sp+0x20], 8)        // slot word 2
ls[ctx+0x114] = 3 * kept               // final kept index count
ls[ctx+0x118] = {0, ...}
ls[ctx+0xA0]  = merge(flag, old)       // job flag, not a count
```

The replay confirms this: the 400-pair union notes in
[`edge-holes-fix.md`](edge-holes-fix.md) §4.1 measured `+0x114` 783 at entry
and 264 after the command, i.e. 88 kept triangles, `3 * 88`.

**Consequence.** A union inside the `0xD080` hook cannot read the final kept
count (it is written by the caller afterwards). The fused pass therefore:

1. runs the body once;
2. lets the caller write the game-grid `+0x114` exactly as the lifted path;
3. calls `c690_cull_fix_end()` from the wrapper's mode-3 branch, which appends
   the sample-only triangles and sets `+0x114 = old + 3 * added`.

That is the same output as the proven union driver, with the second command
body pass and the real-context LS snapshot removed.

## 2. Implementation

| file | change |
|---|---|
| `runtime/spu/c690_holes.c/.h` | pure list merge: canonical vertex-set membership, append in sample-list order, never touch/reorder the original entries, cap from the caller's `+0x1E0`; 3 unit tests |
| `runtime/spu/c690_native_screen.c/.h` | `run_holes()`: sample-grid stage on a private context (LS+GPR copy, 2x transform patch, read kept count at `slot+8`, restore the transform) + exact game grid on the real context; `c690_native_screen_holes_merge()` for the post-count append; new counters |
| `runtime/spu/c690_cull_fix.c/.h` | `begin()` mode 3 (no snapshot, records `+0x110/+0x1E0` and the 0x25/0x26 family from `r4`); `end()` merge + `+0x114` update; `probe_transform()` and `patch_2x()` exported for the private sample run |
| `libs/video/ps3_live_ctl.c/.h` | new live key `PS3RECOMP_C690_HOLES_NATIVE` (`PS3_LIVE_C690_HOLES_NATIVE`, appended to the enum) |
| `tools/c690_ref/patch_cull_hooks.py` | inserts the mode-3 wrapper branch; a fresh patch of the current shared module is byte-identical to the private `spu_lifted-e1` module |
| `tools/c690_ref/run_native_screen_test.sh`, `test_native_screen.c` | fifth gate mode `holes`; kept-count / buffer-extent consistency checks |
| `CMakeLists.txt`, `tests/test_c690_holes_merge.c` | `c690_holes_merge` unit test |

**Sample run.** `run_holes()` reads the family from the value captured at
`begin()` (`r4 & 1`). Every captured **0x25** job carries no screen-transform
block in LS; every captured **0x26** job does. The sample run is therefore
attempted only for 0x26, unless a 0x26 command in the same image has already
cached a transform, in which case 0x25 honors the cached positive too. The
sample run copies the entry LS (256 KiB) and GPRs into a per-lane context,
patches the transform copies by 2 on x, runs the flattened `0xD080` stage,
reads the kept triangle count from the slot, restores the transform, and
discards the context. The real context is never snapshotted/restored.

**Merge.** `c690_holes_merge_lists()` walks the sample list in order, skips a
triangle whose canonical vertex set already exists anywhere in the game list
or an earlier appended one, stops at the proven buffer extent
`(+0x1E0 - +0x110)/2` u16 entries, and never edits or reorders the game
entries. `+0x114` is rewritten only after the caller's own write, so the
walker always sees `3 * (game_tris + added)`.

## 3. Offline gate (128 captured pairs)

`tools/c690_ref/run_native_screen_test.sh` with the private module
`recomp-work/spu_lifted-e1/jobmod_c690c2f7b09a_eF5B8` over
`edge_e1_corpus` (64 x 0x25 + 64 x 0x26, captured 2026-11-04):

| item | value |
|---|---:|
| pairs | 128 |
| baseline lifted vs capture | 0 mismatches |
| native mode 0: LS / GPR / PC differs | **0 / 0 / 0** |
| union removed/reordered | 0 |
| union added triangles | 0 |
| union != lifted union (full LS/GPR) | 0 |
| **holes removed/reordered** | **0** |
| **holes added triangles** | **0** |
| **holes != lifted union (full LS/GPR)** | **0** |
| holes LS/GPR != native union | 0 |
| holes kept count odd | 0 |
| holes kept count != dumped list length | 0 |
| holes kept count != union count | 0 |
| holes buffer end / pointer moved | 0 / 0 |
| holes sample runs / fallbacks | 64 / 64 |
| holes cap-skipped / setup skips | 0 / 0 |
| RESULT | **PASS** |

The same gate passes on `edge_e1_corpus_25` (64 pairs, 0 sample runs, 64
fallbacks, 0 added) and `edge_e1_corpus_26` (64 pairs, 64 sample runs, 0
added). The 128-pair corpus contains no real sample-only triangle, so the
"adds" branch is covered by the merge unit test and by the full-LS equality
with the lifted union (which uses the same merge); the older 400-pair corpus
that had **7 added triangles in one pair** is no longer on disk.

**A real bug found by the gate.** The first pass of the private refactor
cached a negative transform scan for 256 calls. In the gate the first 64
pairs (0x25, no transform) cached the negative and the following 64 0x26
pairs were scored inert (0 sample runs). Live it would have skipped the fix
for up to 256 calls after each no-transform job. `cull_ensure_scan()` now
never caches a negative; only the fused probe may skip a rescan, and only for
0x25.

## 4. Cost and meter estimate

Whole 0x132E0 call, 128 pairs, gcc `-O2 -march=native`. A second mission's
`replay_rsx_engine` ran for part of this session, so absolute values varied
between runs; the ordering was stable. Final gate run (log
`recomp-work/build-e1/e1_gate_final.log`), with the three-repeat medians in
parentheses:

| mode | mean us/call | ratio to lift |
|---|---:|---:|
| lifted body | 22.2 (24.9) | 1.00 |
| native 0xD080, exact (mode 0) | 19.9 (18.9) | 0.90 (0.76) |
| **fused holes (0x25/0x26)** | **41.9 (39.7)** | **1.89 (1.59)** |
| native union (mode 1) | 112.8 (116.3) | 5.08 (4.67) |
| lifted union driver | 119.9 (116.2) | 5.40 (4.67) |

Split by family (quiet 3-repeat medians): 0x25 lifted 19.1 / native 19.2 /
holes **17.6 us**; 0x26 lifted 22.0 / native 18.3 / holes **63.6 us**. 0x25
therefore runs at the native cost (slightly better than lifted); only 0x26
pays for the second stage evaluation.

**Meter estimate.** Step 5 measured 0x25 = 8.6 % and 0x26 = 4.1 % of the
meter c690 wall (25.7 and 37.2 us per lifted call). With the live-validated
native `0xD080` at 1.53x and the measured sample cost:

* 0x25: native is ~1.4x faster than lifted, and the probe adds no scan after
  the first call -> saves ~7-9 us/call, ~36 ms/s of lane time;
* 0x26: two native stages plus the 256 KiB copy -> about +20-26 us/call,
  ~28-36 ms/s.

The two roughly cancel (net within +/-1 % of the c690 lane wall, i.e. below
the frame noise). The union's own excess is ~3.3x the lifted cull, and it
measured -4.8 fps; the fused pass is ~1.5x, about a third of that, so the
expected meter effect is **~-0.5 to 0 fps**. This is an estimate, not a
measurement: the mission forbids game launches.

A true single-evaluation union (one triangle pass accepting if either grid
accepts) would need the acceptance predicate fused inside the generated
`0xD080` stage; that is the remaining optimization if the A/B shows a real
cost.

## 5. Exact commands for the next mission / the user

### 5.1 Merge and build (next mission, in `ps3recomp-play`)

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-play
git fetch fork
git checkout exp/live-ab
git merge --no-ff fork/exp/edge-e1-holes
# re-patch the generated module after the merge (idempotent; a fresh patch is
# byte-identical to the private module used for the offline gate)
python3 tools/c690_ref/patch_cull_hooks.py \
  ../recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c
ninja -C ../recomp-work/build-play MyGameRecomp
```

No `spu_context` field was added; the existing ABI check still passes. Do not
combine `PS3RECOMP_C690_HOLES_NATIVE=1` with `PS3RECOMP_C690_NATIVE_SCREEN=2`
or `PS3RECOMP_C690_CULL_FIX=1`: the fused path wins at `begin()` and the union
key is then ignored (the output is still the fused union, but the counters
become confusing).

### 5.2 Meter A/B (key off/on), BYPASS off

One launch, alternating arms, meter screen; `PITCH_THRESH=35` (25 misses the
cube render targets). Both arms must name every key (the control file does
not revert an omitted key):

```sh
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
TAG=e1holes PITCH_THRESH=35 N=6 SEG=10 DROP=2 METER=1 SKIP_CINE=1 PIN=0 \
  WCB=2 VIDEODRIVER=x11 REPORT=0 LIMIT=900 SETTLE=10 \
  PS3RECOMP_C690_GAP=1 PS3RECOMP_WALKER_PROF2=1 PS3RECOMP_C690_STATS=1 \
  ARMS='A=PS3RECOMP_C690_HOLES_NATIVE=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0|B=PS3RECOMP_C690_HOLES_NATIVE=1;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  BOOT_ARM='PS3RECOMP_C690_HOLES_NATIVE=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  ./ab_live.sh

python3 pin_ab.py --log ../ab_live_e1holes_<ts>.log \
  --marks ../ab_live_e1holes_<ts>.marks --seg 10 --drop 2
```

Gate to ship: **B-A within +/-1 fps, p90/p99 not worse**, and the log's
periodic `[c690-screen] holes-progress` lines show `holes_runs > 0` on the
meter (0x26 sample evaluations). The harness SIGKILLs the process, so the
`atexit` counters are lost; the progress lines carry the live numbers.

### 5.3 User visual check (pitch close-up, eyes), BYPASS off

From `recomp-work`, on the same pitch close-up used in
`edge-native-step4.md` §6 (pitcher's eyes visible), then let it run into the
cinematic:

```sh
# fused holes pass ON (this mission's key)
MLB16_CULL_BYPASS=0 PS3RECOMP_C690_CULL_FIX=0 \
  PS3RECOMP_C690_NATIVE_SCREEN=0 PS3RECOMP_C690_HOLES_NATIVE=1 \
  ./play_windowed.sh

# control: everything off (holes are the baseline artifact)
MLB16_CULL_BYPASS=0 PS3RECOMP_C690_CULL_FIX=0 \
  PS3RECOMP_C690_NATIVE_SCREEN=0 PS3RECOMP_C690_HOLES_NATIVE=0 \
  ./play_windowed.sh
```

Expected: the pin-holes are gone with the key on and the geometry looks
unchanged otherwise (no missing triangles, no new shimmer). If they remain,
look at the exit/progress counters:

* `holes_runs = 0` -> no screen-transform block was found in that scene, so
  the 2x rule was never evaluated (the union would be inert too);
* `holes_runs > 0`, `holes_added = 0` -> the sample grid kept the same list;
  the remaining holes are not this rule;
* `holes_cap_skipped > 0` or `union_skip_range > 0` -> the buffer extent was
  too small; report it before changing anything.

`MLB16_CULL_BYPASS=3` remains the fallback control only (it fixes the holes
at -7.6 fps and is never a shipping default).

## 6. Deviations and caveats

* **Zero game launches** (mission rule). There is no live fps number, no live
  shadow soak and no user visual check in this mission; §5 is prepared for
  the next one.
* **One command pass, two stage evaluations.** The fused pass removes the
  second 0x132E0 body run and the real-context snapshot/restore, but the
  `0xD080` subtree is still run once per grid (game on the real context,
  sample on a private copy). A single-evaluation per-triangle union needs the
  acceptance predicate fused inside the generated stage; the current result
  is already ~3.2-3.5x cheaper than the union.
* **0x26-only sample probe.** The sample run is skipped for 0x25 unless a
  cached positive exists. On the 128-pair corpus every 0x25 job has no
  transform and every 0x26 job has one, so this reproduces the union exactly;
  a future corpus with a 0x25 transform is handled by the cached positive.
* **The 400-pair corpus is gone.** The gate uses the 128-pair corpus
  captured for this mission; it has 0 sample-only triangles, so the "adds"
  path is proven by the merge unit test and the bit-identical union equality,
  not by a real added pair.
* **Timings were taken while another mission's replay benchmark ran.** The
  absolute us/call are inflated and vary; the ratios (fused vs lifted vs
  union) were stable across repeats.
* The emitted mode-3 wrapper is generated by
  `tools/c690_ref/patch_cull_hooks.py`; a fresh patch of the current shared
  module was verified byte-identical to the private `spu_lifted-e1` module.

## 7. Clean room and licence

This is our own reverse engineering of the game's lifted SPU c690 module and
the captured LS/GPR pairs. The stage-split/corpus method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied.
No RPCS3 or other GPL emulator source was read or cited.
