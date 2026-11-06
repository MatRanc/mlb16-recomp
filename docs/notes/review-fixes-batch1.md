# Review fixes, batch 1

Date: 2026-11-04. Offline only (no game launch). Branch `exp/live-ab` in
`ps3recomp-play`, commit **16fc2d28**, build `recomp-work/build-play`.

This closes the batch-1 review follow-ups and the shader disk cache nits from
[`targets-and-backlog.md`](../targets-and-backlog.md). Behaviour is unchanged
except where a review item named it.

## 1. Sticky `ferror()` on a failed write

`runtime/ppu/ppu_fs.cpp` (`cellFsWrite`) and `libs/filesystem/cellFs.c`
(`cellFsWrite`) now call the shared `ps3_fs_write_begin()` before `fwrite`.
`clearerr()` runs first, so a write that succeeds after an earlier failure is
reported as successful instead of returning EIO forever on that fd.

Tests: new `runtime/ppu/tests/test_fs_write_retry.cpp` and
`tests/test_cellfs_write_retry.c`. Both use a soft `RLIMIT_FSIZE` to make one
write fail, raise the limit, then require the next write to return `CELL_OK`
with a full `nwrite`. Run by `tools/test_ppu_fs_write_retry.py` and
`tools/test_cellfs_write_retry.py`.

## 2. One live FS write-check switch

`fs_write_check_on()` now lives in `include/ps3emu/fs_write_check.h` with
`ps3_fs_write_begin()` and the existing short-write predicate. Both runtime
write paths use it, and `libs/filesystem/cellFsUtility.c` now uses it instead
of its own one-off `getenv`, so a live control change is honoured there too.
`tests/test_fs_write_check.c` checks that the live value wins over the
environment.

## 3. lwcond tombstone versus guest address `0xFFFFFFFF`

`runtime/ppu/lwcond_real.h`: the tombstone is `(long)-1`, which casts to
`0xFFFFFFFF`. Both the lookup and the free scan now require
`cur != LWC_TOMBSTONE && (uint32_t)cur == addr`, so a freed slot on the probe
chain is not mistaken for a live lwcond at `0xFFFFFFFF`.
`tests/sync_stress/test_lwcond_real.c` (phase `table`) creates a same-bucket
slot, frees it, and checks the lookup misses, then creates and frees
`0xFFFFFFFF` itself.

## 4. Shader disk cache (`libs/video/rsx_shader_spirv.cpp`)

* At cache open, `*.tmp` files older than an hour are removed (a recent one is
  kept: another live process may be writing it).
* A load rejects a file with bytes past the words its header declares, as well
  as the existing short/version/key/checksum mismatches.
* A load copies the config and path under the cache lock, then does the file
  I/O and checksum outside it. Stores keep their existing atomic
  write-and-rename.
* The words are read straight into the caller's destination, with no
  temporary vector and `memcpy`.

No pruning of valid entries was added. File format, key and
`RSX_SHADER_TRANSLATOR_VERSION` are unchanged.

Tests: `test_shader_spirv` now has 52 checks (was 48). The new ones append a
byte to a valid file and require a rejected miss and rewrite, and create an
old and a recent `.tmp` before a cache open and require only the old one to
disappear.

## Tests run

| suite | result |
|---|---|
| `runtime/spu/tests/run_tests.sh` | 11/11 passed |
| `tools/test_video_fixtures.py` | 30/30 passed |
| `test_shader_spirv` (build-play) | 52 passed, 0 failed |
| `test_lwcond_real` (all phases, build-play) | passed |
| `test_fs_write_check.py` | passed |
| `test_cellfs_vfs_root.py`, `test_ppu_fs_rw_switch.py`, `test_ppu_fs_large_file.py`, `test_ppu_fs_aio.py`, `test_ppu_fs_imports.py` | all passed |
| `check_spu_context_abi.sh` (build-play cache) | shared offsets agree |
| `ninja -C recomp-work/build-play MyGameRecomp` | built |

## Test-harness repairs (no product change)

Two pre-existing breakages on this branch had to be repaired to run the named
suites. They are test-only:

* Four `runtime/ppu/tests/test_fs_*.cpp` fixtures did not define the guest
  access/write hooks that `ppu_fs.cpp` now calls, so they failed to link.
* `runtime/spu/tests/run_tests.sh` did not link `c690_stats.c` /
  `c690_corpus.c`, so 7 of its 11 jobs failed to link on the recent c690
  instrumentation symbols.

The unrelated working-tree change in `scratch/ppu_conformance.cpp` was left
uncommitted.
