# c690 0x13 "16-byte copy" and 0x0A unpack/pack: the 17.7 us was a map artifact

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`. One automated pitch-meter launch
(`ab_live_copy1_20261104_120526`): `METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`,
`PIN=0 WCB=2`, x11, governor `performance`, `PS3RECOMP_C690_OP21=1`, N=3 x 10 s
per arm, 2 s dropped. The two arms were the existing native `0xFDA0` stage
off/on (`PS3RECOMP_C690_NATIVE_CULL=0|1`), so the same launch also re-checked
the native-stage result under the corrected command map. Nothing was enabled
outside the run; no native replacement was added.

This follows [`edge-gpu-phase2b-step5.md`](edge-gpu-phase2b-step5.md),
[`c690-emit-ring.md`](c690-emit-ring.md),
[`c690_function_map.md`](c690_function_map.md),
[`edge-native-step3.md`](edge-native-step3.md),
[`edge-native-step4.md`](edge-native-step4.md) and
[`spu-codegen-audit.md`](spu-codegen-audit.md).

## TL;DR

* **0x13 is a real 16-byte copy and costs 0.72 us/call, not 17.73 us.** The
  step-5 17.73 us number came from the *incomplete opcode map*, not from the
  copy. The old `s_op_map` named only 15 of the module's handlers; 26 were
  missing (0x01, 0x02, 0x03, 0x05, 0x06, 0x0B, 0x0D..0x0F, 0x12, 0x14,
  0x15, 0x18..0x1E, 0x22, 0x23, 0x28, 0x29, 0x2B, 0x2D, 0x2E). A handler
  that is not in the map does not close the previous command, so the whole
  `0x13 -> 0x14/0x0E/0x0F -> 0x18..0x1E -> 0x1F` run was charged to 0x13.
  The full map (commit `1f1cbd57`, landed after the step-5 run) splits it:
  in the same meter workload 0x13 is **0.72 us/call, 0.37 % of the c690
  wall** (~0.06-0.08 ms/frame), and the commands it used to absorb are now
  named and sum to the missing ~17 us.
* **0x13 really is: ring-ticket + 16-byte header + a 16-byte LS copy.** The
  handler `0x11A60` tail-calls `0x11990`, which rounds the size (16), calls
  the WWS ring allocator `0xE6B8`, writes the 16-byte command header at the
  returned slot, and tail-calls the generic 128-byte-block copier `0x5680`
  for one 16-byte store. There is no DMA, no tag/atomic read and no spin on
  the fast path; `0xE6B8` only takes the mailbox/`stop` slow path when the
  ring is full, and that does not happen at the meter. The copy body does
  have one wrinkle (a computed `bi` into the middle of `0x5680`, target
  `0x5714`), but it is cheap: the whole command is 0.72 us.
* **0x0A is genuine and cheap: 2.76 us/call, 6.4 % of the clean window
  (~1.3 ms/frame).** It is the stream bind/descriptor builder
  `0x12478 -> 0x10B88` and its pack/gather family, not a hidden wait. The
  existing bit-exact native kernels for the leaves are at parity
  (`0x6D68` 1.00x, `0x7C18` 0.99x), so there is no clear native win.
* **Native `0xFDA0` is neutral because it covers only part of the 0x22
  class.** Under the clean map the 0x22 command is **573.6 us/call, 20.7 %
  of the c690 wall** (~4.2 ms/frame on this run's 20.2 ms critical-lane
  basis). The entire class is charged to the handler `0x10190`; the native
  stage replaces only the `0xFDA0` subtree inside it (offline 135-460 us,
  median 275 us, i.e. ~25-80 % of the command). At the measured 1.30x that
  is ~0.2-0.7 ms/frame, below the ~1 ms A/B noise. The rest is `0x10190`'s
  own pre/post work and the `0x10480/0x10488/0x1024C/0x10300/0x1036C`
  continuation plus the plain-call helpers `0xFA20/0xFA58/0xFCB8`, which the
  native port does not touch.
* **Live A/B confirms neutrality:** native off 35.12 fps, native on
  34.42 fps (the on arm's pooled std is inflated by one 41 ms segment), p50
  **34.15 ms in both**, p90/p99 40.50/44.21 vs 40.19/43.44 ms. The clean
  map itself is confirmed live: 0x22 is now its own opcode (it was 0x21
  sel1 in step 5).
* **Decision: implement nothing.** The premise "17.7 us is far too long for
  a small copy" is false once the map is complete; the copy is already
  cheap. No native copy/unpack is warranted, and nothing is default-on.

## 1. What was measured

One launch, two arms (native `0xFDA0` off/on) alternating every 10 s for
three segments each. The meter was confirmed 156 s after launch; the first
switch is at 164.3 s and the last segment ends ~225.5 s. The numbers below
are the delta of the two periodic `[c690-stats]` blocks at elapsed 164.2 s
and 224.2 s, i.e. a **60.0 s meter window**, 907,390 jobs, 177.55 s of c690
job wall (2.96 lanes busy over the window, 15,123 jobs/s).

The only code change in the measured build is measurement-only: the
exclusive hot list now also names the 0x13 chain (`0x11990`, `0x5680`) and
the 0x22 continuation chain. It changes no dispatch and is off outside
`PS3RECOMP_C690_STATS`.

### 1.1 Clean per-command map (60 s window)

| op | handler | commands | share of c690 wall | us/call |
|---:|---|---:|---:|---:|
| 0x21 | `0x13080` emit/format | 677,341 | 25.6 % | 66.99 |
| **0x22** | **`0x10190` tree/cull** | **64,213** | **20.7 %** | **573.59** |
| 0x2A | `0x10F78` assembler | 1,077,232 | 13.5 % | 22.17 |
| 0x25 | `0x13520` cull | 502,835 | 7.9 % | 27.73 |
| 0x0A | `0x12478` bind/convert | 4,143,826 | 6.4 % | 2.76 |
| 0x26 | `0x13518` cull | 175,206 | 3.7 % | 37.47 |
| 0x1B | `0xF520` | 985,850 | 2.3 % | 4.17 |
| 0x1D | `0xF680` | 985,851 | 2.3 % | 4.11 |
| 0x19 | `0x4F00` | 677,340 | 2.1 % | 5.44 |
| 0x27 | `0x11790` index copy | 766,900 | 1.6 % | 3.71 |
| 0x18 | `0x53C0` | 677,340 | 1.5 % | 3.86 |
| 0x1F | `0x12948` finalize | 907,389 | 1.4 % | 2.78 |
| 0x04 | `0x11F58` setup | 907,388 | 0.8 % | 1.66 |
| 0x23 | `0x12DC8` | 165,814 | 0.7 % | 7.53 |
| 0x1A | `0x129F0` | 985,850 | 0.7 % | 1.23 |
| 0x08 | `0x11BF8` stream end | 907,388 | 0.7 % | 1.33 |
| 0x07 | `0x11DF0` stream end | 907,388 | 0.5 % | 0.89 |
| 0x0C | `0x12830` index decompress | 766,898 | 0.4 % | 0.98 |
| 0x1C | `0x11BC0` | 985,851 | 0.4 % | 0.74 |
| **0x13** | **`0x11A60` 16-byte copy** | **907,388** | **0.37 %** | **0.72** |
| 0x0E | `0x11AA8` | 678,040 | 0.3 % | 0.78 |
| 0x1E | `0x12A60` | 985,850 | 0.2 % | 0.42 |
| — | entry/parse | — | 5.4 % | — |

The class shares are relative to the full job wall (command time + entry/
parse), the same basis the report prints. 0x22 is 20.7 % and 573.6 us/call,
close to step 5's offline-corrected 23.79 % / 618.9 us. 0x13 is 0.37 % and
0.72 us/call. On this run's own basis (177.55 s / 5 lanes / 60 s = 0.592
lane busy x 34.15 ms = **20.2 ms critical-lane busy per frame**), 0x13 is
**~0.075 ms/frame**, not the 2.35 ms of step 5.

### 1.2 The same workload under the old map

The step-5 run (`ab_live_timing2b5b_20261104_113219`, 189.1 s -> 219.3 s,
403,212 jobs) used the map that named only 15 handlers. Its delta:

| op | commands | us | us/call |
|---|---:|---:|---:|
| 0x21 | 300,883 | 34.63 s | 115.11 |
| 0x13 | 403,213 | 7.148 s | **17.73** |
| 0x0A | 1,841,133 | 5.083 s | 2.76 |
| 0x2A | 478,778 | 10.44 s | 21.82 |
| 0x25/0x26 | 223,710 / 77,945 | 6.22 / 3.00 s | 27.80 / 38.54 |

0x22 is absent because it was not in the map. The new run has the same
meter workload (15,123 vs 13,351 jobs/s) and the same 0x0A (2.76 us), but
0x13 is 0.72 us. The 17.0 us difference is exactly the chain that the old
map could not close at: the 0x14/0x0E/0x0F and 0x18..0x1E setup/stream
commands, whose measured per-job times sum to ~17-21 us (e.g. 0x19 5.44,
0x1B 4.17, 0x1D 4.11, 0x18 3.86, 0x1A 1.23, 0x23 7.53 us). The step-5 doc
fixed the 0x22 entry and warned that the run was corrected offline; it did
not add the other 25 entries, so 0x13 stayed inflated.

This also explains why step 5's `0x11A60` exclusive time was 4.57 us while
its per-command 0x13 was 17.73 us: the exclusive model charges `0x11A60`
until the next *observable* entry, and the un-named chain is only split once
the map is complete.

## 2. What 0x13 really does (static)

LS addresses are the lifted module; the raw module image is at EBOOT file
offset 0x1151300 (EA 0x01161300, LS base 0x4000).

```
0x11A60  handler for opcode 0x13
  r4 = (r4 << 16) >> 16          ; low 16 bits of the command word
  r5 = 16                        ; payload size
  r6 = r3 + 528                  ; header source in the command block
  -> 0x11990

