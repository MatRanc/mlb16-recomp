# c690 0x2A native field pack: 112/131 classified pairs policy-exact, the live A/B is neutral

Date: 2026-11-05. Branch `exp/native-2a` in the `ps3recomp-n2a` worktree (from
`exp/live-ab`), build `recomp-work/build-n2a`. Code pushed to `fork` only;
`PS3RECOMP_C690_NATIVE_2A` stays **default 0**. Two pitch-meter launches (the
cap): run 1 carried a per-layout lookup bug found afterwards; run 2 is the
clean measurement. Offline: the `fncap2b2w3` class corpus, a field-pass replay
gate, a per-class timing bench, the new unit test, the video fixtures and the
ABI check.

This follows [`edge-gpu-binding.md`](edge-gpu-binding.md) (whose §7.6 says the
GPU stage pays only once the SPU pack is replaced), the class decode in
[`edge-gpu-2a-classes.md`](edge-gpu-2a-classes.md),
[`edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md),
[`edge-exactness.md`](edge-exactness.md) and
[`c690_function_map.md`](c690_function_map.md).

## TL;DR

* **The record pack is not `0x10C70`/`0x109C0`.** `0x10C70` parses the stream
  descriptor and `0x109C0` builds the per-stream conversion/prefix table; the
  actual record assembly is four per-field passes over the record list:
  `0x7840` (position copy), `0x7FC0` (normal 11:11:10), `0x7AC0` (fixed s16,
  2/3/4 components) and `0x7540` (f16 x 2). The driver around them (index
  pre-pass, descriptor parse, allocator, cursor) stays lifted in every design
  so far. This mission replaces the four passes, not the command.
* **A native field pack is policy-exact on 112/131 classified pairs
  (85.5 %; 58.3 % of all 192 sampled 0x2A pairs).** Positions are bit-exact,
  normal/s16/f16 are <= 1 LSB, no byte is written outside the field, and the
  only GPRs that differ from the lifted run are the SPU caller-saved
  `r2..r79` scratch (r0/r1/r80..r127 exact). The 19 attribute-first pairs
  (`0x00A00004`, SPU prefix written by the lifted pass) fall back
  byte-for-byte; so do the 31 first-in-job and 30 no-position pairs, which are
  outside the decoded classes.
* **The live A/B is neutral.** Run 2 (`ab_live_native2b_20261105_044645`,
  N=5 x 10 s, 2 s dropped): fps 35.15 -> 35.77 (paired **-0.59 +/- 2.75**,
  95 % CI [-3.34, 2.16]), p50 **34.15 ms both**, p90 38.35 -> 38.12, p99
  41.23 -> 41.04. The per-command timer gives **0x2A 21.92 -> 21.59 us/call
  (-0.33 us, -1.5 %)**; at ~535 0x2A calls/frame that is ~0.18 ms of the
  total c690 lane time, i.e. below the ~1 ms frame noise. The c690 job wall
  and the thumbnails are unchanged.
* **Why the saving is small, and what it means.** Offline, replacing all four
  passes saves **-9.9 us (-43 %)** on the 20-byte class, **-5.8 us (-38 %)**
  on the position-only class and **-6.5 us (-22 %)** on the 44-byte class
  (whole `0x10F78`), but the largest class, position-first 32-byte, is
  **+2.4 us (+11 %)** slower with the scalar pack. Weighted by the meter class
  mix (and only 58 % of sampled pairs covered) that is about the measured
  -0.3 us. **A GPU version of the same stage inherits this ceiling**: a
  perfect pack replacement removes ~0.2-0.5 ms of lane per frame at best,
  while the measured GPU overlay hand-off costs 5.96 ms/frame
  ([`edge-gpu-binding.md`](edge-gpu-binding.md) §7) and the async queue still
  loses ~7.6 ms at p90 (§8). The pack is not the meter bottleneck; the
  driver/pre-pass and the latency-bound burst are.
* **Next stage, by value:** vectorise the 32-byte class pack (the only
  regression), then attack the whole `0x2A` command (pre-pass included) or
  `0x21` (4.74 ms, pure compute). A per-stage CPU rewrite of the culls is
  already known neutral (1.53x, [`edge-native-step4.md`](edge-native-step4.md)).

## 1. What the lifted 0x2A pack actually is

The GPU-binding doc's next-stage hint names `0x10C70`/`0x109C0`. The replay
trace of `0x10F78` shows those are the descriptor/prefix path:

| LS | role | calls per 32-byte job |
|---|---|---|
| `0x10F78` | 0x2A driver: allocator `0x49D4`, staging `0xEA70`, 8 stream descriptors | 1 |
| `0x49D4` | LS allocator (fills the `0x23400` table) | 1 |
| `0xEA70` | staging / empty-list exit | 1 |
| `0x10C70` -> `0x109C0` | per-stream descriptor parse + conversion/prefix table | up to 8 |
| `0x7840` -> `0x78C0` -> `0x7960`/`0x7998` | **position** gather/write | 1 |
| `0x7FC0` -> `0x8040`/`0x8120` -> `0x8160` | **normal** 11:11:10 pack | 1 |
| `0x7AC0` -> `0x7B28` -> `0x7BD8` -> `0x7C18` | **fixed s16** pack (2/3/4 components) | 1+ |
| `0x7540` -> `0x75C0`/`0x7678` -> `0x76B8` | **f16 x 2** pack | 1 |

Each field pass receives everything in GPRs: `r3` source array base, `r4`
destination = record 0 + field offset, `r5` record stride, `r7` index list
(u16 byte offsets), `r11` count, `r6` component count and `r22` field kind
(0 position, 1 f16, 2 fixed, 4 normal). The four passes each write their own
field; the lifted versions write overlapping 16-byte vectors, but for the
decoded classes every byte belongs to exactly one field (plus the SPU-written
attribute word at `+28..31`, which is already correct in LS when the pass
runs; verified on the whole corpus by a field-pass-skipped replay).

The index list at `r7` is a u16 byte-offset table for formats A and the 9
packed-offset sub-variants, and *not a list* for format B (the driver then uses
the implicit `k*16`). Every real list entry is 16-byte aligned and below
`0x8000`; the format-B payload fails that test on all 60 corpus pairs.

## 2. What was built

New runtime `runtime/spu/c690_native_2a.c/.h` and
`tools/c690_ref/patch_native_2a.py` (idempotent):

* the patch renames each of the four field-pass entries to `*_body` and adds an
  entry wrapper that calls `c690_native_2a_try(ctx, addr)`; on a claimed pass
  the wrapper returns (the hook publishes `pc = gpr[0]` and the trampoline,
  exactly like the lifted terminal `SPU_RET`), otherwise it `musttail`s into
  the lifted body;
* the reference is **weak** in the generated module (an older play/gpu tree
  still links and takes the lifted body); a `__attribute__((used))` anchor in
  the game-only `runtime/spu/spu_dispatch_mt.c` makes the strong reference
  that extracts the hook object from the runtime archive;
* the class is keyed on `(descriptor first word, record stride)` read from
  `LS[0x1F2D0]`/`LS[0x1F0D0]` (the same context fields `0x10F78` reads):
  32-byte `0x007C0004`/`0x00940004`, 20-byte `0x00C40008`, 12-byte
  `0x00600008`, 28-byte `0x00900004`, 44-byte `0x011C0004`. Everything else
  falls back;
* the field offset is `r4 - (cursor - count*stride)`, so the same code handles
  the 44-byte class's five fixed-s16 passes (one pass address, five offsets);
* the pack rules are the gated ones from `edge-gpu-2a-classes.md` §2
  (`clamp(trunc(v*scale) - 1)`, f16 mantissa truncation with denormals to
  signed zero), positions copied byte-exactly;
* the index mode is scanned once per invocation on the position pass and
  cached for the other passes (a mismatching cursor/list/count/stride
  rescans);
* counters: `[c690-native2a] progress pass= native= fallback= classes=
  class_native= class_fallback= fallback_share= records=` every ~2 s.

`PS3RECOMP_C690_NATIVE_2A` is a live control-file key
(`libs/video/ps3_live_ctl.*`, appended at the end, default 0), with the same
environment fallback as the other c690 keys. No `spu_context` field was added.

## 3. Offline gates

Corpus: `recomp-work/captures/gpu2a/fncap2b2w3` (192 0x2A pairs). The replay
runs the patched generated module (the exact build's module) on each pair's
captured IN LS/GPR and compares the final LS/GPR with the captured OUT.

| gate | result |
|---|---|
| 32-byte position-first (`32b_pos`, incl. the 9 sub-variants) | **60/60 pairs**, positions 0 differing bytes, packed <= 1 LSB, 0 outside writes |
| 20-byte (`20b`) | **17/17** |
| position-only 12-byte (`pos`) | **17/17** |
| 44-byte (`44b`) | **13/13** |
| 28-byte position-first (`28b_pos`) | **5/5** |
| **native total** | **112/131 classified pairs = 85.5 %**; 112/192 sampled pairs = 58.3 % |
| attribute-first (`32b_attr`, `28b_attr`) | **19/19 fallback, byte-identical to the lifted OUT** |
| first-in-job (31) + no-position (30) | outside the decoded classes; the key-on output equals the key-off output byte-for-byte (the 31 are the known non-replayable low-LS SPURS pairs) |
| GPR after the routine | **r0, r1 and r80..r127 exact on all 112 pairs**; only the caller-saved `r2..r79` field-pass scratch differs (SPU ABI) |
| `test_c690_native_2a` (new) | **PASS** (supported layout, exact positions, pack rules, unknown descriptor fallback) |
| `tools/test_video_fixtures.py` | 31/32; `test_gcm_tick_in_drain` is a timing fixture that also fails intermittently on the base `exp/live-ab` tree under load (its 16999 us bound needs < 1 us of scheduler skew) |
| `check_spu_context_abi.sh` | **PASS** (no `spu_context` change) |

## 4. Offline per-class cost

Whole `0x10F78`, key off vs on, same pair, 700-1500 replays pinned to one CPU
(median of 5-7 alternating runs; includes the per-iteration 256 KiB reset, the
same in both arms):

| class (example seq) | key off | key on | delta |
|---|---:|---:|---:|
| 32-byte position-first (453000) | 21.5 us | 23.8 us | **+2.4 us (+11 %)** |
| 20-byte (184000) | 23.0 us | 13.1 us | **-9.9 us (-43 %)** |
| 44-byte (865000) | 29.6 us | 23.1 us | **-6.5 us (-22 %)** |
| position-only (657000) | 15.1 us | 9.3 us | **-5.8 us (-38 %)** |

The 32-byte class is the largest (31 % of sampled pairs) and the only
regression: its scalar `s16x4`+normal conversion does more work than the
lifted vector path. That is why the live net is small.

## 5. Live pitch-meter A/B

Run 2, `BUILD=build-n2a`, `METER=1 SKIP_CINE=1 WCB=2 PIN=0`,
`VIDEODRIVER=x11`, `PITCH_THRESH=35`, `SETTLE=20`, governor `performance`, one
X, N=5 x 10 s, 2 s dropped, arms `A=PS3RECOMP_C690_NATIVE_2A=0` /
`B=...=1`. The key switched 9 times without a crash.

| arm | frames | fps | p50 ms | p90 ms | p99 ms | std | walker ms | wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| A (key off) | 1406 | 35.15 | **34.15** | 38.35 | 41.23 | 11.00 | 28.11 | 13.18 |
| B (key on) | 1431 | 35.77 | **34.15** | 38.12 | 41.04 | 15.24 | 28.63 | 13.36 |

Paired per-segment difference: **-0.59 +/- 2.75 fps**, 95 % CI [-3.34, 2.16].
The p50 is the known 34.15 ms two-vsync floor in both arms.

Per-command `[c690-stats]` deltas over the four clean 10 s windows:

| metric | A | B |
|---|---:|---:|
| 0x2A inclusive | 21.46-22.14 us (median **21.92**) | 21.34-21.82 us (median **21.59**) |
| 0x2A delta | | **-0.33 us/call (-1.5 %)** |
| 0x2A calls / 10 s | ~178-190 k | ~182-191 k |
| c690 job wall | ~88 ms/frame (all lanes) | ~88 ms/frame |
| 0x2A share of the wall | 15-24 % | 15-24 % |

Coverage from the counters during key-on arms: **class-level fallback 18.4 %**
(attribute-first and the remaining unsupported layouts), pass-level native
56 % (the pass count is weighted by 1-pass position-only and 7-pass 44-byte
classes, and the unsupported classes fall back all their passes). Thumbnails
(2 s, 4x4 grey downsample): within-A mean abs diff 1.74, within-B 1.61,
A<->B 1.91 - no visual divergence beyond the static scene's own frame noise;
the A/A and B/B bands look identical in `..._scene_sheet.png`.

Run 1 (`ab_live_native2a_20261105_044005`) was the same build before the
per-layout lookup fix: the 44-byte class ran only 3 of its 7 passes natively
(the lookup matched the first pass address, not address+offset), and the first
two segments were still settling (17.9 / 14.0 fps). Its 0x2A delta (A median
21.9 us, B 21.7 us) is not used. Both launches are within the 2-launch cap.

## 6. What this implies

### (a) A GPU version of the same stage

The pack replacement is what the GPU stage needs to stop the SPU work, but the
measured value of removing it is **0.33 us per 0x2A call** at the current
coverage (0.2-0.5 ms of lane per frame if the 32-byte regression is fixed).
The GPU binding's own hand-off was measured at **5.96 ms/frame** (submit +
fence) and the async queue still loses ~7.6 ms at p90
([`edge-gpu-binding.md`](edge-gpu-binding.md) §7-§8). So a GPU pack can only
pay after the hand-off is essentially free *and* the whole command
(pre-pass included) moves; skipping the pack alone cannot recover the binding
cost. The 0x2A driver (allocator, descriptor parse, index pre-pass, cursor)
is now the larger share of the command and has no native or GPU version.

### (b) The same native-first approach for 0x21 (4.74 ms) and the culls (3.03 ms)

* **The culls are already answered:** a native `0xD080` stage is exact and
  1.53x per call but live-neutral ([`edge-native-step4.md`](edge-native-step4.md)),
  for the same reason - the burst is latency-bound and the per-stage share is
  a fraction of the frame.
* **0x21 is the better target, but not by porting one leaf.** It is pure
  compute (0 DMA, 0 waits, [`c690-emit-ring.md`](c690-emit-ring.md)) and the
  largest slice. The 0x2A probe shows that replacing one layer of the command
  leaves the driver/pre-pass and yields a fraction of the class time; 0x21
  should be replaced as a whole command (the emit/ring writer), with a
  >= 2x target that moves the burst, not a 1.0-1.5x per-function rewrite.
* **The 0x2A native pack itself is worth keeping as the GPU fallback:** if a
  GPU stage eventually runs, the same class table gives the CPU path for the
  uncovered classes and a correctness reference. It stays default off.

## 7. Reproduction (offline)

```sh
cd /home/m/Documents/mlb16-online/ps3recomp-n2a

# 1. patch the generated module (game data, not committed) and rebuild
python3 tools/c690_ref/patch_native_2a.py
ninja -C ../recomp-work/build-n2a MyGameRecomp

# 2. replay gate on the 192-pair corpus (see the mission's harness; the
#    patched module + runtime/spu/c690_native_2a.c on each pair)
#    positions exact, packed <= 1 LSB, fallback byte-identical

# 3. unit test and ABI
../recomp-work/build-n2a/ps3recomp_sdk/test_c690_native_2a
SDK=$PWD ../recomp-work/ab-tools/check_spu_context_abi.sh

# 4. live A/B (one launch)
cd ../recomp-work/ab-tools
PS3RECOMP_C690_STATS=1 TAG=native2b BUILD=build-n2a N=5 SEG=10 DROP=2 \
  METER=1 SKIP_CINE=1 WCB=2 PIN=0 VIDEODRIVER=x11 REPORT=1 \
  PITCH_THRESH=35 LIMIT=480 SETTLE=20 \
  ARMS='A=PS3RECOMP_C690_NATIVE_2A=0|B=PS3RECOMP_C690_NATIVE_2A=1' \
  BOOT_ARM='PS3RECOMP_C690_NATIVE_2A=0' ./ab_live.sh
```

## 8. Clean room

Own static analysis of the EBOOT (Ghidra), the lifted c690 module, our own
runtime and captures. The stage/corpus/differential method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied.
No RPCS3 or other GPL emulator source was read or cited.
