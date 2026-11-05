#!/usr/bin/env python3
"""Make the team-select / intro Bink sprite wait for its first decoded frame.

UI_Movie_GetTextureIfReady (0x00981488) shows the movie planes after 4 update
ticks (+0x1d0) without checking the decoded-frame count (+0x110). UI_Movie_Open
clears the planes to 0 (green in YUV) and resets both counters, so when the
first decode takes longer than 3 game frames the cleared planes are drawn.
With PS3RECOMP_BINK_WAIT_DECODE=1 the getter also returns 0 while +0x110 == 0.
See mlb16-recomp/docs/bink-video.md.

Usage: patch_bink_ready.py <recompiled dir>   (idempotent)
"""
import pathlib
import sys

ANCHOR = "loc_009814C0:\n"
TAG = "/* BINK_WAIT_DECODE */"
INSERT = (
    ANCHOR
    + "        " + TAG + " { static const int on = [] { const char* e = getenv(\"PS3RECOMP_BINK_WAIT_DECODE\"); return e && *e && *e != '0'; }();\n"
    + "          static const int lg = [] { const char* e = getenv(\"PS3RECOMP_BINK_LOG\"); return e && *e && *e != '0'; }();\n"
    + "          if (on || lg) { static unsigned n = 0, lo = 0, k40 = 0; static uint32_t lc = 0, ld = ~0u;\n"
    + "            const uint32_t c = (uint32_t)ctx->gpr[3], d = vm_read32(c + 0x110), t = vm_read32(c + 0x1D0);\n"
    + "            if (lo != g_bink_open_seq) { lo = g_bink_open_seq; k40 = 0; }\n"
    # Log every call for 40 calls after each UI_Movie_Open, then only on ctx/decode-count changes.
    + "            if ((c != lc || d != ld || k40++ < 40) && n++ < 4000) {\n"
    # Bink frame-buffer desc at +0x128: +4/+8 Y w,h, +0xC/+0x10 chroma w,h; per set (stride 0x30) from +0x18:
    # {present, ptr, pitch} for Y, Cr, Cb, A. Sample 64 bytes down the middle column of each plane.
    + "              const uint32_t fb = vm_read32(c + 0x128), s = d & 1; char pl[4][48];\n"
    + "              for (int k = 0; k < 4; k++) { const uint32_t e = fb + 0x18 + s * 0x30 + k * 0xC;\n"
    + "                const uint32_t has = vm_read32(e), ptr = vm_read32(e + 4), pitch = vm_read32(e + 8);\n"
    + "                const uint32_t w = vm_read32(fb + (k == 0 || k == 3 ? 4 : 0xC)), h = vm_read32(fb + (k == 0 || k == 3 ? 8 : 0x10));\n"
    + "                unsigned z = 0, sum = 0; if (has && ptr && h) for (unsigned r = 0; r < 64; r++) { const uint8_t v = vm_read8(ptr + (r * h / 64) * pitch + w / 2); z += !v; sum += v; }\n"
    + "                snprintf(pl[k], sizeof pl[k], \"%s@%08X:z%u/m%u\", has ? \"\" : \"-\", ptr, z, sum / 64); }\n"
    + "              fprintf(stderr, \"[bink-ready] ms=%llu flip=%u open=%u ctx=0x%08X tick=%u dec=%u set=%u st=%u Y=%s Cr=%s Cb=%s A=%s\\n\", ps3_ms_now(), cellGcm_flip_request_count(), g_bink_open_seq, c, t, d, s, vm_read32(c + 0x1C8), pl[0], pl[1], pl[2], pl[3]);\n"
    + "              lc = c; ld = d; }\n"
    + "            if (on && d == 0) { ctx->gpr[3] = 0; goto loc_00981570; } } }\n"
)


# UI_Movie_Open (0x0097fff0) clears the reused planes before the first decode.
# Our walker reads textures later than the RSX would, so a draw issued before
# the clear can sample the zeroed planes (green flash). With
# PS3RECOMP_BINK_KEEP_PLANES=1 the clear is skipped on the reuse path (+0x1D7),
# where the planes still hold the previous movie's last frame.
KEEP_TAG = "/* BINK_KEEP_PLANES */"
DECL_TAG = "/* BINK_LOG_DECL */"
DECL = (DECL_TAG + " extern \"C\" unsigned long long ps3_ms_now(void); extern \"C\" uint32_t cellGcm_flip_request_count(void);"
        " extern \"C\" unsigned g_bink_open_seq;\n")
OPEN_HEAD = "void func_0097FFF0(ppu_context* ctx) {\n    PPU_MM_SNAP\n"
OPEN_TAG = "/* BINK_LOG_OPEN */"
OPEN_LOG = ("        " + OPEN_TAG + " { g_bink_open_seq++; static const int lg = [] { const char* e = getenv(\"PS3RECOMP_BINK_LOG\"); return e && *e && *e != '0'; }();"
            " static unsigned n; if (lg && n++ < 2000) fprintf(stderr, \"[bink-open] ms=%llu flip=%u open=%u ctx=0x%08X reuse=%u\\n\","
            " ps3_ms_now(), cellGcm_flip_request_count(), g_bink_open_seq, (uint32_t)ctx->gpr[3], (unsigned)vm_read8((uint32_t)ctx->gpr[3] + 0x1D7)); }\n")


