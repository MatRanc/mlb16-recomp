#!/usr/bin/env python3
"""Turn a PS3RECOMP_TIMELINE_HITCH_US binary timeline into a hitch table.

The play scripts write one PSTL timeline per session with
PS3RECOMP_TIMELINE_HITCH_US=250000: only frames longer than the threshold are
kept, together with the spans that ran inside them. This reads that file and
prints one row per hitch (time since the recording started, duration, the work
that dominated it) and a ranking of the causes.

Usage:
  tools/hitch_table.py recomp-work/play_20261103_163747/hitches.pstl
  tools/hitch_table.py hitches.pstl --min-ms 100 --csv hitches.csv

The timestamps are CLOCK_MONOTONIC ns; the PSTL header carries the recording
base, so "time" is seconds since the process started recording, not since the
shell launched it (the difference is the loader's startup, a few hundred ms).
"""
import argparse
import bisect
import collections
import csv
import struct
import sys

EV = {
    0: "none", 1: "spu.run", 2: "spu.interp", 3: "spu.job", 4: "spu.policy",
    5: "spu.workload", 6: "spu.mbox_wait", 7: "spu.signal_wait",
    8: "spu.event_wait", 9: "spu.resv_wait", 10: "spu.out_mbox_wait",
    11: "spu.lockline_retry", 12: "spu.tag_wait", 13: "rsx.drain",
    14: "rsx.sema_wait", 15: "rsx.put_wait", 16: "ppu.lwmutex",
    17: "ppu.lwcond", 18: "ppu.cond", 19: "ppu.semaphore",
    20: "ppu.event_queue", 21: "ppu.event_flag", 22: "gcm.wait_flip",
    23: "gcm.flip_poll", 24: "gcm.finish", 25: "ppu.usleep",
    26: "gcm.flip", 27: "gcm.flip_request", 28: "gcm.flip_vb",
    29: "gcm.flip_done", 30: "spu.overlay_load", 31: "spurs.kick",
    32: "spurs.init", 33: "c690.job", 34: "gcm.spu_put",
    35: "gcm.label_rel", 36: "rsx.park", 37: "gcm.ref_set",
    38: "gcm.ref_pub", 39: "ppu.frame_begin", 40: "ppu.frame_end",
    41: "gfx.frame_end", 42: "wait.wake_lag", 43: "rsx.idle_wait",
    44: "fifo.put_seen", 45: "rsx.wcb_done", 46: "rsx.draw_submit",
    47: "present.submit", 48: "present.fence_wait", 49: "present.queued",
    50: "present.vkqueue", 51: "present.shown", 52: "gcm.user_handler",
    53: "gcm.flip_req_seq", 54: "gcm.flip_handler", 55: "flip.wait_exit",
    56: "HITCH", 57: "pipe.create", 58: "shader.compile", 59: "tex.upload",
    60: "surface.create", 61: "fs.read", 62: "spurs.add_wl",
    63: "spurs.remove_wl", 64: "spu.overlay_verify", 65: "gcm.resync",
}

# Work that can make a frame long. Instants carry their cost in b (us).
WORK = {"pipe.create", "shader.compile", "tex.upload", "surface.create",
        "fs.read", "spurs.add_wl", "spurs.remove_wl", "gcm.resync",
        "c690.job", "rsx.drain", "rsx.wcb_done", "rsx.draw_submit",
        "present.fence_wait", "present.vkqueue", "spu.overlay_load",
        "spu.overlay_verify", "spu.run", "spu.job", "spu.workload"}
INSTANT_US = {"pipe.create"}


