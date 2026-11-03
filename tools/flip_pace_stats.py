#!/usr/bin/env python3
"""Per-arm present-pacing metrics from ab_live.sh's [fliplog] lines and the
MangoHud CSV.

    flip_pace_stats.py RUN.log RUN.marks [--csv CSV] [--seg 10 --drop 2]

The per-flip log (PS3RECOMP_FLIP_LOG=1) carries four timestamps on one
CLOCK_MONOTONIC microsecond clock: req (guest flip), fin (walker reached it),
pres (frame clock handed it to the window) and show (vkQueuePresentKHR
returned). Consecutive `show` values are the intervals the display and
MangoHud see.

Guest milliseconds are that clock minus the first flip's req, the same origin
as `[flip] ... at N ms` and the marks. MangoHud rows are assigned by wall
time, using the CSV filename's own start stamp.
"""
import argparse
import csv
import datetime
import math
import os
import re
import statistics
import sys

FLIPLOG_RE = re.compile(
    r"\[fliplog\] n=(\d+) req=(\d+) fin=(\d+) pres=(\d+) show=(\d+)"
    r" req_to_fin=(\d+) fin_to_pres=(\d+) pres_to_show=(\d+)"
    r" show_gap=(\d+)( skipp?ed)?")
MARK_RE = re.compile(r"^(A|B)\s+(\d+)\s+(\d+)\s*$")
CSV_STAMP_RE = re.compile(
    r"_(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})\.csv$")


def pct(xs, q):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * q / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    if lo == hi:
        return s[lo]
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def stats(xs):
    if not xs:
        return None
    return {
        "n": len(xs),
        "p50": pct(xs, 50), "p90": pct(xs, 90), "p99": pct(xs, 99),
        "mean": statistics.mean(xs),
        "std": statistics.stdev(xs) if len(xs) > 1 else 0.0,
        "lt8": 100.0 * sum(1 for x in xs if x < 8.0) / len(xs),
        "gt40": 100.0 * sum(1 for x in xs if x > 40.0) / len(xs),
    }


