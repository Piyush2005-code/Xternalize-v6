#!/usr/bin/env python3
"""Cost of file writes inside a speculation vs outside (user/speccost.c).

For 1, 2, 4, 8, 16, 32, 48, 56 and 64 KB it measures, in uptime ticks (a tick
is 1/10 s in this xv6), the cost per KB written of:

  out     plain writes
  in      writes held in shadow blocks (one speculation, aborted at the end)
  commit  xbegin + writes + xcommit per repetition (shadows applied to disk)
  abort   xbegin + writes + xabort per repetition

The repetition count is doubled until a measurement covers at least
MIN_TICKS ticks, so the 0.1 s clock is not the noise.  When the shadow table
runs out the write fails: `fails` counts those repetitions and the cliff is
left in the data, not cropped.  Without the shadow table (`spectest probe`
says "no") the inside modes just write to disk, so the run is labelled.

Output: scripts/logs/spec_cost_<cpus>.csv and, if matplotlib is installed,
scripts/logs/spec_cost_<cpus>.png.  Standard library otherwise.

  ./scripts/bench_spec_cost.py [--cpus 1] [--sizes 1 2 4 ...]
"""

import argparse
import csv
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests"))
import test_path_e2e as base  # noqa: E402

MODES = ("out", "in", "commit", "abort")
SIZES = (1, 2, 4, 8, 16, 32, 48, 56, 64)
MIN_TICKS = 10
MAX_REPS = 4096
TICK_MS = 100


def measure(q, mode, kb):
    reps = max(1, 64 // kb)
    while True:
        out = q.run("speccost %s %d %d" % (mode, kb, reps), 300)
        m = re.search(r"ticks=(\d+) fails=(\d+)", out)
        if not m:
            return reps, None, None, out
        ticks, fails = int(m.group(1)), int(m.group(2))
        if ticks >= MIN_TICKS or reps >= MAX_REPS or fails:
            return reps, ticks, fails, out
        reps *= 2


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--sizes", type=int, nargs="+", default=list(SIZES))
    args = ap.parse_args()

    logs = base.LOGDIR
    os.makedirs(logs, exist_ok=True)
    if os.system("make -C %s kernel/kernel fs.img >/dev/null" % base.ROOT) != 0:
        print("build failed")
        return 1
    img = os.path.join(base.ROOT, "fs.img")
    fd, backup = tempfile.mkstemp(prefix="fs.img.", suffix=".bak")
    os.close(fd)
    shutil.copyfile(img, backup)

    rows = []
    q = base.Qemu(args.cpus, os.path.join(logs, "spec_cost_%d.log" % args.cpus))
    try:
        q.boot()
        shadows = "shadow table: yes" in q.run("spectest probe", 60)
        q.run("spectest fill /sc_cost x 65536 create", 60)
        print("shadow table %s" % ("present" if shadows else
              "ABSENT: inside-speculation modes write to disk, results are not meaningful"))
        for kb in args.sizes:
            for mode in MODES:
                reps, ticks, fails, out = measure(q, mode, kb)
                if ticks is None:
                    print("kb=%d mode=%s: no result: %r" % (kb, mode, out[-120:]))
                    continue
                ms_per_kb = ticks * TICK_MS / float(reps * kb)
                rows.append(dict(kb=kb, mode=mode, reps=reps, ticks=ticks, fails=fails,
                                 ms_per_kb=round(ms_per_kb, 3), shadows=int(shadows)))
                print("kb=%-3d %-6s reps=%-5d ticks=%-4d fails=%-4d %.3f ms/KB"
                      % (kb, mode, reps, ticks, fails, ms_per_kb))
    finally:
        q.quit()
        shutil.copyfile(backup, img)
        os.unlink(backup)

    path = os.path.join(logs, "spec_cost_%d.csv" % args.cpus)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["kb", "mode", "reps", "ticks", "fails", "ms_per_kb", "shadows"])
        w.writeheader()
        w.writerows(rows)
    print("wrote", path)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed: CSV only")
        return 0
    fig, ax = plt.subplots(figsize=(7, 4))
    for mode in MODES:
        pts = [(r["kb"], r["ms_per_kb"], r["fails"]) for r in rows if r["mode"] == mode]
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", label=mode)
        bad = [p for p in pts if p[2]]
        ax.plot([p[0] for p in bad], [p[1] for p in bad], "rx", markersize=10)
    ax.set_xscale("log", base=2)
    ax.set_xlabel("KB written per repetition")
    ax.set_ylabel("ms per KB written")
    ax.set_title("write cost, CPUS=%d (red x: writes failed, shadow table full)" % args.cpus)
    ax.legend()
    fig.tight_layout()
    png = os.path.join(logs, "spec_cost_%d.png" % args.cpus)
    fig.savefig(png, dpi=120)
    print("wrote", png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
