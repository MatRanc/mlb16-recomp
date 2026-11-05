# Walker M3: N workers, contiguous chunks, ordered commit

Date: 2026-11-04. Branch `exp/live-ab` in `ps3recomp-play`, built and
measured in `recomp-work/build-mt`. Follows
[`walker-multithread-design.md`](walker-multithread-design.md) section 5 (M3)
and [`walker-mt-m1m2.md`](walker-mt-m1m2.md). **No game launch was used**: the
mission stopped at the first failed gate (offline timing), as the stop rule
requires. Everything here is default off.

## TL;DR

* **Step 0 (capture) fixed in code, recapture still needed.** The live capture
  only images pages the captured frame actually reads. A warm fetch-plan memo
  (or a cached texture that is revalidated only on a later frame) makes the
  live frame skip a skill the cold replay performs, so the replay asks for
  pages the `.mem` never got and drops the affected draws (1,278 backend draws
  of 1,705, 23,535 misses). The capture boundary now bumps the fast-cache epoch
  and notes every cached texture span, so the captured frame performs the same
  guest reads a cold replay will. An offline replay-as-capture experiment
  reproduces the failure and shows the direction (1,278 -> 1,703 draws,
  23,535 -> 211 misses in the simulation); a user recapture is the final proof.
  Command in section 6.
* **M3 is implemented, exact, and default off.** N=1..4 workers get contiguous
  chunks of immutable packets. Each worker resolves its own remap, fetch plan,
  mesh-ring snapshot, topology/index rebuild and vertex conversion in
  per-worker scratch; commit is ordered by a global packet turn counter so the
  backend is touched by one thread at a time in guest order. Keys:
  `PS3RECOMP_WALKER_MT=1 PS3RECOMP_WALKER_MT_WORKERS=N` (1..4),
  `PS3RECOMP_WALKER_MT_SNAPSHOT=0` for fence-only (default 1, conservative).
* **Exactness passes.** `meter_frame` and `walker_frame`, N=off/0/1/2/3/4, with
  `REPLAY_NULL_VSRANGE=1`, all print the serial hashes
  (`a6fcc849341eba9b` / `461ff3a834c62331` and
  `416abee2c5bff47d` / `9b78ac38a25c5cbf`). `test_video_fixtures.py` 30/30.
* **The M3 timing gate FAILS.** 7 interleaved runs, 120 frames + 20 warm-up,
  `REPLAY_NULL_HASH=0`: `walker_frame` N=1 **16.07 ms**, N=3 **14.14 ms**
  (**-12.0%**, gate -25%), N=4 **14.52 ms** (worse than N=3). `meter_frame`
  N=1 10.82, N=3 10.60, N=4 10.89. N=3 is level with the M2 reference
  (14.21 ms on `walker_frame`), not 25% better. Per the mission rule the
  mission stops here: no live A/B, nothing default-on.
* **Why:** M3 moves remap/plan/convert off the state thread, but the state
  thread still owns texture resolution/upload, pipeline fingerprint and the
  reference build, and every texture create/upload/snapshot drains the worker
  pipeline. On `walker_frame` the state `tex` term rises 1.45 -> 7.47 ms at
  W=3 because of those waits, which eats the parallel win. The 25% step is
  the M4 texture/pipeline resolver (and turning uploads into ordered records),
  not more workers. N=4 is CPU oversubscription on this 6C/12T box.
* One real concurrency bug was found and fixed by TSAN: the SPSC ring's
  producer reused a slot after a relaxed read of the consumer cursor. The
  producer now acquire-loads `r` before overwrite; `test_draw_queue` is
  TSAN-clean.

## 1. What M3 changed

`libs/video/rsx_draw_engine.c`:

* `dc_fetch` grew a `refs_only` mode. It builds the source reference list and
  the post-init fetch plan on the state thread and stops there.
* `sink_end_work` writes a packet with the source references, the cuts, the
  plan after `rsx_vertex_fetch_plan_init`, and the draw's primitive/rebuild
  flags. Immediate and INLINE_ARRAY groups keep the state-thread packet path.
* A chunk is `[u32 count][u32 pad]` followed by whole packet records; the state
  thread closes it at 16 packets, at the chunk byte cap
  (`PS3RECOMP_WALKER_MT_CHUNK_KB`, default 4096) or at any sync point, and
  pushes it to worker `chunk_id % N`.
