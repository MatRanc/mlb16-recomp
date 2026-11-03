#!/usr/bin/env python3
"""Hitch census for a PSTL binary timeline written with
PS3RECOMP_TIMELINE_HITCH_US: parse the hitch windows, attribute the records
inside them, and draw a whole-run frame-time plot annotated by cause.

Usage: analyse_hitches.py --timeline TL --log GAME.LOG --markers MARKERS --out DIR
"""
import argparse
import bisect
import collections
import csv
import os
import re
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
# Waits: useful as evidence of what the frame thread was blocked on.
WAIT = {"ppu.lwcond", "ppu.lwmutex", "ppu.cond", "ppu.semaphore",
        "ppu.event_queue", "ppu.event_flag", "ppu.usleep", "rsx.put_wait",
        "rsx.idle_wait", "rsx.sema_wait", "gcm.wait_flip", "gcm.flip_poll",
        "gcm.finish", "spu.mbox_wait", "spu.signal_wait", "spu.event_wait",
        "spu.resv_wait", "spu.out_mbox_wait", "spu.tag_wait"}


def read_markers(path):
    marks = []
    with open(path, "r") as f:
        for line in f:
            p = line.split(None, 1)
            if len(p) == 2:
                marks.append((int(p[0]), p[1].strip()))
    marks.sort()
    return marks


def parse_timeline(path, want_records, windows):
    """First pass: thread names and hitch events. Second pass: records that
    overlap a window. Returns (names, hitches, per_window)."""
    names = {}
    hitches = []
    w0 = [(w[0], w[1]) for w in windows]
    per = [collections.defaultdict(lambda: [0, 0.0]) for _ in windows]  # [count, clipped ns]
    by_thread = [collections.Counter() for _ in windows]
    runs = [collections.Counter() for _ in windows]
    fs_reads = [[] for _ in windows]
    pipes = [[] for _ in windows]
    hitches_out = []

    starts = [w[0] for w in windows]
    f = open(path, "rb")
    hdr = f.read(20)
    if hdr[:4] != b"PSTL":
        raise SystemExit("not a PSTL timeline: %s" % path)
    while True:
        b = f.read(12)
        if len(b) < 12:
            break
        kind, tid, n = struct.unpack("<III", b)
        if kind == 1:
            nm = f.read(n)
            names[tid] = nm.split(b"\0")[0].decode(errors="replace")
            pad = (-n) % 4
            if pad:
                f.read(pad)
        elif kind == 2:
            data = f.read(32 * n)
            if len(data) < 32 * n:
                break
            for i in range(n):
                t0, t1, a, b2, ev, _ = struct.unpack_from("<QQIIII", data, 32 * i)
                if ev == 56:
                    hitches.append((t0, a))
                    continue
                if not windows:
                    continue
                # windows sorted by start; a record can overlap at most a few.
                j = bisect.bisect_right(starts, t1) - 1
                while j >= 0 and windows[j][1] >= t0:
                    w = windows[j]
                    if t1 >= w[0] and t0 <= w[1]:
                        name = EV.get(ev, str(ev))
                        dur = max(0, min(t1, w[1]) - max(t0, w[0]))
                        cell = per[j][name]
                        cell[0] += 1
                        cell[1] += dur
                        by_thread[j][names.get(tid, str(tid))] += dur
                        if name == "fs.read":
                            fs_reads[j].append((t0, dur, a, b2, names.get(tid, str(tid))))
                        if name == "pipe.create":
                            pipes[j].append((t0, a, b2))
                    j -= 1
        else:
            break
    return names, hitches, per, by_thread, fs_reads, pipes


