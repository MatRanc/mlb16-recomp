# c690 SPU job — function map

Reference for the MLB The Show 16 c690 geometry job (job module `jobmod_c690c2f7b09a`, image 6).
Addresses are **Local Store (LS)** addresses in the lifted module (`spu_recomp.c`); the module is
loaded at LS `0x4000` (job entry `0x135B8`). Line numbers are in
`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`.

Status: **[V]** verified by offline replay / capture, **[L]** lifted but not separately replayed,
**[X]** not in the lifted module (low-LS SPURS runtime, see `tools/spu_disasm.py`).

## Pipeline (pitch job, job_01)

```
packed attribute streams (guest EAs)
   -> unpack       0x6D68 (12/12/11 SNORM direction/tangent; also other widths)
   -> transform    0xA798 (3x4 affine) / 0xA9B0 (transform + orthonormalize)
   -> normalize    0xAF98 -> 0xB238 (frsqest + fi)
   -> pack         0x7C18 (snorm16, cflts 157) / 0x8160 (snorm8, cflts 142)
   -> planar arrays (0x1F6B0 normal, 0x20330 position, 0x20FB0 color, 0x21C30 uv0)
   -> assemble     0x2A (0x10F78): gather into 36-byte interleaved vertices @ 0x2F870
   -> cull/index/ring (separate command families; not in the pitch job's list)
```

Note: the captures are from different job instances, so this is the logical chain, not one byte-exact trace.

## Command handlers (opcode -> handler)

| Opcode | Handler | Role |
|---|---|---|
| 0x04 | `0x11F58` [L] | stream/setup (format/count word) |
| 0x07 | `0x11DF0` [L] | stream end |
| 0x08 | `0x11BF8` [L] | stream end |
| 0x09 | `0x05000` [L] | stream setup |
| 0x0A | `0x12478` [L] -> `0x10B88` [L] | bind stream / convert (snorm16 pack family) |
| 0x0C | `0x12830` [L] -> `0x126D8` [L] | index decompress |
| 0x13 | `0x11A60` [L] | 16-byte copy |
| 0x1F | `0x12948` [L] | finalize / mailbox flush |
| 0x21 | `0x13080` [L] | emit (ring packet) |
| 0x25/0x26 | `0x13520`/`0x13518` [L] -> `0x132E0` [L] | triangle cull |
| 0x27 | `0x11790` [L] | index copy + output sizing |
| 0x2A | `0x10F78` [V] | vertex-format assembler (see below) |
| 0x2C | `0x12DC0` [L] -> `0x12C08` [L] | extra-block cull |
| 0x00 | `0x12AB8` [L] -> `0x4A5C` [L] | end/submit |

Command parser: `0x10700` [L] (`spu_recomp.c:25273`); dispatch tables LS `0x14740` (arg counts) and
`0x14770` (handler pointers).

## Per-vertex kernels

| LS | Line | Status | Role |
|---|---|---|---|
| `0x6D68` | 5372 | [V] | 12/12/11 SNORM unpack: `r = ftz(fma(ftz(0x3F800000+B+(F<<2)), A, C))`; output float4 |
| `0x6B30` | 5225 | [L] | dequant constant setup for `0x6D68` |
| `0x6860` / `0x6838` | 4755 / 4740 | [L] | unpack drivers |
| `0xA730` / `0xA798` | 12583 / 12614 | [L] | 3x4 affine transform (matrix at LS `0x29540`) |
| `0xA9B0` | 13385 | [L] | transform + orthonormalize (normals/tangents) |
| `0xAF98` | 13787 | [V] | normalize: `frsqest` + `fi` refinement |
| `0xB238` | 13961 | [V] | normalize continuation (2 vertices) |
| `0x7C18` | 7612 | [L] | snorm16 pack (`cflts 157`), shuffle into stream |
| `0x8160` | 7935 | [L] | snorm8 pack (`cflts 142`) |
| `0x7AC0` / `0x7B28` / `0x7BD8` | 7511 / 7542 / 7591 | [L] | snorm16 pack dispatch chain |
| `0xD2B8` / `0xD4A8` / `0xC98C` | 17878 / 18007 / 16818 | [L] | skin/transform variants (not used by the pitch format) |

## 0x2A vertex assembler (`0x10F78`) — verified

