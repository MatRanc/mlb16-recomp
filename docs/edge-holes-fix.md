# Edge holes: the 0x25/0x26 fixed screen bound and the safe cull union

Date: 2026-11-03. Offline only: no game launch. Worktree
`ps3recomp-holes` (branch `exp/edge-holes`, base `8a082ddb`); the 400
captured cull pairs and the generated c690 module are read-only inputs.
See `docs/edge-native-step3.md` for the stage-1/2 context and the 400-pair
replay gate.

## TL;DR

* The user's `MLB16_CULL_BYPASS=3` result is confirmed: the pin-holes are the
  **0x25/0x26 screen cull** (`0x13518`/`0x13520` -> `0x132E0` -> `0xD080`).
* The cull has **two** screen limits. One is the four-float transform
  `{scale_x, offset_x, scale_y, offset_y}` (`{595, 640, -335, 360}` at 720p).
  The other is the **homogeneous `w` / 720p clip row** of the same matrix
  (`0xD080` reads rows at `[r5+0x10..0x50]`; `0xD540` does the min/max and
  sign test with `fcgt`/`selb`). The `w` row is *not* scaled when the viewport
  row is doubled, so scaling-only pushes triangles past the fixed 720p edge
  and drops them.
* **The shipped fix is a union, not a scale.** With
  `PS3RECOMP_C690_CULL_FIX=1` the wrapper runs the cull once at the 2x1 sample
  grid, keeps that index list, restores the real input, runs the unpatched
  cull, then appends only the sample-grid triangles the original list does not
  contain. The original list is never edited or reordered.
* **The append cap is the cull's real buffer, not the entry count.**
  `+0x114` is the candidate *count* the cull consumes (it is not a
  capacity); `+0x1E0` is the end of the index buffer / start of the next
  allocation. The union appends inside `[+0x110, +0x1E0)`, so it cannot
  reach the next live structure, and no addition is dropped on this corpus.
* Gate on all **400** captured pairs: **0** baseline LS mismatches, **0**
  removed triangles, **7** added triangles total (all in one pair, max 7),
  0 LS changes outside the index list, and 0 cap/range/realloc skips. The
  switch is default off and inert when off.

## 1. Review fixes in the cull path

1. **Spool snapshot order.** `tools/c690_ref/patch_cull_hooks.py` now takes
   the corpus snapshot *before* `c690_cull_fix_begin` patches LS, and the
   exit snapshot *after* the unpatched run. The spooled pair is the game's
   real input/output even when the fix is enabled.
2. **Env fallback.** `cull_live_get()` only trusts the live-control table when
   `ps3_live_ctl_enabled()` says a control file is configured; a weak link
   that resolved to an unseeded table now falls back to the environment
   switch instead of reading 0.
3. **Cull-pair cap at enter.** `c690_corpus.c` reserves the slot with
   `s_cull_started` when the entry decides to capture, not with the
   completed-pair counter at exit. Several lanes can no longer overshoot the
   cap by the lane count.
