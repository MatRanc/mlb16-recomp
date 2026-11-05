# Capture tools fix: walker-prof sampling and full guest imaging

Date: 2026-11-05. Branch `exp/capture-tools` (off `exp/live-ab`) in a new
worktree `ps3recomp-tools`, build `recomp-work/build-tools`. One automated
pitch-meter launch (the mission cap), X once; no other game run. Follows
[`c690-serialisation.md`](c690-serialisation.md), [`walker-mt-m3.md`](walker-mt-m3.md),
[`walker-per-draw.md`](walker-per-draw.md), [`walker-tex-cost.md`](walker-tex-cost.md),
[`walker-prof.md`](walker-prof.md) and [`targets-and-backlog.md`](targets-and-backlog.md).
No RPCS3 or other GPL emulator source was read or cited.

## TL;DR

* **`[walker-prof]` sampled the wrong unit.** The sampler chose a whole frame
  from the flip counter, but the timed work lives in a *drain*. A sampled
  drain could be an empty pass while the window's draws were handled by
  unsampled drains: the report then printed `engine=0.000` next to real draws
  and under-counted the phases. The old log `ab_live_walkermt2_20261104_181555`
  has **322 of 1,698 reports (19 %)** with `draws>0 && engine==0`; the new
  live log has **0 of 2,709**. Sampling is now per drain and scaling is
  `drains_since_report / sampled_drains`; a report is deferred while the
  window has draws but none of the sampled drains produced any.
* **The walker capture missed the tail of multi-MB main-memory reads.** Three
  separate faults: (1) M3's cold-start notes ran *before* `s_wcap_active` was
  set and before the range list was cleared, so both notes were no-ops; (2)
  the `.mem` writer extrapolated one EA across a range, but the GCM IO table
  maps main memory per MB and a 2 MB mesh-ring span crosses a mapping page;
  (3) a live frame can name a narrower read than the cold replay resolves, so
  even the correctly noted pages were not enough. The capture now images
  **every mapped main-memory IO page at begin**, expands every main read to
  its whole 1 MB IO page, and resolves each 4 KB page's EA on its own.
* **Proof.** The old crowded capture replays **1,261 backend draws/frame with
  4,547 read misses**; the same frame with the new page set replays
  **1,694 backend draws (1,702 sink groups, the live count) with 0 misses**.
  A live meter capture taken with the new code wrote 45,279 pages / 176.9 MB
  and replays with **0 misses**. `meter_frame` / `walker_frame` hashes are
  unchanged (`a6fcc849341eba9b` / `416abee2c5bff47d`), so the capture change
  does not perturb the replay.
* **Nothing is default-on.** The profiler is the existing diagnostic; the
  capture is still armed only by `PS3RECOMP_WALKER_CAPTURE`. No renderer key
  was added. No `spu_context` change (ABI check passes).

## 1. Profiler: why sampled reports said engine=0 with draws>0

`PS3RECOMP_WALKER_PROF` times every drain, draw and FIFO method on one drain
in `PS3RECOMP_WALKER_PROF_SAMPLE` (default 8). The report is emitted at the
end of a *sampled drain*, but the old sampler chose that drain from
`s_wprof_frame_seq`, which `rsx_wprof_note_flip()` advanced. Two independent
streams were being mixed:

* Draws are counted on **every** drain (`sink_end_work`, exact, never scaled).
* Phases and dispatch are timed only on the sampled drain.

A title can flip several times between drains, and one visible frame can take
several drains. When the sampled drain fell right after a flip it was often an
empty pass; the window's draws had already been handled by unsampled drains.
The old report then had `draws>0` from the draw counter and `engine=0` from
the empty sampled drain's timers. It also scaled one drain's timings by
`frames_since_report / sampled_frames`, which is only correct at exactly one
drain per frame.

The fix, in `libs/video/rsx_draw_engine.c`:

* Sampling is per drain (`s_wprof_drain_seq`, first drain sampled so a
  one-frame replay still reports). `s_wp_drains_total` counts every drain;
  `s_wp_drains_prof` counts the timed ones; the scale is
  `drains_total / drains_prof`.
* `s_wp_draws_prof` counts the draws seen in timed drains. `drain_end()`
  keeps the window open while `draws_prof == 0 && draws > 0`, so a report is
  only emitted once a sampled drain actually carries draws (or when the
  window is genuinely idle, `draws == 0`, where `engine=0` is consistent).