def scene_of(t, marks):
    # marks: launch, nav_start, load_start, pitch_reached, x_press, meter_up
    labels = {lbl: t for t, lbl in marks}
    if "x_press" in labels and t >= labels["x_press"]:
        return "pitch"
    if "pitch_reached" in labels and t >= labels["pitch_reached"]:
        return "cinematic"
    if "load_start" in labels and t >= labels["load_start"]:
        return "load/cinematic"
    if "nav_start" in labels and t >= labels["nav_start"]:
        return "menu"
    return "boot"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeline", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--markers", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-ms", type=float, default=50.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    marks = read_markers(args.markers)
    labels = {lbl: t for t, lbl in marks}
    t0 = labels.get("launch", marks[0][0] if marks else 0)

    # ---- pass 1: hitch windows -------------------------------------------
    names, hitches, *_ = parse_timeline(args.timeline, False, [])
    hitches = [(t0, dur) for t0, dur in hitches if dur >= args.min_ms * 1000.0]
    hitches.sort()
    windows = [(t0 - dur * 1000, t0) for t0, dur in hitches]
    keep = [i for i, (w, h) in enumerate(zip(windows, hitches)) if h[1] >= args.min_ms * 1000.0]
    windows = [windows[i] for i in keep]
    hitches = [hitches[i] for i in keep]

    # ---- pass 2: records inside the windows -------------------------------
    names, _, per, by_thread, fs_reads, pipes = parse_timeline(
        args.timeline, True, windows)

    # The guest frame thread is the one with the most flip requests.
    frame_tid = None
    flipcnt = collections.Counter()
    with open(args.timeline, "rb") as f:
        f.read(20)
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
                    ev = struct.unpack_from("<QQIIII", data, 32 * i)[4]
                    if ev == 53:
                        flipcnt[tid] += 1
            else:
                break
    if flipcnt:
        frame_tid = flipcnt.most_common(1)[0][0]
    frame_name = names.get(frame_tid, "?")

    # ---- load_end: the end of the big match-load hitch --------------------
    load_start = labels.get("load_start")
    pitch_reached = labels.get("pitch_reached")
    load_end = None
    if load_start is not None:
        cand = [t for t, dur in hitches
                if t >= load_start and (pitch_reached is None or t <= pitch_reached)]
        if cand:
            load_end = max(cand, key=lambda t: dict(hitches).get(t, 0))
            # use the end of the longest hitch in that interval
            best = max(((t, dur) for t, dur in hitches
                        if load_start <= t and (pitch_reached is None or t <= pitch_reached)),
                       key=lambda x: x[1], default=None)
            load_end = best[0] if best else None
    def scene2(t):
        s = scene_of(t, marks)
        if s == "load/cinematic":
            if load_end is not None and t > load_end:
                return "cinematic"
            return "load"
        return s

    # ---- per-hitch table and scene/cause rollups --------------------------
    rows = []
    cause_sum = collections.defaultdict(lambda: [0, 0.0])  # (scene,cause): count, ms
    for i, (wend, dur_us) in enumerate(hitches):
        w0, w1 = windows[i]
        cats = per[i]
        work = []
        for name, (cnt, ns) in cats.items():
            if name in WORK:
                cost = ns
                if name == "pipe.create":
                    cost = sum(b for _, _, b in pipes[i]) * 1000.0  # b is us
                work.append((cost, cnt, name))
        work.sort(reverse=True)
        # The raw union over six SPU lanes overweights parallel work; treat one
        # lane as the wall-time contribution. For the walker leaf work, prefer
        # the leaf over its parent rsx.drain.
        def ms(name):
            cnt, ns = cats.get(name, (0, 0.0))
            return ns / 1e6
        dur_ms = dur_us / 1000.0
        shader_ms, tex_ms, surf_ms = ms("shader.compile"), ms("tex.upload"), ms("surface.create")
        c690_ms, drain_ms = ms("c690.job"), ms("rsx.drain")
        spu_lane_ms = ms("spu.run") / 6.0
        present_ms = ms("present.fence_wait") + ms("present.vkqueue")
        fs_ms = ms("fs.read")
        if shader_ms >= 0.15 * dur_ms:
            top, top_ms = "shader.compile", shader_ms
        elif c690_ms >= 0.15 * dur_ms and c690_ms >= shader_ms:
            top, top_ms = "c690.job", c690_ms
        elif fs_ms >= 0.2 * dur_ms:
            top, top_ms = "fs.read", fs_ms
        elif spu_lane_ms >= 0.30 * dur_ms:
            top, top_ms = "spu.run", spu_lane_ms
        elif present_ms >= 0.3 * dur_ms:
            top, top_ms = "present", present_ms
        elif drain_ms >= 0.5 * dur_ms:
            top, top_ms = "rsx.drain", drain_ms
        elif work:
            top, top_ms = work[0][2], work[0][0] / 1e6
        else:
            top, top_ms = "-", 0.0
        scene = scene2(wend - dur_us * 1000 / 2)
        fsc = sum(ns for name, (cnt, ns) in cats.items() if name == "fs.read")
        fsbytes = sum(b for _, _, _, b, _ in fs_reads[i])
        fs_thr = collections.Counter(th for _, _, _, _, th in fs_reads[i])
        pcount = len(pipes[i])
        pms = sum(b for _, _, b in pipes[i]) / 1000.0
        rows.append(dict(
            start_s=(w0 - t0) / 1e9, end_s=(w1 - t0) / 1e9, dur_ms=dur_us / 1000.0,
            scene=scene, top=top, top_ms=top_ms,
            fs_ms=fsc / 1e6, fs_bytes=fsbytes, fs_count=len(fs_reads[i]),
            fs_threads=";".join(f"{k}:{v}" for k, v in fs_thr.most_common(3)),
            pipe_count=pcount, pipe_ms=pms,
            walker_ms=drain_ms, spu_lane_ms=spu_lane_ms,
            frame_thread=frame_name,
        ))
        for cost, cnt, name in work:
            key = (scene, name)
            cause_sum[key][0] += cnt
            cause_sum[key][1] += cost / 1e6

    # ---- write per-hitch CSV ---------------------------------------------
    with open(os.path.join(args.out, "hitches.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else
                           ["start_s", "end_s", "dur_ms", "scene", "top"])
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # ---- write cause rollup ----------------------------------------------
    with open(os.path.join(args.out, "causes.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scene", "cause", "count", "ms"])
        for (scene, cause), (cnt, ms) in sorted(
                cause_sum.items(), key=lambda kv: (kv[0][0], -kv[1][1])):
            w.writerow([scene, cause, cnt, f"{ms:.1f}"])

    # ---- print summary ----------------------------------------------------
    print(f"timeline: {args.timeline}")
    print(f"markers:  {args.markers}  frame thread: {frame_name}")
    print(f"hitches >= {args.min_ms:.0f} ms: {len(hitches)}")
    byscene = collections.Counter(r["scene"] for r in rows)
    print("by scene:", dict(byscene))
    print("\ntop hitches:")
    for r in sorted(rows, key=lambda x: -x["dur_ms"])[:15]:
        print(f"  {r['start_s']:8.1f}s {r['dur_ms']:9.1f}ms {r['scene']:14s} "
              f"{r['top']:16s} {r['top_ms']:8.1f}ms "
              f"pipes={r['pipe_count']} fs={r['fs_count']}/{r['fs_bytes']}B")
    print("\ncauses by scene (work only, ranked by ms):")
    for scene in ["boot", "menu", "load", "cinematic", "pitch"]:
        cs = sorted(((c, v) for (s, c), v in cause_sum.items() if s == scene),
                    key=lambda x: -x[1][1])
        if not cs:
            continue
        print(f"  {scene}:")
        for c, (cnt, ms) in cs[:8]:
            print(f"    {c:20s} {ms:10.1f}ms  n={cnt}")

    # ---- frame times from the flip log -----------------------------------
    frames = []
    pat = re.compile(r"\[fliplog\] n=(\d+) req=(\d+) fin=(\d+)")
    with open(args.log, "r", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                frames.append((int(m.group(1)), int(m.group(3))))
    frames.sort()
    ft = []
    for i in range(1, len(frames)):
        d = (frames[i][1] - frames[i - 1][1]) / 1000.0
        if d > 0:
            ft.append(((frames[i][1] - t0) / 1e9, d))
    with open(os.path.join(args.out, "frame_times.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "frame_ms"])
        for t, d in ft:
            w.writerow([f"{t:.3f}", f"{d:.3f}"])
    print(f"\nframes from flip log: {len(ft)}")

    # ---- plot -------------------------------------------------------------
    make_plot(args.out, ft, rows, marks, t0, hitches, windows, load_end)
    print(f"wrote {args.out}/hitch_timeline.png")


def make_plot(out, ft, rows, marks, t0, hitches, windows, load_end=None):
    from PIL import Image, ImageDraw, ImageFont
    W, H = 2400, 1200
    bx0, by0, bx1, by1 = 90, 40, W - 60, H - 90
    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)
    font = None
    for p in ("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if os.path.exists(p):
            try:
                font = ImageFont.truetype(p, 18)
                break
            except OSError:
                pass
    if font is None:
        font = ImageFont.load_default()
    small = font

    end_s = max([t for t, _ in ft] + [(w[1] - t0) / 1e9 for w in windows] + [1.0])
    xmax = max(1.0, end_s * 1.02)
    import math
    ymin, ymax = 1.0, 10000.0
    def X(t): return bx0 + (t / xmax) * (bx1 - bx0)
    def Y(ms):
        ms = max(ymin, min(ymax, ms))
        return by1 - (math.log10(ms / ymin) / math.log10(ymax / ymin)) * (by1 - by0)

    # scene bands
    sc_col = {"boot": (240, 240, 255), "menu": (235, 255, 235),
              "load": (255, 240, 225), "cinematic": (245, 235, 255),
              "pitch": (230, 245, 250)}
    labels = {lbl: t for t, lbl in marks}
    bounds = [("boot", labels.get("launch"), labels.get("nav_start")),
              ("menu", labels.get("nav_start"), labels.get("load_start")),
              ("load", labels.get("load_start"),
               load_end if load_end is not None else
               ((labels.get("load_start") + 20e9) if "load_start" in labels else None)),
              ("cinematic", load_end if load_end is not None else
               ((labels.get("load_start") + 20e9) if "load_start" in labels else None),
               labels.get("pitch_reached")),
              ("pitch", labels.get("pitch_reached"), None)]
    for name, a, b in bounds:
        if a is None:
            continue
        xa = X((a - t0) / 1e9)
        xb = X((b - t0) / 1e9) if b is not None else bx1
        d.rectangle([xa, by0, xb, by1], fill=sc_col.get(name, (248, 248, 248)))
        d.text((xa + 6, by0 + 6), name, fill=(90, 90, 90), font=small)

    # grid
    for ms in (10, 16.7, 33.3, 50, 100, 300, 1000, 5000):
        y = Y(ms)
        d.line([bx0, y, bx1, y], fill=(210, 210, 210))
        d.text((10, y - 10), f"{ms:g} ms", fill=(80, 80, 80), font=small)

    # frame-time line
    pts = [(X(t), Y(ms)) for t, ms in ft]
    if len(pts) > 1:
        d.line(pts, fill=(30, 80, 200), width=1)

    # hitch markers, coloured by dominate cause
    cc = {"shader.compile": (200, 30, 30), "pipe.create": (220, 100, 0),
          "tex.upload": (0, 140, 90), "surface.create": (140, 60, 160),
          "fs.read": (20, 120, 200), "rsx.drain": (100, 100, 100),
          "c690.job": (170, 120, 0), "present.fence_wait": (0, 160, 160),
          "present.vkqueue": (0, 160, 160), "gcm.resync": (0, 0, 0),
          "spurs.add_wl": (120, 80, 40), "spurs.remove_wl": (120, 80, 40)}
    for r in rows:
        x = X((r["start_s"] + r["end_s"]) / 2)
        y = Y(r["dur_ms"])
        col = cc.get(r["top"], (180, 0, 0))
        d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=col)
    # Label at most two hitches per scene, so the boot cluster stays readable.
    top = []
    for scene in ("load", "boot", "cinematic", "pitch"):
        n = 1 if scene == "boot" else 2
        rr = sorted((r for r in rows if r["scene"] == scene),
                    key=lambda r: -r["dur_ms"])[:n]
        top.extend(rr)
    for i, r in enumerate(top):
        x = X((r["start_s"] + r["end_s"]) / 2)
        y = Y(r["dur_ms"])
        txt = f"{r['dur_ms']:.0f} ms {r['scene']} {r['top']}"
        tx = min(x + 12, bx1 - 300)
        ty = max(by0 + 4, y - 26 - (i % 3) * 20)
        d.line([x, y, tx, ty + 8], fill=(120, 0, 0))
        d.text((tx, ty), txt, fill=(120, 0, 0), font=small,
               stroke_width=2, stroke_fill="white")

    # axes
    d.rectangle([bx0, by0, bx1, by1], outline=(0, 0, 0))
    d.text((bx0, by1 + 55), "time since launch (s)", fill=(0, 0, 0), font=font)
    d.text((W - 340, by0 - 32), "frame time (log scale)", fill=(0, 0, 0), font=font)
    for s in range(0, int(xmax) + 1, max(10, int(xmax / 20))):
        x = X(s)
        d.line([x, by1, x, by1 + 6], fill=(0, 0, 0))
        d.text((x - 12, by1 + 22), str(s), fill=(0, 0, 0), font=small)
    img.save(os.path.join(out, "hitch_timeline.png"))


if __name__ == "__main__":
    main()
