#!/usr/bin/env python3
"""Per-frame PPU busy/blocked split for the pitch meter, from a PS3RECOMP_TRACE.

Complements analyse_ppu_path.py, which reports one window. This tool keeps the
PPU wait sites separate per frame and splits the frames into the in-run A/B
arms using ab_live.sh's marks file, so one trace that spans several 10 s
segments answers "what does the PPU block on in arm A vs arm B".

The trace is streamed: the 5M-line files the full SPU trace produces are not
held in memory, only the PPU wait spans, walker spans and flip instants.

Inputs
  TRACE        PS3RECOMP_TRACE file (line-oriented JSON)
  --marks      ab_live.sh .marks file: "<arm> <flip_ms> <wall_ms>"
  --origin-ms  wall time written when the trace trigger file was touched
               (the wrapper records it); used to map marks wall -> trace ts
  --seg/--drop seconds per segment and seconds discarded after each switch

Outputs a per-arm table (fps, main blocked split by wait site, main running,
walker drain/put_wait, DisplayFlip waits, vblank ticks per flip) plus optional
per-frame CSV and markdown.
"""
import argparse
import collections
import json
import sys

WAIT_NAMES = {
    "ppu.lwcond", "ppu.lwmutex", "ppu.cond", "ppu.semaphore",
    "ppu.event_queue", "ppu.event_flag", "ppu.usleep",
}
KEEP_NAMES = WAIT_NAMES | {"rsx.drain", "rsx.put_wait", "gcm.flip_vb"}


def num(v, d=0):
    if isinstance(v, str):
        try:
            return int(v, 0)
        except ValueError:
            return d
    try:
        return int(v)
    except (TypeError, ValueError):
        return d


def load(trace):
    names = {}
    spans = collections.defaultdict(list)   # (tid, name) -> [(t0,t1,args)]
    flips = []                              # (ts, req, vblank)
    first_ts = None
    with open(trace, "r", errors="replace") as f:
        for line in f:
            if first_ts is None:
                if '"ts":' in line:
                    i = line.find('"ts":')
                    try:
                        first_ts = float(line[i + 5:].split(",", 1)[0])
                    except ValueError:
                        pass
            # Cheap prefilter: the writer emits compact JSON, no spaces.
            if '"name":"thread_name"' in line:
                try:
                    o = json.loads(line.strip().rstrip(","))
                except json.JSONDecodeError:
                    continue
                if o.get("ph") == "M":
                    names[int(o.get("tid", 0))] = (o.get("args") or {}).get("name", "")
                continue
            if not any('"name":"' + n + '"' in line for n in KEEP_NAMES):
                continue
            try:
                o = json.loads(line.strip().rstrip(","))
            except json.JSONDecodeError:
                continue
            name = o.get("name", "")
            tid = int(o.get("tid", 0))
            ts = float(o.get("ts", 0) or 0)
            args = o.get("args") or {}
            ph = o.get("ph")
            if name == "gcm.flip_vb":
                flips.append((ts, num(args.get("flip")), num(args.get("vblank"))))
            elif ph == "X":
                dur = float(o.get("dur", 0) or 0)
                spans[(tid, name)].append((ts, ts + dur, args))
            elif ph == "b":
                spans[(tid, name)].append([ts, None, args, o.get("id")])
            elif ph == "e":
                q = spans.get((tid, name), [])
                for e in reversed(q):
                    if len(e) == 4 and e[1] is None and e[3] == o.get("id"):
                        e[1] = ts
                        break
    # close any unclosed begin/end pair at the last seen instant
    for lst in spans.values():
        for e in lst:
            if len(e) == 4:
                if e[1] is None:
                    e[1] = e[0]
                e.pop()
    return names, spans, sorted(flips), first_ts


def union(iv):
    iv = sorted((a, b) for a, b in iv if b > a)
    total, cur = 0.0, None
    for a, b in iv:
        if cur is None or a > cur[1]:
            if cur:
                total += cur[1] - cur[0]
            cur = [a, b]
        else:
            cur[1] = max(cur[1], b)
    if cur:
        total += cur[1] - cur[0]
    return total


def clip(iv, lo, hi):
    return union([(max(a, lo), min(b, hi)) for a, b in iv if min(b, hi) > max(a, lo)])


