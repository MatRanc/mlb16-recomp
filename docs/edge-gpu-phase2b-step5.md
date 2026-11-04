# c690 Edge on the GPU — phase 2b step 5 (clean map: 0x22 tree/cull was hiding inside 0x21 sel1)

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. Two automated pitch-meter launches (`ab_live.sh`,
governor `performance`), no capture spool, plus offline parsing of the run's
2 s reports and the raw module handler table. Nothing is default-on; the play
build has no capture wrappers.

This follows [`edge-gpu-phase2b-step4.md`](edge-gpu-phase2b-step4.md),
[`edge-gpu-phase2b-step3.md`](edge-gpu-phase2b-step3.md),
[`edge-gpu-phase2b-step2.md`](edge-gpu-phase2b-step2.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690_function_map.md`](c690_function_map.md) and
[`pin-and-burst-path.md`](pin-and-burst-path.md).

## TL;DR

* **The capture bug from step 4 is fixed.** The per-key pair cap is now
  checked before the 256 KiB snapshot, and a command-boundary 0x21 whose key
  is not known until the sub-selector entry defers the snapshot to that entry
  (still after the cap check). A new `cap` unit case sends six same-key
  commands with `PS3RECOMP_C690_TAIL_PAIRS=2` and verifies **2 pairs and
  exactly 2 snapshots**; a capped call now costs a key lookup and one atomic
  load, not 6.75 us of `memcpy`.
* **The step-3/step-4 histogram was not the only error: the command map was
  missing opcode 0x22.** The raw handler table at LS 0x14770 maps **opcode
  0x22 -> LS 0x10190**, the continuation of the `0xFDA0` tree/cull stage.
  `c690_stats` did not know that handler, so a 0x21 command never closed at
  0x22 and the entire 0x22 command was charged to the 0x21 window. That is
  the source of the "sel1 = 390 us/call" number.
* **Clean meter map (30.2 s window, 403,212 jobs, 74.31 s of c690 lane wall):**
  the real 0x22 tree/cull is **17.67 s, 23.8 % of the c690 wall, 618.9
  us/call**; the real 0x21 sel1 is **2.46 s, 3.3 %, 48.7 us/call**; sel7 is
  **14.43 s, 19.4 %, 57.6 us/call**. True 0x21 is **22.8 %** of the c690
  wall, not 43-47 %.
* **Full-tail GPU C is ~12 ms, not ~17 ms.** On the documented 24.39 ms
  critical-lane basis, 0x21 + 0x25/0x26 + 0x2A is 49.3 % -> **12.0 ms**. The
  old 17.4 ms model used a 0x21 share inflated by the missing 0x22. Only if
  the already-native 0x22 tree/cull is added does the total return to
  ~17.8 ms.
* **Target order changes.** 0x2A is the cheapest remaining win (kernel
  already bit-exact); the 0x22 tree/cull is the largest slice but already
  has a CPU-native port; the best remaining GPU target is sel7/A3E0 at
  ~4.7 ms / 2,493 lines. sel1 is now a 0.8 ms target and no longer worth a
  port.
* **Sanity:** same-run `build-play` p50 is **34.15 ms** (exactly the known
  value); the clean segment is 35.88 fps. Instrumentation overhead is
  estimated at ~0.9 % of one lane on top of the existing ~1.95 % stats timer.

## 1. Capture fix

`c690_tail_open` used to take the 256 KiB LS + GPR snapshot before
`c690_tail_close` checked the per-key pair cap (step 4, 6.75 us/copy). Now:

* the wrapper computes the format key before `c690_tail_open` (0x21:
  `(sub-selector LSA << 8) | (r6 & 0x1f)`, otherwise the opcode);
* `c690_tail_open` checks `pairs < PS3RECOMP_C690_TAIL_PAIRS` and only then
  reads the eight command words and copies LS/GPRs;
* a command-boundary 0x21 enters with an unknown key, so its snapshot is
  deferred to the sub-selector entry; the cap is checked there first;
* the trigger check was already before the open, so an un-triggered call is
  still only a file-poll/flag test.

`runtime/spu/tests/test_c690_tail_capture.c` gained a `cap` mode and
`tools/test_c690_tail_capture.py` checks it:

```
PASS: c690 tail capture IN/OUT pairs, format key, bitmap, cap, stride,
      0x2A function exit, A730 leaf, non-consuming trigger, off
```

The `[c690-tail] hist` line now reports `snapshots=`, so the test can assert
`snapshots=2` for six calls at a cap of two. The spool still counts every
call and its time for the histogram; only capped snapshots are skipped.

## 2. Clean timing profile

### 2.1 What was measured

The second launch used the existing timing-only key
`PS3RECOMP_C690_OP21=1` (no tail capture, no snapshots). The c690 stats code
already has the per-function exclusive clock at every lifted entry and a
per-command inclusive clock; this mission added:

* per-selector **windows** for 0x21 (sel1 at 0x13140, sel7 at 0x131A0,
  other) with a dynamic per-window LS-keyed exclusive table, so the bodies
  on each path are named instead of collapsing into the hot-table "other";
* an **all-class DMA** interval around the synchronous fast-path copy,
  charged to the current command (the runtime has no MFC tag wait; the copy
  is the host-side "DMA wait").

The numbers below are a **delta of two periodic reports**, elapsed
189.1 s -> 219.3 s (30.2 s), which is the meter segment window. The run's
flip counter ticks ~1.86 times per presented frame, so `jobs/frame` from that
counter is not used; calls/frame below use 884 frames at the measured p50
34.15 ms. The class shares and us/call do not depend on that.

### 2.2 Class map at the meter

Window: 403,212 c690 jobs, 74.31 s of c690 lane wall, 13,351 jobs/s.

| class | calls | calls/frame | share of c690 wall | us/call | ms/frame @24.39 ms | ms/frame @16.8 ms |
|---|---:|---:|---:|---:|---:|---:|
| 0x22 tree/cull (`0x10190`, 0xFDA0) | 28,560 | 32 | **23.79 %** | **618.86** | 5.80 | 4.00 |
| 0x21 sel7 (`0x131A0`) | 250,308 | 283 | 19.41 % | 57.63 | 4.74 | 3.26 |
| 0x2A assembler | 478,778 | 542 | 14.06 % | 21.82 | 3.43 | 2.36 |
| 0x13 16-byte copy | 403,213 | 456 | 9.62 % | 17.73 | 2.35 | 1.62 |
| 0x25 cull | 223,710 | 253 | 8.37 % | 27.80 | 2.04 | 1.41 |
| 0x0A unpack/pack | 1,841,133 | 2,083 | 6.84 % | 2.76 | 1.67 | 1.15 |
| entry/parse + dispatcher | - | - | 5.20 % | - | 1.27 | 0.87 |
| 0x26 cull | 77,945 | 88 | 4.04 % | 38.54 | 0.99 | 0.68 |
| 0x21 sel1 (`0x13140`) | 50,575 | 57 | 3.31 % | 48.65 | 0.81 | 0.56 |
| 0x27 index copy | 340,143 | 385 | 1.50 % | 3.29 | 0.37 | 0.25 |
| 0x1F finalize | 403,213 | 456 | 1.28 % | 2.36 | 0.31 | 0.22 |
| 0x04/0x07/0x08 setup | 1,209,642 | 1,368 | 1.73 % | 1.06 | 0.42 | 0.29 |
| 0x0C index decompress | 340,143 | 385 | 0.72 % | 1.56 | 0.17 | 0.12 |
| 0x09/0x2C/other | 2,380 | 3 | 0.03 % | 10.38 | 0.01 | 0.01 |

`16.8 ms` is this run's critical-lane busy per 34.15 ms frame (74.31 s
/ 5 lanes / 30.2 s x 34.15 ms); `24.39 ms` is the documented big-burst
critical-lane busy from [`pin-and-burst-path.md`](pin-and-burst-path.md),
kept for comparison with the earlier phase-2 numbers.

The all-class DMA total is **0.291 s (0.39 % of the c690 wall)**, all in
0x13 (2.067 M copies, 254.9 ms) and 0x04 (340 k, 36.1 ms). 0x21 has **0**
DMA and **0** tag/atomic/decrementer reads. There is no hidden DMA wait.

### 2.3 The missing opcode 0x22

The raw module's parser handler table at LS 0x14770 (EBOOT file offset
0x1161370, big-endian words) has 41 non-empty entries. Its entry for opcode
0x22 is **0x10190**, and 0x10190 is the continuation of the 0xFDA0
tree/cull state machine (the same stage as the native
`c690_native_stage.inc`; see [`edge-native-step2.md`](edge-native-step2.md)).
The old `s_op_map` ended at 0x21 -> 0x25, so 0x22 was invisible. The
per-window table is what exposed it:

```
win sel1 @0x13140 0x10190  17,674,586 us  87.8%  n=28,560  avg=618.86 us
win sel1 @0x13140 0x0B830     728,148 us   3.6%  n=4,115,020  avg=0.18 us
win sel1 @0x13140 0x0A8A0     602,552 us   3.0%  n=4,535,685  avg=0.13 us
...
```

The sel1 window is 20.13 s. Subtracting the 0x22 handler leaves **2.46 s**
of real sel1 emit, which is the 0xB830/A8A0/A808/B958/A730/A798 pack/format
family. The old "390 us/call sel1" is therefore 618.9 us of 0x22 on 56 % of
the calls plus 48.7 us of sel1 on the rest, not an emit kernel.

The complete 0x22 mapping is now in `s_op_map` (`0x10190 -> 0x22`) together
with the other missing table entries, so a future run will close a 0x21
command at a 0x22 entry. The numbers in this document are corrected offline
from the run's per-window function table, not from a third launch.

The 0x22 count above is the number seen **inside sel1 windows**. If a 0x22
ever follows another command it is not counted here, so 23.79 % is a lower
bound. In the measured workload it never appears in a sel7 window, and the
sel1/sel7 totals reconcile with the 0x21 inclusive time, which is consistent
with 0x22 following sel1 only.

### 2.4 Inside sel1 and sel7

sel1 (50,575 calls, 20.13 s inclusive with 0x22; 2.46 s without):

| LS | us in window | share | entries | us/entry |
|---|---:|---:|---:|---:|
| 0x10190 (0x22 / 0xFDA0) | 17,674,586 | 87.78 % | 28,560 | 618.86 |
| 0x0B830 (A730 leaf) | 728,148 | 3.62 % | 4,115,020 | 0.18 |
| 0x0A8A0 | 602,552 | 2.99 % | 4,535,685 | 0.13 |
| 0x0A808 | 555,535 | 2.76 % | 2,961,910 | 0.19 |
| 0x0B958 | 196,903 | 0.98 % | 1,405,390 | 0.14 |
| 0x0A730 | 182,196 | 0.90 % | 1,147,755 | 0.16 |
| 0x0A798 | 102,609 | 0.51 % | 849,065 | 0.12 |
| 0x0BA10 | 36,393 | 0.18 % | 155,295 | 0.23 |
| 0x13140 | 31,631 | 0.16 % | 50,575 | 0.63 |
| 0x0BB20 | 22,207 | 0.11 % | 50,575 | 0.44 |
| 0x0A940 | 2,120 | 0.01 % | 5,355 | 0.40 |

sel7 (250,308 calls, 14.43 s):

| LS | us in window | share | entries | us/entry |
|---|---:|---:|---:|---:|
| 0x0AF98 normalize | 5,198,506 | 36.03 % | 13,353,956 | 0.39 |
| 0x0B268 normalize cont. | 1,974,250 | 13.68 % | 7,147,069 | 0.28 |
| 0x0A730 transform | 1,846,559 | 12.80 % | 10,591,756 | 0.17 |
| 0x0C9D0 | 1,143,586 | 7.93 % | 3,652,705 | 0.31 |
| 0x0C5B8 | 1,046,796 | 7.26 % | 2,390,115 | 0.44 |
| 0x0A798 transform | 681,923 | 4.73 % | 5,078,403 | 0.13 |
| 0x0A818 | 520,102 | 3.61 % | 2,573,160 | 0.20 |
| 0x0BED0 | 497,806 | 3.45 % | 2,021,215 | 0.25 |
| 0x09828 | 296,667 | 2.06 % | 1,310,785 | 0.23 |
| 0x0B488 | 284,976 | 1.98 % | 801,664 | 0.36 |
| 0x0A8A0 | 238,206 | 1.65 % | 1,732,606 | 0.14 |
| 0x097C8 | 142,290 | 0.99 % | 552,160 | 0.26 |
| (remainder: 0x131A0, 0xC0F8, 0xB748, 0xCD80, ...) | 1,185,121 | 8.22 % | - | - |

The 0xA730/0xA798/0xB830 entries are the same transform/normalize/pack
family named in step 3; the new result is that on sel1 they are only
3.3 % of the wall because 0x22 dominates the window, while on sel7 they are
the real 19.4 %.

## 3. Instrumentation overhead

The play build already runs `PS3RECOMP_C690_STATS=1` (ab_live's default);
this mission adds the selector windows and the all-class DMA timer only when
`PS3RECOMP_C690_OP21=1`.

| item | count in 30.2 s | unit cost | CPU over 30.2 s | per lane |
|---|---:|---:|---:|---:|
| existing stats entry timer | 135.4 M entries | 21.7 ns (`clock_gettime`) | 2.94 s | ~1.95 % |
| selector-window charge (scan + add) | 68.1 M entries (15.3 M sel1, 52.8 M sel7) | ~10-18 ns | ~0.9-1.2 s | ~0.6-0.8 % |
| all-class DMA timer | 2.41 M copies | 2 x 21.7 ns | 0.10 s | ~0.07 % |
| **added by this mission** | | | **~1.0-1.3 s** | **~0.7-0.9 %** |

The window scan is a linear search over the distinct functions actually
entered (11 in sel1, 22 in sel7 in this window), so the estimate is the
number of entries x half the table x ~1.5 ns. The existing stats timer is
the larger overhead and was already in every previous ab_live run. The
end-to-end p50 does not move (34.15 ms, below), so the added ~1 % of one lane
is inside the run-to-run noise; it is reported rather than subtracted.

## 4. Sanity: play build

Same run, `build-play` rebuilt with the capture wrappers stripped (only the
always-on cull wrapper remains), `PS3RECOMP_C690_OP21=1`,
`PS3RECOMP_C690_STATS=1`, `PIN=0`, `WCB=2`, x11, meter, 3 x 10 s with 2 s
dropped:

| segment | frames | fps | p50 | p90 | p99 |
|---|---:|---:|---:|---:|---:|
| 0 | 206 | 25.75 | 34.15 | 38.80 | 41.66 |
| 1 | 232 | 29.00 | 34.15 | 38.96 | 42.26 |
| 2 | 287 | 35.88 | 34.15 | 39.92 | 41.76 |
| pooled | 725 | 30.21 | **34.15** | 39.27 | 42.00 |

Segments 0/1 carry the usual long present stalls (std 115 ms); the clean
segment is 35.88 fps. The p50 is exactly the known ~34.15 ms two-vsync
floor, so the play build is not regressed by the default-off timing code.
The profiling build is the same `build-play` binary, not a capture build, so
the comparison is valid.

## 5. Re-rank: GPU and CPU-native targets

Line counts are the port scope from step 3 (to port, not the existing
kernel size); ms/frame is the clean share on the 24.39 ms critical-lane
basis. "us/frame/line" is ms/frame x 1000 / lines.

| rank | target | clean ms/frame | lines to port | us/frame/line | verdict |
|---:|---|---:|---:|---:|---|
| 1 | **0x2A assembler** | 3.43 | 0 (kernel exists, 123-line `.comp`) | 0 marginal | Cheapest win: bit-exact kernel already signed off; wire/enable it |
| 2 | **0x22 tree/cull (`0xFDA0`)** | **5.80** | 1,872 (native CPU stage exists) | 3.10 | Largest slice, but not a new GPU kernel: it already has a CPU-native D port; make sure it is on |
| 3 | **0x21 sel7 + A730/A3E0/BB98** | 4.74 | 2,493 | 1.90 | Best remaining GPU target; ~1-2 weeks |
| 4 | **0x25/0x26 cull (`0xD080`)** | 3.03 | 1,551 | 1.95 | Same per-line as sel7, smaller absolute; ~1-2 weeks |
| 5 | 0x13 16-byte copy | 2.35 | small | n/a | Not a GPU kernel; CPU/memcpy/native path |
| 6 | 0x0A unpack | 1.67 | (0x6D68 etc.) | n/a | Already has native stages; CPU-native D |
| 7 | 0x21 sel1 emit | 0.81 | ~500-1,000 | 0.8-1.6 | Not worth a port |
| 8 | 0xB830 leaf alone | 0.24 | 78 | 3.10 | Best per line, but the kernel is written and the absolute saving is ~0.1-0.2 ms |

The 0x22 result is the important one: it is 23.8 % of the c690 wall, more
than the entire 0x21 emit path, and it is exactly the stage that already has
a native CPU replacement. The GPU question should not be "port 0x21" but
"is the native 0x22 stage enabled and how much does it save on this scene",
followed by sel7 and the culls.

## 6. Is full-tail C still ~17 ms?

No.

Using the same 24.39 ms critical-lane basis as the earlier phase-2 models:

| set | share of c690 wall | critical-lane saving |
|---|---:|---:|
| old model: 0x21 10.5 + 0x2A 3.83 + cull 3.10 | - | 17.4 ms |
| clean: true 0x21 (sel1+sel7) | 22.82 % | 5.57 ms |
| clean: 0x2A | 14.06 % | 3.43 ms |
| clean: 0x25 + 0x26 | 12.41 % | 3.03 ms |
| **clean GPU full-tail C** | **49.29 %** | **12.03 ms** |
| plus 0x22 tree/cull (already native, not GPU) | +23.79 % | +5.80 ms -> 17.8 ms |

So the GPU full-tail is **~12 ms**, not ~17 ms. The extra ~5.8 ms needed to
reach 17.8 ms is the 0x22 tree/cull, which is a CPU-native D target, not a
new Edge-on-GPU kernel. In this run's own 16.8 ms lane-busy basis the GPU
full-tail is ~8.3 ms.

## 7. Reproduction

```sh
# capture unit test (cap before snapshot)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_c690_tail_capture.py

# timing build: no capture wrappers
ninja -C ../recomp-work/build-play MyGameRecomp
BUILD_CACHE=../recomp-work/build-play/CMakeCache.txt \
  ../recomp-work/ab-tools/check_spu_context_abi.sh

# one meter run with the timing-only keys (no tail capture)
cd ../recomp-work
TAG=timing2b5b N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 PITCH_THRESH=35 LIMIT=420 \
  ARMS='A=' BOOT_ARM='' PS3RECOMP_C690_OP21=1 \
  ./ab-tools/ab_live.sh

# frame sanity
python3 ab-tools/pin_ab.py --log ab_live_timing2b5b_*.log \
  --marks ab_live_timing2b5b_*.marks --seg 10 --drop 2

# class window: delta the two [c690-stats] periodic reports at
# elapsed=189.1s and 219.3s (the run's meter segments). The 0x22 correction
# uses the per-window 0x10190 line and the raw handler table at LS 0x14770.
```

The raw handler table is read from the module at EBOOT file offset
0x1161370 (EA 0x01161300, LS base 0x4000), 41 big-endian words.

## 8. Clean room

Own static analysis of the EBOOT (handler table), the lifted c690 module, our
own runtime counters and our own run logs. The stage/corpus/differential
method follows NHL Legacy Native (MIT, (c) 2026 HeckYouMan) with
attribution; no NHL code was copied. The SPU-gate/shared-GPU-image pattern is
from the inFAMOUS 2 PC port (MIT, ThiagoFraga761 and contributors), design
reference only. No RPCS3 or other GPL emulator source was read or cited.
