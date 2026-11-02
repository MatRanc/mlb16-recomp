#!/usr/bin/env python3
"""Attribute the RSX FIFO walker's SPU wait to the jobs that caused it.

Reads a PS3RECOMP_TRACE timeline recorded with the c690/spurs events
(c690.job, spurs.kick, gcm.spu_put, gcm.label_rel, rsx.park) plus the
existing rsx.put_wait, spu.policy_module, spu.workload and gcm.flip events.
For every walker wait it finds the SPU PUT that filled the parked ring slot,
the c690 job on that worker that produced it, and splits the wait into:

  a  not_pushed        the job's workload had not been admitted yet
  b  queued_idle       pushed, waiting to start while some lane was idle
  c  queued_all_busy   pushed, waiting to start while every lane was busy
  d  running           the attributed job was executing
  e  post_finish       after the job finished, before the walker resumed

It also reports per-lane busy/c690/other/idle time per frame and the
non-c690 overlays that loaded on the workers.

The trace format is line-oriented JSON, so a file cut short by SIGKILL still
parses (the last partial line is dropped).
"""
import argparse
import bisect
import collections
import json
import statistics
import sys


def load_events(path):
    events = []
    with open(path, "r", errors="replace") as f:
        for raw in f:
            line = raw.strip().rstrip(",")
            if not line.startswith("{"):
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue  # a partial last line, or the array brackets
    return events


