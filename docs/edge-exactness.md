# Edge (c690) exactness ledger

What every non-lifted Edge path produces compared with the lifted SPU code. **If a mesh looks wrong (cracks, sparkles,
shimmer, missing or extra triangles, odd lighting), start here:** find the stages that are not exact, turn their switch
off, and see if the artifact goes away.

Every mission that adds or changes a GPU or native Edge stage must update this table in the same commit.

## Policy (user-approved 2026-11-04)

| Output | Rule | Why |
|---|---|---|
| Vertex positions | **Exact** (bit-identical to the lifted SPU) | GPU and SPU-fallback batches meet at shared edges; any position drift shows as cracks/sparkles and depth shimmer (decals, logos, shadows). |
| Normals, colours, UVs | **<= 1 LSB** on packed values | A 1-LSB lighting/colour change is invisible. Most porting effort is saved here. |
| Cull (kept-index lists, counts) | **Conservative only**: may keep extra triangles, must never drop one the SPU keeps | An extra triangle costs a little fill; a dropped one is a hole (cf. the eye holes). |
| Command hole / ring tickets / labels | **Exact**, always produced by the SPU path | The walker parks on the hole; any difference is a hang or a wrong draw. |

Gameplay is not at risk from any of these: no CPU code reads Edge output (edge-gpu-phase1.md §1), only the walker.

## Per-stage status

