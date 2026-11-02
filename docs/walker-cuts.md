# Walker cuts 1 and 3: fetch/remap cache, pipeline memo, and the replay output gate

Date: 2026-11-02. `ps3recomp-probe` branch `feat/walker-cuts` (off
`feat/walker-prof`, not merged). Offline only: no game run was made for this
work. Cut 2 (texture check/hash) is not implemented and Vulkan recording is
untouched.

## What was added

### `PS3RECOMP_WALKER_FAST=1` (default off)

Read once at `rsx_draw_engine_init`. With it off, both fast paths are a cached
integer compare and every draw runs the old code.

**Cut 1 -- fetch plan/remap memo (`libs/video/rsx_draw_engine.c`, `dc_fetch`).**
A draw's prepared `rsx_vertex_fetch_plan` and its `rsx_vertex_remap` are kept
together, keyed by:

- the `rsx_vertex_layout_plan` (mask, stride, attribute order),
- the transform base offset and divider mask,
- all sixteen decoded VTXFMT descriptors and their default values,
- `allow_remap` and the `PS3RECOMP_ENG_VTX_REMAP` setting,
- the complete source reference list `(vertex_id, base_index)`.

The reference list is what the remap is built from and what the fetch plan's
spans are resolved against. A hit skips the remap hash build and the
per-attribute span resolution, and restores the plan, the unique references and
the occurrence-to-unique map. An INLINE_ARRAY stream and an immediate-vertex
group bypass the memo: they have no plan or no stable source.

**Cut 3 -- pipeline input memo (`eng_pipeline_get`).** One pipeline answer is
kept per fingerprint:

- `fixed` (built-in vs guest pair) and the transform-program bytes,
- the fragment-program bytes (read from guest memory),
- the pipeline-relevant registers: `SHADER_CONTROL`, `0x1FEC`, `SURFACE_FORMAT`,
  `0x1FF8`, `0x1478`,
- every enabled fragment and vertex texture unit's eight raw words (plus
  `TEX_SIZE1` for fragment units),
- the input layout, the decoded render state, `rt_fmt` and `rt_count`.

The answer includes the pipeline handle, `fixed`, `tex_used`, the fragment
debug hash, the `vp_c` range, and the buffered fragment-constant block. On a
hit the caller gets all of it without re-walking either program. The transform
constant upload window and the immediate-vertex registers are deliberately not
in the fingerprint: they change on almost every group and are not inputs to the
pipeline key.

Both memo tables are bounded (1024 fetch entries, 512 pipeline entries) and
reused round-robin; they are freed at engine shutdown and reset at init.

### Replay output gate (`tools/replay_null_draw_backend.c`)

`replay_rsx_engine` now reports a 64-bit digest of everything the null backend
receives in a frame: init/state, every resource creation (texture, target,
depth, pipeline; by its semantic arguments, not the fake handle), every upload
payload, every bind (targets, pipeline, both constant blocks, textures and
vertex textures with their samplers), viewport/scissor/stencil/bounds/bias,
every draw's topology/count/stride/index bytes/vertex bytes, clears, present
and readback requests. Handles are mapped to the digest of the request that
created them, so the gate is independent of allocation order and comparable
between builds. `replay_null_reset` starts a new frame.

`replay_rsx_engine` prints the last measured frame's hash and a chain over all
measured frames:

```
output hash:  <last frame>
output gate:  <FNV chain over the measured frames>
```

The hash of a replayed capture is bit-identical on every repeat, with the fast
path on or off. With `--verbose` each frame's hash is printed too.

## Invalidation argument

**Fetch plan/remap.** The memo holds *no guest bytes*. It holds decoded array
descriptors, guest addresses/spans resolved by `eng_guest_ptr`, the unique
reference list, and the occurrence map. Every fetch still reads the current
guest payload through `rsx_vertex_fetch_many*`, so changing vertex data --
within a frame or between frames -- cannot be masked by a cached conversion.
What must invalidate the memo is a change in the *plan* or the *reference set*:

- any VTXFMT/base/divider/layout/remap change is in the key;
- the reference set is read from the draw batches and, for an indexed draw,
  from the guest index array. Different index bytes produce different
  references and therefore a new key: the source data for the reference set is
  part of the identity;
- a new guest-memory resolver (`rsx_draw_engine_set_guest_memory`) bumps
  `s_fast_epoch`, so plans prepared against an old mapping are never reused.

This is why a source-data change between frames is a miss when it changes what
the draw reads (the index source), while a change to the vertex payload is
always observed because it is re-fetched.

**Pipeline answer.** The fingerprint contains the raw fragment and vertex
program bytes, not just their registers. A title that overwrites a program in
place at the same `SHADER_PROGRAM` address changes the fingerprint and misses.
The render state is included through the same hash the pipeline key uses.
Texture-derived masks are covered by the enabled units' raw registers. The
memo stores the exact buffered constant block it answered with, so a hit cannot
serve the previous program's constants. Because the identity covers everything
the key covers and more, a hit cannot change a draw's pipeline, `tex_used`,
constant block or `vp_c` range.