def read_timeline(path):
    """Return (base_ns, [(t0, t1, a, b, ev, tid), ...])."""
    with open(path, "rb") as f:
        hdr = f.read(20)
        if hdr[:4] != b"PSTL":
            raise SystemExit("not a PSTL timeline: %s" % path)
        base = struct.unpack_from("<Q", hdr, 12)[0]
        recs = []
        while True:
            b = f.read(12)
            if len(b) < 12:
                break
            kind, tid, n = struct.unpack("<III", b)
            if kind == 1:
                f.read(n + ((-n) % 4))
            elif kind == 2:
                data = f.read(32 * n)
                if len(data) < 32 * n:
                    break
                for i in range(n):
                    t0, t1, a, bb, ev, _ = struct.unpack_from("<QQIIII", data, 32 * i)
                    recs.append((t0, t1, a, bb, ev, tid))
            else:
                break
    return base, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("timeline", help="PSTL file written with PS3RECOMP_TIMELINE_HITCH_US")
    ap.add_argument("--min-ms", type=float, default=0.0,
                    help="ignore hitches shorter than this (default 0: all in the file)")
    ap.add_argument("--top", type=int, default=30, help="rows in the table (default 30)")
    ap.add_argument("--csv", help="also write the per-hitch table here")
    args = ap.parse_args()

    base, recs = read_timeline(args.timeline)
    hitches = [(t1, a) for (t0, t1, a, b, ev, tid) in recs
               if ev == 56 and a >= args.min_ms * 1000.0]
    hitches.sort()
    if not hitches:
        print("no hitches >= %.0f ms in %s" % (args.min_ms, args.timeline))
        return 0

    windows = [(end - dur * 1000, end) for end, dur in hitches]
    wends = [w[1] for w in windows]
    per = [collections.defaultdict(float) for _ in windows]
    counts = [collections.Counter() for _ in windows]

    # Records are in time order per thread, not globally. Sort by t0 and give
    # each record to every window it overlaps; the windows are consecutive
    # frame intervals, so there is normally one.
    for (t0, t1, a, b, ev, tid) in sorted(recs):
        if ev == 56:
            continue
        name = EV.get(ev, str(ev))
        if name not in WORK:
            continue
        j = bisect.bisect_left(wends, t0)
        while j < len(windows) and windows[j][0] <= t1:
            w0, w1 = windows[j]
            if t1 >= w0 and t0 <= w1:
                if name in INSTANT_US:
                    dur = b * 1000.0
                else:
                    dur = max(0, min(t1, w1) - max(t0, w0))
                # Six SPU lanes run in parallel; one lane is the wall-time
                # contribution the census compares against the frame.
                if name == "spu.run":
                    dur /= 6.0
                per[j][name] += dur
                counts[j][name] += 1
            j += 1

    rows = []
    total = collections.Counter()
    dominant = collections.Counter()
    for i, (end, dur_us) in enumerate(hitches):
        w0 = windows[i][0]
        ms = dur_us / 1000.0
        best_name, best_ms = "-", 0.0
        for name, ns in per[i].items():
            total[name] += ns / 1e6
            if ns / 1e6 > best_ms:
                best_name, best_ms = name, ns / 1e6
        dominant[best_name] += 1
        detail = ""
        if "shader.compile" in per[i]:
            detail = "shader %.0f ms" % (per[i]["shader.compile"] / 1e6)
        elif "pipe.create" in per[i]:
            detail = "%d pipes, %.0f ms" % (counts[i]["pipe.create"],
                                            per[i]["pipe.create"] / 1e6)
        rows.append(((w0 - base) / 1e9, ms, best_name, best_ms, detail))

    print("timeline: %s" % args.timeline)
    print("hitches:  %d (>= %.0f ms), total %.1f s"
          % (len(rows), args.min_ms, sum(r[1] for r in rows) / 1000.0))
    print()
    print("%10s %10s  %-20s %10s  %s"
          % ("time(s)", "ms", "dominant", "cause ms", "detail"))
    for (t, ms, name, cost, detail) in rows[:args.top]:
        print("%10.2f %10.1f  %-20s %10.1f  %s" % (t, ms, name, cost, detail))
    if len(rows) > args.top:
        print("... %d more rows (--top)" % (len(rows) - args.top))

    print()
    print("cause ranking (clipped work inside the hitches, spu.run / 6):")
    print("%-22s %8s %12s" % ("cause", "hitches", "ms"))
    for name, ms in total.most_common():
        print("%-22s %8d %12.1f" % (name, dominant[name], ms))

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s", "ms", "dominant", "cause_ms", "detail"])
            for (t, ms, name, cost, detail) in rows:
                w.writerow(["%.3f" % t, "%.1f" % ms, name, "%.1f" % cost, detail])
        print("\ncsv: %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
