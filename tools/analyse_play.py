#!/usr/bin/env python3
"""Per-window analysis of a windowed multi-scene play log, with a contact sheet.

    tools/analyse_play.py recomp-work/play_20261102_120000.log
    tools/analyse_play.py play.log --thumbs recomp-work/play_20261102_120000
    tools/analyse_play.py play.log --window 2 --sheet /tmp/sheet.html

Every 2 s window reports:
  * fps from the [flip] lines;
  * c690 jobs/s and per-lane busy % (from the cumulative [c690-stats] periodic
    reports: the wall-sum delta spread over the lanes by job share);
  * the walker's own CPU time ([walker-prof] walker=) against the rest of the
    frame (frame time - walker = wait, the approximation when no
    PS3RECOMP_TRACE timeline is present);
  * the top walker phases;
  * the [RSX vulkan] window present line (shown, dropped, max pump gap).

It also writes an HTML contact sheet that puts each window next to the
nearest thumb_<epoch-ms>.png from the play run, so scenes can be labelled
afterwards. Fields that a log does not contain are shown as "n/a" -- an old
log without c690 or present stats still analyses as far as it can.
"""
import argparse
import datetime as dt
import glob
import html
import math
import os
import re
import sys


TS_RE = re.compile(r"^(\d{2}):(\d{2}):(\d{2})\s+(.*)$")
FLIP_RE = re.compile(r"\[flip\]\s+(\d+)\s+at\s+(\d+)\s+ms")
WPROF_RE = re.compile(
    r"\[walker-prof\] report=(\d+) flips=(\d+) draws=([0-9.]+) "
    r"walker=([0-9.]+)ms engine=([0-9.]+)ms parse=([0-9.]+)ms "
    r"fetch=([0-9.]+) tex=([0-9.]+) pipe=([0-9.]+) bind=([0-9.]+) "
    r"draw=([0-9.]+) other=([0-9.]+)")
C690_RE = re.compile(
    r"\[c690-stats\] (periodic|exit) elapsed=([0-9.]+)s jobs=(\d+) "
    r"jobs/s=([0-9.]+) in-flight=(\d+)")
C690_FRAMES_RE = re.compile(r"\[c690-stats\] frames=(\d+) jobs/frame=([0-9.]+)")
C690_WALL_RE = re.compile(
    r"\[c690-stats\] wall_us p50=(\d+|n/a) p90=(\d+|n/a) "
    r"max=(\d+|n/a)")
C690_SUM_RE = re.compile(r"\[c690-stats\] wall_us .*\(sum=(\d+)\)")
C690_WORKER_RE = re.compile(
    r"\[c690-stats\] worker=(extra|\d+) spu=(\S+) jobs=(\d+)")
PRESENT_RE = re.compile(
    r"\[RSX vulkan\] window: (\d+) pumps, max pump gap (\d+) ms \| "
    r"blits (\d+), shown (\d+), dropped (\d+) no-slot \+ (\d+) not-ready, "
    r"longest vkQueuePresentKHR (\d+) ms")
PLAY_START_RE = re.compile(r"\[play\] start epoch=(\d+)")
PHASES = ("fetch", "tex", "pipe", "bind", "draw", "other")


def second_of_day(h, m, s):
    return int(h) * 3600 + int(m) * 60 + int(s)


def read_start_epoch(path, lines):
    for line in lines:
        m = PLAY_START_RE.search(line)
        if m:
            return int(m.group(1))
    meta = os.path.join(os.path.splitext(path)[0], "meta.txt")
    if os.path.exists(meta):
        for line in open(meta, errors="replace"):
            if line.startswith("PLAY_START_EPOCH="):
                try:
                    return int(line.split("=", 1)[1])
                except ValueError:
                    pass
    # Fall back: the file's mtime is the end of the run.
    first = last = None
    for line in lines:
        m = TS_RE.match(line)
        if m:
            t = second_of_day(m.group(1), m.group(2), m.group(3))
            first = t if first is None else first
            last = t
    if first is not None and last is not None:
        return int(os.path.getmtime(path) - (last - first))
    return int(os.path.getmtime(path))


