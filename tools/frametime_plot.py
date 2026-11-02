#!/usr/bin/env python3
"""Frametime-over-time plot and summary for a MangoHud CSV log.

    tools/frametime_plot.py play_20261102_120000/MyGameRecomp_....csv
    tools/frametime_plot.py log.csv -o shot.png --no-plot

Reads MangoHud's CSV format: a system-info line, then a header row that
contains `fps` and `frametime`, then one row per sample. `frametime` is in
milliseconds. The time axis comes from the `elapsed`/`time` column when it is
usable, otherwise from the cumulative frametime.

Prints average fps, 1 % and 0.1 % low fps (1000 / mean of the slowest
frametimes), p50/p99 frametime and the number of spikes over 50 ms. Writes a
PNG with the frametime line and the 16.7 ms (60 fps) and 33.3 ms (30 fps)
guides. It only needs Pillow; without it the text summary is still printed.

Standalone on purpose: automated windowed A/B runs use it on their CSV too.
"""
import argparse
import csv
import math
import os
import sys


def _f(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def read_mangohud_csv(path):
    with open(path, "r", errors="replace", newline="") as f:
        rows = list(csv.reader(f))
    header = None
    start = None
    for i, row in enumerate(rows):
        low = [c.strip().lower() for c in row]
        if "fps" in low and "frametime" in low:
            header, start = low, i + 1
            break
    if header is None:
        raise SystemExit(f"{path}: no MangoHud header row with fps/frametime")

    col = {name: header.index(name) for name in header}
    tcol = None
    for name in ("elapsed", "time"):
        if name in col:
            tcol = col[name]
            break

    fps, ft, times = [], [], []
    for row in rows[start:]:
        if len(row) < 2:
            continue
        def cell(name):
            i = col.get(name)
            return row[i] if i is not None and i < len(row) else ""
        a, b = _f(cell("fps")), _f(cell("frametime"))
        if a is not None and b is not None and b > 0:
            fv = a if a > 0 else 1000.0 / b
            fps.append(fv)
            ft.append(b)
        elif b is not None and b > 0:
            ft.append(b)
            fps.append(1000.0 / b)
        elif a is not None and a > 0:
            fps.append(a)
            ft.append(1000.0 / a)
        else:
            continue
        if tcol is not None and tcol < len(row):
            t = _f(row[tcol])
            times.append(t if t is not None else None)
        else:
            times.append(None)

    if not ft:
        raise SystemExit(f"{path}: no numeric frametime rows")
    return fps, ft, times


def choose_time(ft, times):
    """Return (time_seconds, label)."""
    n = len(ft)
    cum = []
    acc = 0.0
    for x in ft:
        acc += x / 1000.0
        cum.append(acc)
    if any(t is None for t in times):
        return cum, "cumulative frametime"

    span_t = times[-1] - times[0]
    span_c = cum[-1] - cum[0] if n > 1 else 0.0
    if span_t <= 0 or span_c <= 0:
        return cum, "cumulative frametime"

    # Decide whether the column is seconds or milliseconds: compare one step
    # with the frametime in milliseconds.
    steps = [times[i] - times[i - 1] for i in range(1, n) if times[i] > times[i - 1]]
    if steps:
        med_step = sorted(steps)[len(steps) // 2]
        med_ft = sorted(ft)[n // 2]
        as_seconds = abs(med_step * 1000.0 - med_ft) <= abs(med_step - med_ft)
        scale = 1.0 if as_seconds else 0.001
        t = [(x - times[0]) * scale for x in times]
        ratio = (t[-1] - t[0]) / span_c if span_c else 0.0
        if 0.2 <= ratio <= 5.0:
            return t, "elapsed column (s)" if as_seconds else "elapsed column (ms)"
    return cum, "cumulative frametime"


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * p
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def stats(fps, ft, t):
    fts = sorted(ft)
    n = len(ft)
    mean_ft = sum(ft) / n
    avg_fps = sum(fps) / n
    k1 = max(1, int(math.ceil(n * 0.01)))
    k01 = max(1, int(math.ceil(n * 0.001)))
    low1 = 1000.0 / (sum(fts[-k1:]) / k1)
    low01 = 1000.0 / (sum(fts[-k01:]) / k01)
    spikes = sum(1 for x in ft if x > 50.0)
    duration = t[-1] - t[0] if n > 1 else 0.0
    return {
        "n": n,
        "duration": duration,
        "avg_fps": avg_fps,
        "mean_ft": mean_ft,
        "low1": low1,
        "low01": low01,
        "p50": percentile(fts, 0.50),
        "p99": percentile(fts, 0.99),
        "max": fts[-1],
        "min": fts[0],
        "spikes": spikes,
    }


def fmt_summary(path, s, axis_label):
    lines = [
        f"file: {path}",
        f"samples: {s['n']}  duration: {s['duration']:.2f} s  axis: {axis_label}",
        f"avg fps: {s['avg_fps']:.2f}   mean frametime: {s['mean_ft']:.2f} ms",
        f"1% low: {s['low1']:.2f} fps   0.1% low: {s['low01']:.2f} fps",
        f"frametime p50: {s['p50']:.2f} ms  p99: {s['p99']:.2f} ms  "
        f"min/max: {s['min']:.2f}/{s['max']:.2f} ms",
        f"spikes >50 ms: {s['spikes']}"
        + (f" ({s['spikes'] * 60.0 / s['duration']:.1f}/min)" if s['duration'] > 0 else ""),
    ]
    return "\n".join(lines)


def render_png(path, t, ft, s):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("frametime_plot: Pillow not installed; PNG not written", file=sys.stderr)
        return None

    W, H = 1400, 650
    ml, mr, mt, mb = 78, 30, 58, 64
    img = Image.new("RGB", (W, H), (250, 250, 250))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 15)
        small = ImageFont.truetype("DejaVuSans.ttf", 13)
        title_font = ImageFont.truetype("DejaVuSans-Bold.ttf", 19)
    except OSError:
        font = small = title_font = ImageFont.load_default()

    x0, x1 = t[0] if t else 0.0, t[-1] if t else 1.0
    if x1 <= x0:
        x1 = x0 + 1.0
    # Keep the y range readable: clip at p99 * 1.5 (but at least 40 ms), and
    # report the clip only in the plot caption.
    ymax = max(40.0, min(max(ft), s["p99"] * 1.5))
    ymin = 0.0
    px = lambda x: ml + (x - x0) / (x1 - x0) * (W - ml - mr)
    py = lambda y: H - mb - (y - ymin) / (ymax - ymin) * (H - mt - mb)

    # grid and ticks
    for i in range(7):
        x = x0 + (x1 - x0) * i / 6.0
        d.line([(px(x), mt), (px(x), H - mb)], fill=(225, 225, 225))
        d.text((px(x) - 14, H - mb + 8), f"{x:.1f}s", fill=(60, 60, 60), font=small)
    for i in range(6):
        y = ymin + (ymax - ymin) * i / 5.0
        d.line([(ml, py(y)), (W - mr, py(y))], fill=(225, 225, 225))
        d.text((6, py(y) - 8), f"{y:.1f}", fill=(60, 60, 60), font=small)
    d.text((ml, H - 26), "time (s)", fill=(40, 40, 40), font=font)
    d.text((6, mt - 26), "ms", fill=(40, 40, 40), font=font)

    for level, color, label in (
        (16.7, (40, 110, 220), "16.7 ms (60 fps)"),
        (33.3, (210, 60, 60), "33.3 ms (30 fps)"),
    ):
        if ymin <= level <= ymax:
            y = py(level)
            for x in range(ml, W - mr, 12):
                d.line([(x, y), (min(x + 6, W - mr), y)], fill=color, width=2)
            d.text((W - mr - 150, y - 18), label, fill=color, font=small)

    # One point per x bucket (the bucket maximum keeps spikes visible).
    n = len(ft)
    buckets = 2200
    if n <= buckets:
        xs = [(px(t[i]), py(ft[i])) for i in range(n)]
    else:
        xs = []
        span = x1 - x0
        for b in range(buckets):
            a = int(b * n / buckets)
            z = int((b + 1) * n / buckets)
            if z <= a:
                continue
            j = max(range(a, z), key=lambda i: ft[i])
            xv = x0 + span * (b + 0.5) / buckets
            xs.append((px(xv), py(min(ft[j], ymax))))
    if len(xs) > 1:
        d.line(xs, fill=(20, 130, 90), width=2)

    d.text((ml, 14), f"Frametime — {os.path.basename(path)}", fill=(20, 20, 20), font=title_font)
    d.text((ml, 36),
           f"avg {s['avg_fps']:.1f} fps | 1% low {s['low1']:.1f} | 0.1% low {s['low01']:.1f} "
           f"| p50 {s['p50']:.1f} ms | p99 {s['p99']:.1f} ms | >50 ms {s['spikes']}",
           fill=(70, 70, 70), font=small)
    if max(ft) > ymax:
        d.text((W - mr - 220, H - 26), f"y clipped at {ymax:.0f} ms (max {max(ft):.1f})",
               fill=(120, 120, 120), font=small)

    img.save(path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", help="MangoHud CSV log")
    ap.add_argument("-o", "--output", help="PNG path (default: <csv>.png)")
    ap.add_argument("--no-plot", action="store_true", help="text summary only")
    args = ap.parse_args()

    fps, ft, times = read_mangohud_csv(args.csv)
    t, axis = choose_time(ft, times)
    s = stats(fps, ft, t)
    print(fmt_summary(args.csv, s, axis))

    if not args.no_plot:
        out = args.output or (os.path.splitext(args.csv)[0] + ".png")
        written = render_png(out, t, ft, s)
        if written:
            print(f"plot: {written}")


if __name__ == "__main__":
    main()
