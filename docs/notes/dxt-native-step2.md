# Native DXT, step 2: the kernel is the unlifted `0x010A1700` module

Date: 2026-11-03. Branch `exp/live-ab` in `ps3recomp-play`, binary
`recomp-work/build-play`. One game launch: `ab_live_dxtcap1_20261103_224034`
(start screen, `PIN=1 WCB=2`, standard `play_windowed.sh` environment,
governor `performance`). No GPL emulator source was read. This note closes the
code-attribution gap left by `dxt-native-step1.md` and records why step 2 (the
native encoder) still does not start.

## TL;DR

* **The DXT kernel is the unlifted base-0 job module at `0x010A1700`.** Every
  wid-0 job loads 0x2C10 code bytes from guest EA `0x010A1700` to LS `0x4000`
  on all four lanes. The 16-byte header is
  `42465602431A9282422D77024363A382`, entry `0x30`, image size `0x2C30`, load
  base `0`; the first 0x2C10 captured bytes are byte-identical to the EBOOT
  copy. `spu_overlay_match_sig` returns **0**: none of the seven lifted job
  modules (images 200-206) is this code.
* **The job runs interpreted, not lifted.** The module entry lands at LS
  `0x4030`; image 1's lift has no function there, so the runtime takes the
  no-lift path (`spu_smc_microstep` -> `spu_interp_run_until`). That is the
  33.5 s of wid-0 lane time in `boot-spu-hitches.md`, not lifted code.
* **The corpus is complete and usable.** 93 MiB, four lane spools: 304 code
  records (300 clean jobs), 327 parameter blocks, 53.8 MB of input texture (841,340 4x4 blocks)
  and 6.4 MB of DXT1 + 0.6 MB of DXT5 output (the same 841,340 blocks). Each
  16 KiB input GET has one matching output PUT (2 KiB DXT1 / 4 KiB DXT5), and
  the parameter block's output EA equals the first PUT EA on all 300 jobs.
* **Per the phase-1 rule, this is a stop.** The required bit-exact gate is
  "native vs the lifted kernel run in isolation"; that kernel is not lifted.
  Lifting `0x010A1700` (base 0, entry `0x30`) is a separate decision, and it
  should come before any encoder work.

## 1. Capture

Hook (new, default off): `runtime/spu/dxt_capture.{c,h}`. It is armed only by
`PS3RECOMP_DXT_CAPTURE=<dir>`; otherwise `g_dxt_capture_on` is 0, no file is
opened and the DMA path costs one predictable branch. `spurs_policy.c:395`
arms it for `wid == 0` around `spu_run_with_halt`; the four MFC transfer sites
in `spu_dma.h` (fast/slow GET/PUT, lines 423, 449, 914, 980) spool the payload
with EA/LSA/size. It is deliberately independent of the c690 corpus.

One spool per SPURS lane (`dxt_lane<lane>_<n>.bin`), records:

| type | contents |
|---:|---|
| 1 RUN_BEGIN | wid, lane, image, pc, `LS[0x1320]` code base, `wkl_data`, LS `0x100..0x21f` |
| 2 GET | payload read from guest `ea` into LS `lsa` |
| 3 PUT | payload written from LS `lsa` to guest `ea` |
| 4 RUN_END | exit pc/status/code base |
| 5 CODE | a GET whose payload has the `C0DEC0DE` job-module header; ea/size = code DMA source, lsa = code buffer, aux = lifted image (0 = none) |
| 6 PARAM | the 0x30-byte DXT parameter block found inside a GET payload |

Env knobs: `_RUNS` (4000), `_STRIDE` (1), `_MAX_MB` (180), `_TRIGGER`. The
run used the defaults; 93 MiB was written, below the cap.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
PS3RECOMP_DXT_CAPTURE=$PWD/dxt_cap1 TAG=dxtcap1 START_SCREEN=1 N=0 BOOT=38 \
  LIMIT=120 PIN=1 WCB=2 SCENE_SHOTS=0 ./ab-tools/ab_live.sh
```

The launch reached the start screen at ~47 s; the boot DXT burst starts at
~21 s (first capture at 22:40:55, 21 s after launch). `N=0` stops the harness
right after the press-start screen is verified, so no input is sent.

## 2. The kernel

Per-code-load record, identical on lanes 0-3:

```
CODE lane=N ea=0x010A1700 size=0x2C10 lsa=0x04000 image=0
     hdr=42465602431A9282422D77024363A382
