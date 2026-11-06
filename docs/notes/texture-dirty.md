# Texture revalidation by guest-page write generation

Date: 2026-11-03. Branch `exp/live-ab` (`ps3recomp-play`), binary
`recomp-work/build-play`. This is plan item 1 of
[`walker-profile.md`](walker-profile.md): replace the per-frame sparse hash and
the full hash every eight frames in `eng_texture_slot` with "has any byte of
this texture's guest range been written since I last uploaded it".

## TL;DR

* **Live meter A/B (one launch, 6 x 10 s per arm, 2 s dropped per switch):**
  walker drain **26.64 -> 23.27 ms** (-3.37), `tex` **10.34 -> 5.89 ms**
  (-4.45), guest fps **29.21 -> 31.13** (+1.93). Per game flip: p50
  **33.80 -> 32.00 ms**, p99 **44.57 -> 41.06 ms**, stddev
  **4.12 -> 3.73 ms**.
* **No stale entries:** the paranoid mode (full hash of every texture every
  N frames, compared with the hash of the bytes actually uploaded) checked
  **44,656** texture/frame pairs at the meter with **0 stale** entries.
* **Exact when it should be:** the meter and pitch replay gates are unchanged
  with the switch on and off. With the switch off the old hash path runs
  unchanged.
* **Coverage:** every writer in this tree is either intercepted at a choke
  point or faults through the page protection: lifted PPU stores (scalar,
  vector, raw, `dcbz`), SPU MFC PUTs (fast/normal/list), SPU atomic
  PUTLLC/PUTLLUC, the raw-SPU proxy, the NV3089/NV309E 2D transfers, every
  engine/WCB readback store, cellFs/sys_fs/ppu_fs reads, HLE memcpy/memset,
  save-data/font/urandom reads and network `recv`/`recvfrom`.
* **The win is 4.45 ms of texture time, not all 10.34.** The remainder is the
  real decode/upload of the textures that do change, plus the page scans and
  the write-protection syscalls. The feature removes hashing, not uploads.
* Enabled by `PS3RECOMP_TEX_DIRTY=1` (live key `PS3_LIVE_TEX_DIRTY` through
  `PS3RECOMP_CTL`); the engine default is off, `play_windowed.sh` exports it
  on after this measurement.

## 1. Design

### 1.1 A write sequence per 4K guest page

`libs/video/rsx_wcb.c` owns the state (it already owns the page-protection
fault handler and the `wcb_lock`):

| array | size | meaning |
|---|---:|---|
| `page_seq[1 << 20]` (`u64`) | 8 MB | newest write sequence for each 4K page of the 4 GB guest map |
| `page_cov[1 << 20]` (`u16`) | 2 MB | how many cached texture entries cover the page |
| `page_flags[1 << 20]` (`u8`) | 1 MB | bit 0: page is `PROT_READ` for us; bit 1: page is `PROT_NONE` for WCB |

`g_texdirty_seq` is a monotonic counter. A writer bumps it once and stores the
new value in every page it touches. A texture entry stores the guest EA of its
span and `dirty_seq`, the sequence sampled **just before** it reads the bytes
for an upload. Revalidation is:

```
stale  <=>  max(page_seq[p] for p in [ea, ea + span)) > dirty_seq
```

No bytes are read. A write that races with the upload is caught because the
sequence is sampled before the bytes are read: the racing write gets a newer
sequence and the next bind re-uploads. Uploads after a generation change store
the new sample, so a texture re-uploads at most once per frame, the same
cadence as the old per-entry hash check (`last_hash_frame`).

### 1.2 PPU stores: write-protect the pages, catch the fault

A texture's pages are `mprotect(PROT_READ)` when the entry is created and
after each re-upload. The PPU's lifted code writes host memory directly, so
the store faults; `wcb_signal` (already installed for the WCB, same handler)
calls `rsx_texdirty_on_fault` first. A fault on a page with the texture bit
can only be a write (reads are allowed), so the handler bumps the page
sequence, opens the page read/write, and retries the store. Static textures
pay nothing after their first protection; a dynamic texture pays one fault
per page per frame it is written directly.

The fault handler is installed even when `PS3RECOMP_RSX_WCB=0`, so texture
tracking does not depend on the WCB being armed.

