#!/usr/bin/env python3
"""Report an ab_live.sh run: per-segment fps and walker own/wait, arm means
with 95% confidence intervals, and the paired difference.

    ab_live_report.py --log RUN.log --marks RUN.marks --seg 10 --drop 2 \
        --out RUN [--csv MangoHud.csv --csv-epoch EPOCH]

fps comes from the guest [flip] lines: the counter prints every 10th flip
after the first 400, so the rate is (n1-n0)/(at1-at0), never a line count.
Walker numbers come from [walker-prof] `at=`, `walker=` and `wait=`.
"""
import argparse
import datetime
import json
import math
import os
import re
import statistics
import subprocess
import sys

T95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
    8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160,
    14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093,
    20: 2.086, 21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060,
    26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042,
}

FLIP_RE = re.compile(r"\[flip\] (\d+) at (\d+) ms")
WPROF_RE = re.compile(
    r"\[walker-prof\] at=(\d+)ms .*?walker=([0-9.]+)ms wait=([0-9.]+)ms")
WPROF_FULL_RE = re.compile(
    r"\[walker-prof\] at=(\d+)ms .*?walker=([0-9.]+)ms wait=([0-9.]+)ms "
    r"engine=([0-9.]+)ms parse=([0-9.]+)ms fetch=([0-9.]+) tex=([0-9.]+) "
    r"pipe=([0-9.]+) bind=([0-9.]+) draw=([0-9.]+) other=([0-9.]+) "
    r"\| src_main=([0-9.]+) src_mesh=([0-9.]+) mainms=([0-9.]+) "
    r"meshms=([0-9.]+)")
PHASES = ("engine", "parse", "fetch", "tex", "pipe", "bind", "draw", "other",
          "src_main", "src_mesh", "mainms", "meshms")
MARK_RE = re.compile(r"^(A|B)\s+(\d+)\s+(\d+)\s*$")
WINDOW_RE = re.compile(
    r"window:.*blits (\d+), shown (\d+), dropped (\d+) no-slot")
ARM_RE = re.compile(r"^(\d\d):(\d\d):(\d\d) \[ab_live\] ([AB]): (.*)$")
CTL_RE = re.compile(
    r"^(\d\d):(\d\d):(\d\d) \[live-ctl\] flip=\d+ (\w+)=(-?\d+) \(was (-?\d+)\)")
RUNSTAMP_RE = re.compile(r"_(\d{8})_(\d{6})\.log$")


def parse_arm_body(body):
    """`EXP_NO_DRAW=1; EXP_TICK_US=8000` -> {'EXP_NO_DRAW': '1', ...}."""
    out = {}
    for part in body.split(";"):
        part = part.strip()
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def read_ctl_timeline(log_path, marks):
    """Map [live-ctl] changes to wall-clock seconds.

    The run log's timestamps are time-of-day only; the date comes from the
    run stamp in the filename. The marks carry both the log's `at` guest ms
    and the wall epoch, which is enough to anchor the whole run.
    """
    m = RUNSTAMP_RE.search(log_path)
    if not m:
        return None
    day = datetime.datetime.strptime(m.group(1), "%Y%m%d")
    events = []
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            cm = CTL_RE.match(line)
            if not cm:
                continue
            hh, mm, ss = map(int, cm.group(1, 2, 3))
            t = day.replace(hour=hh, minute=mm, second=ss).timestamp()
            events.append((t, cm.group(4), int(cm.group(5)), int(cm.group(6))))
    return events


def ctl_state_at(events, key, t):
    """Value of `key` at wall time t, or None if the log never mentions it."""
    if not events:
        return None
    ev = [e for e in events if e[1] == key]
    if not ev:
        return None
    val = ev[0][3]                       # the "(was N)" before the first change
    for et, _k, v, _was in ev:
        if et <= t:
            val = v
        else:
            break
    return val


def mean_ci(xs):
    n = len(xs)
    if n == 0:
        return 0.0, 0.0
    m = sum(xs) / n
    if n < 2:
        return m, 0.0
    s = statistics.stdev(xs)
    h = T95.get(n - 1, 1.96) * s / math.sqrt(n)
    return m, h


def fmt(x, h):
    return f"{x:.2f} +/- {h:.2f}"


