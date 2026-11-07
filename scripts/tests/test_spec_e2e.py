#!/usr/bin/env python3
"""End-to-end test of the externalization barrier through the qemu-bridge.

Boots xv6 with `make qemu-bridge`, starts agent/qemu_bridge.py, and sends
commands to it over TCP exactly as the agent's tools do.  For CPUS=1 and
CPUS=3 it checks:

  * output of `xrun <id> ...` is held, and the shell's own output is not
  * `xcommit <id>` releases the held output, in order, before its own line
  * `xabort <id>` drops it, and the speculation cannot be used afterwards
  * spectest (the in-guest tests: pipes, limits, permissions) passes
  * no kernel panic

Standard library only.  The transcript of every run goes to
scripts/logs/spec_e2e_<cpus>.log.  fs.img is restored afterwards.
Exits non-zero on any failure.

  ./scripts/tests/test_spec_e2e.py [--cpus 1 3]
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGDIR = os.path.join(ROOT, "scripts", "logs")
SERIAL_PORT = 5591
BRIDGE_PORT = 4491
READY_TIMEOUT = 120


class Failed(Exception):
    pass


class Session:
    """QEMU in bridge mode plus agent/qemu_bridge.py, for one boot."""

    def __init__(self, cpus):
        self.cpus = cpus
        self.log = open(os.path.join(LOGDIR, "spec_e2e_%d.log" % cpus), "w")
        env = dict(os.environ, XV6_SERIAL_PORT=str(SERIAL_PORT),
                   QEMU_BRIDGE_PORT=str(BRIDGE_PORT))
        self.qemu = subprocess.Popen(
            ["make", "-s", "qemu-bridge", "CPUS=%d" % cpus,
             "XV6_SERIAL_PORT=%d" % SERIAL_PORT],
            cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT,
            start_new_session=True)
        self.bridge = subprocess.Popen(
            [sys.executable, "-u", os.path.join(ROOT, "agent", "qemu_bridge.py")],
            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        deadline = time.time() + READY_TIMEOUT
        while time.time() < deadline:
            line = self.bridge.stdout.readline()
            if not line:
                break
            self.log.write("[bridge] " + line.split("] ", 1)[-1])
            if "Listening" in line:
                return
        self.close()
        raise Failed("the bridge never became ready (see %s)" % self.log.name)

    def run(self, cmd, timeout=60):
        """Send one command through the bridge, return its output."""
        with socket.create_connection(("127.0.0.1", BRIDGE_PORT), timeout=timeout) as s:
            s.sendall(cmd.encode() + b"\n")
            out = b""
            while True:
                data = s.recv(4096)
                if not data:
                    break
                out += data
        out = out.decode(errors="replace").strip()
        self.log.write("$ %s\n%s\n" % (cmd, out))
        self.log.flush()
        if "panic" in out:
            raise Failed("kernel panic after `%s`: %s" % (cmd, out))
        return out

    def close(self):
        for proc in (self.bridge, self.qemu):
            try:
                os.killpg(proc.pid, 9)
            except ProcessLookupError:
                pass
            proc.wait()
        self.log.close()


def spec_id(out):
    m = re.search(r"speculation (\d+)", out)
    if not m:
        raise Failed("xbegin printed %r" % out)
    return m.group(1)


def checks(s):
    """Yield (name, ok) for each check, in order."""
    a = spec_id(s.run("xbegin"))
    out1 = s.run("xrun %s echo held-one" % a)
    out2 = s.run("xrun %s echo held-two" % a)
    yield "xrun output is held", "held-" not in out1 + out2
    yield "shell outside the speculation is not held", s.run("echo outside") == "outside"
    out = s.run("xcommit %s" % a)
    yield "xcommit releases held output in order", (
        out.split("\n") == ["held-one", "held-two", "speculation %s committed" % a])

    b = spec_id(s.run("xbegin"))
    out = s.run("xrun %s echo secret-text" % b) + s.run("xabort %s" % b)
    yield "xabort drops held output", (
        "secret-text" not in out and "speculation %s aborted" % b in out)
    out = s.run("xrun %s echo again" % b)
    yield "an aborted speculation cannot be used", "no open speculation" in out
    yield "nothing was released after the abort", "secret-text" not in s.run("echo after")

    out = s.run("spectest", timeout=240)
    yield "spectest", "spectest: all tests passed" in out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    args = ap.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    subprocess.run(["make", "-s", "kernel/kernel", "fs.img"], cwd=ROOT, check=True)
    fsimg = os.path.join(ROOT, "fs.img")
    saved = tempfile.NamedTemporaryFile(delete=False)
    saved.close()
    shutil.copyfile(fsimg, saved.name)

    failed = 0
    try:
        for cpus in args.cpus:
            print("== CPUS=%d" % cpus)
            try:
                s = Session(cpus)
            except Failed as e:
                print("FAIL boot: %s" % e)
                failed += 1
                continue
            try:
                for name, ok in checks(s):
                    print("%s %s" % ("PASS" if ok else "FAIL", name))
                    failed += not ok
            except (Failed, OSError) as e:
                print("FAIL %s" % e)
                failed += 1
            finally:
                s.close()
                shutil.copyfile(saved.name, fsimg)
    finally:
        os.unlink(saved.name)

    print("spec e2e: %s" % ("all passed" if failed == 0 else "%d failed" % failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