def m(v):
    return v / 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--marks", required=True)
    ap.add_argument("--origin-ms", type=int, required=True)
    ap.add_argument("--seg", type=float, default=10.0)
    ap.add_argument("--drop", type=float, default=2.0)
    ap.add_argument("--main", default=None,
                    help="main-thread track name (default: ppu main, else the "
                         "track that blocks most on lwcond 0x1245090)")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--md", default=None)
    a = ap.parse_args()

    names, spans, flips, first_ts = load(a.trace)
    tid_of = {v: k for k, v in names.items()}
    main_t, flip_t, walk_t = tid_of.get("ppu main"), tid_of.get("ppu DisplayFlip"), tid_of.get("rsx walker")
    if a.main:
        main_t = tid_of.get(a.main)
    if main_t is None:
        # The boot thread is named before the trace is initialised, so its
        # "ppu main" thread_name event is lost and it shows as "host thread N".
        # The render main is the track that blocks on the buffer-ref lwcond.
        wt = collections.Counter()
        for (t, n), lst in spans.items():
            if n == "ppu.lwcond" and t != flip_t:
                for t0, t1, args in lst:
                    if num(args.get("lwcond"), -1) == 0x1245090:
                        wt[t] += t1 - t0
        if wt:
            main_t = wt.most_common(1)[0][0]
    if main_t is None or walk_t is None:
        sys.exit("trace has no ppu main / rsx walker track: %s" % sorted(tid_of))
    if len(flips) < 10:
        sys.exit("fewer than 10 flips in the trace window")

    # Wall -> trace ts. The first kept event is a few ms after the trigger.
    first_kept = min([t for t, _, _ in flips] +
                     [e[0] for lst in spans.values() for e in lst])
    def wall_to_ts(ms):
        return first_kept + (ms - a.origin_ms) * 1000.0

    marks = []
    with open(a.marks) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                marks.append((p[0], int(p[1]), wall_to_ts(int(p[2]))))
    marks.sort(key=lambda x: x[2])
    if not marks:
        sys.exit("empty marks file")

    def arm_at(t):
        arm, sw = None, None
        for lab, _, st in marks:
            if st <= t:
                arm, sw = lab, st
            else:
                break
        return arm, sw

    wait_cat = {
        "epu": "other",
        "ref": "lwcond 0x1245090 (FUN_000655cc)",
        "flip_throttle": "lwcond 0x1245100 (flip queue)",
        "lwcond_other": "lwcond other",
        "lwmutex": "lwmutex",
        "usleep": "usleep (guest poll)",
        "cond": "cond",
        "sem": "semaphore",
        "event": "event",
    }

    def cat_of(name, args):
        if name == "ppu.lwcond":
            ea = num(args.get("lwcond"), -1)
            if ea == 0x1245090:
                return "ref"
            if ea == 0x1245100:
                return "flip_throttle"
            return "lwcond_other"
        return {
            "ppu.lwmutex": "lwmutex",
            "ppu.usleep": "usleep",
            "ppu.cond": "cond",
            "ppu.semaphore": "sem",
            "ppu.event_queue": "event",
            "ppu.event_flag": "event",
        }.get(name, "other")

    main_wait = collections.defaultdict(list)
    for (t, n), lst in spans.items():
        if t != main_t or n not in WAIT_NAMES:
            continue
        for t0, t1, args in lst:
            main_wait[cat_of(n, args)].append((t0, t1))
    flip_wait = collections.defaultdict(list)
    if flip_t is not None:
        for (t, n), lst in spans.items():
            if t != flip_t or n not in WAIT_NAMES:
                continue
            for t0, t1, args in lst:
                cat = cat_of(n, args)
                key = {"ref": "flip_ref", "flip_throttle": "flip_done"}.get(cat, "flip_" + cat)
                flip_wait[key].append((t0, t1))
    drain = [(e[0], e[1]) for e in spans.get((walk_t, "rsx.drain"), [])]
    putwait = [(e[0], e[1]) for e in spans.get((walk_t, "rsx.put_wait"), [])]

    frames = []
    for i in range(len(flips) - 1):
        t0, req, vb = flips[i]
        t1, req1, vb1 = flips[i + 1]
        if t1 <= t0:
            continue
        arm, sw = arm_at(t0)
        if arm is None or (t0 - sw) < a.drop * 1e6:
            continue
        dt = (t1 - t0) / 1000.0
        blocked_by = {k: m(clip(v, t0, t1)) for k, v in main_wait.items()}
        blocked = m(clip([iv for v in main_wait.values() for iv in v], t0, t1))
        refs = [(max(a0, t0), min(b0, t1)) for a0, b0 in main_wait.get("ref", [])
                if min(b0, t1) > max(a0, t0)]
        row = {
            "t": t0, "arm": arm, "req": req, "dt": dt,
            "blocked": blocked, "running": dt - blocked,
            "vblank_dt": vb1 - vb,
            "drain": m(clip(drain, t0, t1)),
            "put_wait": m(clip(putwait, t0, t1)),
            "ref_drain": m(sum(clip(drain, r0, r1) for r0, r1 in refs)),
            "ref_putwait": m(sum(clip(putwait, r0, r1) for r0, r1 in refs)),
        }
        for k, v in blocked_by.items():
            row["b_" + k] = v
        for k, v in flip_wait.items():
            row["f_" + k] = m(clip(v, t0, t1))
        frames.append(row)

    if not frames:
        sys.exit("no frames left after the drop filter")

    keys = ["dt", "blocked", "running", "drain", "put_wait", "vblank_dt",
            "ref_drain", "ref_putwait"]

    def summary(rows):
        out = {}
        for k in keys:
            v = [r.get(k, 0.0) for r in rows]
            out[k] = sum(v) / len(v)
        for k in sorted({k for r in rows for k in r if k.startswith("b_") or k.startswith("f_")}):
            v = [r.get(k, 0.0) for r in rows]
            out[k] = sum(v) / len(v)
        out["n"] = len(rows)
        out["fps"] = 1000.0 / out["dt"] if out["dt"] else 0.0
        return out

    arms = collections.OrderedDict()
    for lab in ("A", "B"):
        rows = [r for r in frames if r["arm"] == lab]
        if rows:
            arms[lab] = summary(rows)

    segs = collections.OrderedDict()
    for r in frames:
        segs.setdefault((r["arm"], int((r["t"] - frames[0]["t"]) // (a.seg * 1e6))), []).append(r)

    if a.csv:
        cols = ["arm", "t", "req", "dt", "blocked", "running", "vblank_dt", "drain",
                "put_wait", "ref_drain", "ref_putwait"] + sorted({k for r in frames for k in r if k.startswith(("b_", "f_"))})
        with open(a.csv, "w") as f:
            f.write(",".join(cols) + "\n")
            for r in frames:
                f.write(",".join(str(r.get(c, "")) for c in cols) + "\n")

    lines = []
    lines.append("# Meter pacing: per-frame PPU busy/blocked\n")
    lines.append("Trace `%s`, marks `%s`, trigger wall %d ms.\n" % (a.trace, a.marks, a.origin_ms))
    lines.append("Main track: `%s` (tid %d); DisplayFlip `%s`; walker `%s`.\n" %
                 (names.get(main_t, main_t), main_t, names.get(flip_t, flip_t),
                  names.get(walk_t, walk_t)))
    lines.append("Frames used after the %.0f s switch drop; %.0f s segments.\n" % (a.drop, a.seg))
    lines.append("| per frame (ms) | %s |" % " | ".join("arm " + k for k in arms))
    lines.append("|---|" + "---:|" * len(arms))
    labels = [
        ("frame (flip to flip)", "dt"), ("ppu main blocked", "blocked"),
        ("ppu main running", "running"), ("vblank ticks/flip", "vblank_dt"),
        ("rsx walker drain", "drain"), ("rsx walker put_wait", "put_wait"),
        ("  inside main ref wait: drain", "ref_drain"),
        ("  inside main ref wait: producer", "ref_putwait"),
    ]
    for lab, k in labels:
        lines.append("| %s | %s |" % (lab, " | ".join("%.2f" % arms[x].get(k, 0.0) for x in arms)))
    lines.append("| fps (from frame dt) | %s |" %
                 " | ".join("%.2f" % arms[x]["fps"] for x in arms))
    lines.append("| frames | %s |" % " | ".join("%d" % arms[x]["n"] for x in arms))
    lines.append("\n## Where ppu main blocks\n")
    lines.append("| wait | %s |" % " | ".join("arm " + k for k in arms))
    lines.append("|---|" + "---:|" * len(arms))
    cats = sorted({k[2:] for x in arms for k in arms[x] if k.startswith("b_")},
                  key=lambda c: -max(arms[x].get("b_" + c, 0.0) for x in arms))
    for c in cats:
        lines.append("| %s | %s |" % (wait_cat.get(c, c),
                    " | ".join("%.2f" % arms[x].get("b_" + c, 0.0) for x in arms)))
    lines.append("\n## DisplayFlip waits (ms/frame)\n")
    lines.append("| wait | %s |" % " | ".join("arm " + k for k in arms))
    lines.append("|---|" + "---:|" * len(arms))
    for c in sorted({k[2:] for x in arms for k in arms[x] if k.startswith("f_")}):
        lines.append("| %s | %s |" % (c, " | ".join("%.2f" % arms[x].get("f_" + c, 0.0) for x in arms)))
    lines.append("\n## Per segment\n")
    lines.append("| seg | arm | frames | fps | main blocked | running | vblank/flip |")
    lines.append("|---:|---|---:|---:|---:|---:|---:|")
    for (arm, s), rows in sorted(segs.items(), key=lambda kv: kv[0][1]):
        sm = summary(rows)
        lines.append("| %d | %s | %d | %.2f | %.2f | %.2f | %.2f |" %
                     (s, arm, sm["n"], sm["fps"], sm["blocked"], sm["running"], sm["vblank_dt"]))
    text = "\n".join(lines) + "\n"
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    print(text)


if __name__ == "__main__":
    main()
