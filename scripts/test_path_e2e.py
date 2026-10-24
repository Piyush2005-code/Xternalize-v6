#!/usr/bin/env python3
"""End-to-end test of command path resolution (execvp) in the real xv6 shell.

Builds xv6, boots `make qemu CPUS=<n>`, types commands at the shell and checks
the output, for CPUS=1 and CPUS=3.  Standard library only.

  ./scripts/test_path_e2e.py [--cpus 1 3] [--skip-usertests] [--timeout SECONDS]

The raw QEMU console log of each run is saved to scripts/logs/path_e2e_<n>.log.
fs.img is backed up first and restored after every boot, so a run never leaves
a modified fs.img behind.  Exits non-zero on any failure.
"""

import argparse
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.join(ROOT, "scripts", "logs")
PROMPT = b"$ "
CMD_TIMEOUT = 30        # seconds, default per command
BOOT_TIMEOUT = 60
PATHTEST_TIMEOUT = 240
USERTESTS_TIMEOUT = 900


class Timeout(Exception):
    pass


class Qemu:
    def __init__(self, cpus, logpath):
        self.buf = bytearray()
        self.log = open(logpath, "wb")
        self.proc = subprocess.Popen(
            ["make", "qemu", "CPUS=%d" % cpus], cwd=ROOT,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, start_new_session=True)
        os.set_blocking(self.proc.stdout.fileno(), False)

    def _pump(self, wait):
        r, _, _ = select.select([self.proc.stdout], [], [], wait)
        if r:
            try:
                data = os.read(self.proc.stdout.fileno(), 65536)
            except BlockingIOError:
                return True
            if not data:
                return False
            self.buf += data
            self.log.write(data)
            self.log.flush()
        return True

    def wait_prompt(self, start, timeout, what):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.buf[start:].endswith(PROMPT) and b"\n" in self.buf[start:]:
                return
            if not self._pump(0.2) and self.proc.poll() is not None:
                raise Timeout("QEMU exited while waiting for prompt after: " + what)
        raise Timeout("timed out after %ds: %s" % (timeout, what))

    def boot(self):
        self.wait_prompt(0, BOOT_TIMEOUT, "boot")

    def run(self, cmd, timeout=CMD_TIMEOUT):
        start = len(self.buf)
        self.proc.stdin.write(cmd.encode() + b"\n")
        self.proc.stdin.flush()
        self.wait_prompt(start, timeout, cmd)
        text = self.buf[start:].decode(errors="replace").replace("\r", "")
        lines = text.split("\n")[1:]          # drop the echoed command line
        out = "\n".join(lines)
        if out.endswith("$ "):
            out = out[:-2]
        return out

    def quit(self):
        try:
            self.proc.stdin.write(b"\x01x")   # Ctrl-A x: QEMU exit
            self.proc.stdin.flush()
        except OSError:
            pass
        deadline = time.time() + 10
        while time.time() < deadline and self.proc.poll() is None:
            self._pump(0.2)
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
        while self._pump(0.1) and self.proc.poll() is None:
            pass
        self.log.close()


def lines_of(out):
    return [l.strip() for l in out.split("\n") if l.strip()]


def names_of(out):
    """First column of each ls output line."""
    return [l.split()[0] for l in lines_of(out)]


