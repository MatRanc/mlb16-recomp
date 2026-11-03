#!/usr/bin/env python3
"""Launch one full MLB16 boot->menus->load->cinematics->pitch run with the
hitch-only timeline, and stamp phase markers on the same CLOCK_MONOTONIC.

The game path is the standard ab_live.sh configuration (config from
play_windowed.sh), SKIP_CINE=0.  It stops shortly after the pitch meter is
confirmed.  Markers are written as "<monotonic_ns> <label>" lines.
"""
import glob
import os
import re
import subprocess
import sys
import time

W = "/home/m/Documents/mlb16-online/recomp-work"
S = os.path.join(W, "ab-tools")
tag = os.environ.get("TAG", "hitch")
ts = time.strftime("%Y%m%d_%H%M%S")
markers = os.environ.get("MARKERS", f"/tmp/hitch_markers_{ts}.txt")
tl = os.environ.get("PS3RECOMP_TIMELINE", f"/tmp/hitch_tl_{ts}.bin")

env = os.environ.copy()
env["TAG"] = tag
env["N"] = os.environ.get("N", "1")
env["SEG"] = os.environ.get("SEG", "20")
env["DROP"] = "0"
env["SETTLE"] = os.environ.get("SETTLE", "10")
env["LIMIT"] = os.environ.get("LIMIT", "900")
env["REPORT"] = "0"
env["SCENE_SHOTS"] = "1"
env["METER"] = "1"
env["SKIP_CINE"] = "0"
env["PIN"] = os.environ.get("PIN", "1")
env["WCB"] = os.environ.get("WCB", "2")
env["VIDEODRIVER"] = os.environ.get("VIDEODRIVER", "x11")
env["BOOT"] = os.environ.get("BOOT", "40")
env["PITCH_WAIT_S"] = os.environ.get("PITCH_WAIT_S", "600")
env["PS3RECOMP_TIMELINE"] = tl
env["PS3RECOMP_TIMELINE_HITCH_US"] = os.environ.get("PS3RECOMP_TIMELINE_HITCH_US", "50000")
env["PS3RECOMP_TIMELINE_RING_MB"] = os.environ.get("PS3RECOMP_TIMELINE_RING_MB", "16")
env["PS3RECOMP_TIMELINE_FLUSH_MB"] = os.environ.get("PS3RECOMP_TIMELINE_FLUSH_MB", "2")
env["PS3RECOMP_PIPE_TIMING"] = "1"
env["PS3RECOMP_FLIP_LOG"] = "1"
env["PS3RECOMP_C690_STATS"] = "1"
env.setdefault("PS3RECOMP_VK_PIPELINE_CACHE", "1")

boot = int(env["BOOT"])
outer = open(f"/tmp/hitch_outer_{ts}.log", "wb")
p = subprocess.Popen(["bash", os.path.join(S, "ab_live.sh")],
                     stdout=outer, stderr=subprocess.STDOUT, env=env)

mark = open(markers, "w")
def stamp(label):
    mark.write(f"{time.monotonic_ns()} {label}\n")
    mark.flush()

t_launch = time.monotonic_ns()
stamp("launch")
stamp_nav_est = t_launch + boot * 1_000_000_000

patterns = [
    ("load_start", re.compile(r"^nav3: .*options|^nav3: .* > .*$")),
    ("pitch_reached", re.compile(r"pitch reached after")),
    ("x_press", re.compile(r"pressing X once")),
    ("meter_up", re.compile(r"pitch meter confirmed")),
]
seen = set()
log = None
pos = 0
while p.poll() is None:
    if log is None:
        cand = sorted(glob.glob(os.path.join(W, f"ab_live_{tag}_*.log")),
                      key=os.path.getmtime)
        if cand:
            log = cand[-1]
    if time.monotonic_ns() >= stamp_nav_est and "nav_start" not in seen:
        stamp("nav_start")
        seen.add("nav_start")
    if log:
        try:
            with open(log, "r", errors="replace") as f:
                f.seek(pos)
                data = f.read()
                pos = f.tell()
        except OSError:
            data = ""
        for line in data.splitlines():
            for label, pat in patterns:
                if label not in seen and pat.search(line):
                    stamp(label)
                    seen.add(label)
    time.sleep(0.2)

rc = p.wait()
stamp("game_exit")
mark.close()
outer.close()
print(f"markers={markers}")
print(f"timeline={tl}")
print(f"log={log}")
print(f"rc={rc}")
