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
| 0x2A assembler (0x10F78), **36-byte record class** | GPU kernel `tools/replay/c690_2a_assembler.comp` / `c690_2a_batch.comp`; plumbing harness `test_c690_2a_plumbing.c` | offline only, not wired | exact | exact | n/a | 2 real captures + synthetic colour stress, 0 differing bytes; ring bytes and replay hash match (edge-gpu-plumbing.md); **0/128 meter 0x2A pairs are this class** | edge-gpu-phase2a.md, -phase2b-step1.md |
| 0x2A assembler (0x10F78), **position-first 32-byte record class** (pos f32x3, normal 11:11:10, 4x s16, 2x f16, 4-byte SPU index pair) | no GPU kernel; layout and pack rules decoded offline (edge-gpu-2a-wiring.md §3) | none (not wired) | exact copy by construction (all sampled records) | pack rules proven per word (`trunc(v*scale)-1`); whole-pair gather/index contract not yet verified | n/a | layout probes on 2 real meter pairs; **55/128 meter 0x2A pairs** (43.0 %) | edge-gpu-2a-wiring.md |
| 0x2A assembler (0x10F78), **20-byte / 48-byte / other classes** | no kernel | none | - | - | n/a | stride classifier on 128 meter pairs: 12 / 16 / 45 pairs; fallback with 36+32 kernels = 57.0 % of 0x2A (> 5 % gate) | edge-gpu-2a-wiring.md §2 |
| 0x21 sel1 A730 leaf (0xB830) | GPU kernel `tools/replay/c690_b830_batch.comp` | offline only, not wired | exact | exact | n/a | 64/64 meter pairs, 0 ULP, 9 live GPRs exact | edge-gpu-phase2b-step4.md |
| 0x22 tree/cull subtree (0xFDA0) | CPU native | `PS3RECOMP_C690_NATIVE_CULL` (off) | exact | exact | exact | replay corpus, live A/B | edge-native-step3/4/5.md |
| 0x0A unpack/pack leaves (0x6D68, 0x7C18, 0x8160) | CPU native | per edge-native docs (off) | exact | exact | n/a | replay corpus | edge-native-step*.md |
| 0x25/0x26 screen cull (0xD080) | CPU native | `PS3RECOMP_C690_NATIVE_SCREEN` (off) | exact | exact | exact | replay corpus | edge-native-step4/5.md |
| 0x25/0x26 cull sample grid | lifted, patched | `MLB16_CULL_BYPASS` (0); =3 bypasses 0x25/0x26 | n/a | n/a | **not exact by design** (=3 keeps all; fixes eye holes, -7.6 fps) | user play test | edge-holes-fix.md, cull-bypass-ab.md |
| **0x25/0x26 cull sample grid, fused native pass** | CPU native, one 0x132E0 body pass (game grid on the real LS, sample grid on a private copy; caller count modelled) | `PS3RECOMP_C690_HOLES_NATIVE` (off) | exact | exact | **conservative**: 0 removed/reordered vs SPU on 128 pairs; 0 added on that corpus; full LS/GPR bit-identical to the lifted union; `+0x114` count/list consistent | 128-pair replay corpus, offline only (no live run) | edge-e1-holes.md |
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

**Parked:** GPU 0x2A record classes (36-byte exact offline; the 32-byte
position-first class is 55/128 meter pairs and the rest of the family is not
decoded), GPU 0x21 sel7 / 0x25/0x26 cull, the 0x22 whole-body native body, and
the extra host lanes (`wip/c690-extra-lanes`, unfinished; `SPURS_LANE6` wedges
in ~10 s). They are parked because each is a large mission, not because they
are finished. The meter p50 measurement now says these are the remaining p50
levers: the median frame is the c690 producer (c690 union p50 32.01 ms, last
job 32.97 ms in a 34.15 ms frame), and the flip deadline / 17 ms wait is not
the limiter ([`meter-p50-pin.md`](meter-p50-pin.md)). A ~2x cut of 0x22 + 0x21
+ 0x2A is ~5-7 ms of burst, i.e. p50 ~28-29 ms.
