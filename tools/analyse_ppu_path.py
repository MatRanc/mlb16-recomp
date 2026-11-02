#!/usr/bin/env python3
"""Map the PPU side of the per-frame serial chain from a PS3RECOMP_TRACE file.

The RSX FIFO walker is one host thread and is the serial resource: a frame is
walker drain + walker producer wait.  This tool measures that on a pitch-screen
trace and reports, per inter-flip frame:

* frame time (flip_request to flip_request);
* ppu main: time blocked, split by wait site (the ref-cond wait in
  FUN_000655cc is the PPU waiting for the walker; the lwcond EAs are resolved
  from the guest code, see docs/ppu-critical-path.md);
* rsx walker: drain (its own CPU) and put_wait, split into slot-filled-by-SPU
  (c690 production, before the PUT) and walker resume lag (after the PUT), and
  the no-SPU-PUT classes (park patched by the PPU vs caught up with put);
* ppu DisplayFlip: ref wait and flip-done wait;
* c690 cumulative job time across lanes.

It also reports how much of ppu main's ref wait the walker spent draining vs
waiting for a producer, which is the link that decides whether a fix has to
speed the walker up or overlap the producer.

The trace is line-oriented JSON (one object per line); the last line of a run
killed with SIGKILL may be partial and is dropped.

Usage:
  python3 tools/analyse_ppu_path.py TRACE [--skip-first N] [--skip-last N]
                                       [--md FILE] [--json FILE]
"""
import argparse
import bisect
import collections
import json
import sys


def load_events(path):
    out = []
    with open(path, "r", errors="replace") as f:
        for raw in f:
            line = raw.strip().rstrip(",")
            if not line.startswith("{"):
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


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


class Ev:
    __slots__ = ("name", "tid", "t0", "t1", "args")

    def __init__(self, name, tid, t0, t1, args):
        self.name, self.tid, self.t0, self.t1, self.args = name, tid, t0, t1, args


def parse(events):
    names = {}
    spans = collections.defaultdict(list)
    instants = collections.defaultdict(list)
    begins = collections.defaultdict(dict)
    for o in events:
        ph = o.get("ph")
        name = o.get("name", "")
        tid = int(o.get("tid", 0))
        args = o.get("args") or {}
        if ph == "M":
            if name == "thread_name":
                names[tid] = args.get("name", names.get(tid, ""))
            continue
        ts = float(o.get("ts", 0) or 0)
        if ph == "X":
            spans[(tid, name)].append(Ev(name, tid, ts, ts + float(o.get("dur", 0) or 0), args))
        elif ph == "i":
            instants[name].append(Ev(name, tid, ts, ts, args))
        elif ph == "b":
            begins[(tid, name)].setdefault(o.get("id"), []).append(Ev(name, tid, ts, ts, args))
        elif ph == "e":
            q = begins.get((tid, name), {}).get(o.get("id"))
            if q:
                e = q.pop(0)
                e.t1 = ts
                spans[(tid, name)].append(e)
    return names, spans, instants


def union(ivals):
    ivals = sorted((a, b) for a, b in ivals if b > a)
    total, cur = 0, None
    for a, b in ivals:
        if cur is None or a > cur[1]:
            if cur:
                total += cur[1] - cur[0]
            cur = [a, b]
        else:
            cur[1] = max(cur[1], b)
    if cur:
        total += cur[1] - cur[0]
    return total


def clip_union(ivals, lo, hi):
    return union([(max(a, lo), min(b, hi)) for a, b in ivals if min(b, hi) > max(a, lo)])


