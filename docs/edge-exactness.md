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
| 0x2A assembler (0x10F78), **36-byte record class only** (the meter mostly uses a 32-byte class, not covered) | GPU kernel `tools/replay/c690_2a_assembler.comp` / `c690_2a_batch.comp`; plumbing harness `test_c690_2a_plumbing.c` | offline only, not wired | exact | exact | n/a | 2 real captures + synthetic colour stress, 0 differing bytes; ring bytes and replay hash match (edge-gpu-plumbing.md) | edge-gpu-phase2a.md, -phase2b-step1.md |
| 0x21 sel1 A730 leaf (0xB830) | GPU kernel `tools/replay/c690_b830_batch.comp` | offline only, not wired | exact | exact | n/a | 64/64 meter pairs, 0 ULP, 9 live GPRs exact | edge-gpu-phase2b-step4.md |
| 0x22 tree/cull subtree (0xFDA0) | CPU native | `PS3RECOMP_C690_NATIVE_CULL` (off) | exact | exact | exact | replay corpus, live A/B | edge-native-step3/4/5.md |
| 0x0A unpack/pack leaves (0x6D68, 0x7C18, 0x8160) | CPU native | per edge-native docs (off) | exact | exact | n/a | replay corpus | edge-native-step*.md |
| 0x25/0x26 screen cull (0xD080) | CPU native | `PS3RECOMP_C690_NATIVE_SCREEN` (off) | exact | exact | exact | replay corpus | edge-native-step4/5.md |
| 0x25/0x26 cull sample grid | lifted, patched | `MLB16_CULL_BYPASS` (0); =3 bypasses 0x25/0x26 | n/a | n/a | **not exact by design** (=3 keeps all; fixes eye holes, -7.6 fps) | user play test | edge-holes-fix.md, cull-bypass-ab.md |
| 0x21 sel7 A3E0/BB98, 0x25/0x26 GPU, 0x2A wiring | planned | - | - | - | - | - | edge-gpu-plumbing.md |

Rows marked "not wired" do not run in the game yet. When a stage is wired into the walker, add its live key and change
"Verified on" to include the A/B run.