def shell_cases(args):
    """(name, command, timeout, check(output) -> bool)."""
    nofail = lambda o: "failed" not in o
    c = [
        ("mkdir /e2e", "mkdir /e2e", CMD_TIMEOUT, nofail),
        ("cd /e2e", "cd /e2e", CMD_TIMEOUT, nofail),
        ("bare ls in subdir", "ls", CMD_TIMEOUT,
         lambda o: "exec ls failed" not in o and "." in names_of(o)),
        ("mkdir d", "mkdir d", CMD_TIMEOUT, nofail),
        ("cd d", "cd d", CMD_TIMEOUT, nofail),
        ("echo > redirect", "echo hello > f", CMD_TIMEOUT, nofail),
        ("cat f", "cat f", CMD_TIMEOUT, lambda o: lines_of(o) == ["hello"]),
        ("pipe cat | wc", "cat f | wc", CMD_TIMEOUT,
         lambda o: lines_of(o) == ["1 1 6"]),
        ("grep", "grep hello f", CMD_TIMEOUT, lambda o: lines_of(o) == ["hello"]),
        ("ln f g", "ln f g", CMD_TIMEOUT, nofail),
        ("rm g", "rm g", CMD_TIMEOUT, nofail),
        ("ls after ln/rm", "ls", CMD_TIMEOUT,
         lambda o: "f" in names_of(o) and "g" not in names_of(o)),
        ("nonexistent command", "nosuchcmd", CMD_TIMEOUT,
         lambda o: "exec nosuchcmd failed" in o),
        ("/ls absolute", "/ls", CMD_TIMEOUT,
         lambda o: "exec" not in o and "." in names_of(o)),
        ("command list ;", "echo one; echo two", CMD_TIMEOUT,
         lambda o: lines_of(o) == ["one", "two"]),
        ("background &", "echo bgjob &", CMD_TIMEOUT, None),   # checked below
        ("after background", "echo after", CMD_TIMEOUT, None),
        ("pathtest from subdir", "pathtest", PATHTEST_TIMEOUT,
         lambda o: "pathtest: ALL OK" in o and "FAIL" not in o),
        ("cleanup: rm f", "rm f", CMD_TIMEOUT, nofail),
        ("cleanup: cd /", "cd /", CMD_TIMEOUT, nofail),
        ("cleanup: rm /e2e/d", "rm /e2e/d", CMD_TIMEOUT, nofail),
        ("cleanup: rm /e2e", "rm /e2e", CMD_TIMEOUT, nofail),
        ("e2e dir gone", "ls", CMD_TIMEOUT,
         lambda o: "e2e" not in names_of(o) and "ls" in names_of(o)),
    ]
    if not args.skip_usertests:
        c.append(("usertests -q", "usertests -q", USERTESTS_TIMEOUT,
                  lambda o: "ALL TESTS PASSED" in o))
    return c


def run_config(cpus, args, results):
    logpath = os.path.join(LOGDIR, "path_e2e_%d.log" % cpus)
    shutil.copyfile(args.backup, os.path.join(ROOT, "fs.img"))
    q = Qemu(cpus, logpath)
    t0 = time.time()
    outputs = {}
    try:
        q.boot()
        cases = shell_cases(args)
        for i, (name, cmd, tmo, check) in enumerate(cases):
            label = "CPUS=%d %s" % (cpus, name)
            if time.time() > args.deadline:
                results.append((label, False, "overall timeout"))
                continue
            try:
                out = q.run(cmd, min(tmo, max(5, args.deadline - time.time())))
            except Timeout as e:
                results.append((label, False, str(e)))
                for n2, *_ in cases[i + 1:]:
                    results.append(("CPUS=%d %s" % (cpus, n2), False, "not run"))
                break
            outputs[name] = out
            if check is None:
                continue
            ok = check(out)
            results.append((label, ok, "" if ok else repr(out[-200:])))
        # background job: its output may land before or after the next prompt
        bg = outputs.get("background &", "") + outputs.get("after background", "")
        if "background &" in outputs:
            ok = "bgjob" in bg and "after" in bg
            results.append(("CPUS=%d background &" % cpus, ok, "" if ok else repr(bg)))
    finally:
        q.quit()
        shutil.copyfile(args.backup, os.path.join(ROOT, "fs.img"))
    print("CPUS=%d finished in %.0fs, log: %s" % (cpus, time.time() - t0,
                                                  os.path.relpath(logpath, ROOT)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    ap.add_argument("--skip-usertests", action="store_true")
    ap.add_argument("--timeout", type=int, default=2400,
                    help="overall timeout in seconds")
    args = ap.parse_args()
    args.deadline = time.time() + args.timeout
    os.makedirs(LOGDIR, exist_ok=True)

    print("building...")
    if subprocess.run(["make", "kernel/kernel", "fs.img"], cwd=ROOT,
                      stdout=subprocess.DEVNULL).returncode != 0:
        print("build failed")
        return 1
    fd, args.backup = tempfile.mkstemp(prefix="fs.img.", suffix=".bak")
    os.close(fd)
    shutil.copyfile(os.path.join(ROOT, "fs.img"), args.backup)

    results = []
    try:
        for cpus in args.cpus:
            try:
                run_config(cpus, args, results)
            except Timeout as e:
                results.append(("CPUS=%d boot" % cpus, False, str(e)))
    finally:
        shutil.copyfile(args.backup, os.path.join(ROOT, "fs.img"))
        os.unlink(args.backup)

    width = max(len(r[0]) for r in results)
    print()
    for label, ok, detail in results:
        print("%-*s  %s%s" % (width, label, "PASS" if ok else "FAIL",
                              "" if ok else "  " + detail))
    nfail = sum(1 for r in results if not r[1])
    print("\n%d/%d passed" % (len(results) - nfail, len(results)))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