def tod_to_epoch(tod, start_epoch):
    base = dt.datetime.fromtimestamp(start_epoch).date()
    best = None
    for days in (-1, 0, 1):
        cand = (dt.datetime.combine(base + dt.timedelta(days=days), dt.time())
                + dt.timedelta(seconds=tod))
        ts = int(cand.timestamp())
        if best is None or abs(ts - start_epoch) < abs(best - start_epoch):
            best = ts
    return best


def parse(path):
    with open(path, "r", errors="replace") as f:
        lines = f.read().splitlines()

    start_epoch = read_start_epoch(path, lines)
    flips = []                      # (rel_ms, count, epoch_ms)
    wprof = []                      # dict
    present = []                    # dict
    snaps = []                      # dict
    cur = None
    now_tod = None
    now_epoch = start_epoch

    for line in lines:
        m = TS_RE.match(line)
        if m:
            now_tod = second_of_day(m.group(1), m.group(2), m.group(3))
            now_epoch = tod_to_epoch(now_tod, start_epoch)
            body = m.group(4)
        else:
            body = line
        ems = now_epoch * 1000

        m = FLIP_RE.search(body)
        if m:
            flips.append((int(m.group(2)), int(m.group(1)), ems))
            continue

        m = WPROF_RE.search(body)
        if m:
            wprof.append({
                "epoch_ms": ems,
                "report": int(m.group(1)),
                "flips": int(m.group(2)),
                "draws": float(m.group(3)),
                "walker": float(m.group(4)),
                "engine": float(m.group(5)),
                "parse": float(m.group(6)),
                "fetch": float(m.group(7)),
                "tex": float(m.group(8)),
                "pipe": float(m.group(9)),
                "bind": float(m.group(10)),
                "draw": float(m.group(11)),
                "other": float(m.group(12)),
            })
            continue

        m = PRESENT_RE.search(body)
        if m:
            present.append({
                "epoch_ms": ems,
                "pumps": int(m.group(1)),
                "max_gap_ms": int(m.group(2)),
                "blits": int(m.group(3)),
                "shown": int(m.group(4)),
                "noslot": int(m.group(5)),
                "notready": int(m.group(6)),
                "qmax_ms": int(m.group(7)),
            })
            continue

        m = C690_RE.search(body)
        if m:
            if cur is not None:
                snaps.append(cur)
            cur = {
                "epoch_ms": ems,
                "tag": m.group(1),
                "elapsed": float(m.group(2)),
                "jobs": int(m.group(3)),
                "jobs_per_s": float(m.group(4)),
                "frames": None,
                "wall_sum_us": None,
                "p50_us": None,
                "p90_us": None,
                "workers": {},
            }
            continue

        if cur is None:
            continue
        m = C690_FRAMES_RE.search(body)
        if m:
            cur["frames"] = int(m.group(1))
            continue
        m = C690_WALL_RE.search(body)
        if m:
            if m.group(1) != "n/a":
                cur["p50_us"] = int(m.group(1))
                cur["p90_us"] = int(m.group(2))
            ms = C690_SUM_RE.search(body)
            if ms:
                cur["wall_sum_us"] = int(ms.group(1))
            continue
        m = C690_WORKER_RE.search(body)
        if m:
            cur["workers"][m.group(1)] = int(m.group(3))

    if cur is not None:
        snaps.append(cur)

    if flips:
        first_rel = flips[0][0]
        origin = flips[0][2]
        flips = [(origin + (rel - first_rel), n, origin) for rel, n, _ in flips]
        # keep order as (epoch_ms, count)
        flips = [(t, n) for t, n, _ in flips]
    return flips, wprof, present, snaps, start_epoch


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def fmt(v, spec, dash="n/a"):
    return dash if v is None else format(v, spec)