0x11990
  size = (r5 & 0xFFFF + 15) & ~15 ; 16
  slot = 0xE6B8(r3, size)         ; WWS ring allocator (LS +0xA0/+0xB0)
  header = shufb(slot, *(r6), r6) ; 16-byte command header
  *(r6) = header
  -> 0x5680(slot, src, size)      ; generic LS block copy

0x5680
  one 128-byte load window, a computed `bi` into the store block at
  0x5714 (16-byte case), one stqd, return.
```

So the command is a **ring-ticket allocation plus a 16-byte header and a
16-byte copy**, once per job. `0xE6B8` reads the ring write/limit words at
`r3+0xA0`/`r3+0xB0`; the fast path is ~10 instructions and writes the new
write pointer. The slow path (`0xE734`) calls the WWS job-service syscall
`0x13528` (`WrOutMbox` request, `WrOutIntrMbox` 0x100, `RdInMbox` reply)
and, if the ring is still full, executes `stop 0x1`. That is a genuine
guest backpressure wait, but it is not taken on this workload: the whole
0x13 command is 0.72 us, the ring allocator fast path is a few LS accesses,
and a rare ring-full call could not produce the *stable* 17.7 us that the
step-5 report printed in every 2 s window.

The same map artifact explains step 5's "0x13 DMA 254.9 ms": the all-class
DMA timer charges the current command, and the old map left 0x1A (`0x129F0`
fenced RSX injection) un-named. In this run the all-DMA table has **no 0x13
row**; the transfers are charged to `0x04` (1,111,641 copies, 120.0 ms) and
`0x1A` (6,715,857 copies, 811.9 ms). The 16-byte copy itself issues no DMA.

The copy body's computed `bi` target (`0x5714`, mid-block) does take the
runtime's slow resolver path, but the resolved work is one store and the
total is still sub-microsecond. There is no byte-wise host copy, no DMA tag
wait and no atomic in the measured path. A host `memcpy` for 16 bytes would
save nothing measurable.

**Conclusion:** the 17.7 us premise is a measurement artifact; 0x13 is not a
CPU-native target. The right fix was the complete opcode map, which is
already in `s_op_map` (commit `1f1cbd57`).

## 3. 0x0A unpack/pack

`0x0A` is the stream bind/descriptor builder `0x12478 -> 0x10B88` and its
pack/gather family (`0x7AC0/0x7D40/0x7840/0x7540/0x7FC0`). In the clean
60 s window it is **4,143,826 calls, 2.76 us/call, 6.4 % of the wall**
(~1.3 ms/frame). The per-command time matches step 5 exactly (2.76 us), so
this class was already measured correctly.

It is cheap and has no hidden wait. The existing bit-exact native kernels
for the unpack/pack leaves are at parity: `0x6D68` **1.00x** and `0x7C18`
**0.99x** (8-wide AVX2, `spu-codegen-audit.md`), and the dispatcher itself
(`0x10B88`) is 57 SPU instructions. The cost is LS traffic and descriptor
work, not the arithmetic. There is no clear cheap exact native win, so
nothing was implemented.

## 4. Why native 0xFDA0 is neutral

Under the clean map the 0x22 class is **64,213 calls, 573.59 us/call,
20.7 % of the c690 wall** (~4.2 ms/frame on the 20.2 ms critical-lane
basis). The exclusive report charges the whole class to `0x10190`
(36.83 s, 573.6 us/entry): `0xFDA0` and the `0xFA20/0xFA58/0xFCB8` helpers
are plain calls, so they have no separate entry of their own.

The native stage replaces the `0xFDA0` subtree only. On the captured stage
that subtree is 135-460 us per sample (median 275 us, `spu-codegen-audit.md`),
i.e. roughly a quarter to four-fifths of the 573.6 us command. At the
measured 1.30x, the saving is therefore ~30-106 us/call. At ~1,070 calls/s
over five lanes that is ~1-4 % of the c690 wall, or **~0.2-0.7 ms/frame** —
below the ~1 ms / ~1 fps A/B noise. That is exactly what the live A/B shows.

The other ~20-75 % of the command is `0x10190`'s own pre/post work and the
`0x10480/0x10488/0x1024C/0x10300/0x1036C` continuation state machine, none
of which the native port covers. A faster native or GPU version would have
to cover the **whole 0x10190 command body**, including that continuation and
the plain-call leaf helpers, and would still be bounded by the same LS
traffic: the per-stage gates are 1.0-1.5x, not 3x. That is why a GPU port of
just `0xFDA0` cannot move the frame.

## 5. Live A/B

Arms (both named every key; `PS3RECOMP_C690_OP21=1` for the whole run):

| arm | control file | meaning |
|---|---|---|
| A | `NATIVE_CULL=0; NATIVE_SCREEN=0; CULL_FIX=0; MLB16_CULL_BYPASS=0` | today |
| B | `NATIVE_CULL=1; NATIVE_SCREEN=0; CULL_FIX=0; MLB16_CULL_BYPASS=0` | native 0xFDA0 |

`pin_ab.py`, 10 s segments, 2 s dropped, three segments per arm:

| metric | A | B |
|---|---:|---:|
| frames | 843 | 826 |
| fps | **35.12** | **34.42** |
| p50 ms | 34.15 | 34.15 |
| p90 ms | 40.50 | 40.19 |
| p99 ms | 44.21 | 43.44 |
| std ms | 11.50 | 24.92* |
| walker ms | 16.36 | 16.26 |
| wait ms | 11.91 | 11.77 |

\*B's pooled std is inflated by segment 3 (41.15 ms); its other five
segments are 11.2-11.7 ms. The p50 is the known 34.15 ms two-vsync floor in
both arms, and the p90/p99 direction is faintly positive but inside the
noise, the same result as steps 3 and 4. No clean winner.

## 6. Decision

| item | result |
|---|---|
| 0x13 native copy | **not warranted**: 0.72 us/call, already cheap; the 17.7 us was the incomplete opcode map |
| 0x0A native unpack/pack | **not warranted**: 2.76 us/call, existing kernels at parity |
| native 0xFDA0 | **neutral**: covers only part of the 573.6 us 0x22 class |
| enabled by default | **nothing** |
| `spu_context` fields added | **none** (ABI check PASS) |

The only kept change is the measurement-only hot-list extension in
`runtime/spu/c690_stats.c`, which names the 0x13 chain and the 0x22
continuation in the existing `PS3RECOMP_C690_STATS` report.

## 7. Reproduce

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play && ninja MyGameRecomp
cd /home/m/Documents/mlb16-online/recomp-work
TAG=copy1 N=3 SEG=10 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 \
  VIDEODRIVER=x11 REPORT=0 SETTLE=10 PITCH_THRESH=35 LIMIT=700 \
  ARMS='A=PS3RECOMP_C690_NATIVE_CULL=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0|B=PS3RECOMP_C690_NATIVE_CULL=1;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  BOOT_ARM='PS3RECOMP_C690_NATIVE_CULL=0;PS3RECOMP_C690_NATIVE_SCREEN=0;PS3RECOMP_C690_CULL_FIX=0;MLB16_CULL_BYPASS=0' \
  PS3RECOMP_C690_OP21=1 ./ab-tools/ab_live.sh
python3 ab-tools/pin_ab.py --log ab_live_copy1_*.log \
  --marks ab_live_copy1_*.marks --seg 10 --drop 2
# class map: delta the periodic [c690-stats] blocks at elapsed 164.2 s and
# 224.2 s (the meter segments); 0x13/0x22/0x0A are the op table rows.
```

The raw module image and the handler table are read from the EBOOT at file
offset 0x1151300 (EA 0x01161300, LS base 0x4000); the handler table is at LS
0x14770 (41 big-endian words).

## 8. Clean room and licence

Own static analysis of the EBOOT/lifted c690 module, our own runtime
counters and our own run logs. The stage/corpus method follows NHL Legacy
Native (MIT, (c) 2026 HeckYouMan) with attribution; no NHL code was copied.
The SPU-gate/shared-GPU-image pattern is from the inFAMOUS 2 PC port (MIT,
ThiagoFraga761 and contributors), design reference only. No RPCS3 or other
GPL emulator source was read or cited.