| Stage (LS) | Path | Switch (default) | Positions | Normals/colour/UV | Cull | Verified on | Doc |
|---|---|---|---|---|---|---|---|
| 0x2A assembler (0x10F78), **36-byte record class** | GPU kernel `tools/replay/c690_2a_assembler.comp` / `c690_2a_batch.comp`; plumbing harness `test_c690_2a_plumbing.c` | offline only, not wired | exact | exact | n/a | 2 real captures + synthetic colour stress, 0 differing bytes; ring bytes and replay hash match (edge-gpu-plumbing.md); **0/128 old phase-2b meter pairs; 1/192 regenerated meter pairs (0.52 %)** | edge-gpu-phase2a.md, -phase2b-step1.md |
| 0x2A assembler (0x10F78), **position-first 32-byte record class** (pos f32x3, normal 11:11:10, 4x s16, 2x f16, 4-byte SPU index pair) | GPU kernel `tools/replay/c690_2a_32b.comp`; gate `test_c690_2a_32b.c/.py`; the generic class gate `c690_2a_32b_pos.comp` also gates the 9 sub-variant pairs | offline only, not wired | **exact** (0 differing position bytes) | **<= 1 LSB** (normal/tangent/UV max 1; SPU attr +28..31 exact) | n/a | **60/60 position-first pairs, 22,296 records** on the regenerated meter spool (`fncap2b2w3`) via the class gate; the 51-pair `c690_2a_32b` gate is unchanged (20,312 records); 9 sub-variant pairs now gated with an exported u32 entry index; **60/192 meter 0x2A pairs** (31.2 %) | edge-gpu-2a-32b.md, edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **32-byte GPU ring binding, asynchronous hand-off** (live key; SPU export + Vulkan compute on a dedicated queue + walker resolver) | `PS3RECOMP_C690_GPU_2A` (default **0**); `runtime/spu/c690_gpu_2a.c`, `libs/video/rsx_vulkan_backend.c`, resolver in `rsx_draw_engine.c`; gate `tools/replay/test_c690_2a_binding.py` + `test_c690_2a_compact.c` + `tools/c690_vk_timeline_validate.c` | **off (default 0)** | **exact** (the CPU resolver takes the exact SPU seed until the timeline value completes, then the GPU bytes; positions exact either way) | **<= 1 LSB** for the GPU bytes; the seed fallback is bit-exact | n/a | offline: resolver transparency MATCH (`44f9...`), GPU bytes policy-exact, compact packing A/B PASS, 5-lane harness ASan/UBSan+TSan clean, replay hash unchanged, offline validation arm 200 rounds **0 errors**; **live pitch-meter A/B (2 launches):** run 1 waited on the compute and collapsed (p50 74.1 ms); run 2 (dedicated queue + timeline, no CPU wait) has **submit 0.061 ms/frame, wait 0**, but p90/p99 still worse (**46.4/50.9 vs 38.8/42.6 ms**) from compute-queue scheduling (~1.9 ms per submit); p50 34.65 vs 34.15, fps 27.6 vs 37.1. Output clean (thumbnails). Default stays off | edge-gpu-binding.md §8 |
| 0x2A assembler (0x10F78), **32-byte index-pre-pass contract** | SPU keeps the pre-pass; exports the u16 byte-offset list (`cmd[0]`, or identity `k*16`) and the 4-byte attribute word | none (offline) | exact | n/a | n/a | 15 format-A + 36 format-B pairs; all 20,312 records gather all four arrays at the same offset | edge-gpu-2a-32b.md §3 |
| 0x2A assembler (0x10F78), **position-only `pos` class** (12-byte stream; the 48-byte family is four consecutive positions) | GPU kernel `tools/replay/c690_2a_pos.comp` (shared `.inc`); gate `test_c690_2a_classes.c/.py` | offline only, not wired | exact | n/a | n/a | **17/17 pairs, 7,388 records**, 0 differing position bytes, 0 outside writes (`fncap2b2w3`) | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **20-byte class** (pos + f16 uv + 2 x s16 1/8192) | GPU kernel `tools/replay/c690_2a_20b.comp` | offline only, not wired | exact | <= 1 LSB | n/a | **17/17 pairs, 5,004 records** | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **28-byte position-first** (pos + normal + 4 x s16 tan + f16 uv) | GPU kernel `tools/replay/c690_2a_28b_pos.comp` | offline only, not wired | exact | <= 1 LSB | n/a | **5/5 pairs, 700 records** | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **32-byte attribute-first** (SPU prefix 8B + 3 x normal + pos@20) | GPU kernel `tools/replay/c690_2a_32b_attr.comp` | offline only, not wired | exact | <= 1 LSB; prefix 0 differing | n/a | **9/9 pairs, 3,412 records** | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **28-byte attribute-first** (SPU prefix 4B + 3 x normal + pos@16) | GPU kernel `tools/replay/c690_2a_28b_attr.comp` | offline only, not wired | exact | <= 1 LSB; prefix 0 differing | n/a | **10/10 pairs, 4,120 records** | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **44-byte class** (pos + normal + 2 x s16 1/1024 + 4 x s16 tan + 2 x s16 1/16384 + 2 x 3 x s16 1/1024) | GPU kernel `tools/replay/c690_2a_44b.comp` | offline only, not wired | exact | <= 1 LSB | n/a | **13/13 pairs, 2,520 records** | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **all seven classes combined** | GPU kernels above, one shared `c690_2a_records.inc`; gate `test_c690_2a_classes.c/.py` | offline only, not wired | exact | <= 1 LSB | n/a | **131/131 classified pairs, 45,440 records**, max 1 LSB, 0 writes outside records; **coverage 131/192 = 68.2 %** of meter 0x2A pairs; fallback 61/192 = 31.8 % (31 first-in-job runtime-path, 19 no-position outputs, 11 rare 64/40/16/4/112-byte layouts) | edge-gpu-2a-classes.md |
| 0x2A assembler (0x10F78), **native field pack** (five position-first classes; the four field passes `0x7840`/`0x7FC0`/`0x7AC0`/`0x7540`; the driver/pre-pass stays lifted) | CPU native entry hooks, `runtime/spu/c690_native_2a.c` + `tools/c690_ref/patch_native_2a.py` | `PS3RECOMP_C690_NATIVE_2A` (default **0**) | **exact** | **<= 1 LSB** (SPU attr +28..31 untouched) | n/a | **112/131 classified pairs (85.5 %; 58.3 % of the 192 sampled)**, positions 0 differing bytes, packed max 1 LSB, 0 writes outside the field; 19 attribute-first pairs fall back byte-identical; r0/r1/r80..r127 exact (only caller-saved r2..r79 scratch differs); live A/B neutral: 0x2A 21.92 -> 21.59 us/call, p50 34.15 both | edge-native-2a.md |
| 0x21 sel1 A730 leaf (0xB830) | GPU kernel `tools/replay/c690_b830_batch.comp` | offline only, not wired | exact | exact | n/a | 64/64 meter pairs, 0 ULP, 9 live GPRs exact | edge-gpu-phase2b-step4.md |
| 0x22 tree/cull subtree (0xFDA0) | CPU native | `PS3RECOMP_C690_NATIVE_CULL` (off) | exact | exact | exact | replay corpus, live A/B | edge-native-step3/4/5.md |
| 0x0A unpack/pack leaves (0x6D68, 0x7C18, 0x8160) | CPU native | per edge-native docs (off) | exact | exact | n/a | replay corpus | edge-native-step*.md |
| 0x25/0x26 screen cull (0xD080) | CPU native | `PS3RECOMP_C690_NATIVE_SCREEN` (off) | exact | exact | exact | replay corpus | edge-native-step4/5.md |
| 0x25/0x26 cull sample grid | lifted, patched | `MLB16_CULL_BYPASS` (0); =3 bypasses 0x25/0x26 | n/a | n/a | **not exact by design** (=3 keeps all; fixes eye holes, -7.6 fps) | user play test | edge-holes-fix.md, cull-bypass-ab.md |
| **0x25/0x26 cull sample grid, fused native pass** | CPU native, one 0x132E0 body pass (game grid on the real LS, sample grid on a private copy; caller count modelled) | `PS3RECOMP_C690_HOLES_NATIVE` (off) | exact | exact | **conservative offline**: 0 removed/reordered vs SPU on 128 pairs; 0 added on that corpus; full LS/GPR bit-identical to the lifted union; `+0x114` count/list consistent. **Live: 121,242 added triangles and giant brown occluder polygons in the pitch view; p90/p99 +2 ms.** Not shippable | 128-pair replay corpus + one live pitch-meter A/B (2026-11-04) | edge-e1-holes.md §5.4 |
| 0x21 sel7 A3E0/BB98, 0x25/0x26 GPU, 0x2A wiring | planned | - | - | - | - | - | edge-gpu-plumbing.md, edge-gpu-2a-wiring.md §4–6 |

