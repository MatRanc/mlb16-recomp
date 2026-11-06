#!/usr/bin/env python3
"""Contact sheet of the ab_live scene thumbnails with the A/B bands.

    scene_sheet.py OUTDIR MARKS OUTPNG

Thumbnails are `scene_<epoch-ms>.png` written by ab_live.sh every 2 s. Marks
are the script's `arm guest_ms wall_ms` lines; a thumbnail belongs to the arm
of the last mark at or before its wall time. The border and label show that
arm, so the sheet is evidence that the scene stayed on the meter for the whole
measurement and that no arm left it.
"""
import glob
import os
import re
import sys
from PIL import Image, ImageDraw

TILE_W, TILE_H = 240, 135
COLS = 6
PAD = 8
HEADER = 34
LABEL = 20

COLORS = {"A": (40, 180, 70), "B": (50, 130, 255),
          "pre": (130, 130, 130), "post": (200, 140, 40)}


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    outdir, marks_path, outpng = sys.argv[1:4]

    shots = []
    for p in glob.glob(os.path.join(outdir, "scene_*.png")):
        m = re.search(r"scene_(\d+)\.png$", p)
        if m:
            shots.append((int(m.group(1)), p))
    if not shots:
        print(f"scene_sheet: no scene_*.png in {outdir}", file=sys.stderr)
        return 1
    shots.sort()

    marks = []
    if os.path.exists(marks_path):
        for line in open(marks_path, errors="replace"):
            parts = line.split()
            if len(parts) >= 3 and parts[0] in ("A", "B"):
                try:
                    marks.append((parts[0], int(parts[2])))
                except ValueError:
                    pass
    marks.sort(key=lambda x: x[1])
    # Only the measurement window matters: keep the settle shots just before
    # the first switch and everything after it, not boot/nav.
    if marks:
        keep_from = marks[0][1] - 30000
        shots = [s for s in shots if s[0] >= keep_from]

    def arm_at(t):
        arm = "pre"
        for a, w in marks:
            if w <= t:
                arm = a
            else:
                break
        return arm

    t0 = shots[0][0]
    last_mark = marks[-1][1] if marks else None
    tiles = []
    for t, p in shots:
        arm = arm_at(t)
        if last_mark is not None and t > last_mark + 3000:
            arm = "post"
        try:
            im = Image.open(p).convert("RGB").resize((TILE_W, TILE_H))
        except Exception:
            continue
        tiles.append((arm, (t - t0) / 1000.0, im))

    rows = (len(tiles) + COLS - 1) // COLS
    W = COLS * (TILE_W + PAD) + PAD
    H = HEADER + rows * (TILE_H + LABEL + PAD) + PAD
    sheet = Image.new("RGB", (W, H), (18, 18, 18))
    d = ImageDraw.Draw(sheet)
    d.text((PAD, 10), f"{os.path.basename(outdir)}  ({len(tiles)} frames, "
                      f"{len(marks)} switches)", fill=(230, 230, 230))
    for i, (arm, rel, im) in enumerate(tiles):
        c, r = i % COLS, i // COLS
        x = PAD + c * (TILE_W + PAD)
        y = HEADER + r * (TILE_H + LABEL + PAD)
        col = COLORS.get(arm, COLORS["pre"])
        d.rectangle([x - 2, y - 2, x + TILE_W + 1, y + TILE_H + 1],
                    outline=col, width=3)
        sheet.paste(im, (x, y))
        d.text((x, y + TILE_H + 3), f"{arm}  t=+{rel:6.1f}s", fill=col)
    sheet.save(outpng)
    print(f"scene_sheet: {outpng} ({len(tiles)} frames, {len(marks)} marks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
