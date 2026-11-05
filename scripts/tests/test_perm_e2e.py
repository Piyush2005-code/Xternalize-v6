#!/usr/bin/env python3
"""End-to-end test of file permissions and syscall privileges under QEMU.

Modelled on test_path_e2e.py (it reuses its Qemu class).  For CPUS=1 and
CPUS=3 it runs three boots:

  A  admin session  : permtest (admin cases + user cases in a dropped child),
                      permtest setup
     user session   : shell-level checks, permtest nokill1, creates /uf
     admin session  : permtest teardown, creates /af
  B  reboot on the same fs.img, no rebuild: owners/modes persisted?  user can
     still write /uf but not /af.  The raw image is then parsed on the host.
  C  fresh fs.img   : as user, `kill 1` must leave the system up, then the full
     permtest (including kill(1)).  A kernel that lets a user kill init
     panics here, which is reported as a failure.

Sessions switch with `exit` (back to the login screen).  Standard library
only.  Raw QEMU logs go to scripts/logs/perm_e2e_<cpus>_<boot>.log, and fs.img
is restored after every run.  Exits non-zero on any failure.

  ./scripts/tests/test_perm_e2e.py [--cpus 1 3] [--timeout SECONDS]
"""

import argparse
import os
import re
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_path_e2e as base  # noqa: E402  (Qemu, Timeout, prompts, ROOT)

PERMTEST_TIMEOUT = 240
KILL1_TIMEOUT = 20


class Aborted(Exception):
    """The boot is unusable (timeout or panic); remaining steps cannot run."""


class Boot(base.Qemu):
    def __init__(self, cpus, boot_id):
        self.name = "CPUS=%d boot %s" % (cpus, boot_id)
        super().__init__(cpus, os.path.join(base.LOGDIR, "perm_e2e_%d_%s.log" % (cpus, boot_id)))

    def login(self, user):
        mark = len(self.buf)
        self.wait_for(mark, base.LOGIN_PROMPT, base.BOOT_TIMEOUT, "login prompt")
        mark = len(self.buf)
        self.send(user)
        if user == "admin":
            self.wait_for(mark, base.PASSWORD_PROMPT, base.CMD_TIMEOUT, "password prompt")
            mark = len(self.buf)
            self.send(base.ADMIN_PASSWORD)
        self.wait_prompt(mark, base.CMD_TIMEOUT, "shell prompt for " + user)

    def switch(self, user):
        """Leave the shell (back to the login screen) and log in as `user`."""
        self.send("exit")
        self.login(user)

    def panicked(self):
        return b"panic" in self.buf


def lines_of(out):
    return [l.strip() for l in out.split("\n") if l.strip()]


class Run:
    def __init__(self, deadline):
        self.results = []
        self.deadline = deadline

    def add(self, label, ok, detail=""):
        self.results.append((label, bool(ok), "" if ok else detail))

    def cmd(self, q, label, command, check, timeout=base.CMD_TIMEOUT):
        """Run one shell command and record check(output)."""
        if time.time() > self.deadline:
            self.add(label, False, "overall timeout")
            raise Aborted("overall timeout")
        try:
            out = q.run(command, min(timeout, max(5, self.deadline - time.time())))
        except base.Timeout as e:
            why = "kernel panic" if q.panicked() else str(e)
            self.add(label, False, why)
            raise Aborted(why)
        self.add(label, check(out), repr(out[-200:]))
        return out

    def permtest(self, q, who, args, timeout=PERMTEST_TIMEOUT):
        """Run permtest and turn each PASS/FAIL line into a result row."""
        command = ("permtest " + args).strip()
        try:
            out = q.run(command, timeout)
        except base.Timeout as e:
            why = "kernel panic" if q.panicked() else str(e)
            # rows printed before the system died are still in the log
            tail = bytes(q.buf).decode(errors="replace").replace("\r", "")
            for m in re.finditer(r"^(PASS|FAIL) (.*)$", tail.split(command)[-1], re.M):
                self.add("%s [%s session] %s" % (q.name, who, m.group(2)), m.group(1) == "PASS", "")
            self.add("%s [%s session] %s (finished)" % (q.name, who, command), False, why)
            raise Aborted(why)
        for m in re.finditer(r"^(PASS|FAIL) (.*)$", out, re.M):
            self.add("%s [%s session] %s" % (q.name, who, m.group(2)), m.group(1) == "PASS", "")
        summary = [l for l in lines_of(out) if l.startswith("permtest: ")]
        self.add("%s [%s session] %s (finished)" % (q.name, who, command), bool(summary), repr(out[-200:]))
        return out