def percentile_vals(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    pos = (len(xs) - 1) * p
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


def window_stats(ws, we, flips, wprof, present, snaps):
    wf = [(t, n) for t, n in flips if ws <= t < we]
    fps = None
    frame_ms = None
    if len(wf) >= 2:
        duration = (wf[-1][0] - wf[0][0]) / 1000.0
        if duration > 0:
            fps = (wf[-1][1] - wf[0][1]) / duration
    elif wf:
        duration = (we - ws) / 1000.0
        if duration > 0:
            fps = len(wf) / duration
    if fps and fps > 0:
        frame_ms = 1000.0 / fps

    wp = [x for x in wprof if ws <= x["epoch_ms"] < we]
    walker = mean([x["walker"] for x in wp])
    engine = mean([x["engine"] for x in wp])
    phases = {p: mean([x[p] for x in wp]) for p in PHASES}
    top = sorted(((v, k) for k, v in phases.items() if v is not None),
                 reverse=True)[:3]
    wait_ms = (frame_ms - walker) if (frame_ms is not None and walker is not None) else None
    wait_frac = (wait_ms / frame_ms * 100.0
                 if (wait_ms is not None and frame_ms) else None)

    # c690: diff the cumulative periodic snapshots that bracket this window.
    base = None
    top_s = None
    for s in snaps:
        if s["epoch_ms"] <= ws:
            base = s
        if s["epoch_ms"] <= we:
            top_s = s
    c690 = None
    if top_s is not None and base is not None and top_s is not base:
        del_elapsed = top_s["elapsed"] - base["elapsed"]
        djobs = top_s["jobs"] - base["jobs"]
        if del_elapsed > 0 and top_s.get("wall_sum_us") is not None \
                and base.get("wall_sum_us") is not None:
            dwall = top_s["wall_sum_us"] - base["wall_sum_us"]
            total_busy = (dwall / 1e6) / del_elapsed * 100.0
            idx = sorted(set(base["workers"]) | set(top_s["workers"]))
            lanes = {k: (top_s["workers"].get(k, 0) - base["workers"].get(k, 0))
                     for k in idx}
            busy = {}
            for k, dj in lanes.items():
                share = (dj / djobs) if djobs else 0.0
                busy[k] = (dwall * share / 1e6) / del_elapsed * 100.0
            if not busy:
                for k in range(6):
                    busy[str(k)] = 0.0
            wall_frame = (dwall / 1000.0 / len(wf)) if wf else None
            c690 = {
                "jobs_s": djobs / del_elapsed,
                "jobs": djobs,
                "wall_sum_us": dwall,
                "wall_frame_ms": wall_frame,
                "busy_total": total_busy,
                "busy_mean": total_busy / len(busy) if busy else None,
                "busy": busy,
                "p50_us": top_s.get("p50_us"),
                "p90_us": top_s.get("p90_us"),
                "ratio_wait": (wall_frame / wait_ms) if (wall_frame is not None
                                                        and wait_ms and wait_ms > 0) else None,
            }

    pr = [x for x in present if ws <= x["epoch_ms"] < we]
    pres = None
    if pr:
        pres = {
            "pumps": sum(x["pumps"] for x in pr),
            "max_gap_ms": max(x["max_gap_ms"] for x in pr),
            "blits": sum(x["blits"] for x in pr),
            "shown": sum(x["shown"] for x in pr),
            "dropped": sum(x["noslot"] + x["notready"] for x in pr),
            "noslot": sum(x["noslot"] for x in pr),
            "notready": sum(x["notready"] for x in pr),
            "qmax_ms": max(x["qmax_ms"] for x in pr),
        }

    return {
        "start_ms": ws,
        "end_ms": we,
        "fps": fps,
        "frame_ms": frame_ms,
        "flips": len(wf),
        "walker": walker,
        "engine": engine,
        "wait_ms": wait_ms,
        "wait_frac": wait_frac,
        "phases": phases,
        "top": top,
        "c690": c690,
        "present": pres,
    }


def find_thumbs(log, override):
    if override:
        return sorted(glob.glob(os.path.join(override, "thumb_*.png")))
    base = os.path.splitext(log)[0]
    cands = [base, base + "_frames"]
    for d in cands:
        got = sorted(glob.glob(os.path.join(d, "thumb_*.png")))
        if got:
            return got
    return []


def thumb_for(thumbs, mid_ms, max_dist_ms=4000):
    best, bestd = None, None
    for p in thumbs:
        m = re.search(r"thumb_(\d+)\.png$", p)
        if not m:
            continue
        d = abs(int(m.group(1)) - mid_ms)
        if bestd is None or d < bestd:
            best, bestd = p, d
    return best if (bestd is not None and bestd <= max_dist_ms) else None


def hhmmss(ms):
    return dt.datetime.fromtimestamp(ms / 1000.0).strftime("%H:%M:%S")


def write_sheet(path, rows, thumbs, log):
    rel = lambda p: os.path.relpath(p, os.path.dirname(os.path.abspath(path)))
    out = []
    out.append("<!doctype html><meta charset='utf-8'>")
    out.append("<title>Multi-scene play — " + html.escape(os.path.basename(log)) + "</title>")
    out.append("""<style>
body{font:14px/1.4 system-ui,sans-serif;margin:18px;background:#fafafa;color:#222}
h1{font-size:20px;margin:0 0 4px}
.sub{color:#666;margin-bottom:14px}
table{border-collapse:collapse;background:#fff}
th,td{border:1px solid #ddd;padding:5px 7px;vertical-align:top;white-space:nowrap}
th{background:#f0f0f0;position:sticky;top:0}
td.img{padding:3px}img{display:block;width:320px;height:180px;object-fit:contain;background:#111}
tr.hot td{background:#fff2d8}tr.hot td.img{background:#fff2d8}
.mono{font-variant-numeric:tabular-nums}.small{color:#666;font-size:12px}
</style>""")
    out.append("<h1>Multi-scene play contact sheet</h1>")
    out.append("<div class='sub'>" + html.escape(log) +
               " — each row is a 2 s window; the thumbnail is the nearest "
               "saved frame. Rows tinted amber have c690 lanes ≥85% busy or "
               "wait ≥40% of the frame.</div>")
    out.append("<table><tr><th>window</th><th>thumbnail</th><th>fps</th>"
               "<th>c690 jobs/s</th><th>lane busy %</th><th>walker ms</th>"
               "<th>wait ms</th><th>top phases</th><th>present</th></tr>")
    for r in rows:
        c = r["c690"]
        p = r["present"]
        hot = False
        if c and c.get("busy_mean") is not None and c["busy_mean"] >= 85.0:
            hot = True
        if r["wait_frac"] is not None and r["wait_frac"] >= 40.0:
            hot = True
        if p and (p["dropped"] > 0 or p["max_gap_ms"] > 100):
            hot = True
        tp = " ".join(f"{k} {v:.2f}" for v, k in r["top"]) if r["top"] else "n/a"
        if c:
            lanes = " ".join(f"{k}:{v:.0f}" for k, v in
                             sorted(c["busy"].items(), key=lambda kv: int(kv[0]) if kv[0].isdigit() else 99))
            lane_txt = f"mean {c['busy_mean']:.0f}%<br><span class='small'>[{lanes}]</span>"
            c690_txt = f"{c['jobs_s']:.0f}"
        else:
            lane_txt = c690_txt = "n/a"
        pres_txt = "n/a" if not p else (
            f"shown {p['shown']} drop {p['dropped']}<br>"
            f"<span class='small'>gap {p['max_gap_ms']} ms</span>")
        mid = (r["start_ms"] + r["end_ms"]) // 2
        th = thumb_for(thumbs, mid) if thumbs else None
        img = (f"<a href='{html.escape(rel(th))}'><img src='{html.escape(rel(th))}'></a>"
               if th else "<span class='small'>no thumbnail</span>")
        out.append(
            f"<tr class='{'hot' if hot else ''}'>"
            f"<td class='mono'>{hhmmss(r['start_ms'])}<br>"
            f"<span class='small'>+{(r['end_ms'] - r['start_ms']) / 1000:.0f}s</span></td>"
            f"<td class='img'>{img}</td>"
            f"<td class='mono'>{fmt(r['fps'], '.1f')}</td>"
            f"<td class='mono'>{c690_txt}</td>"
            f"<td class='mono'>{lane_txt}</td>"
            f"<td class='mono'>{fmt(r['walker'], '.2f')}</td>"
            f"<td class='mono'>{fmt(r['wait_ms'], '.2f')}"
            f"<br><span class='small'>{fmt(r['wait_frac'], '.0f')}%</span></td>"
            f"<td class='mono'>{tp}</td>"
            f"<td class='mono'>{pres_txt}</td></tr>")
    out.append("</table>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="game log from play_windowed.sh")
    ap.add_argument("--thumbs", help="directory with thumb_<epoch-ms>.png")
    ap.add_argument("--window", type=float, default=2.0, help="window seconds (default 2)")
    ap.add_argument("--sheet", help="HTML output path (default <log>_sheet.html)")
    args = ap.parse_args()

    flips, wprof, present, snaps, start_epoch = parse(args.log)
    if not flips and not wprof and not present and not snaps:
        raise SystemExit(f"{args.log}: no [flip], [walker-prof], [c690-stats] or "
                         "present lines found")

    events = ([t for t, _ in flips] + [x["epoch_ms"] for x in wprof] +
              [x["epoch_ms"] for x in present] + [x["epoch_ms"] for x in snaps])
    start = flips[0][0] if flips else min(events)
    end = max(events)
    win_ms = int(args.window * 1000)
    if end <= start:
        end = start + win_ms

    rows = []
    ws = start
    while ws < end:
        we = ws + win_ms
        rows.append(window_stats(ws, we, flips, wprof, present, snaps))
        ws = we

    thumbs = find_thumbs(args.log, args.thumbs)
    print(f"{'window':>8} {'fps':>6} {'c690/s':>7} {'lane%':>6} {'walker':>7} "
          f"{'wait':>6} {'top phase':<22} {'shown':>5} {'drop':>4} {'gap':>5}")
    for r in rows:
        c = r["c690"]
        p = r["present"]
        top = r["top"][0][1] if r["top"] else "n/a"
        print(f"{hhmmss(r['start_ms']):>8} {fmt(r['fps'], '6.1f'):>6} "
              f"{fmt(c['jobs_s'] if c else None, '7.0f'):>7} "
              f"{fmt(c['busy_mean'] if c else None, '6.0f'):>6} "
              f"{fmt(r['walker'], '7.2f'):>7} {fmt(r['wait_ms'], '6.2f'):>6} "
              f"{top:<22} {fmt(p['shown'] if p else None, '5d'):>5} "
              f"{fmt(p['dropped'] if p else None, '4d'):>4} "
              f"{fmt(p['max_gap_ms'] if p else None, '5d'):>5}")

    # Summary over windows that have the fields.
    busy = [r["c690"]["busy_mean"] for r in rows
            if r["c690"] and r["c690"]["busy_mean"] is not None]
    waits = [r["wait_frac"] for r in rows if r["wait_frac"] is not None]
    print()
    if busy:
        print(f"c690 lane busy: mean {mean(busy):.1f}%  max {max(busy):.1f}%  "
              f"windows >=85%: {sum(1 for x in busy if x >= 85)}/{len(busy)}")
    if waits:
        print(f"walker wait: mean {mean(waits):.1f}% of the frame  "
              f"max {max(waits):.1f}%")
    pjob = [r["c690"] for r in rows if r["c690"]]
    if pjob:
        last = pjob[-1]
        print(f"c690 job wall (last window's cumulative report): "
              f"p50 {fmt(last['p50_us'], '.0f')} us  p90 {fmt(last['p90_us'], '.0f')} us")
    ratios = [r["c690"]["ratio_wait"] for r in rows
              if r["c690"] and r["c690"]["ratio_wait"] is not None]
    if ratios:
        print(f"c690 wall per frame / wait: mean {mean(ratios):.2f} "
              "(work-to-wait ratio, not the traced share)")
    drops = sum(r["present"]["dropped"] for r in rows if r["present"])
    if any(r["present"] for r in rows):
        print(f"present: {drops} dropped frame(s) over the run; "
              f"max pump gap {max(r['present']['max_gap_ms'] for r in rows if r['present'])} ms")
    print("note: without PS3RECOMP_TRACE, wait is frame time minus walker CPU; "
          "the c690/wait ratio is work-to-wait, not a traced share.")

    sheet = args.sheet or (os.path.splitext(args.log)[0] + "_sheet.html")
    write_sheet(sheet, rows, thumbs, args.log)
    print(f"contact sheet: {sheet}"
          + (f" ({len(thumbs)} thumbnail(s))" if thumbs else " (no thumbnails found)"))


if __name__ == "__main__":
    main()