def add_decl(s: str, head: str) -> str:
    if DECL_TAG in s:
        return s
    i = s.index(head)
    return s[:i] + DECL + s[i:]

KEEP_LRS = ("0x0098078C", "0x00980810", "0x00980894", "0x00980920")


def patch_keep(root: pathlib.Path) -> None:
    for f in sorted(root.glob("ppu_recomp_*.cpp")):
        s = f.read_text()
        if "void func_0097FFF0(ppu_context* ctx) {" not in s:
            continue
        if KEEP_TAG in s:
            print(f"{f.name}: keep-planes already patched")
            return
        for lr in KEEP_LRS:
            call = f"ctx->lr = {lr}; func_004DB4D4(ctx); DRAIN_TRAMPOLINE(ctx);"
            assert s.count(call) == 1, f"{f.name}: {lr} count {s.count(call)}"
            s = s.replace(call, KEEP_TAG + " { static const int keep = [] { const char* e = getenv(\"PS3RECOMP_BINK_KEEP_PLANES\"); return e && *e && *e != '0'; }();"
                          " if (!(keep && vm_read8(ctx->gpr[27] + 0x1D7))) { " + call + " } }")
        f.write_text(s)
        print(f"{f.name}: keep-planes patched")
        return
    raise SystemExit("func_0097FFF0 not found")


def patch_open(root: pathlib.Path) -> None:
    for f in sorted(root.glob("ppu_recomp_*.cpp")):
        s = f.read_text()
        if OPEN_HEAD not in s:
            continue
        if OPEN_TAG in s:
            print(f"{f.name}: open-log already patched")
            return
        s = add_decl(s, OPEN_HEAD)
        s = s.replace(DECL, DECL + "unsigned g_bink_open_seq = 0;\n", 1)  # defined next to UI_Movie_Open
        assert s.count(OPEN_HEAD) == 1
        f.write_text(s.replace(OPEN_HEAD, OPEN_HEAD + OPEN_LOG))
        print(f"{f.name}: open-log patched")
        return
    raise SystemExit("func_0097FFF0 not found")


# UI_Movie_Close (0x0097fd18) clears all eight planes right after stopping the
# decoder, with no GPU wait (the wait comes after, before Asset_Release). The RSX
# has long sampled them by then; our walker runs ~1 frame behind and samples the
# cleared planes: the team-select green flash. The planes are released (or
# reopened, which clears them again) right after, so with
# PS3RECOMP_BINK_CLOSE_KEEP=1 (default on) the close-path clear is skipped.
CLOSE_TAG = "/* BINK_CLOSE_KEEP */"
CLOSE_CALL = "ctx->lr = 0x0097FDDC; func_004DB4D4(ctx); DRAIN_TRAMPOLINE(ctx);"


def patch_close(root: pathlib.Path) -> None:
    for f in sorted(root.glob("ppu_recomp_*.cpp")):
        s = f.read_text()
        if "void func_0097FD18(ppu_context* ctx) {" not in s:
            continue
        if CLOSE_TAG in s:
            print(f"{f.name}: close-keep already patched")
            return
        assert s.count(CLOSE_CALL) == 1, f"{f.name}: close clear count {s.count(CLOSE_CALL)}"
        f.write_text(s.replace(CLOSE_CALL, CLOSE_TAG + " { static const int keep = [] { const char* e = getenv(\"PS3RECOMP_BINK_CLOSE_KEEP\"); return !e || *e != '0'; }();"
                               " if (!keep) { " + CLOSE_CALL + " } }"))
        print(f"{f.name}: close-keep patched")
        return
    raise SystemExit("func_0097FD18 not found")


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    patch_keep(root)
    patch_open(root)
    patch_close(root)
    for f in sorted(root.glob("ppu_recomp_*.cpp")):
        s = f.read_text()
        if "void func_00981488(ppu_context* ctx) {" not in s:
            continue
        s = add_decl(s, "void func_00981488(ppu_context* ctx) {")
        if TAG in s:
            i = s.index(ANCHOR)
            j = s.index("\n", s.index("} }\n", s.index(TAG))) + 1
            if s[i:j] == INSERT:
                print(f"{f.name}: already patched")
                return 0
            s = s[:i] + ANCHOR + s[j:]
        assert s.count(ANCHOR) == 1, f"{f.name}: anchor count {s.count(ANCHOR)}"
        f.write_text(s.replace(ANCHOR, INSERT))
        print(f"{f.name}: patched")
        return 0
    print("func_00981488 not found", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