# ---- raw image parsing (xv6 layout, 68-byte dinode) -------------------------

BSIZE = 1024
DINODE = struct.Struct("<hhhhI13Ihh")  # type major minor nlink size addrs[13] uid mode


def read_inodes(path):
    """Return {name: (uid, mode)} for the entries of the root directory."""
    d = open(path, "rb").read()
    # superblock: magic size nblocks ninodes nlog logstart inodestart bmapstart
    inodestart = struct.unpack_from("<8I", d, BSIZE)[6]
    ipb = BSIZE // DINODE.size

    def ino(i):
        off = (inodestart + i // ipb) * BSIZE + (i % ipb) * DINODE.size
        f = DINODE.unpack_from(d, off)
        return f[4], f[5:18], f[18], f[19]

    size, addrs, uid, mode = ino(1)
    data = b"".join(d[a * BSIZE:(a + 1) * BSIZE] for a in addrs[:12] if a)[:size]
    out = {"/": (uid, mode)}
    for k in range(0, len(data), 16):
        inum, name = struct.unpack_from("<H14s", data, k)
        if inum:
            _, _, u, m = ino(inum)
            out[name.split(b"\0")[0].decode()] = (u, m)
    return out


# ---- the three boots --------------------------------------------------------

def boot_a(cpus, run):
    q = Boot(cpus, "A")
    try:
        q.login("admin")
        run.permtest(q, "admin", "")
        run.permtest(q, "admin", "setup")
        q.switch("user")
        run.cmd(q, q.name + " [user shell] whoami", "whoami", lambda o: lines_of(o) == ["user (uid=1)"])
        run.cmd(q, q.name + " [user shell] ls / works", "ls /",
                lambda o: "README" in o and "failed" not in o)
        run.cmd(q, q.name + " [user shell] echo x > /README fails", "echo x > /README",
                lambda o: "open /README failed" in o)
        run.cmd(q, q.name + " [user shell] rm /ls fails", "rm /ls", lambda o: "failed to delete" in o)
        run.cmd(q, q.name + " [user shell] /ls still there", "ls /", lambda o: "ls" in [l.split()[0] for l in lines_of(o)])
        run.cmd(q, q.name + " [user shell] cat /README works", "cat /README", lambda o: "xv6" in o)
        run.permtest(q, "user", "nokill1")
        run.cmd(q, q.name + " [user shell] echo > /uf", "echo ufdata > /uf", lambda o: "failed" not in o)
        q.switch("admin")
        run.permtest(q, "admin", "teardown")
        run.cmd(q, q.name + " [admin shell] echo > /af", "echo afdata > /af", lambda o: "failed" not in o)
    except Aborted as e:
        run.add(q.name + " aborted", False, str(e))
    finally:
        q.quit()


def boot_b(cpus, run):
    """Same fs.img as boot A, nothing rebuilt."""
    q = Boot(cpus, "B")
    try:
        q.login("user")
        run.cmd(q, q.name + " [user shell] /uf survived reboot", "cat /uf", lambda o: lines_of(o) == ["ufdata"])
        run.cmd(q, q.name + " [user shell] still owns /uf (can write)", "echo again > /uf", lambda o: "failed" not in o)
        run.cmd(q, q.name + " [user shell] /uf has the new data", "cat /uf", lambda o: lines_of(o) == ["again"])
        run.cmd(q, q.name + " [user shell] cannot write admin's /af", "echo x > /af", lambda o: "open /af failed" in o)
        run.cmd(q, q.name + " [user shell] can read admin's /af", "cat /af", lambda o: lines_of(o) == ["afdata"])
    except Aborted as e:
        run.add(q.name + " aborted", False, str(e))
    finally:
        q.quit()
    # the raw image after the reboot
    try:
        tbl = read_inodes(os.path.join(base.ROOT, "fs.img"))
    except (OSError, struct.error) as e:
        run.add("CPUS=%d image: parse fs.img" % cpus, False, str(e))
        return
    label = "CPUS=%d image: " % cpus
    run.add(label + "/uf owned by uid 1", tbl.get("uf", (None,))[0] == 1, repr(tbl.get("uf")))
    run.add(label + "/af owned by uid 0", tbl.get("af", (None,))[0] == 0, repr(tbl.get("af")))
    run.add(label + "/uf mode: owner rw, others r only", tbl.get("uf", (0, 0))[1] == 0o64, repr(tbl.get("uf")))
    run.add(label + "/af mode: owner rw, others r only", tbl.get("af", (0, 0))[1] == 0o64, repr(tbl.get("af")))
    run.add(label + "/ls owned by admin, others r-x", tbl.get("ls", (None, 0))[0] == 0 and tbl["ls"][1] & 0o5 == 0o5, repr(tbl.get("ls")))
    run.add(label + "/ owned by admin, others can search+read", tbl["/"][0] == 0 and tbl["/"][1] & 0o5 == 0o5, repr(tbl["/"]))


def boot_c(cpus, run):
    """kill(1) isolated: a vulnerable kernel panics here."""
    q = Boot(cpus, "C")
    try:
        q.login("admin")
        run.permtest(q, "admin", "setup")
        q.switch("user")
        run.cmd(q, q.name + " [user shell] `kill 1` leaves the system up", "kill 1",
                lambda o: "panic" not in o, KILL1_TIMEOUT)
        run.cmd(q, q.name + " [user shell] still works after `kill 1`", "echo alive", lambda o: lines_of(o) == ["alive"])
        run.permtest(q, "user", "")
        q.switch("admin")
        run.permtest(q, "admin", "teardown")
    except Aborted as e:
        run.add(q.name + " aborted", False, str(e))
    finally:
        q.quit()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    ap.add_argument("--timeout", type=int, default=1500, help="overall timeout in seconds")
    args = ap.parse_args()
    run = Run(time.time() + args.timeout)
    os.makedirs(base.LOGDIR, exist_ok=True)

    print("building...")
    if os.system("make -C %s kernel/kernel fs.img >/dev/null" % base.ROOT) != 0:
        print("build failed")
        return 1
    fd, backup = tempfile.mkstemp(prefix="fs.img.", suffix=".bak")
    os.close(fd)
    img = os.path.join(base.ROOT, "fs.img")
    shutil.copyfile(img, backup)
    try:
        for cpus in args.cpus:
            for fn, fresh in ((boot_a, True), (boot_b, False), (boot_c, True)):
                if fresh:
                    shutil.copyfile(backup, img)
                try:
                    fn(cpus, run)
                except base.Timeout as e:  # boot / login never reached
                    run.add("CPUS=%d %s" % (cpus, fn.__name__), False, str(e))
    finally:
        shutil.copyfile(backup, img)
        os.unlink(backup)

    width = max(len(r[0]) for r in run.results)
    print()
    for label, ok, detail in run.results:
        print("%-*s  %s%s" % (width, label, "PASS" if ok else "FAIL", "" if ok else "  " + detail))
    nfail = sum(1 for r in run.results if not r[1])
    print("\n%d/%d passed" % (len(run.results) - nfail, len(run.results)))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
