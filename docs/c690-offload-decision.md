# c690 GPU-offload decision (measure-and-decide)

Date: 2026-11-01. Host: Ryzen 5 2600X, RTX 3060 Ti (clocks locked 1400–1980), Fedora 44.
One game run (`measure_fast.sh`, `PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1`), pitch reached, **34.63 fps**
(350 flips / 10 s). All Phase-2 numbers below are the **pitch-window delta** (245.4 s → 255.4 s, 10.0 s),
not the cumulative report (the run includes the match load; the load is 127 s of the 255 s).

## Measured

**Fence/submit cost** (`tools/replay/test_vk_sync.c`, 10k iters, locked clocks):
- record+submit+wait: **41 µs p50, 49 µs p90, 74 µs p99** (mean 45).
- submit-only: **2.2 µs p50, 11 µs p99**.
- batched: N=8 **5.9 µs/dispatch**, N=64 **2.0 µs/dispatch**.

**Post-transform consumers (Phase 1B):** `0x2A` assembler (`0x10F78`), culls `0x25/0x26` (`0x132E0`) and `0x2C`
(`0x12C08`), and producer-side `0x0A` pack (`0x10B88`). Everything else is index/metadata/packets.

**Pitch-window c690** (10.0 s, 34.63 fps):
- **429 jobs/frame** (148,629 jobs / 10 s / 34.63), 14,863 jobs/s.
- job wall (entry `0x135B8` → end `0x12AB8`): **73.5 ms/frame cumulative** across workers (~14.7 ms/worker),
  25.44 s over the window; **p50 115 µs, p90 239 µs, max 3536 µs** per job.
- opcode histogram: `0x0A` **30.6 %**, `0x2A` **7.9 %** (1.19/job → essentially every job), setup
  `0x04/0x07/0x08/0x13/0x1F/0x00` 6.7 % each, `0x0C/0x27` 5.7 %, `0x21` 5.0 %, `0x25` 3.7 %, `0x26` 1.3 %,
  `0x2C` 0 % (0.55+0.19 cull/job → a majority of jobs run a cull).
- exclusive functions captured by the stats: `0xAF98` 6.4 %, `0xA798` 0.9 %, `0x10F78` 0.5 %, **other 92.2 %**.
- **Walker** (`VK_DRAWPROF`): ~525 draws/frame × **9.8 µs/draw ≈ 5.2 ms/frame** Vulkan recording
  (desc 1.4, copy 1.4, vtxgpu 2.9, barrier 0.8, rp_begin 1.9, state 0.4, bind_draw 0.6, rp_end 0.5).
  The rest of the walker's ~14–15 ms is texture checks / vertex remap / bookkeeping (prior `VK_DRAWPROF`
  breakdown), not in this drawprof.

**Leaf cross-check** (entries × offline ns/call vs counted exclusive):
- `0xAF98`: 4,947,308 × 216 ns = **1.07 s** vs counted **1.63 s** → **1.52×** (the last-entry-owns-interval
  model over-bills the leaf by ~52 %, exactly the caller-loop inflation).
- `0x6D68`, `0x7C18`, `0x8160`: **entries = 0 in the stats** — they are reached via `goto`/native-swap, not
  `SPU_DIRECT`, so this run cannot attribute them; no `0x7C18`/`0x8160` entry capture exists for offline ns/call.
  Bucket shares therefore come from the earlier gdb sampling profile (140 c690 samples): **per-vertex ~50 %,
  tree/leaf ~34 %, index/ring ~15 %** — unbiased by the exclusive-time model, but a smaller sample.

## Design A — GPU math + CPU fence per job

Win per job = per-vertex µs saved − fence round trip. Per-vertex ≈ 50 % of the job wall (gdb profile):

| job | wall | per-vertex (50 %) | fence p50 / p99 | win @p50 / @p99 |
|---|---:|---:|---:|---:|
| p50 | 115 µs | 57.5 µs | 41 / 74 µs | +16.5 / **−16.5 µs** |
| p90 | 239 µs | 119.5 µs | 41 / 74 µs | +78.5 / +45.5 µs |
| mean | 171 µs | 85.5 µs | 41 / 74 µs | +44.5 / +11.5 µs |

Frame: 429 jobs × mean win 44.5 µs = 19.1 ms/frame cumulative ÷ 5 workers ≈ **3.8 ms/frame wall** →
28.9 → 25.1 ms → **~40 fps**. At p99 fence the p50 job *loses*; if the per-vertex share is 35 % the mean win
falls to ~19 µs → ~37 fps. **Design A is fence-limited to ~37–40 fps**, a ~3–5 fps gain for a full integration.

## Design B — full GPU/native job, no CPU wait (the only 60 path)

Because `0x2A` runs in essentially every job (1.19/job) and a cull in a majority, Design B cannot be
"GPU math + SPU assembler": it must move **the whole job** — per-vertex unpack/transform/normalize/pack,
the `0x2A` 36-byte assembler, the `0x25/0x26/0x2C` culls, and the index/ring sizing — to the GPU, with the
GPU writing draw counts and the walker issuing **indirect draws** (no CPU fence).

Gain: removes the c690 command wall (73.5 ms/frame cumulative, ~14.7 ms/worker) and the walker's ~10 ms/frame
SPU wait. The frame then approaches the walker's own CPU (~14–15 ms) + GPU; **~55–60 fps plausible**.
Work: a native/GPU reimplementation of the whole c690 job across the format variants, with bit-exact SPU float
semantics and indirect draws — the plan's **2–4 months**. Not a spike.

## Verdict

**No-go for Design A** (fence eats most of the per-job win; ~37–40 fps, not worth the integration risk).
**Design B is the only path to 60** but is a 2–4 month native/GPU reimplementation; do not start it without a
go-ahead. If B is not taken, the next lever ranked by measured ms:

1. **Tree/leaf lifter codegen** — ~34 % of the SPU wall (~25 ms/frame cumulative at 34.6 fps); recursive,
   pointer-chasing, hard, but the largest non-offload bucket.
2. **Walker per-draw** — ~14–15 ms/frame (5.2 ms Vulkan + ~9–10 ms texture/remap/bookkeeping); already under
   the 16.6 ms budget, so limited headroom.
3. **Per-vertex GPU offload without assembly** — not viable (Design A).