def fmt_stats(label, st):
    if not st:
        return f"{label}: no samples"
    return (f"{label}: n={st['n']} p50={st['p50']:.2f} p90={st['p90']:.2f} "
            f"p99={st['p99']:.2f} mean={st['mean']:.2f} std={st['std']:.2f} "
            f"<8ms={st['lt8']:.1f}% >40ms={st['gt40']:.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("marks")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--seg", type=float, default=10.0)
    ap.add_argument("--drop", type=float, default=2.0)
    args = ap.parse_args()

    recs = []
    first_req = None
    with open(args.log, errors="replace") as f:
        for line in f:
            m = FLIPLOG_RE.search(line)
            if not m:
                continue
            seq, req, fin, pres, show = (int(m.group(i)) for i in range(1, 6))
            if first_req is None:
                first_req = req
            recs.append({"seq": seq, "req": req, "fin": fin, "pres": pres,
                         "show": show, "skipped": bool(m.group(10))})
    if first_req is None:
        print("no [fliplog] lines (run with PS3RECOMP_FLIP_LOG=1)")
        return 1
    for r in recs:
        r["gms"] = (r["req"] - first_req) / 1000.0
        r["fin_ms"] = r["fin"] / 1000.0 if r["fin"] else 0.0
        r["pres_ms"] = r["pres"] / 1000.0 if r["pres"] else 0.0
        r["show_ms"] = r["show"] / 1000.0 if r["show"] else 0.0

    marks = []
    with open(args.marks) as f:
        for line in f:
            m = MARK_RE.match(line.strip())
            if m:
                marks.append({"arm": m.group(1), "at": int(m.group(2)),
                              "wall": int(m.group(3))})
    marks.sort(key=lambda x: x["wall"])

    print(f"flip log: {len(recs)} flips, first req {first_req} us")
    print(f"{'seg':>3} {'arm':>3} {'window gms':>18} {'flips':>6} {'fps':>7} "
          f"{'skipped':>7} {'req-fin':>8} {'fin-pres':>9} {'pres-show':>10}")
    arms = {"A": [], "B": []}
    for i, mk in enumerate(marks):
        w0, w1 = mk["at"] + args.drop * 1000, mk["at"] + args.seg * 1000
        sel = [r for r in recs if w0 <= r["gms"] <= w1]
        shown = [r for r in sel if r["show"]]
        fps = len(shown) / ((w1 - w0) / 1000.0) if shown else 0.0
        rf = [r["fin_ms"] - r["req"] / 1000.0 for r in shown if r["fin_ms"]]
        fp = [r["pres_ms"] - r["fin_ms"] for r in shown
              if r["pres_ms"] and r["fin_ms"]]
        ps = [r["show_ms"] - r["pres_ms"] for r in shown
              if r["show_ms"] and r["pres_ms"]]
        print(f"{i:>3} {mk['arm']:>3} {w0:>8.0f}-{w1:<8.0f} {len(sel):>6} "
              f"{fps:>7.2f} {sum(1 for r in sel if r['skipped']):>7} "
              f"{(statistics.mean(rf) if rf else 0):>8.2f} "
              f"{(statistics.mean(fp) if fp else 0):>9.2f} "
              f"{(statistics.mean(ps) if ps else 0):>10.2f}")
        arms[mk["arm"]].append({"sel": sel, "shown": shown, "fps": fps})

    print("\n== per arm: [fliplog] ==")
    for arm in ("A", "B"):
        segs = arms[arm]
        shows = sorted(r["show_ms"] for s in segs for r in s["shown"])
        diffs = [shows[i] - shows[i - 1] for i in range(1, len(shows))
                 if 0 < shows[i] - shows[i - 1] < 1000]
        fps = statistics.mean([s["fps"] for s in segs]) if segs else 0.0
        rf = [r["fin_ms"] - r["req"] / 1000.0 for s in segs for r in s["shown"]
              if r["fin_ms"]]
        fp = [r["pres_ms"] - r["fin_ms"] for s in segs for r in s["shown"]
              if r["pres_ms"] and r["fin_ms"]]
        ps = [r["show_ms"] - r["pres_ms"] for s in segs for r in s["shown"]
              if r["show_ms"] and r["pres_ms"]]
        skipped = sum(1 for s in segs for r in s["sel"] if r["skipped"])
        print(f"arm {arm}: guest fps {fps:.2f}")
        print("  " + fmt_stats("present interval", stats(diffs)))
        for name, v in (("req->fin", rf), ("fin->pres", fp), ("pres->show", ps)):
            if v:
                print(f"  {name}: p50={pct(v, 50):.2f} p90={pct(v, 90):.2f} "
                      f"mean={statistics.mean(v):.2f} ms")
        print(f"  flips not shown: {skipped}")

    if args.csv and os.path.exists(args.csv):
        m = CSV_STAMP_RE.search(os.path.basename(args.csv))
        if not m:
            print("\nCSV: no start stamp in the filename")
            return 0
        csv_epoch = datetime.datetime(
            int(m.group(1)), int(m.group(2)), int(m.group(3)),
            int(m.group(4)), int(m.group(5)), int(m.group(6))).timestamp()
        rows = list(csv.reader(open(args.csv, errors="ignore")))
        data = rows[2:]
        print(f"\n== per arm: MangoHud CSV ({os.path.basename(args.csv)}) ==")
        for arm in ("A", "B"):
            iv = []
            for mk in [x for x in marks if x["arm"] == arm]:
                w0 = mk["wall"] / 1000.0 + args.drop
                w1 = mk["wall"] / 1000.0 + args.seg
                for r in data:
                    if not r or len(r) < 2:
                        continue
                    try:
                        ft, el = float(r[1]), float(r[-1])
                    except ValueError:
                        continue
                    t = csv_epoch + el / 1e9
                    if w0 <= t <= w1 and ft > 0:
                        iv.append(ft)
            print("arm " + arm + ": " + fmt_stats("frametime", stats(iv)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