### 1.3 WCB interaction

A render target can be armed `PROT_NONE` for on-demand readback and also be a
texture's source. The two mechanisms share the page flags and the lock:

* arming a WCB region clears the texture bit and sets the WCB bit (the page
  is `PROT_NONE`, not `PROT_READ`);
* the texture tracker never protects a WCB-pending page;
* when the WCB reopens a page, it reopens `PROT_READ` if a cached texture
  still covers it, otherwise read/write;
* the WCB's copy calls `rsx_wcb_store`, which bumps the sequence (including
  the `/proc/self/mem` path that bypasses page protection), so the texture
  re-uploads from the new bytes.

### 1.4 Every writer, and how it is covered

| writer | file:line | how |
|---|---|---|
| lifted PPU stores (`stb/sth/stw/std`, FP, string/multiple, `stvx/stvlx`, `stve*`, `stwbrx`, `dcbz`) | generated code writes `vm_base + ea` directly | page is `PROT_READ`; SIGSEGV -> `rsx_texdirty_on_fault` |
| PPU atomics (`stwcx.`, `stdcx.`) | `runtime/ppu/ppu_loader.cpp` | same fault path (their stores are host stores) |
| SPU MFC PUT, fast and full paths, PUTL | `runtime/spu/spu_dma.h:421`, `:920` | `ps3_guest_write(ea_ptr, size)` before `mfc_put_coherent` |
| SPU atomic PUTLLC / PUTLLUC | `runtime/spu/spu_channels.c:890`, `:989` | `ps3_guest_write(mem, 128)` before the line store |
| raw-SPU proxy PUT | `runtime/spu/spu_raw.c:578` | `ps3_guest_write(vm_base + ea, size)` |
| NV3089 row copy | `libs/video/cellGcmSys.c:1340` | `ps3_guest_write` before `memmove` |
| NV309E swizzled blit | `libs/video/cellGcmSys.c:1433` | `ps3_guest_write` over the destination span |
| NV308A inline image | `libs/video/cellGcmSys.c:1625` (`vm_write32`) | direct host store -> fault path |
| NV0039 | not implemented in this tree | nothing writes |
| WCB / engine colour and depth readback | `libs/video/rsx_wcb.c` (`rsx_wcb_store`), `rsx_draw_engine.c` (`eng_color_rows_store`, `eng_zdepth_store`) | sequence note in `rsx_wcb_store`; note in the non-forced memcpy branches |
| cellFs / sys_fs / ppu_fs reads and AIO | `libs/filesystem/cellFs.c:531`, `runtime/ppu/ppu_fs.cpp:492`, `:993`, `runtime/syscalls/sys_fs.c:517` | `ps3_guest_write` before `fread` (a kernel write would otherwise fail `EFAULT` on a read-only page) |
| cellFsUtility, save data, urandom | `libs/filesystem/cellFsUtility.c:68`, `libs/system/cellSaveData.c:706`, `libs/system/sysPrxForUser.c:1124`, `libs/network/cellSsl.c` | same |
| network receive | `libs/network/sysNet.c:637`, `:657`, `libs/network/cellHttp.c:720` | `ps3_guest_write` before `recv`/`recvfrom` |
| HLE memcpy/memset | `libs/system/sysPrxForUser.c` (`_sys_memcpy`, `_sys_memset`), `runtime/ppu/ppu_memory.h` (`vm_memcpy_to`, `vm_memset`) | `ps3_guest_write` before the copy |
| Vulkan/D3D host resources | - | not guest memory; readback goes through the WCB stores above |

`ps3_guest_write` is a single inline in `include/ps3emu/guest_access.h`:
two pointer loads when the feature is off (the write hook is only published
while it is on). It first calls the existing `ps3_guest_access` hook so a
WCB-armed range is resolved, then the texture write hook.

**Rule for future code:** a new path that writes guest memory from host code
must call `ps3_guest_write(host, len)` before the write. A path that writes
through a normal C store on a page the tracker may have protected is covered
by the fault handler; a path that hands guest memory to the kernel is not,
because the kernel returns `EFAULT` instead of faulting.

### 1.5 Mode switch and paranoid check