def fps_window(flips, w0, w1):
    """Guest fps over [w0,w1] using the counter values, not line counts."""
    if not flips:
        return None
    before0 = [f for f in flips if f[1] <= w0]
    before1 = [f for f in flips if f[1] <= w1]
    if before0 and before1:
        n0, t0 = before0[-1]
        n1, t1 = before1[-1]
        if t1 > t0 and n1 >= n0:
            return (n1 - n0) * 1000.0 / (t1 - t0)
    inside = [f for f in flips if w0 <= f[1] <= w1]
    if len(inside) < 2:
        return None
    n0, t0 = inside[0]
    n1, t1 = inside[-1]
    if t1 <= t0:
        return None
    return (n1 - n0) * 1000.0 / (t1 - t0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--marks", required=True)
    ap.add_argument("--seg", type=float, default=10.0)
    ap.add_argument("--drop", type=float, default=2.0)
    ap.add_argument("--out", required=True)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--csv-epoch", type=float, default=None)
    args = ap.parse_args()

    flips = []
    wprof = []
    wprof_full = []
    win_lines = win_frozen = win_noslot = 0
    with open(args.log, "r", errors="replace") as f:
        for line in f:
            m = FLIP_RE.search(line)
            if m:
                flips.append((int(m.group(1)), int(m.group(2))))
                continue
            m = WPROF_FULL_RE.search(line)
            if m:
                vals = [float(x) for x in m.groups()[1:]]
                wprof_full.append(dict(zip(("at", "walker", "wait") +
                                           PHASES, [int(m.group(1))] + vals)))
                wprof.append((int(m.group(1)), float(m.group(2)),
                              float(m.group(3))))
                continue
            m = WPROF_RE.search(line)
            if m:
                wprof.append((int(m.group(1)), float(m.group(2)),
                              float(m.group(3))))
                continue
            m = WINDOW_RE.search(line)
            if m:
                win_lines += 1
                shown, noslot = int(m.group(2)), int(m.group(3))
                win_noslot += noslot
                if shown == 0:
                    win_frozen += 1
    flips.sort(key=lambda x: x[1])
    wprof.sort(key=lambda x: x[0])
    print(f"window stats: {win_lines} s, {win_frozen} with shown=0 "
          f"(frozen), {win_noslot} no-slot drops")

    marks = []
    with open(args.marks, "r", errors="replace") as f:
        for line in f:
            m = MARK_RE.match(line.strip())
            if m:
                marks.append((m.group(1), int(m.group(2)), int(m.group(3))))

    if not marks:
        print("ab_live_report: no marks")
        return 1

    # The script logs the two arm bodies ("A: NAME=value; ...") and every
    # applied control-file change. Together with the marks they are enough to
    # prove which arm was actually live in each measured window -- the check
    # that the A/B bands mean what the report says they mean.
    arm_body = {}
    with open(args.log, "r", errors="replace") as f:
        for line in f:
            am = ARM_RE.match(line)
            if am:
                arm_body[am.group(4)] = parse_arm_body(am.group(5))
    ctl_events = read_ctl_timeline(args.log, marks)

    segs = []
    for i, (arm, at, wall) in enumerate(marks):
        w0 = at + int(args.drop * 1000)
        w1 = at + int(args.seg * 1000)
        fps = fps_window(flips, w0, w1)
        ww = [w for w in wprof if w0 <= w[0] <= w1]
        own = statistics.mean([w[1] for w in ww]) if ww else None
        wait = statistics.mean([w[2] for w in ww]) if ww else None
        segs.append({"seg": i, "arm": arm, "at": at, "w0": w0, "w1": w1,
                     "wall": wall, "fps": fps, "walker_ms": own,
                     "wait_ms": wait, "nprof": len(ww)})

    print("\n== per segment ==")
    print(f"{'seg':>3} {'arm':>3} {'window ms':>17} {'fps':>7} "
          f"{'walker ms':>10} {'wait ms':>8} {'n':>5}")
    for s in segs:
        fps = f"{s['fps']:.2f}" if s["fps"] is not None else "-"
        own = f"{s['walker_ms']:.3f}" if s["walker_ms"] is not None else "-"
        wait = f"{s['wait_ms']:.3f}" if s["wait_ms"] is not None else "-"
        print(f"{s['seg']:>3} {s['arm']:>3} {s['w0']:>8}-{s['w1']:<8} "
              f"{fps:>7} {own:>10} {wait:>8} {s['nprof']:>5}")

    arms = {}
    for arm in ("A", "B"):
        ss = [s for s in segs if s["arm"] == arm]
        fps = [s["fps"] for s in ss if s["fps"] is not None]
        own = [s["walker_ms"] for s in ss if s["walker_ms"] is not None]
        wait = [s["wait_ms"] for s in ss if s["wait_ms"] is not None]
        arms[arm] = {"fps": fps, "walker": own, "wait": wait, "segs": ss}

    # Self-check: the arm label of each segment must match the control state
    # that was actually applied during that segment's measured window
    # [at+drop, at+seg]. This is what validates the shaded A/B bands.
    print("\n== A/B band self-check (live-ctl vs marks) ==")
    check_ok = ctl_events is not None
    checked = 0
    for s in segs:
        exp = arm_body.get(s["arm"], {})
        if not exp:
            continue
        w0 = (s["wall"] + int(args.drop * 1000)) / 1000.0
        w1 = (s["wall"] + int(args.seg * 1000)) / 1000.0
        bad = []
        for key, want in exp.items():
            try:
                want_i = int(want, 0)
            except ValueError:
                want_i = want
            # Log timestamps have 1 s resolution and the next switch's
            # control write lands near this window's end, so probe the start
            # and the middle, not the boundary.
            for when, tq in (("start", w0), ("mid", (w0 + w1) / 2.0)):
                got = ctl_state_at(ctl_events, key, tq)
                if got is None:
                    continue    # never logged: it kept its environment value
                if got != want_i:
                    bad.append(f"{key}={got} at {when} (want {want})")
        checked += 1
        if bad:
            check_ok = False
            print(f"  seg {s['seg']} arm {s['arm']}: MISMATCH " + ", ".join(bad))
    if ctl_events is None:
        print("  SKIP: no run stamp / live-ctl lines in the log")
    elif not checked:
        print("  SKIP: no arm bodies logged")
    else:
        print(f"  {checked} segments, " +
              ("PASS: every band label matches the applied control state"
               if check_ok else "FAIL: band label does not match live-ctl"))

    print("\n== per arm ==")
    summary = {}
    for arm in ("A", "B"):
        a = arms[arm]
        mf, hf = mean_ci(a["fps"])
        mo, ho = mean_ci(a["walker"])
        mw, hw = mean_ci(a["wait"])
        spread = statistics.stdev(a["fps"]) if len(a["fps"]) > 1 else 0.0
        rng = (min(a["fps"]), max(a["fps"])) if a["fps"] else (0, 0)
        print(f"arm {arm}: fps {fmt(mf, hf)} (n={len(a['fps'])}, "
              f"std={spread:.2f}, range={rng[0]:.2f}-{rng[1]:.2f}) "
              f"walker {fmt(mo, ho)} ms wait {fmt(mw, hw)} ms")
        summary[arm] = {"fps": a["fps"], "walker": a["walker"],
                        "wait": a["wait"], "fps_mean": mf, "fps_ci": hf,
                        "fps_std": spread, "fps_range": list(rng),
                        "walker_mean": mo, "walker_ci": ho,
                        "wait_mean": mw, "wait_ci": hw}

    # Per-phase walker breakdown (all [walker-prof] reports inside the
    # measured windows of each arm). engine is the dispatch total; parse is
    # drain - dispatch; fetch/tex/pipe/bind/draw are inside engine, other is
    # the unaccounted remainder; mainms/meshms split the draw sink by source.
    if wprof_full:
        print("\n== walker phases (ms/frame, measured windows) ==")
        print(f"{'phase':>9} {'A':>9} {'B':>9}")
        phase_means = {}
        for arm in ("A", "B"):
            ws = [s for s in segs if s["arm"] == arm]
            sel = [w for w in wprof_full
                   if any(s["w0"] <= w["at"] <= s["w1"] for s in ws)]
            phase_means[arm] = {
                p: (statistics.mean([w[p] for w in sel]) if sel else 0.0)
                for p in PHASES}
        for p in PHASES:
            print(f"{p:>9} {phase_means['A'][p]:>9.3f} "
                  f"{phase_means['B'][p]:>9.3f}")
        summary["phases"] = phase_means

    a_fps, b_fps = arms["A"]["fps"], arms["B"]["fps"]
    k = min(len(a_fps), len(b_fps))
    diffs = [a_fps[i] - b_fps[i] for i in range(k)]
    dm, dh = mean_ci(diffs)
    print(f"\npaired difference A-B: {fmt(dm, dh)} fps (n={k})")
    ci_lo, ci_hi = dm - dh, dm + dh
    print(f"95% CI: [{ci_lo:.2f}, {ci_hi:.2f}] fps")
    same = (ci_lo <= 0.0 <= ci_hi) and abs(dm) <= 0.5
    print(f"A/A check (|diff|<=0.5 and CI covers 0): "
          f"{'PASS' if same else 'NOT AN A/A LEVEL' if abs(dm) > 0.5 else 'CI misses 0'}")

    result = {"log": args.log, "marks": args.marks, "seg_secs": args.seg,
              "drop_secs": args.drop, "segments": segs, "arms": summary,
              "diff_mean": dm, "diff_ci": dh,
              "diff_ci_bounds": [ci_lo, ci_hi],
              "window": {"stats_seconds": win_lines,
                         "frozen_seconds": win_frozen,
                         "no_slot_drops": win_noslot}}
    outjson = args.out + "_ab.json"
    with open(outjson, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\njson: {outjson}")

    # Segment bands for the frametime plot, in MangoHud elapsed seconds.
    # Use the measured window, not the whole switch-to-switch interval: the
    # first `drop` seconds are discarded by the report, so shading them made a
    # band include the other arm's tail right after a switch.
    if args.csv and args.csv_epoch:
        seg_path = args.out + "_segments.txt"
        span = max(0.0, args.seg - args.drop)
        with open(seg_path, "w") as f:
            for s in segs:
                t0 = s["wall"] / 1000.0 - args.csv_epoch + args.drop
                # 4th field: the guest [flip] rate for this band, printed in
                # the band so the host present-frametime trace is not read as
                # game fps.
                fps = f" {s['fps']:.2f}" if s["fps"] is not None else ""
                f.write(f"{t0:.3f} {t0 + span:.3f} {s['arm']}{fps}\n")
        plot = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "frametime_plot.py")
        png = args.out + "_ft.png"
        print(f"\nframetime plot: {png} (segments {seg_path})")
        r = subprocess.run([sys.executable, plot, args.csv, "-o", png,
                            "--segments", seg_path])
        if r.returncode != 0:
            print("frametime_plot failed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