* Each worker prepares the **whole chunk** before committing: it copies the
  source refs, builds its own `rsx_vertex_remap`, resolves the spans with the
  worker guest reader, rebuilds the topology/index buffer, snapshots every
  stable source span into a 32 MB worker arena (default; `SNAPSHOT=0` skips
  this), computes the GPU-unpack descriptor and converts the unique vertices.
  Outputs live in the arena and are committed from there.
* Ordered commit is a global packet `seq` and `next_commit`: a worker may only
  touch the backend when `next_commit == p->seq`, then advances it. The
  backend therefore still sees one thread at a time in guest order; drawing
  and write tracking stay in packet order.
* `ps3_live_ctl` keys were already present from M1/M2; no new keys and no
  `spu_context` change (ABI check passes).

`libs/video/rsx_draw_queue.c`:

* `pub`/`r`/`stop` are C11 atomics (they were plain integers accessed through
  atomic builtins, which TSAN rightly flags).
* The worker publishes `r` after each record, and the producer **acquire-
  loads** `r` before reusing a ring slot. That is the missing half of the
  hand-off the TSAN run exposed.

## 2. Step 0: the crowded capture

### 2.1 Root cause

`recomp-work/walker_replay.*` replays 1,278 backend draws/frame with 23,535
read misses (M0 gate: >= 1,300 draws, 0 misses). The walker's profiler line in
the capture log shows 1,696 binds/draws for the same frame, so ~418 draws are
dropped by the replay.

`cap_read` keys on `(location, offset)` and returns NULL when a page is not in
the `.mem`. The live capture only calls `rsx_wcap_note_read` for reads the
captured frame actually performs. Two things break that assumption:

1. the fetch-plan memo turns a draw's span read into a memo hit, so the frame
   never notes the vertex pages it is about to draw;
2. a texture that is cached is not necessarily re-read on the captured frame,
   while a cold replay validates it.

The replay then reads spans/pages the `.mem` does not contain, the vertex
fetch fails, and the draw is dropped. The hashes are self-consistent only
because the replay always takes the same (incomplete) path.

### 2.2 Fix

At the capture boundary (`rsx_wcap_begin_if_triggered`):

* `s_fast_epoch++`, which invalidates the fetch/remap/pipeline memos so every
  draw in the captured frame re-resolves its spans and notes them;
* `wcap_note_cached_textures()` notes every cached texture's span, so a cold
  replay's revalidation finds its bytes.

An offline simulation (replaying the capture while re-capturing a warm frame)
reproduced the failure; re-capturing a cold first frame went from 1,278 draws
/ 23,535 misses to **1,703 draws / 211 misses**. The remaining simulation gap
is the cold-cache texture class the cached-span note now covers. The real
recapture is the final check.

### 2.3 Recapture command (user)

```sh
cd /home/m/Documents/mlb16-online/recomp-work
rm -f walker_replay.trigger
PS3RECOMP_WALKER_CAPTURE=$PWD/walker_replay \
PS3RECOMP_WALKER_PROF=1 \
./play_windowed.sh
# In the window: boot to a match, enter the instant replay, zoom out until the
# whole field and both teams are on screen, let it run one frame, then:
touch walker_replay.trigger
# The next flip arms and the next drain writes walker_replay.{fifo,mem,state}.
```

