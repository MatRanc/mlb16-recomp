#!/usr/bin/env python3
"""Per-thread CPU time vs run-queue wait for the running game, over a window.

/proc/<pid>/task/<tid>/schedstat = on-CPU ns, run-queue wait ns (runnable but not
running), timeslices. No privileges needed. A thread with a large run-queue wait
is losing its core to other threads (CPU contention), not blocking on a lock.

Usage: thread_sched.py [seconds=20]   (finds the game by its build path)
"""
import os
import subprocess
import sys
import time


def game_pid() -> int:
    out = subprocess.run(["pgrep", "-of", "build-.*/MyGameRecomp"], capture_output=True, text=True).stdout.split()
    if not out:
        sys.exit("no game running")
    return int(out[0])


def sample(pid: int) -> dict:
    res = {}
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                name = f.read().strip()
            with open(f"/proc/{pid}/task/{tid}/schedstat") as f:
                run, wait, _ = (int(x) for x in f.read().split())
        except OSError:
            continue  # thread exited mid-sample
        res[int(tid)] = (name, run, wait)
    return res


def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    pid = game_pid()
    a = sample(pid)
    time.sleep(secs)
    b = sample(pid)
    rows = []
    for tid, (name, run, wait) in b.items():
        if tid not in a:
            continue
        rows.append((name, tid, (run - a[tid][1]) / 1e6 / secs, (wait - a[tid][2]) / 1e6 / secs))
    rows.sort(key=lambda r: -(r[2] + r[3]))
    print(f"pid {pid}, {secs:.0f} s window: ms per second of wall time")
    print(f"{'thread':<18}{'tid':>8}{'on-cpu':>9}{'runq-wait':>11}")
    for name, tid, run, wait in rows[:25]:
        if run + wait >= 1.0:
            print(f"{name:<18}{tid:>8}{run:>9.0f}{wait:>11.0f}")
    print(f"{'TOTAL':<26}{sum(r[2] for r in rows):>9.0f}{sum(r[3] for r in rows):>11.0f}"
          f"   ({os.cpu_count()} logical CPUs = {os.cpu_count() * 1000} ms/s)")


if __name__ == "__main__":
    main()
