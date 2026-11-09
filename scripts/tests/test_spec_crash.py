#!/usr/bin/env python3
"""Crash-during-xcommit test: crashat(n) panics at each crash point of a
speculative commit, the machine is rebooted on the SAME fs.img (so log
recovery runs), and the file must be wholly old or wholly new, never mixed.

For CPUS=1 and CPUS=3 and for n = 1, 2, 3, ... until a commit finishes
without crashing, each round:

  boot A  (fresh fs.img) : /cf = 3000 x 'o', xbegin, xrun spectest fill /cf n 3000
                           (a file change plus a console line, both held),
                           then `spectest crashcommit <id> n`  -> panic
  boot B  (same fs.img)  : recovery runs; `spectest state /cf o n`

Checked per round: the held console line was NOT printed by a commit that
crashed (it is released only after end_op returns); the file is old or new,
the state flips old -> new exactly once as n grows (commit point =
write_head), and the round that finishes prints the line and leaves the new
file.

Without the shadow table (Heril's PR, `spectest probe` says "no") file
changes are not held, so the file-state checks are reported SKIP and only
the output check runs (the one crash point after end_op).  The shadow cases
turn on by themselves once the table is merged.

Standard library only.  Raw QEMU logs: scripts/logs/spec_crash_<cpus>_<n>_<A|B>.log.
fs.img is restored afterwards.  Exits non-zero on any FAIL.

  ./scripts/tests/test_spec_crash.py [--cpus 1 3] [--max-points 80]
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_path_e2e as base  # noqa: E402  (Qemu, Timeout, prompts, ROOT)

CRASH_WAIT = 40   # seconds for crashcommit to panic or finish
MARK = b"spectest: wrote /cf"  # printed by the held `fill`; never typed by us


class Boot(base.Qemu):
    def __init__(self, cpus, tag):
        super().__init__(cpus, os.path.join(base.LOGDIR, "spec_crash_%d_%s.log" % (cpus, tag)))

    def login(self):
        self.boot()

    def panicked(self):
        return b"panic: " in self.buf

    def send_and_wait_end(self, cmd):
        """Send cmd; wait for a shell prompt or a kernel panic."""
        start = len(self.buf)
        self.send(cmd)
        deadline = time.time() + CRASH_WAIT
        while time.time() < deadline:
            tail = bytes(self.buf[start:])
            if b"panic: " in tail:
                time.sleep(1)       # let the rest of the panic text arrive
                self._pump(0.5)
                return "crashed", bytes(self.buf[start:])
            if tail.endswith(base.PROMPTS) and b"\n" in tail:
                return "finished", tail
            if not self._pump(0.2) and self.proc.poll() is not None:
                break
        return "timeout", bytes(self.buf[start:])


def spec_id(out):
    m = re.search(r"speculation (\d+)", out)
    if not m:
        raise RuntimeError("xbegin printed %r" % out)
    return m.group(1)


def probe(cpus):
    """Does the build have the shadow table?"""
    q = Boot(cpus, "probe")
    try:
        q.login()
        out = q.run("spectest probe", 60)
    finally:
        q.quit()
    return "shadow table: yes" in out


def round_(cpus, n, shadows, img, backup):
    """One crash round.  Returns (outcome, file_state, printed, recovered)."""
    shutil.copyfile(backup, img)
    a = Boot(cpus, "%d_A" % n)
    try:
        a.login()
        a.run("spectest fill /cf o 3000 create")
        a.run("sync")
        sid = spec_id(a.run("xbegin"))
        a.run("xrun %s spectest fill /cf n 3000" % sid)
        outcome, out = a.send_and_wait_end("spectest crashcommit %s %d" % (sid, n))
        printed = MARK in out
    finally:
        a.quit()
    b = Boot(cpus, "%d_B" % n)
    try:
        b.login()
        recovered = b"recovering" in b.buf
        out = b.run("spectest state /cf o n")
        m = re.search(r"state of /cf: (\w+)", out)
        state = m.group(1) if m else "unreadable"
    finally:
        b.quit()
    return outcome, state, printed, recovered


def run_config(cpus, max_points, img, backup, rows):
    shadows = probe(cpus)
    print("CPUS=%d: shadow table %s" % (cpus, "present" if shadows else "absent (file-state checks SKIP)"))
    states = []
    for n in range(1, max_points + 1):
        outcome, state, printed, recovered = round_(cpus, n, shadows, img, backup)
        label = "CPUS=%d crashat(%d)" % (cpus, n)
        if outcome == "crashed":
            ok_out = not printed
            if shadows:
                ok_state = state in ("old", "new")
                states.append(state)
                fstate = state
            else:
                ok_state = True
                fstate = "SKIP (needs shadow table)"
            rows.append((label, "crashed", fstate, "printed" if printed else "not printed",
                         "yes" if recovered else "no", ok_out and ok_state))
        elif outcome == "finished":
            ok_out = printed
            if shadows:
                ok_state = state == "new"
                fstate = state
            else:
                ok_state = True
                fstate = "SKIP (needs shadow table)"
            rows.append((label, "no crash (commit finished)", fstate,
                         "printed" if printed else "not printed", "-", ok_out and ok_state))
            break
        else:
            rows.append((label, "timeout", state, "printed" if printed else "not printed", "-", False))
            break
    else:
        rows.append(("CPUS=%d" % cpus, "never finished within %d points" % max_points, "-", "-", "-", False))
    if shadows:
        # old... then new..., exactly one flip; both must occur
        flips = sum(1 for x, y in zip(states, states[1:]) if x != y)
        ok = states == sorted(states, key=lambda s: s == "new") and flips == 1
        rows.append(("CPUS=%d file state is old until the commit point, then new" % cpus,
                     "-", " ".join(states), "-", "-", ok))
    else:
        rows.append(("CPUS=%d file state before/after the commit point" % cpus,
                     "-", "SKIP (needs shadow table)", "-", "-", True))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    ap.add_argument("--max-points", type=int, default=80)
    args = ap.parse_args()

    os.makedirs(base.LOGDIR, exist_ok=True)
    if os.system("make -C %s kernel/kernel fs.img >/dev/null" % base.ROOT) != 0:
        print("build failed")
        return 1
    img = os.path.join(base.ROOT, "fs.img")
    fd, backup = tempfile.mkstemp(prefix="fs.img.", suffix=".bak")
    os.close(fd)
    shutil.copyfile(img, backup)
    rows = []
    try:
        for cpus in args.cpus:
            run_config(cpus, args.max_points, img, backup, rows)
    finally:
        shutil.copyfile(backup, img)
        os.unlink(backup)

    head = ("crash point", "boot A", "file after reboot", "held output", "log recovered", "")
    table = [head] + [r[:5] + ("PASS" if r[5] else "FAIL",) for r in rows]
    widths = [max(len(str(r[i])) for r in table) for i in range(6)]
    print()
    for r in table:
        print("  ".join(str(c).ljust(w) for c, w in zip(r, widths)).rstrip())
    nfail = sum(1 for r in rows if not r[5])
    print("\n%d/%d passed" % (len(rows) - nfail, len(rows)))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
