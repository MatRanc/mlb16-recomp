# c690 per-vertex GPU offload — plan and decision

**Decision (2026-11-01): commit to offloading the c690 job's per-vertex math to GPU compute.**

Why this and nothing else:

- Per-function native SIMD rewrites are **bit-exact but at parity** (`0x6D68` 1.00×, `0xAF98+B238` 1.03×); the lift
  already emits the same `vpshufb/vpsllvd/vfmadd132ps` mix. Micro-optimization is closed.
- The cull bypass loses in every variant (all = 33.6 fps, only `0x2C` = 34.85, baseline 36.6); the culls are a net
  win because they remove triangles before the expensive per-vertex math.
- The walker's *own* CPU is already ~14–15 ms (under the 16.6 ms budget); its 24.65 ms drain is mostly waiting on
  the SPU. The SPU wall is ~25.5 ms: ~50 % per-vertex math (~12.7 ms), ~34 % tree/leaf (~8.7 ms), ~15 % index/ring
  (~3.8 ms). Moving the per-vertex math to the GPU is the only lever that can take the SPU below the walker and
  unlock ~55–60 fps.

## What runs where

Keep on the SPU/CPU (serial or branchy; offloading these is not worth it):

- Culling: `0x25`/`0x26` → `0x132E0`, `0x2C` → `0x12C08`.
- Tree/leaf: `0xFDA0`, `0xFCB8`, `0xFCE4`.
- Index expansion / tables: `0x6780`, `0x6860`, `0x10700`.
- Ring allocation and RSX packet emission: `0xE0A8`, `0x4B6C`, `0x4D80`, `0x11F58`, `0x12010`. These must stay
  serial in chain order (WKC2 finding).

Move to GPU compute (per-vertex math):

- Unpack/dequantize: `0x6D68`.
- Transform/skin: `0xD2B8`, `0xD4A8`, `0xC98C` (4-influence blend, plus the heavier orthonormalize variant).
- Normalize: `0xAF98` → `0xB238`.
- Pack/quantize: `0x7C18`, `0x8160`.

## Data flow (from `SPU_JOB_LS_DUMP`, committed `f47deb52`)

- Job code is DMA'd into LS `0x4000` from guest `0x105EE00` (variant `0x106D380`).
- The job entry is LS `0x135B8`; it runs under image 6 (the SPURS policy). Descriptor at LS `0x1330`; `+0xC` →
  a 16×16-byte attribute-format table at guest `0x01243E00`.
- Inputs: per-mesh attribute streams from `0x0173xxxxx`–`0x0180xxxxx` (blocks of `0x180`–`0x500`), params/matrices
  at `0x172F2FF0`, a `0x2000` block from RSX memory `0x05800C000`, and `0x2000` from `0x1359A80`.
- Outputs: PUT back to the **same** `0x0173xxxxx` EAs (in-place), plus `0x011B00000` (`0x2400`), `0x146A800`
  (`0x700`), and RSX memory `0x05800C000` (`0x2000`).

## The sync problem (and how to test it)

The walker must know the output vertex buffer location/count before it can build the RSX command stream. If the
compute dispatch is asynchronous, the walker needs a barrier/fence before building the draw. The research's
`ttmx/rpcs3-infamous2` rejected vertex-copy offload because small batches + barriers exceeded the SPU time.

Mitigations to evaluate in the spike:

1. **Frame pipelining**: compute frame N's vertices while the walker builds frame N−1 (the existing 3-set
   `VK_FRAME_PIPELINE` machinery is the natural place).
2. **Per-job fence** on a dedicated compute queue, with the walker's draw build deferred until the fence signals.
3. **Coarse batching**: one compute dispatch per mesh/job rather than per draw.

## Feasibility spike (1–2 weeks, go/no-go)

1. Pick one format family from the captured job (attribute table at `0x01243E00`); decode its input streams,
   bone palette/weights, and output packing.
2. Write a Vulkan compute shader (start from `libs/video/vulkan/vtx_unpack.comp`) that reads the captured input
   streams and writes the same packed output. Validate **bit-exactly** against the offline harness
   (`tools/replay/`) before touching the runtime.
3. Hook it for that format only: skip the corresponding SPU math, dispatch the compute pass, keep culling/index/ring
   on the SPU. Measure the fence/sync cost and the fps delta at the pitch.
4. **Go/no-go**: if the sync cost (plus any host copies) exceeds the SPU time removed, stop. If it wins, extend to
   the hot formats, then add frame pipelining.

## Risks

- **Format explosion**: ~40 dispatch variants (`tools/lift_spu.sh`); a generic uber-shader will be slow, per-format
  shaders are a lot of work. Start with the format(s) the pitch screen actually uses.
- **Bit-exactness**: SPU float semantics (`cflts`, `frest`/`frsqest`, `shufb` packing, big-endian) are load-bearing;
  use the replay harness to diff every kernel.
- **Sync/serialization**: see above; this is the make-or-break.
- **Host copies**: the existing GPU vertex path still `memcpy`s raw bytes into a host-coherent ring; the c690 path
  must avoid reintroducing that copy.
- **Ring ordering**: output ring allocation and RSX packet emission stay serial.

## Milestones

- **M0** Format identification + input/output layout for the pitch-screen mesh (from the capture).
- **M1** Compute shader for one format, bit-exact vs the harness.
- **M2** Runtime integration + sync measurement (**go/no-go**).
- **M3** Cover the hot formats.
- **M4** Frame pipelining / walker overlap.

## Prerequisites already in place

- Offline capture/replay harness (`tools/replay/`, `SPU_FN_CAPTURE`) with bit-exact reference checksums.
- Data-flow map (DMA EAs, descriptor, attribute table) from `SPU_JOB_LS_DUMP`.
- Per-function reference checksums: `0x6D68` = `0xfc2e9569dc2feb11`; `0xAF98+B238` = `0x9e6f186e7c711e1c`.