def num(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


def hx(v, d=0):
    """Trace address fields are written as "0x..." strings."""
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
    __slots__ = ("name", "tid", "t0", "t1", "args", "ph")

    def __init__(self, name, tid, t0, t1, args, ph):
        self.name, self.tid, self.t0, self.t1, self.args, self.ph = \
            name, tid, t0, t1, args, ph

    def __repr__(self):
        return f"{self.name}@{self.t0:.0f}-{self.t1:.0f}"


def parse(events):
    names = {}
    spans = collections.defaultdict(list)          # name -> [Ev]
    instants = collections.defaultdict(list)       # name -> [Ev]
    async_begins = collections.defaultdict(dict)   # name -> id -> [Ev]
    out_waits = []
    for o in events:
        ph = o.get("ph")
        name = o.get("name", "")
        tid = int(o.get("tid", 0))
        if ph == "M":
            if name == "thread_name":
                names[tid] = (o.get("args") or {}).get("name", names.get(tid, ""))
            continue
        ts = num(o.get("ts"))
        args = o.get("args") or {}
        if ph == "X":
            ev = Ev(name, tid, ts, ts + num(o.get("dur")), args, ph)
            spans[name].append(ev)
        elif ph == "i":
            instants[name].append(Ev(name, tid, ts, ts, args, ph))
        elif ph == "b":
            aid = o.get("id")
            async_begins[name].setdefault(aid, []).append(
                Ev(name, tid, ts, ts, args, ph))
        elif ph == "e":
            aid = o.get("id")
            q = async_begins.get(name, {}).get(aid)
            if not q:
                continue
            b = q.pop(0)
            b.t1 = ts
            out_waits.append(b)
    # any begin without an end: a span that outlived the window, drop it
    for name, byid in async_begins.items():
        for aid, q in byid.items():
            for b in q:
                b.t1 = b.t0  # keep as zero-length, flagged below
    waits = [w for w in out_waits if w.name == "rsx.put_wait" and w.t1 > w.t0]
    for lst in list(spans.values()) + list(instants.values()):
        lst.sort(key=lambda e: (e.t0, e.t1))
    return names, spans, instants, waits


class Timeline:
    """Sorted interval index per thread for fast overlap/point queries."""

    def __init__(self, spans):
        self.spans = sorted(spans, key=lambda e: e.t0)
        self.starts = [e.t0 for e in self.spans]
        self.maxdur = max((e.t1 - e.t0 for e in self.spans), default=0)

    def active_at(self, t):
        i = bisect.bisect_right(self.starts, t) - 1
        # a span can start far before t and still contain it; walk back a little
        j = i
        while j >= 0 and self.starts[j] >= t - self.maxdur:
            if self.spans[j].t1 > t:
                return True
            j -= 1
        return False

    def containing(self, t):
        i = bisect.bisect_right(self.starts, t) - 1
        j = i
        while j >= 0 and self.starts[j] >= t - self.maxdur:
            if self.spans[j].t0 <= t < self.spans[j].t1:
                return self.spans[j]
            j -= 1
        return None

    def nearest_before(self, t, slack=0.0):
        i = bisect.bisect_right(self.starts, t + slack) - 1
        return self.spans[i] if i >= 0 else None


def union_len(intervals, lo, hi):
    if hi <= lo:
        return 0.0
    clipped = sorted((max(lo, a), min(hi, b)) for a, b in intervals if b > lo and a < hi)
    total, cur_a, cur_b = 0.0, None, None
    for a, b in clipped:
        if cur_b is None:
            cur_a, cur_b = a, b
        elif a <= cur_b:
            cur_b = max(cur_b, b)
        else:
            total += cur_b - cur_a
            cur_a, cur_b = a, b
    if cur_b is not None:
        total += cur_b - cur_a
    return total


def pct(vals, p):
    if not vals:
        return 0.0
    vals = sorted(vals)
    i = min(len(vals) - 1, max(0, int(round((len(vals) - 1) * p / 100.0))))
    return vals[i]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--manifest", help="jobmods_manifest.json from recomp-work/spu_lifted")
    ap.add_argument("--json", help="write the summary as JSON here")
    ap.add_argument("--md", help="write the markdown report here")
    ap.add_argument("--sample-us", type=float, default=25.0,
                    help="granularity for the lane-idle fraction (default 25 us)")
    a = ap.parse_args()

    events = load_events(a.trace)
    if not events:
        sys.exit("no events parsed from " + a.trace)
    names, spans, instants, waits = parse(events)

    # A title flips either through the FIFO 0xFEAD marker (gcm.flip) or the
    # direct cellGcmSetFlip path (gcm.flip_request); MLB The Show 16 uses the
    # latter. Prefer whichever this run recorded.
    flips = (instants.get("gcm.flip") or instants.get("gcm.flip_request")
             or instants.get("gcm.flip_done") or [])
    if flips:
        win0, win1 = flips[0].t0, flips[-1].t0
    else:
        all_t = [e.t0 for evs in instants.values() for e in evs] + \
                [e.t0 for evs in spans.values() for e in evs]
        win0, win1 = min(all_t), max(all_t)
    win_dur = max(1e-9, win1 - win0)

    policies = spans.get("spu.policy_module", [])
    jobs = spans.get("c690.job", [])
    workloads = spans.get("spu.workload", [])
    kicks = instants.get("spurs.kick", [])
    puts = instants.get("gcm.spu_put", [])
    labels = instants.get("gcm.label_rel", [])
    parks = instants.get("rsx.park", [])
    overlays = instants.get("spu.overlay_load", [])

    lanes = sorted({e.tid for e in policies})
    lane_index = {tid: i for i, tid in enumerate(lanes)}
    inits = instants.get("spurs.init", [])
    n_lanes = max([int(e.args.get("lanes", 0)) for e in inits] + [len(lanes)])
    pol_by_tid = {tid: Timeline([e for e in policies if e.tid == tid]) for tid in lanes}
    job_by_tid = collections.defaultdict(list)
    for j in jobs:
        job_by_tid[j.tid].append(j)
    job_tl = {tid: Timeline(v) for tid, v in job_by_tid.items()}

    # ---- job durations ---------------------------------------------------
    job_durs = [j.t1 - j.t0 for j in jobs]
    j_p50, j_p90 = pct(job_durs, 50), pct(job_durs, 90)
    long_tail = {id(j): (j.t1 - j.t0) for j in jobs if j.t1 - j.t0 > j_p90}

    # ---- kick lookup -----------------------------------------------------
    # (wid, lane) -> sorted kick times
    kick_by_wid_lane = collections.defaultdict(list)
    kick_by_wid = collections.defaultdict(list)
    for k in kicks:
        wid = int(k.args.get("wid", 0))
        lr = int(k.args.get("lane_ready", 0))
        lane, ready = lr & 0xFF, lr >> 8
        kick_by_wid_lane[(wid, lane)].append(k.t0)
        kick_by_wid[wid].append((k.t0, lane, ready))
    for v in kick_by_wid_lane.values():
        v.sort()
    for v in kick_by_wid.values():
        v.sort()

    # ---- PUT lookup ------------------------------------------------------
    put_starts = [p.t0 for p in puts]
    lane_busy = {tid: pol_by_tid[tid] for tid in lanes}

    def lane_busy_count(t):
        return sum(1 for tid in lanes if lane_busy[tid].active_at(t))

    def frac_all_busy(lo, hi):
        if hi <= lo:
            return 0.0
        step = max(a.sample_us, 1.0)
        n = max(1, int((hi - lo) / step))
        # sample the middle of each cell; cap the count so huge waits stay cheap
        n = min(n, 4000)
        step = (hi - lo) / n
        busy = 0
        for i in range(n):
            t = lo + (i + 0.5) * step
            # all configured lanes, including lanes that never ran a policy
            if lane_busy_count(t) >= n_lanes:
                busy += 1
        return busy / n

    def job_for_put(p):
        tl = job_tl.get(p.tid)
        if not tl:
            return None
        j = tl.containing(p.t0)
        if j:
            return j
        j = tl.nearest_before(p.t0, slack=500.0)
        if j and j.t1 >= p.t0 - 500.0:
            return j
        return None

    def policy_for_job(j):
        tl = pol_by_tid.get(j.tid)
        return tl.containing(j.t0) if tl else None

    def push_for(j, pol):
        if not pol:
            return j.t0, None
        wid = int(pol.args.get("wid", 0))
        lane = lane_index.get(j.tid)
        cand = kick_by_wid_lane.get((wid, lane), [])
        i = bisect.bisect_right(cand, j.t0) - 1
        if i >= 0 and cand[i] >= pol.t0 - 2000:
            return cand[i], wid
        # the lane can differ from the one the coordinator finally used
        cand = kick_by_wid.get(wid, [])
        i = bisect.bisect_right(cand, j.t0) - 1
        if i >= 0 and cand[i][0] >= pol.t0 - 2000:
            return cand[i][0], wid
        return pol.t0, wid

    # ---- classify waits --------------------------------------------------
    classes = collections.Counter()          # class -> us
    class_n = collections.Counter()
    class_by_frame = collections.defaultdict(collections.Counter)
    wait_rows = []
    matched_slot = 0
    matched_job = 0
    park_waits = 0
    push_delays = []
    queue_delays = []
    long_tail_us = 0.0
    normal_us = 0.0
    publish_lags = []
    resume_lags = []
    put_delays = []
    frame_starts = [f.t0 for f in flips]
    frame_idx = {f.t0: i for i, f in enumerate(flips)}

    def frame_of(t):
        i = bisect.bisect_right(frame_starts, t) - 1
        return max(0, i)

    for w in waits:
        t0, t1 = w.t0, w.t1
        get = hx(w.args.get("get"))
        put = hx(w.args.get("put"))
        dur = t1 - t0
        frame = frame_of(t0)
        park_hit = any(abs(pk.t0 - t0) < 2000 and hx(pk.args.get("get")) == get
                       for pk in parks)
        if park_hit:
            park_waits += 1

        # The causal PUT must overwrite the park word itself: its range has to
        # contain `get`. Nearby PUTs (the next slot in the same segment) are
        # not what unblocked this wait.
        i0 = bisect.bisect_left(put_starts, t0 - 5000.0)
        i1 = bisect.bisect_right(put_starts, t1 + 2000.0)
        cand = puts[i0:i1]
        slot = [p for p in cand
                if p.args.get("io") is not None
                and hx(p.args.get("io"), -1) <= get <
                    hx(p.args.get("io"), -1) + hx(p.args.get("size"))]
        if slot:
            matched_slot += 1
            slot.sort(key=lambda p: p.t0)
            before = [x for x in slot if x.t0 <= t1 + 1000.0]
            p = before[-1] if before else slot[0]
        else:
            p = None       # no PUT overwrote the park: not a c690 job wait

        j = job_for_put(p) if p else None
        if j:
            matched_job += 1

        if p is None:
            ucls = "u_park_patched" if park_hit else "u_caught_up"
            classes[ucls] += dur
            class_by_frame[frame][ucls] += dur
            class_n[ucls] += 1
            wait_rows.append({
                "t": t0, "dur": dur, "get": get, "put": put,
                "slot": False, "job": bool(j), "job_desc": None,
                "job_dur": None, "push_lag": None, "lane": None,
                "park": park_hit, "classes": {ucls: dur},
            })
            continue
        if j is None:
            classes["u_put_no_job"] += dur
            class_by_frame[frame]["u_put_no_job"] += dur
            class_n["u_put_no_job"] += 1
            wait_rows.append({
                "t": t0, "dur": dur, "get": get, "put": put,
                "slot": True, "job": False, "job_desc": None,
                "job_dur": None, "push_lag": None,
                "lane": lane_index.get(p.tid),
                "classes": {"u_put_no_job": dur},
            })
            continue
        pol = policy_for_job(j) if j else None
        push, wid = push_for(j, pol) if j else (t0, None)
        js, je = (j.t0, j.t1) if j else (t0, t1)

        addr = collections.Counter()

        def add(cls, us):
            if us <= 0:
                return
            classes[cls] += us
            class_by_frame[frame][cls] += us
            addr[cls] += us

        # running: the attributed job is executing
        run0, run1 = max(t0, js), min(t1, je)
        if run1 > run0:
            add("d_running", run1 - run0)
            if j and id(j) in long_tail:
                long_tail_us += run1 - run0
            else:
                normal_us += run1 - run0

        # before the job started: not pushed / queued
        pre_end = min(t1, js)
        if pre_end > t0:
            a_end = min(pre_end, push)
            if a_end > t0:
                add("a_not_pushed", a_end - t0)
            q0 = max(t0, push)
            if pre_end > q0:
                allb = frac_all_busy(q0, pre_end)
                if allb >= 0.5:
                    add("c_queued_all_busy", pre_end - q0)
                else:
                    add("b_queued_idle", pre_end - q0)
                queue_delays.append(pre_end - q0)

        # after it finished: split into "the PUT had not happened yet" and
        # "the walker had not run again yet"
        post0 = max(t0, je)
        if t1 > post0:
            if p and p.t0 > post0:
                pub_end = min(p.t0, t1)
                add("e_publish", pub_end - post0)
                if t1 > pub_end:
                    add("e_walker", t1 - pub_end)
            else:
                add("e_walker", t1 - post0)

        if j and push > t0:
            push_delays.append(push - t0)
        if j and p:
            publish_lags.append(p.t0 - je)
            resume_lags.append(max(0.0, t1 - p.t0))
            put_delays.append(p.t0 - t0)

        for cls, us in addr.items():
            class_n[cls] += 1
        wait_rows.append({
            "t": t0, "dur": dur, "get": get, "put": put,
            "slot": bool(slot), "job": bool(j),
            "job_desc": hx(j.args.get("desc")) if j else None,
            "job_dur": (je - js) if j else None,
            "push_lag": (push - t0) if j else None,
            "lane": lane_index.get(p.tid) if p else None,
            "classes": dict(addr),
        })

    # Any wait time not covered because a segment was zero-length: sanity.
    total_wait = sum(w.t1 - w.t0 for w in waits)
    accounted = sum(classes.values())
    slot_wait_us = sum(r["dur"] for r in wait_rows if r["slot"])
    noslot_wait_us = sum(r["dur"] for r in wait_rows if not r["slot"])
    slot_n = sum(1 for r in wait_rows if r["slot"])
    noslot_n = len(wait_rows) - slot_n

    # ---- per-frame / per-lane utilisation -------------------------------
    frame_rows = []
    for k in range(len(flips) - 1):
        f0, f1 = flips[k].t0, flips[k + 1].t0
        if f1 <= f0:
            continue
        row = {"frame": k, "t0": f0, "dur": f1 - f0, "lanes": {}}
        for tid in lanes:
            pol = [(e.t0, e.t1) for e in policies if e.tid == tid]
            cj = [(e.t0, e.t1) for e in job_by_tid.get(tid, [])]
            wl = [(e.t0, e.t1) for e in workloads if e.tid == tid]
            busy = union_len(pol + wl, f0, f1)
            c690 = union_len(cj, f0, f1)
            row["lanes"][tid] = {
                "busy": busy, "c690": c690,
                "other": max(0.0, busy - c690),
            }
        frame_rows.append(row)

    frames = len(frame_rows)
    fps = (len(flips) - 1) / (win_dur / 1e6) if len(flips) > 1 else 0.0
    lane_stats = {}
    for li, tid in enumerate(lanes):
        busy = sum(r["lanes"][tid]["busy"] for r in frame_rows)
        c690 = sum(r["lanes"][tid]["c690"] for r in frame_rows)
        other = sum(r["lanes"][tid]["other"] for r in frame_rows)
        total = sum(r["dur"] for r in frame_rows)
        lane_stats[tid] = {
            "name": names.get(tid, ""),
            "busy_pct": 100.0 * busy / total if total else 0.0,
            "c690_pct": 100.0 * c690 / total if total else 0.0,
            "other_pct": 100.0 * other / total if total else 0.0,
            "idle_pct": 100.0 * max(0.0, total - busy) / total if total else 0.0,
        }

    overlay_counts = collections.Counter()
    overlay_ea = {}
    for o in overlays:
        img = int(o.args.get("image", 0))
        overlay_counts[img] += 1
        overlay_ea[img] = hx(o.args.get("ea"))

    manifest = {}
    if a.manifest:
        try:
            for m in json.load(open(a.manifest)):
                manifest[m["ea"]] = m["name"]
        except (OSError, ValueError, KeyError):
            pass

    summary = {
        "trace": a.trace,
        "window_us": win_dur,
        "frames": frames,
        "fps": fps,
        "lanes_configured": n_lanes,
        "waits": len(waits),
        "wait_total_us": total_wait,
        "wait_per_frame_us": total_wait / frames if frames else 0.0,
        "slot_wait_us": slot_wait_us,
        "noslot_wait_us": noslot_wait_us,
        "slot_waits": slot_n,
        "noslot_waits": noslot_n,
        "slot_wait_per_frame_us": slot_wait_us / frames if frames else 0.0,
        "noslot_wait_per_frame_us": noslot_wait_us / frames if frames else 0.0,
        "slot_matched": matched_slot,
        "job_matched": matched_job,
        "park_waits": park_waits,
        "classes_us": dict(classes),
        "classes_pct": {k: (100.0 * v / total_wait if total_wait else 0.0)
                        for k, v in classes.items()},
        "classes_per_frame_us": {k: (v / frames if frames else 0.0)
                                 for k, v in classes.items()},
        "classes_pct_of_slot": {k: (100.0 * v / slot_wait_us if slot_wait_us else 0.0)
                                for k, v in classes.items()},
        "class_events": dict(class_n),
        "job_p50_us": j_p50,
        "job_p90_us": j_p90,
        "running_long_tail_us": long_tail_us,
        "running_normal_us": normal_us,
        "lanes": {str(k): v for k, v in lane_stats.items()},
        "overlays": {str(k): {"count": v, "ea": overlay_ea[k],
                              "module": manifest.get(overlay_ea[k], "")}
                     for k, v in overlay_counts.items()},
        "jobs": len(jobs),
        "jobs_per_frame": len(jobs) / frames if frames else 0.0,
        "kicks": len(kicks),
        "puts": len(puts),
        "labels": len(labels),
        "accounted_pct": 100.0 * accounted / total_wait if total_wait else 0.0,
        "queue_delay_p50_us": pct(queue_delays, 50),
        "queue_delay_p90_us": pct(queue_delays, 90),
        "push_lag_p50_us": pct(push_delays, 50),
        "publish_lag_p50_us": pct(publish_lags, 50),
        "publish_lag_p90_us": pct(publish_lags, 90),
        "resume_lag_p50_us": pct(resume_lags, 50),
        "resume_lag_p90_us": pct(resume_lags, 90),
        "put_delay_p50_us": pct(put_delays, 50),
        "put_delay_p90_us": pct(put_delays, 90),
        "wait_dur_p50_us": pct([w.t1 - w.t0 for w in waits], 50),
        "wait_dur_p90_us": pct([w.t1 - w.t0 for w in waits], 90),
    }

    def fmt_us(us):
        if us >= 1e6:
            return f"{us/1e6:.2f} s"
        return f"{us/1e3:.2f} ms"

    out = []
    P = out.append
    P("# Walker SPU-wait attribution\n")
    P(f"Trace: `{a.trace}`\n")
    P(f"Window: {fmt_us(win_dur)}, {frames} frames, {fps:.2f} fps\n")
    P(f"Workers/lanes that ran a policy module: {len(lanes)}\n")
    P(f"c690 jobs: {len(jobs)} ({summary['jobs_per_frame']:.1f}/frame); "
      f"job wall p50 {j_p50:.0f} us, p90 {j_p90:.0f} us\n")
    P(f"SPURS kicks: {len(kicks)}; SPU PUTs in RSX memory: {len(puts)}; "
      f"label releases: {len(labels)}; parks: {len(parks)}\n")
    P(f"\n## Walker waits\n")
    P(f"{len(waits)} waits, {fmt_us(total_wait)} total, "
      f"{fmt_us(summary['wait_per_frame_us'])}/frame; "
      f"p50 {summary['wait_dur_p50_us']:.0f} us, p90 {summary['wait_dur_p90_us']:.0f} us\n")
    P(f"Slot-matched PUT: {matched_slot}/{len(waits)}; attributed to a c690 job: "
      f"{matched_job}/{len(waits)}; parked: {park_waits}/{len(waits)}\n")
    P(f"Waits a SPU PUT explains: {slot_n} waits, {fmt_us(slot_wait_us)} "
      f"({summary['slot_wait_per_frame_us']/1e3:.2f} ms/frame, mean "
      f"{slot_wait_us/slot_n/1e3 if slot_n else 0:.2f} ms)\n")
    P(f"Waits no SPU PUT explains: {noslot_n} waits, {fmt_us(noslot_wait_us)} "
      f"({summary['noslot_wait_per_frame_us']/1e3:.2f} ms/frame, mean "
      f"{noslot_wait_us/noslot_n/1e3 if noslot_n else 0:.2f} ms) -- "
      f"PPU/park-patch or walker cadence, not c690 output.\n")
    P(f"The `u_` rows are waits that no SPU PUT explains; the classes a-d are "
      f"only meaningful over the slot-matched waits.\n")
    P("\n| class | ms | % of all wait | % of slot wait | waits touching |")
    P("|---|---:|---:|---:|---:|")
    labels_c = [
        ("a_not_pushed", "a job not yet pushed"),
        ("b_queued_idle", "b pushed, queued with an idle lane"),
        ("c_queued_all_busy", "c pushed, queued with all lanes busy"),
        ("d_running", "d job running"),
        ("e_publish", "e1 finished, PUT not yet visible"),
        ("e_walker", "e2 PUT visible, walker not resumed"),
        ("u_caught_up", "u caught up with put (no park, PPU append)"),
        ("u_park_patched", "u park patched by a non-SPU writer"),
        ("u_put_no_job", "u PUT touched the slot, no c690 job on that worker"),
    ]
    for key, label in labels_c:
        us = classes.get(key, 0)
        pct_slot = summary["classes_pct_of_slot"].get(key, 0) if not key.startswith("u_") else 0.0
        P(f"| {label} | {us/1e3:.2f} | {summary['classes_pct'].get(key, 0):.1f} | "
          f"{pct_slot:.1f} | {class_n.get(key, 0)} |")
    P(f"\nAccounted: {summary['accounted_pct']:.1f}% of total wait time\n")
    P(f"Running split: long-tail (>p90 job) {fmt_us(long_tail_us)}, "
      f"normal {fmt_us(normal_us)}\n")
    P(f"Queue delay when queued: p50 {summary['queue_delay_p50_us']:.0f} us, "
      f"p90 {summary['queue_delay_p90_us']:.0f} us\n")
    P(f"Wait time before the attributed job's kick: p50 "
      f"{summary['push_lag_p50_us']:.0f} us\n")
    P(f"Wait start to the slot-filling PUT: p50 "
      f"{summary['put_delay_p50_us']:.0f} us, p90 "
      f"{summary['put_delay_p90_us']:.0f} us; PUT to walker resume: p50 "
      f"{summary['resume_lag_p50_us']:.0f} us, p90 "
      f"{summary['resume_lag_p90_us']:.0f} us\n")
    P(f"Job end to its PUT: p50 {summary['publish_lag_p50_us']:.0f} us (the "
      f"job emits the slot before its end command), p90 "
      f"{summary['publish_lag_p90_us']:.0f} us\n")

    P("\n## Worker utilisation\n")
    P("| lane | thread | busy | c690 | other | idle |")
    P("|---:|---|---:|---:|---:|---:|")
    for tid in lanes:
        s = lane_stats[tid]
        P(f"| {lane_index[tid]} | {s['name'] or tid} | {s['busy_pct']:.1f}% | "
          f"{s['c690_pct']:.1f}% | {s['other_pct']:.1f}% | {s['idle_pct']:.1f}% |")
    total_lane_us = sum(r["dur"] for r in frame_rows) * max(1, n_lanes)
    busy_avg = 100.0 * sum(s["busy"] for r in frame_rows for s in r["lanes"].values()) / total_lane_us if total_lane_us else 0
    c690_avg = 100.0 * sum(s["c690"] for r in frame_rows for s in r["lanes"].values()) / total_lane_us if total_lane_us else 0
    P(f"\nConfigured lanes: {n_lanes} ({len(lanes)} seen in the trace). "
      f"Mean lane busy {busy_avg:.1f}%, c690 {c690_avg:.1f}% (missing lanes counted idle)\n")
    if overlay_counts:
        P("\nJob-module overlays loaded by the workers:\n")
        P("| image | EA | module | loads |")
        P("|---:|---|---|---:|")
        for img, cnt in overlay_counts.most_common():
            ea = overlay_ea[img]
            P(f"| {img} | 0x{ea:08X} | {manifest.get(ea, '?')} | {cnt} |")
    P("")

    text = "\n".join(out)
    print(text)
    if a.md:
        open(a.md, "w").write(text + "\n")
    if a.json:
        json.dump(summary, open(a.json, "w"), indent=2)
    # compact machine-readable line for quick diffing
    print("SUMMARY " + json.dumps({k: v for k, v in summary.items()
                                   if k not in ("lanes",)}, default=str))


if __name__ == "__main__":
    main()