**Replay frame restore.** `rsx_draw_engine_state_restore` replaces the register
file wholesale, which bypasses `rsx_dispatch_method` and its per-unit texture
write counters. It now bumps `tex_wgen` for every unit, so `eng_tex` re-decodes
the restored registers instead of serving the previous frame's last write.
Without that, the old path's pipeline key saw a stale texture snapshot across
replay frames while the new fingerprint saw the restored registers; the two
disagreed from frame 1 until the fix. This is a replay/capture-restore path
only; live play never calls it.

## Offline A/B

Capture: `recomp-work/walker_frame`, replayed through `replay_rsx_engine` with
`--frames 20 --warmup 5` (20 measured frames per repeat). Five repeats per
setting; values are the median of the 20 per-frame profiler reports, then the
median/min/max over the five repeats.

| phase | off (ms/frame) | on (ms/frame) | delta |
|---|---:|---:|---:|
| fetch | 4.200 (4.174-4.240) | 2.572 (2.559-2.592) | **-1.628 ms (-38.8 %)** |
| pipe  | 2.067 (2.058-2.076) | 0.981 (0.972-0.986) | **-1.087 ms (-52.6 %)** |
| engine | 14.978 (14.870-15.087) | 12.377 (12.293-12.500) | **-2.601 ms (-17.4 %)** |

The output gate was identical for all ten runs: `0f91a96c79a7aadf`
(chain over the 20 measured frames), and the last-frame hash matched too. The
existing `replay_rsx` dispatcher hash still verifies:
`./build/replay_rsx kauffman_pitch.bin --frames 5 --verify` reports
**0x85fc2ff5a1655c26, VERIFIED**.

The gate itself is not free (it hashes the VP constant block and every draw's
vertex bytes): the `bind` phase is ~0.83 ms in this build against ~0.03 ms
without the gate. It is present in both A/B arms, so the phase deltas above are
the cuts' own.

## Unit tests

`libs/video/tests/test_rsx_draw_engine.c` (built with the engine, no GPU):

- the first group prepares the plan; an identical group hits it;
- a vertex payload written between draws is fetched fresh on a hit (the memo
  holds no payload);
- the same index source hits; **different index bytes are a different
  reference set and miss**;
- a VTXFMT stride change misses;
- an identical group reuses the pipeline answer;
- enabling depth bounds misses and builds the new pipeline; returning to the
  old state reuses it;
- **rewriting the fragment program in guest memory between frames misses and
  builds a new pipeline** -- the old program's answer is not returned.

All checks pass. The test sets and clears `PS3RECOMP_WALKER_FAST` around its
own engine init, so the rest of the suite runs on the default path.

## In-game test plan (one run)

1. Use the `recomp-work/ab-tools/measure6` flow with
   `PS3RECOMP_WALKER_FAST=1 PS3RECOMP_WALKER_PROF=1`, record one pitch window
   (same trigger as the profile runs).
2. In the `[walker-prof]` reports, expect `fetch` around **2.5-2.7 ms/frame**
   (was ~3.6 in-game) and `pipe` around **1.0-1.5 ms/frame** (was ~1.9); the
   walker drain should fall by roughly 2-3 ms. Note the fps.
3. Watch one pitch for visual regressions: players/crowd geometry (fetch
   plan/remap), uniforms and UI materials (pipeline memo), and any missing or
   black objects. A wrong pipeline usually shows as a constant colour or a
   missing draw, not as corrupt geometry.
4. Check `[replay-engine]` is absent (this is the live engine) and that the
   run does not log new `pipeline ... FAILED` lines beyond the usual warm-up.
5. If anything looks wrong, the next run with `PS3RECOMP_WALKER_FAST=0` is the
   control; do not change any other setting between the two.

## Reproduce

```sh
# ps3recomp-probe, feat/walker-cuts
cmake -S . -B build -G Ninja -DPS3RECOMP_BUILD_TESTS=ON && ninja -C build replay_rsx replay_rsx_engine
./build/replay_rsx kauffman_pitch.bin --frames 5 --verify          # 0x85fc2ff5a1655c26
PS3RECOMP_WALKER_FAST=0 ./build/replay_rsx_engine ../recomp-work/walker_frame --frames 20 --warmup 5
PS3RECOMP_WALKER_FAST=1 ./build/replay_rsx_engine ../recomp-work/walker_frame --frames 20 --warmup 5
```

## Not done

- Cut 2 (texture page-dirty generation / decode memo). The offline replay is not
  a valid A/B for it: a single captured frame has no texture updates.
- No change to Vulkan command recording or the live present path.