4. **`cull_f` close.** Each lane closes its `cull_f` as soon as the global
   cap has been reserved out (on that lane's next cull exit or job boundary);
   the destructor closes anything still owned by the exiting thread. The
   stage/cull spool pointers and sequence numbers now survive
   `finish_tail()`'s per-job reset, so a sampled job no longer forgets them.
5. **Index-buffer cap and skipped unions (review of `1d3633aa`).** The append
   cap used to be the entry count at `+0x114`. That field is the candidate
   count, not a proven capacity; the buffer end is `+0x1E0` (see §4.1). The
   append now uses `(+0x1E0 - +0x110)/2` entries, validates the three context
   fields first, and counts every skipped union (`union_skip_setup`,
   `union_skip_range`, `union_skip_realloc`, `union_cap_hits`,
   `union_cap_skipped`) instead of returning silently.

## 2. The second fixed screen bound

`0xD080` is the cull's vertex/screen test. It reads a transform block through
`r5`:

```
[r5+0x10] [r5+0x20] [r5+0x30] [r5+0x40]   homogeneous matrix rows
[r5+0x50] = {595, 640, -335, 360}          viewport row (x/y)
```

`0xD540` then evaluates the per-vertex screen min/max with `fcgt`/`selb`
(`0xD560`/`0xD580`, `0xD578`/`0xD588`, `0xD598`/`0xD5F8`, `0xD620`/...),
combines the results with `andc`/`or`, and tests the sign bit
(`andbi r80, r80, 128` at `0xD690`, `ceqi ... 0` at `0xD6A0`). There is **no
1270/1280 float immediate** in the cull: the limit comes from the other
matrix rows, i.e. the homogeneous `w` (the 720p clip/guard plane). The
viewport row supplies the numerator `x/(w)`; the fixed row supplies the
denominator.

The empirical bound matches that. For the first affected pair
(`cull_000` seq 5), the cull writes screen coordinates at `0x21C60` and
rejects the first triangle once `x` reaches ~1269.5 px, i.e. the 1280-wide
logical viewport minus its half-pixel/guard margin; the left edge is 0.
Scaling only `{595, 640}` by 2 doubles the numerator but not `w`, so
triangles that are still inside the 2560-wide sample target are pushed past
this fixed 720p edge and dropped. That is the "separate fixed bound" the
step-3 note could not find, and it is why a scale-only fix is unsafe.

## 3. Why a visible triangle is rejected at the 2x1 grid

The cull runs in the **1280x720 logical pixel space**, while the scene target
is **2560x720, 2x1 antialiased**. Sample `x` is `2 * logical x`. A thin
triangle at the eyes can cover only one of the two samples and still have a
logical-pixel extent that misses every logical pixel centre (or collapses in
the fixed-point min/max test). The cull's `fcgt`/`selb` min/max + sign test
sees "no logical pixel" and drops it; the 2x1 resolve then shows the missing
sample as a pin-hole. Running the same test at 2x sample coordinates makes
that triangle cover a sample, but a pure scale patch also crosses the fixed
`w`/720p edge above, which is why the union is used.

## 4. The union fix

`runtime/spu/c690_cull_fix.c` (default off):

```
mode = c690_cull_fix_begin(ctx, 1);   /* snapshot input, patch 2x */
if (mode == 1) {
    body(ctx);                        /* sample-grid list */
    c690_cull_fix_mid(ctx);           /* save list, restore input, unpatch */
    body(ctx);                        /* the game's real, unpatched run */
    corpus_cull_exit(ctx);            /* spool the real output */
    c690_cull_fix_end(ctx);           /* append sample-grid-only triangles */
}
```

* The original index list and its order are preserved. Added triangles are
  appended (canonical vertex-set membership, so the opposite winding of a
  triangle the game already keeps is not duplicated).
* The append is capped by the cull's real index-buffer extent,
  `(+0x1E0 - +0x110) / 2` u16 entries, so it cannot overwrite the next live
  allocation. Every skip path is counted (`union_cap_hits`,
  `union_cap_skipped`, `union_skip_setup`, `union_skip_range`,
  `union_skip_realloc`); all are 0 on the 400 pairs.
* Cost when enabled: one extra lifted `0x132E0` body per cull plus a 256 KiB
  LS + 2 KiB GPR snapshot per lane. Cost when disabled: one
  predicted-not-taken branch. No `spu_context` fields were added (ABI
  unchanged).

### 4.1 What `+0x114` holds and the real buffer size

The cull context (`r3`, LS `0x1F000` on every captured pair) describes the
index list with three fields:

| offset | role |
|---|---|
| `+0x110` | pointer to the u16 index buffer |
| `+0x114` | candidate count (3 indices per triangle) the cull consumes |
| `+0x1E0` | end of that buffer / start of the next allocation |

`+0x114` is a **count, not a capacity**. Two independent checks:

* Changing it by `n` changes the kept-index count by `n` one-for-one on the
  replay (first pair: `783 -> 240`; `-3 -> 237`, `+3 -> 243`;
  `-30 -> 210`, `+30 -> 258`). The cull reads exactly the candidates the
  field declares. Reproduce with the gate's probe mode:
  `CULL_TEST_PROBE_SEQ=1 CULL_TEST_PROBE_DELTA=30 $OUT/test_cull_pairs <corpus>`.
* In all 400 pairs the entry count is divisible by 3, the output count is
  `<=` the entry count, and the output pointer equals the input pointer: the
  list is filtered in place. `+0x114` is overwritten with the output count at
  exit, which is exactly what a count field does.

The real buffer extent is `+0x1E0 - +0x110`. Across the 400 pairs:

* `+0x1E0` is 16-byte aligned, greater than `+0x110`, and
  `(+0x1E0 - +0x110)/2 >= +0x114` (slack 24..1197 entries, 0 bad pairs).
* `+0x1E0` is never modified by the cull (`400/400` input = output); it is
  read and null-checked by both cull bodies (`0x132E0`, `0x12C08`) and the
  family-2 allocator cursor `+0x340` advances to `+0x1E0 + 0x80` when it
  allocates (pairs 3/4).
* The bytes at `+0x1E0` are a live command header, not free space: after its
  first word the same 32 bytes (`002d04d3 001902b7 c1cf0cb6 ...`) appear in
  every pair. Seven captured buffers end *exactly* at another captured
  buffer's `+0x110`, i.e. the arena tiles.
* The cull itself never writes past its input count: with the entry count
  unchanged, a 32-byte canary immediately after the input list is untouched
  on the probed pairs.

So the proven writable extent for the union is `[ptr, +0x1E0)`, capacity
`(+0x1E0 - ptr)/2` entries. It is larger than the entry count (that is why
the eye close-up can still get its additions), and it stops before the next
allocation (that is why it cannot corrupt LS). `c690_cull_fix_begin()`
validates the three fields; if they are inconsistent the union is skipped and
`union_skip_setup` is incremented instead of guessing a cap.

### Gate (`tools/c690_ref/run_cull_pair_test.sh`)

`corpus_edge3s2`, 400 pairs:

| item | value |
|---|---:|
| pairs | 400 |
| baseline LS mismatches | **0** |
| pairs where the transform heuristic did not match (inert) | 87 |
| removed triangles | **0** |
| original triangles | 78,903 |
| added triangles | **7** (one pair, max 7) |
| LS changes outside the index list | **0** |
| cap hits / cap-skipped triangles | 0 / 0 |
| setup / range / realloc skips | 0 / 0 / 0 |
| odd tails | 0 |

The 87 inert pairs are cull layouts whose transform block does not match the
`{595, 640, -335, 360}` first-quadrant pattern (they are unchanged, hence
still supersets). On this meter corpus the 2x run mostly *drops* triangles,
so the union adds only 7. The eye close-up is the scene the switch is for;
the gate only proves the superset property and the cost.

## 5. Play test (do not enable by default)

From `recomp-work`, on the pitch close-up where the pitcher's eyes are
visible:

```sh
# union fix, default off in every script/build
PS3RECOMP_C690_CULL_FIX=1 ./play_windowed.sh

# control: the confirmed family bypass (draws the culled triangles)
MLB16_CULL_BYPASS=3 ./play_windowed.sh
```

Expected: `=3` removes the holes. If `PS3RECOMP_C690_CULL_FIX=1` also
removes them, the union is doing its job. If it does not, the 2x run is not
adding the hole triangles in that scene; the next step is the full-input-list
superset (append all candidate triangles, equivalent to bypass without
changing the original list), which is safe but has the full cull cost. It is
not shipped and not enabled anywhere.

The switch is implemented behind `PS3RECOMP_C690_CULL_FIX` (family 1,
`0x25/0x26`) and `PS3RECOMP_C690_CULL_FIX_2C` (family 2, `0x2C`, old
single-grid patch). Both are off by default.

## 6. Clean room

This work is our own reverse engineering of the game's SPU job module
(EBOOT EA `0x01161300`, LS base `0x4000`) and the captured LS/GPR pairs. The
stage-split/corpus method follows NHL Legacy Native (MIT, (c) 2026
HeckYouMan) with attribution; no NHL code was copied. No RPCS3 or other GPL
emulator source was read or cited.