`PS3RECOMP_TEX_DIRTY` is a live key (`PS3_LIVE_TEX_DIRTY`), polled once per
drain like the other experiment switches. On enable, every cached texture is
covered and marked for one re-upload (its bytes are current, but this
establishes the protection). On disable, the pages are uncovered, the
protections are removed, and each entry is marked so the old hash path
re-establishes its reference hash. `PS3RECOMP_TEX_DIRTY_PARANOID=N` (frames)
turns on the debug check: every N frames a bound texture's span is hashed in
full and compared with the hash of the bytes that were uploaded; a difference
the generation check did not predict is logged as `[texdirty] STALE` and
counted. It also prints `[texdirty] paranoid ... checked=N stale=0` once per
60 frames. Normal mode does no hashing at all.

## 2. Measurements

### 2.1 Live A/B at the pitch meter

One launch, `ab_live.sh`, X once, `METER=1 SKIP_CINE=1 WCB=2 PIN=1
PS3RECOMP_RSX_CUBE_RT=1`, x11, `N=6 SEG=10 DROP=2`, `PITCH_THRESH=35`,
`PS3RECOMP_FLIP_LOG=1`. Arms: `PS3RECOMP_TEX_DIRTY=0` (A) and `=1` (B),
flipped through `PS3RECOMP_CTL`. Run
`recomp-work/ab_live_texdirty_ab_20261103_004959`, measured windows
165.6-285.6 s, 12 segments, the live-ctl self-check passed.

| per arm (mean +/- 95 % CI over 6 segments) | A (off) | B (on) | delta |
|---|---:|---:|---:|
| walker drain ms | 26.64 +/- 0.61 | 23.27 +/- 0.57 | **-3.37** |
| walker wall wait ms | 7.42 +/- 0.35 | 8.82 +/- 0.30 | +1.40 |
| engine ms | 25.157 | 21.643 | -3.51 |
| `tex` ms | **10.343** | **5.892** | **-4.45** |
| `fetch` ms | 3.819 | 4.066 | +0.25 |
| `draw` ms | 4.672 | 4.964 | +0.29 |
| `pipe` ms | 1.862 | 2.030 | +0.17 |
| `other` ms | 4.263 | 4.480 | +0.22 |
| parse ms | 1.481 | 1.610 | +0.13 |
| guest fps | 29.21 +/- 0.34 | 31.13 +/- 0.77 | **+1.93** |

Frame time per game flip (from the `[flip]` counter, dividing by the flip
delta after the log's every-10th decimation; n=134 A / 142 B):

| per flip | A (off) | B (on) |
|---|---:|---:|
| p50 | 33.80 ms | **32.00 ms** |
| p90 | 40.01 ms | **37.37 ms** |
| p99 | 44.57 ms | **41.06 ms** |
| mean | 34.36 ms | **32.09 ms** |
| stddev | 4.12 ms | **3.73 ms** |
| in 15-25 ms | 0.0 % | 1.4 % |

The MangoHud present-frametime means agree (34.34 vs 31.23 ms). The walker
saves 3.37 ms; the frame p50 only moves 1.8 ms because the walker is one of
several serial terms. The wall wait rises 1.4 ms as the frame gets shorter
and the present/park wait absorbs part of the gain.

### 2.2 Paranoid check at the meter

One launch, both arms on (`ARM_A=ARM_B=PS3RECOMP_TEX_DIRTY=1`),
`PS3RECOMP_TEX_DIRTY_PARANOID=30`, `N=3 SEG=10 DROP=2`, same scene config.
Run `recomp-work/ab_live_texdirty_paranoid_20261103_005550`, measured window
~187-226 s. The A/A self-check passed; `tex` is 7.05 ms (the hashing the
debug mode adds).

```
[texdirty] on: 0 cached textures covered, paranoid=30
...
[texdirty] paranoid frame=13871 checked=44656 stale=0
```

**44,656 checks, 0 stale.** This covers the whole meter run including WCB=2
readbacks, SPU PUTs, and the direct PPU stores.

### 2.3 Replay gates

Both captures, with the switch off and on (`PS3RECOMP_TEX_DIRTY=1`). The
replay reads guest bytes through `cap_read`, so it deliberately keeps the
hash path (the tracker is a `vm_base` mechanism); this is also why the gates
are a regression check, not a test of the tracker:

```
meter_frame  --frames 64 --warmup 10
  output hash: a6fcc849341eba9b   gate: ff2a220c28750bc3
walker_frame --frames 5 --warmup 2
  output hash: 416abee2c5bff47d   gate: 3e6f6ad4ebb23ed6
```

Identical in both arms.

### 2.4 Unit test

`libs/video/tests/test_rsx_draw_engine.c::test_texture_dirty_generation`
(with `PS3RECOMP_TEX_DIRTY=1`):

* the first bind uploads once; a later frame with no writes does not
  re-decode;
* a tracked writer (`ps3_guest_write`, the same call the MFC PUT/2D/file
  paths make) makes the next bind re-upload;
* a direct PPU store to the protected page faults, the handler opens it and
  charges the write, and the next bind re-uploads.

`tools/test_video_fixtures.py` passes with the new fixture (the only failure
in the suite is the pre-existing `test_gcm_flip_wrap` link error, unrelated).

## 3. Limitations and risks

* **4K host pages.** The tracker needs the host page size to equal the 4K
  guest page (x86-64 Linux, the measurement host). On a 16K-page host
  (Apple Silicon) it declines and the old hash path runs.
* **Kernel writes must call `ps3_guest_write`.** A new `fread`/`recv`/ioctl
  into guest memory that does not call it would fail with `EFAULT` on a
  protected page rather than fault. All sites found in this tree are hooked;
  the rule is in section 1.4.
* **One frame of delay after the last bind in a frame.** A texture is
  revalidated at most once per frame, as before. A write after its bind is
  seen at the next frame's bind (the old hash path behaved the same way
  because of `last_hash_frame`).