* A timed drain before the first flip is counted in `drains_prof` too (its
  timings are already in the sums), so the first report's scale is right.

The draw count stays exact (`draws` is still accumulated on every drain); only
the phase sums are sampled and scaled, and they now describe the same work.

## 2. Capture: why the crowded replay dropped 440 draws

`PS3RECOMP_WALKER_CAPTURE` records one frame's FIFO and the guest pages its
draws read. The crowded replay `recomp-work/walker_replay` (1,702 submitted
draws) replayed only 1,261 backend draws with 4,547 read misses per frame. All
misses were `location=1` (main memory) at IO offsets 0x900000-0x15xxxxx: the
2 MB SPU mesh-ring vertex spans. The replay's `[walker-prof]` showed
`n/fetch=1701` but `bind=draw=1261`.

### 2.1 The three faults

1. **M3's cold-start notes were no-ops.** `rsx_wcap_begin_if_triggered()`
   called `wcap_note_cached_textures()` before `s_wcap_active = 1` (the noter
   drops input while the window is not active) and then cleared
   `s_wcap_nranges` after the notes. The cold-start texture/epoch fix from
   `walker-mt-m3.md` therefore never contributed a range on a real capture.
   The window is now armed (resets done, `s_wcap_active = 1`) *before* the
   notes.
2. **One EA was extrapolated across a whole range.** `wcap_write_mem()` wrote
   `p->ea = r->ea - (r->offset - off)` for every page. The GCM IO table maps
   main memory one MB at a time (`s_ea_address_table`), so a 2 MB ring span
   crosses a mapping page and the tail's EA was wrong. Each page is now
   resolved on its own with `cellGcmResolveLocated()`; the range head's EA is
   only the fallback.
3. **Live and replay read different spans.** The cold replay resolves the
   same index buffer to a wider min..max vertex span than the live frame
   happened to name (warm plan, narrower live range). Noting only the live
   pages leaves the replay's tail out. The capture now images:
   * **every mapped main-memory IO page at begin**
     (`wcap_note_mapped_main()`: `cellGcmResolveIO(p << 20) != 0`);
   * every main read expanded to its whole 1 MB IO page;
   * local reads at their exact 4 KB pages (local memory is linear);
   * the cached texture spans, now actually noted.

The mapped-main pass costs at most the title's mapped main memory (main RAM is
256 MB on PS3), so a capture is larger but still under the 768 MB cap. For
this title the crowded capture is expected around 250-350 MB; it lives under
`recomp-work/captures/`, never `/tmp` and never in git.

## 3. Numbers

### 3.1 Profiler

| log | reports | `draws>0 && engine=0` |
|---|---:|---:|
| `ab_live_walkermt2_20261104_181555.log` (old) | 1,698 | **322 (19.0 %)** |
| `ab_live_tools_meter_20261105_024605.log` (fixed) | 2,709 | **0** |

The fixed run is the mission's one pitch-meter launch (see 3.3).

### 3.2 Crowded capture replay

One frame, null backend, default digest on:

| capture | sink `draws` | backend draws/frame | guest reads |
|---|---:|---:|---:|
| `walker_replay` (old `.mem`) | 1,702 | 1,261 | 218,895 hit / **4,547 miss** |
| `captures/walker_replay_fixed` (new page set) | **1,702** | **1,694** | 19,809 hit / **0 miss** |

The fixed file is the old capture's FIFO/state with a `.mem` rebuilt to the
new rule: the old present pages, every 1 MB IO page those reads fall in, and
every page the replay asked for. Pages the old capture never imaged are
zero-filled, so this proves the *page set* restores the draw count; it is not
a visual reference. The last two 1 MB regions the replay needed
(0x1300000, 0x1400000) are mapped main memory, which the final
mapped-main pass images.

### 3.3 Live meter capture (one launch)

`ab_live.sh` with `BUILD=build-tools`, `N=1`, `METER=1`, X once,
`CAPTURE_PREFIX=recomp-work/captures/meter_tools`:

* capture: 79,249 words, 45,279 pages, **176.9 MB**, no `.missing`;
* replay: 312 sink draws, **306 backend draws/frame, 0 read misses**
  (the old `meter_frame` is 502 backend draws / 0 misses, a different frame).