Selector: `(r4>>20)&7`; for `0x2A00`, selector 0. Reads the descriptor at `ctx+0x2D0` (`0x1F690`),
type table `ctx+0x2A0` (`0x1F2A0`), source bases `ctx+0x260` (`0x1F260`).

| LS | Line | Status | Role |
|---|---|---|---|
| `0x10F78` | 26272 | [V] | assembler entry |
| `0x10C70` | 25860 | [V] | per-stream descriptor parse (x4) |
| `0x109C0` | 25578 | [V] | address/scale builder |
| `0x7840` | 7324 | [V] | gather setup (per stream) |
| `0x78C0` | 7361 | [V] | gather loop head (144-byte stride = 4x36) |
| `0x7960` | 7406 | [V] | gather write |
| `0x7998` | 7425 | [V] | gather write (rest) |
| `0x7D40` | 7624 | [V] | float->u8 color pack setup |
| `0x7D7C`/`0x7DA8`/`0x7E58`/`0x7E98` | 7644/7660/7709/7730 | [V] | color pack chain (`cfltu 149`) |
| `0x49D4` | 871 | [V] | LS allocator (fills `0x23400` table `8+16k`) |
| `0xEA70` | 21131 | [V] | staging (empty-list exit for 0x2A; no DMA) |

Output: 36-byte records at `0x2F870` = `[UV0.xy][COLOR0 u8x4][POSITION.xyz][NORMAL.xyz]`; sources
`0x21C30`/`0x20FB0`/`0x20330`/`0x1F6B0`. Draw count from `r8` (196 buffer, 193 valid).

## Format context / setup

| LS | Line | Status | Role |
|---|---|---|---|
| `0x135B8` | 31211 | [L] | job entry |
| `0x135F0` / `0x1361C` | 31234 / 31284 | [L] | entry subs |
| `0xE750` / `0xE860` | 20628 / 20947 | [L] | format context setup |
| `0xE8E8` / `0xE92C` | 21005 / 21027 | [L] | context setup |
| `0xE6B8` | 20551 | [L] | allocator |
| `0x13528` | 31000 | [L] | mailbox/syscall service |

## Cull / index / ring (stay on SPU)

| LS | Line | Status | Role |
|---|---|---|---|
| `0x132E0` | 30694 | [L] | triangle cull (screen test `0xD080`) |
| `0x12C08` | 29763 | [L] | extra-block cull (0x2C) |
| `0x12830`/`0x126D8` | 29213 / 29053 | [L] | index decompress |
| `0x11790` | 27226 | [L] | index copy + output sizing |
| `0x12948` | 29412 | [L] | finalize |
| `0x13080` | 30392 | [L] | emit/ring packet |
| `0xE0A8` | 19516 | [L] | ring allocation |
| `0x4B6C`/`0x4D80` | 1095 / 1498 | [L] | RSX packet helpers |
| `0x12010` | 28236 | [L] | RSX packet emission |

## Tree / leaf / index tables

| LS | Line | Status | Role |
|---|---|---|---|
| `0xFDA0` | 23881 | [L] | BVH/tree traversal (hot) |
| `0xFCB8`/`0xFCE4` | 23763 / 23828 | [L] | tree/leaf helpers |
| `0x6780`/`0x6860` | 4676 / 4755 | [L] | index/table |

## Low-LS SPURS runtime (not lifted; disassemble with `tools/spu_disasm.py`)

| LS | Role |
|---|---|
| `0x1690` | format/job driver (calls `0x22B0`, `0x1B08`, `0x1AE0`) |
| `0x22B0` | tag/MFC wait |
| `0x1B08` | command parser / loop |
| `0x1AE0` | finalize |

## PPU side (EBOOT.elf, Ghidra)

| Address | Role |
|---|---|
| `FUN_000730c8` | ICE draw builder: emits the 16-bit command stream (`0x1a00`..`0x2e00`) |
| `FUN_00070e14` | per-stream descriptor builder (count/width/stride/offset) |
| `FUN_000624b8` | `cellSpursInitializeWithAttribute(&DAT_01243e00)` — `0x01243E00` is the SPURS instance |

## Open questions

- Co-temporal capture of the producing chain (`0x6D68` -> `0xA798` -> `0xAF98`) in one job.
- The `0x0A` job's `sr9`/`sr10` pack scale/offset (not exercised by the `0x2A` capture).
- The matrix at `0x29540` for the same job as the unpack.
- Exact descriptor field packing at `0x1F690`/type table `0x1F2A0` (only partially decoded).