* **Over-protection is conservative, not wrong.** A page shared by a texture
  and unrelated guest data makes those writes fault and re-upload the
  texture. It costs time, never correctness.
* **`tex` does not go to zero.** At the meter it is 5.89 ms: decode/upload of
  the textures that do change, the O(pages) scan, and mprotect/fault work.
  Removing the remaining uploads needs a finer dirty region (sub-page or a
  rectangle from the writer) or a persistent GPU texture per target.
* The replay and any `s_guest_reader` path keep the hash implementation.

## 4. Reproduce

```sh
# A/B (one game launch, ~5 min after boot)
cd recomp-work/ab-tools
TAG=texdirty_ab METER=1 SKIP_CINE=1 WCB=2 PIN=1 VIDEODRIVER=x11 \
  PITCH_THRESH=35 N=6 SEG=10 DROP=2 PS3RECOMP_RSX_CUBE_RT=1 \
  PS3RECOMP_FLIP_LOG=1 \
  ARM_A=$'PS3RECOMP_TEX_DIRTY=0' ARM_B=$'PS3RECOMP_TEX_DIRTY=1' \
  ./ab_live.sh

# paranoid mode (one game launch)
TAG=texdirty_paranoid METER=1 SKIP_CINE=1 WCB=2 PIN=1 VIDEODRIVER=x11 \
  PITCH_THRESH=35 N=3 SEG=10 DROP=2 PS3RECOMP_RSX_CUBE_RT=1 \
  PS3RECOMP_TEX_DIRTY=1 PS3RECOMP_TEX_DIRTY_PARANOID=30 \
  BOOT_ARM=$'PS3RECOMP_TEX_DIRTY=1' \
  ARM_A=$'PS3RECOMP_TEX_DIRTY=1' ARM_B=$'PS3RECOMP_TEX_DIRTY=1' \
  ./ab_live.sh

# exactness
cd ../build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/meter_frame --frames 64 --warmup 10
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_frame --frames 5 --warmup 2
```

## 5. Images

* `recomp-work/visuals/texdirty_meter_AB.png` - meter frame from an A segment
  (off, left) beside a B segment (on, right), same parked scene.
* `recomp-work/visuals/texdirty_meter_diff.png` - 3x contrast difference. The
  field, logos and uniforms are black (identical); the players and crowd
  differ because the frames are ~9 s apart in an animated scene. No missing,
  black or wrongly coloured texture in either frame.

The exactness check that does not depend on animation timing is the replay
gate above, which is byte-identical.