The live launch used the ordering fix and the 1 MB per-read expansion. The
mapped-main begin pass was added after the launch (one-launch cap) and is
covered by the unit fixture; the crowded recapture below is its live gate.

### 3.4 Exactness

Replaying the unchanged captures with the final binary:

| capture | last hash |
|---|---|
| `meter_frame` | `a6fcc849341eba9b` |
| `walker_frame` | `416abee2c5bff47d` |

Both unchanged. `python3 tools/test_video_fixtures.py`: **32/32** on clean
runs. The pre-existing timing-sensitive `test_gcm_tick_in_drain` flaked in two
loaded runs (different assertion each time) and passes standalone; it does not
compile `rsx_draw_engine.c`.

## 4. Tests

* `libs/video/tests/test_walker_prof.c` (new, in the fixture runner): drives
  `rsx_wprof_drain_begin` / `note_dispatch` / `note_flip` / `drain_end` with
  synthetic drains. It checks that (a) an empty sampled drain after a report
  defers instead of printing `engine=0` with `draws>0`, (b) an idle window
  still reports `draws=0 engine=0`, (c) sampling is per drain with stride 2,
  and (d) a 200-drain mixed sequence never produces the bad pair.
* `libs/video/tests/test_walker_capture.c` (new): drives the real trigger ->
  flip -> begin -> note -> finish path on a private `mmap` guest window. It
  checks that a 12-byte main read images its whole 1 MB IO page and not the
  next, that mapped main IO pages are imaged with no read at all, that a local
  read images only its 4 KB page, that the payload bytes match the guest, and
  that no page is dropped as unbacked.

## 5. User command: crowded-replay recapture

The crowded scene is the user's to drive (the automated harness only reaches
the pitch meter). `build-tools` is already built from `exp/capture-tools`.

```sh
cd /home/m/Documents/mlb16-online/recomp-work
rm -f captures/walker_replay2.trigger
BUILD=build-tools \
PS3RECOMP_WALKER_CAPTURE=$PWD/captures/walker_replay2 \
PS3RECOMP_WALKER_PROF=1 \
./play_windowed.sh
# In the window: boot to a match, enter the instant replay, zoom out until the
# whole field and both teams are on screen, let it run one frame, then:
touch captures/walker_replay2.trigger
# The next flip arms and the next drain writes walker_replay2.{fifo,mem,state}
# (expect ~250-350 MB: the whole mapped main memory is imaged).
```

Leave `PS3RECOMP_WALKER_MT` unset (capture is a walker-thread operation).
Verify offline, with the digest at its default (do **not** set
`REPLAY_NULL_HASH=0`):

```sh
cd /home/m/Documents/mlb16-online/recomp-work
./build-tools/ps3recomp_sdk/replay_rsx_engine captures/walker_replay2 \
  --frames 5 --warmup 2
# gate: draws/frame >= 1300 (expect ~1,694), guest reads 0 miss,
# [walker-prof] draws ~1,700.
```

The morning result to report back: the capture's `[walker-cap] wrote ...
pages=... MB=...` line, the replay's `draws/frame` and `guest reads`, and
whether a `.missing` file appeared.

## 6. Caveats

* One game launch (the cap), pitch meter only, X once. The final mapped-main
  pass was added after it; the unit fixture and the crowded synthetic page
  set cover it, and the user recapture is the final proof.
* The crowded synthetic replay zero-fills the pages the old capture lacked.
  It proves the captured *page set* is what dropped the draws; it is not a
  visual or hash reference for that frame.
* The new profiler sampling changes the *absolute* phase numbers slightly
  (they now include the draws' drains); it leaves `walker=`/`wait=` semantics
  and the A/B direction unchanged. Old logs still need the `engine=0` filter.
* Imaging all mapped main memory makes a capture larger. The 768 MB cap is
  unchanged; a title that maps more than the cap will record the overflow in
  `.missing` instead of silently truncating.

## 7. Clean room

This is our own runtime, replay captures, trace/log stream and EBOOT. The
reused ideas (cold-start notes, mesh-ring EA range) are from our own earlier
missions. No RPCS3 or other GPL emulator source was read or cited.