Rows marked "not wired" do not run in the game yet. When a stage is wired into the walker, add its live key and change
"Verified on" to include the A/B run.

## Live vs parked (2026-11-04)

**Live (play defaults):** the lifted Edge path plus the fairness-gated native
c690 dispatch (`SPURS_PM_NATIVE_DISPATCH=6`) and `SPURS_PM_FAST_RESCHED=64`.
These change *when* a lane re-enters the PM, not the job output; the exactness
policy above is unchanged. Meter effect: p90 43.2 -> 39.2 ms, p99 46.7 -> 42.7,
40-50 ms share 24.3 -> 7.1 %, p50 34.15 unchanged
([`c690-native-dispatch.md`](c690-native-dispatch.md)).

**E1 (fused native holes) is merged but not shipped.** `exp/live-ab` now
carries it default off. The one live pitch-meter A/B (2026-11-04) is fps
neutral (-0.33 +/- 1.26 SE) but raises p90/p99 by ~2 ms and, more importantly,
draws giant brown occluder polygons in arm B. The offline 128-pair corpus had
0 added triangles, so the live "adds" path is the open bug. Do not enable the
key until a live capture isolates the bogus sample-only triangles
([`edge-e1-holes.md`](edge-e1-holes.md) §5.4).

**Parked:** GPU 0x2A record classes (36-byte exact offline; the 32-byte
position-first class has an exact kernel on 60/60 meter pairs and a live
binding + resolver. The live crash is fixed: the lane key was `spu_id % 16`
and every c690 job context has `spu_id 0`, so all five lanes shared one export
slot; lane state is now thread-local. The hand-off is now asynchronous
(dedicated compute queue, timeline semaphore, no CPU wait): submit is
0.061 ms/frame but the compute queue's scheduling still costs ~7.6 ms at p90,
so the binding stays default off until the dispatches join the frame's own
command buffer and the draw consumes the GPU bytes. Six more record
layouts are exact offline, 131/192 = 68.2 % of meter 0x2A pairs, while the 31
first-in-job pairs that enter the low-LS SPURS runtime and the 19
attribute/index-only outputs have no kernel). The **native field pack**
(`PS3RECOMP_C690_NATIVE_2A`, default 0) replaces the four field passes
for the five position-first classes: policy-exact on 112/131 classified
pairs and live-neutral (0x2A 21.92 -> 21.59 us/call, p50 34.15 both);
the 32-byte class is 11 % slower with the scalar pack and needs
vectorising. GPU 0x21 sel7 /
0x25/0x26 cull, the 0x22 whole-body native body, and
the extra host lanes (`wip/c690-extra-lanes`, unfinished; `SPURS_LANE6` wedges
in ~10 s). They are parked because each is a large mission, not because they
are finished. The meter p50 measurement now says these are the remaining p50
levers: the median frame is the c690 producer (c690 union p50 32.01 ms, last
job 32.97 ms in a 34.15 ms frame), and the flip deadline / 17 ms wait is not
the limiter ([`meter-p50-pin.md`](meter-p50-pin.md)). A ~2x cut of 0x22 + 0x21
+ 0x2A is ~5-7 ms of burst, i.e. p50 ~28-29 ms.