```

* Source EA `0x010A1700`, 11,280 bytes, destination LS `0x4000`, every
  sampled job (300 loads). The captured payload equals the EBOOT bytes at
  `0x010A1700` for all 0x2C10 bytes; the module's last 0x20 bytes are zero
  (uninitialised tail).
* Header: entry `0x30`, image size `0x2C30`, load base `0`, `C0DEC0DE` at
  +0x20. That is the eighth, unlifted module `lift_jobmods.py --engine-elf`
  skips (it only accepts base `0x4000`). The job's CRT computes its own base
  with `brsl`/`sf` into r126, so it is position-independent.
* No lifted module is ever resident for wid 0: `spu_overlay_match_sig` = 0 on
  all 300 loads, and the seven registered jobmods (images 200-206) have other
  headers. `jobmod_c566`/`jobmod_96de` have the 565/`0x5556` colour code named
  in step 1, but the wid-0 code DMA never names either of them.
* Execution is the interpreter rejoin: image 1's lift has functions at
  `0x4000`, `0x4080`, `0x40b8`... but not at the module entry `0x4030`, so
  `spu_lookup(0x4030, 1)` misses, and the no-lift path runs
  `spu_smc_microstep` (`spu_channels.c:3617`) -> `spu_interp_run_until`
  (`spu_channels.c:3048`). The run log's `[spu-smc] ... handing to the
  interpreter` lines are that path (the cap makes the first 8 lines name PM
  code, not `0x4030`). The wid-0 spans have no `spu.interp` trace event
  because only `spu_run_interp_job` emits it, not the in-context rejoin.

## 3. What one job looks like

A representative run (lane 0, first job) in DMA order:

| step | transfer |
|---|---|
| job list line | GET 128 B from `0x16023980` -> LS `0xd00` (four 0xA0-byte job-source EAs) |
| job source | GET 0xA0 from `0x16024c60` -> LS `0xc00` |
| code | GET 0x2C10 from `0x010A1700` -> LS `0x4000` |
| N input tiles | GET 2/4/8/16 KiB from `0x1480xxxx` -> LS `0x7000`/`0xf000` |
| N output tiles | PUT 128..2048 B from the same LS windows -> `0xc98xxxxx` |
| completion | GET 128 B from `0x16023980` -> LS `0xd00`, then policy exit |

The 0xA0-byte job source is a WWS command list, not the parameter block:

```
0000: 00 00 02 00 10 0C 01 00 00 00 02 20 1C 20 02 00
0010: 0B 04 A4 00 01 0A 17 00 10 00 84 20 14 80 73 80   <- 010A1700, 14807380
...
0070: 14 80 73 80 00 00 04 00 00 00 01 00 00 00 00 00   <- 14807380, w=1024, h=256
0080: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   <- swizzle bytes (host LE)
0090: C9 80 86 00 00 00 00 01 00 00 00 01 00 00 00 00   <- out EA, DXT1, mip=1, scratch=0
```

The 0x30-byte parameter block is at **+0x70 of the job source**:

| off | field | sample |
|---:|---|---|
| 0x00 | source EA | `0x14807380` |
| 0x04 | width | 1024 |
| 0x08 | height | 256 |
| 0x10..0x1c | channel swizzle, **host-endian bytes** | `00 01 .. 0f` = LE word `0x03020100` |
| 0x20 | output EA | `0xC9808600` |
| 0x24 | output code (1/3/5) | 1 (DXT1) |
| 0x28 | mip count | 1 |
| 0x2c | scratch pointer | 0 |

This corrects two details of step 1's table: the swizzle words are stored
host-endian (the PPU writes them little-endian; the rest of the block is
big-endian), and `+0x20` is the output EA. The capture hook's first version
scanned the swizzle words big-endian and therefore found 0 parameter blocks at
run time; the branch now scans them little-endian. The corpus itself keeps the
raw job-source GETs, so the params are recoverable offline (327 found here).

## 4. Corpus numbers

| item | value |
|---|---:|
| spool size | 93 MiB (4 files) |
| wid-0 policy runs captured | 310 (306 with a clean RUN_END; 4 tails torn by SIGKILL) |
| runs with the code load | 300 |
| CODE records (all `0x010A1700`) | 304 |
| parameter blocks | 327 |
| GET records / bytes | 6,841 / 57,753,696 |
| PUT records / bytes | 4,878 / 7,087,840 |
| input texture bytes / 4x4 blocks | 53,845,760 / **841,340** |
| DXT1 output bytes / blocks | 6,410,464 / 801,308 |
| DXT5 output bytes / blocks | 640,512 / 40,032 |
| DXT3 output | 0 in this sample |

The parameter sets seen at the start screen:

* output code 1 (DXT1) 304 parameter blocks, 5 (DXT5) 23; no DXT3.
* widths 16..2048, heights 4..256, all powers of two and all
  `height <= width`.
* swizzle `0x03020100` (LE bytes `00 01 .. 0f`) for all 327; mip 1; scratch 0.
* output EA equals the first PUT EA on all 300 code runs.

Each run is a clean block-pair unit: one 16 KiB input GET is 256 4x4 input
blocks, and its PUT is 2048 bytes (256 x DXT1 8-byte blocks) or 4096 bytes
(256 x DXT5 16-byte blocks). The observed size pairs are
(16 KiB -> 2 KiB) x32 and (16 KiB -> 4 KiB) x8, plus the smaller
2/4/8 KiB -> 0.25/0.5/1 KiB variants. This is exactly the corpus a bit-exact
encoder gate needs.

## 5. Why step 2 does not start

The mission's gate is "100 % byte match on all captured pairs + synthetic
inputs vs the **lifted kernel run in isolation**". The kernel is not lifted,
and the runtime has no way to call it in isolation:

* `lift_jobmods.py --engine-elf` rejects this module because its header says
  load base 0, not `0x4000`.
* The c690/Edge method needs a callable lifted entry and its generated
  reference to diff against. Neither exists here.
* Writing an encoder from the captured pairs alone would be a guess at the
  endpoint/index/search order, which step 1 (correctly) refused. DXT output
  feeds the title's texture caches; "close" is a different cache entry.

So the one capture unblocks the *identification*, not the encoder. Lifting
`0x010A1700` is the next decision, and it is a separate one.

## 6. Recommended next step (not done here)

1. **Lift `0x010A1700`.** Wrap it as a base-`0x4000`/entry-`0x4030` SPU ELF
   (the code is position-independent) and register a verified streamed region
   keyed by source EA `0x010A1700`, span `[0x4030, code_end)`, exactly like the
   seven jobmods. The PM's `bie` to `0x4030` then dispatches lifted code
   instead of the interpreter. This alone removes the interpreted 33.5 s of
   wid-0 lane time in the boot burst (`boot-spu-hitches.md` §1) and is worth
   measuring on its own.
2. **Gate the lift offline.** The DXT module is pure LS/GPR with DMA I/O, so
   the corpus can replay like `tools/replay/test_job_c690_10f78_replay.c`:
   feed one run's GETs, enter `0x4030`, compare the resulting LS/PUTs with the
   captured image. The 300 code runs / 841,340 block pairs are the exactness
   gate.
3. **Only then** write the native encoder, the `PS3RECOMP_NATIVE_DXT` switch
   and the shadow/live A/B, following the c690 job-level pattern
   (`job_c690_native.c`, `edge-native-step3.md`).

The expected prize is unchanged: wid 0 is 33,988 ms of lane time in the 14
boot hitches and 4,479 ms in the 2 s replay; on four lanes that is ~8.5 s of
the 10.1 s boot hitch window.

## 7. What was not done

* No lift of `0x010A1700`; no native encoder; no `PS3RECOMP_NATIVE_DXT`;
  no shadow or live A/B.
* No second/third launch (the phase-1 rule stopped the mission after the
  identifying capture). One launch used of the three allowed.
* No change to the game files, the VFS, or the test defaults. The capture
  hook is default off and inert in every existing run.

## 8. Artifacts and licence

* Code: `runtime/spu/dxt_capture.{c,h}`, the calls in
  `runtime/spu/spurs_policy.c:395-397` and `runtime/spu/spu_dma.h:423/449/914/980`
  (branch `exp/live-ab`, pushed to `fork`).
* Corpus: `recomp-work/dxt_cap1/dxt_lane{0,1,2,3}_*.bin` (93 MiB, game data,
  not committed). Kept for the lift gate; it is below the 1 GiB cleanup
  threshold.
* Log: `recomp-work/ab_live_dxtcap1_20261103_224034.log`.
* Clean room: the corpus/differential method follows NHL Legacy Native (MIT,
  (c) 2026 HeckYouMan) with attribution; no NHL, RPCS3 or other GPL emulator
  source was read or cited. The only external facts used are the public BC1
  (DXT1) and BC3 (DXT5) block layouts.