def pcts(vals):
    if not vals:
        return {"n": 0, "p50": 0.0, "p90": 0.0, "mean": 0.0}
    s = sorted(vals)
    return {
        "n": len(s),
        "p50": s[len(s) // 2],
        "p90": s[int(len(s) * 0.9)],
        "mean": sum(s) / len(s),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--skip-first", type=int, default=0, help="drop these leading frames")
    ap.add_argument("--skip-last", type=int, default=0, help="drop these trailing frames")
    ap.add_argument("--md", default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    names, spans, instants = parse(load_events(a.trace))
    tid_of = {v: k for k, v in names.items()}

    def tid(name):
        return tid_of.get(name)

    main_t = tid("ppu main")
    flip_t = tid("ppu DisplayFlip")
    walk_t = tid("rsx walker")
    if main_t is None or flip_t is None or walk_t is None:
        sys.exit("trace has no ppu main / ppu DisplayFlip / rsx walker track")

    flips = sorted(e.t0 for e in instants.get("gcm.flip_request", []))
    if len(flips) < 3:
        sys.exit("fewer than 3 flip requests in the trace window")
    f0 = a.skip_first
    f1 = len(flips) - 1 - a.skip_last
    if f1 <= f0 + 1:
        sys.exit("skip window leaves no frames")
    frames = [(flips[i], flips[i + 1]) for i in range(f0, f1)]

    def per_frame(ivals):
        return [clip_union(ivals, lo, hi) / 1000.0 for lo, hi in frames]

    # Blocking spans. ppu.gcm_finish is a poll heuristic (a run of ref reads),
    # not a wait: it is excluded from blocked time.
    main_block_names = {"ppu.lwcond", "ppu.lwmutex", "ppu.cond", "ppu.semaphore",
                        "ppu.event_queue", "ppu.event_flag", "ppu.usleep"}
    main_all = [(e.t0, e.t1) for (t, n), lst in spans.items()
                if t == main_t and n in main_block_names for e in lst]

    def main_ea(ea):
        return [(e.t0, e.t1) for e in spans.get((main_t, "ppu.lwcond"), [])
                if num(e.args.get("lwcond"), -1) == ea]

    # Walker waits, classified: a gcm.spu_put that overwrites the parked get
    # means the slot was filled by the SPU; before that PUT the wait is c690
    # production, after it the walker resume lag. No covering PUT: a nearby
    # rsx.park with the same get means the PPU patched the park; otherwise the
    # walker was caught up with put and waiting for the PPU to append.
    puts = sorted(((e.t0, e.args) for e in instants.get("gcm.spu_put", [])), key=lambda x: x[0])
    put_t = [t for t, _ in puts]
    parks = instants.get("rsx.park", [])

    def covering_put(t0, t1, get):
        i = bisect.bisect_left(put_t, t0)
        while i < len(puts) and puts[i][0] <= t1:
            t, args = puts[i]
            io, sz = num(args.get("io")), num(args.get("size"))
            if io <= get < io + sz:
                return t
            i += 1
        return None

    classes = collections.defaultdict(list)
    walk_putwaits = []
    for e in spans.get((walk_t, "rsx.put_wait"), []):
        get, put = num(e.args.get("get")), num(e.args.get("put"))
        walk_putwaits.append((e.t0, e.t1))
        tp = covering_put(e.t0, e.t1, get)
        if tp is not None:
            classes["c690_before_put"].append((e.t0, min(tp, e.t1)))
            classes["resume_after_put"].append((max(tp, e.t0), e.t1))
        else:
            parked = any(abs(p.t0 - e.t0) < 2000 and num(p.args.get("get")) == get for p in parks)
            classes["u_park_patched" if parked else "u_caught_up"].append((e.t0, e.t1))
    classes["put_wait_all"] = walk_putwaits

    drain = [(e.t0, e.t1) for e in spans.get((walk_t, "rsx.drain"), [])]
    flip_ref = [(e.t0, e.t1) for e in spans.get((flip_t, "ppu.lwcond"), [])
                if num(e.args.get("lwcond"), -1) == 0x1245090]
    flip_done = [(e.t0, e.t1) for e in spans.get((flip_t, "ppu.lwcond"), [])
                 if num(e.args.get("lwcond"), -1) == 0x1245100]
    main_ref = main_ea(0x1245090)

    dt = [(hi - lo) / 1000.0 for lo, hi in frames]
    win_us = frames[-1][1] - frames[0][0]
    out = {
        "window_us": win_us,
        "frames": len(frames),
        "frame_dt_ms": pcts(dt),
        "main_blocked_ms": pcts(per_frame(main_all)),
        "main_ref_wait_ms": pcts(per_frame(main_ref)),
        "walker_drain_ms": pcts(per_frame(drain)),
        "flip_ref_wait_ms": pcts(per_frame(flip_ref)),
        "flip_done_wait_ms": pcts(per_frame(flip_done)),
    }
    out["main_running_ms"] = pcts([d - b for d, b in
                                   zip(dt, per_frame(main_all))])
    for k, iv in classes.items():
        out[k + "_ms"] = pcts(per_frame(iv))

    # While ppu main is blocked on the ref cond, is the walker draining or
    # waiting for a producer? Per-frame union inside each ref-wait span.
    drain_in, putwait_in = [], []
    for lo, hi in frames:
        iv = [(max(x, lo), min(y, hi)) for x, y in main_ref if min(y, hi) > max(x, lo)]
        d = sum(clip_union(drain, x, y) for x, y in iv)
        w = sum(clip_union(walk_putwaits, x, y) for x, y in iv)
        drain_in.append(d / 1000.0)
        putwait_in.append(w / 1000.0)
    out["main_ref_drain_ms"] = pcts(drain_in)
    out["main_ref_putwait_ms"] = pcts(putwait_in)

    c690 = [(e.t0, e.t1) for (t, n), lst in spans.items() if n == "c690.job" for e in lst]
    # Cumulative means the sum over lanes, so do not union overlapping jobs.
    c690_sum = [sum(max(0.0, min(y, hi) - max(x, lo)) for x, y in c690) / 1000.0
                for lo, hi in frames]
    out["c690_cumulative_ms"] = pcts(c690_sum)
    out["c690_jobs_per_frame"] = sum(1 for t, _ in c690
                                     if frames[0][0] <= t < frames[-1][1]) / max(1, len(frames))

    # Top blocked tracks, for the per-thread table.
    tot_by_tid = collections.Counter()
    for (t, n), lst in spans.items():
        if t in names and n in main_block_names:
            tot_by_tid[t] += union([(e.t0, e.t1) for e in lst])
    out["ppu_blocked_by_thread_s"] = {
        names.get(t, str(t)): tot_by_tid[t] / 1e6 for t in tot_by_tid
        if names.get(t, "").startswith("ppu ")
    }

    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)

    def line(label, key):
        v = out[key]
        return "| %s | %.2f | %.2f | %.2f |" % (label, v["p50"], v["p90"], v["mean"])

    md = []
    md.append("# PPU critical path\n")
    md.append("Trace: `%s`\n" % a.trace)
    md.append("Window %.2f s, %d frames, %.2f fps\n" %
              (out["window_us"] / 1e6, out["frames"],
               out["frames"] / (out["window_us"] / 1e6)))
    md.append("| per frame (ms) | p50 | p90 | mean |")
    md.append("|---|---:|---:|---:|")
    md.append(line("frame (flip to flip)", "frame_dt_ms"))
    md.append(line("ppu main blocked", "main_blocked_ms"))
    md.append(line("  of which ref cond 0x1245090", "main_ref_wait_ms"))
    md.append(line("ppu main running (dt - blocked)", "main_running_ms"))
    md.append(line("rsx walker drain (CPU)", "walker_drain_ms"))
    md.append(line("rsx walker put_wait", "put_wait_all_ms"))
    md.append(line("  c690 production (before PUT)", "c690_before_put_ms"))
    md.append(line("  resume lag (after PUT)", "resume_after_put_ms"))
    md.append(line("  park patched by PPU", "u_park_patched_ms"))
    md.append(line("  caught up with PPU put", "u_caught_up_ms"))
    md.append(line("ppu DisplayFlip ref wait", "flip_ref_wait_ms"))
    md.append(line("ppu DisplayFlip flip-done wait", "flip_done_wait_ms"))
    md.append(line("c690 cumulative (all lanes)", "c690_cumulative_ms"))
    md.append("")
    md.append("While ppu main waits on the ref cond:")
    md.append(line("  walker draining", "main_ref_drain_ms"))
    md.append(line("  walker waiting for a producer", "main_ref_putwait_ms"))
    md.append("")
    md.append("c690 jobs/frame: %.1f\n" % out["c690_jobs_per_frame"])
    md.append("| PPU track | blocked s (window) |")
    md.append("|---|---:|")
    for name, s in sorted(out["ppu_blocked_by_thread_s"].items(), key=lambda x: -x[1])[:12]:
        md.append("| %s | %.3f |" % (name, s))
    text = "\n".join(md) + "\n"
    if a.md:
        with open(a.md, "w") as f:
            f.write(text)
    print(text)
    print("SUMMARY " + json.dumps(out))


if __name__ == "__main__":
    main()