Leave `PS3RECOMP_WALKER_MT` unset (capture is a walker-thread operation).
Verify offline:

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
./ps3recomp_sdk/replay_rsx_engine ../../recomp-work/walker_replay --frames 5 --warmup 2
# gate: draws/frame >= 1300, guest reads 0 miss
```

## 3. Gates

### 3.1 Digest vector (pass)

`build-mt`, `--frames 5 --warmup 2`, `REPLAY_NULL_VSRANGE=1`:

| capture | off / N=0 | N=1 | N=2 | N=3 | N=4 |
|---|---|---|---|---|---|
| `meter_frame` last | `461ff3a834c62331` | same | same | same | same |
| `walker_frame` last | `9b78ac38a25c5cbf` | same | same | same | same |

Without VSRANGE the same holds with the historical
`a6fcc849341eba9b` / `416abee2c5bff47d`. `test_video_fixtures.py`: **30/30**.

### 3.2 Timing (fail)

7 interleaved runs, 120 measured frames, 20 warm-up, `REPLAY_NULL_HASH=0`,
play defaults, unpinned:

| capture | N=1 | N=2 | N=3 | N=4 | N=3 vs N=1 |
|---|---:|---:|---:|---:|---:|
| `meter_frame` | 10.821 | 10.244 | 10.600 | 10.887 | **-2.0%** |
| `walker_frame` | 16.066 | 13.953 | 14.144 | 14.521 | **-12.0%** |

Gate: N=3 at least 25% better than M2, N=4 not worse than N=3. Both fail.
M2's own `walker_frame` W=1 median was 14.213 ms, so M3's N=3 is ~0.5% better
than M2, not 25%; and N=4 is ~2.7% worse than N=3.

## 4. Why the gate fails

`walker_frame` phase split (`WALKER_PROF=1`, sampled frame):

| phase | MT off | W=3 |
|---|---:|---:|
| parse | 1.01 | 1.47 |
| fetch | 4.91 | 1.14 |
| tex | 1.45 | **7.47** |
| pipe | 2.14 | 2.13 |
| draw | 4.82 | 3.78 |
| other | 2.09 | 2.31 |

The fetch/convert work did move and shrink on the state thread. But `tex`
grows by ~6 ms because texture create/upload/snapshot still call `eng_mt_sync`
and now wait for the worker chunk that contains every draw since the last
texture operation. That wait plus the remaining state-thread texture,
pipeline and reference work is the floor. Parallelising more workers cannot
remove it: it is the M4 resolver (texture/pipeline keys resolved off the
per-draw critical path, uploads turned into ordered records instead of a
state-thread backend call) that is supposed to take this step. On the crowded
capture the replay-only texture hashing makes the same term even larger.

N=4 being slower than N=3 is the 6C/12T box at four converting workers plus
the state thread, the c690/PPU lanes in a live run, and the commit turn.

**Decision:** keep the code as an experiment, default off
(`PS3RECOMP_WALKER_MT=0`), do not claim M3, and do not run the live A/B or the
user crowd test. The next mission is M4 with the state-thread `tex` term as
the target (<=5 ms/frame), then re-run this timing gate.

## 5. What is already correct and reusable

* The packet carries source references, cuts and the post-init plan; a worker
  can do the entire prepare without touching the state thread.
* The turn counter is a correct ordered commit for an arbitrary worker count
  and needs no global backend lock; the backend still sees one thread at a
  time.
* `PS3RECOMP_WALKER_MT_SNAPSHOT=0` is the fence-only path; `=1` (default)
  copies every stable span into the worker arena. The offline byte output is
  identical either way.
* The SPSC ring and the turn queue are TSAN-clean (`test_draw_queue` built
  with `-fsanitize=thread`).

## 6. User commands (prepared, not run)

Recapture: section 2.3. Zoomed-out replay A/B (40 ms hitch recording), once a
crowded capture is clean and M4 has landed:

```sh
cd /home/m/Documents/mlb16-online/recomp-work

# Arm A: play defaults, walker MT off.
PS3RECOMP_TIMELINE=/tmp/mt_A_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_WALKER_MT=0 \
./play_windowed.sh

# Arm B: same shot, MT on with three record workers.
PS3RECOMP_TIMELINE=/tmp/mt_B_$(date +%Y%m%d_%H%M%S).pstl \
PS3RECOMP_TIMELINE_HITCH_US=40000 \
PS3RECOMP_TIMELINE_RING_MB=32 \
PS3RECOMP_TIMELINE_FLUSH_MB=2 \
PS3RECOMP_WALKER_MT=1 \
PS3RECOMP_WALKER_MT_WORKERS=3 \
./play_windowed.sh
```

Compare `draws=` (must match), `[walker-prof]` `walker=`/`tex=`, the
`hitch_table.py --min-ms 40 --bins --classify` bands, and `[walker-mt]`
fallbacks. Live fallback at any time, in the same launch:

```sh
printf 'PS3RECOMP_WALKER_MT=0\n' > /tmp/mt_ctl
# start the game with PS3RECOMP_CTL=/tmp/mt_ctl; the walker drains the
# workers and resumes the old path at the next drain.
```

## 7. Clean room

This is our own runtime, replay captures, trace stream and EBOOT. The
SPSC record queue and the copied borrowed payloads are reused from the MIT
inFAMOUS 2 port (`rsx_be_thread.c`, `bq_direct_begin`); the header and the
packet block carry the required notice:

> Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
> contributors, MIT.

No RPCS3 or other GPL emulator source was read or cited.
