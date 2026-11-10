#!/usr/bin/env python3
"""Official Demo Script: Speculative 'rm a' and 'echo hi > b'.

Demonstrates the core semantics of speculative transactions:
  1. Abort run: Speculative 'rm a' and 'echo hi > b' executed inside a speculation,
     followed by xabort.
     -> Verifies that abort leaves NO trace: file 'a' remains intact, file 'b' is never created.
  2. Commit run: Speculative 'rm a' and 'echo hi > b' executed inside a speculation,
     followed by xcommit.
     -> Verifies that commit makes BOTH appear at once atomically: file 'a' is deleted,
     and file 'b' appears with content 'hi'.

Usage:
  ./scripts/demo_spec.py [--cpus CPUS] [--mode {all,abort,commit}]

Standard library only. Raw logs are saved to scripts/logs/demo_spec_<cpus>.log.
fs.img is backed up and restored automatically.
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.join(ROOT, "scripts", "logs")

sys.path.insert(0, os.path.join(ROOT, "scripts", "tests"))
import test_path_e2e as base  # Qemu, Timeout, PROMPTS, etc.


class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    YELLOW = "\033[93m"
    BOLD = "\033[1m"
    RESET = "\033[0m"


def log(msg, color=None):
    if color:
        print(f"{color}{msg}{Colors.RESET}")
    else:
        print(msg)


def run_abort_demo(q):
    log("\n" + "=" * 65, Colors.CYAN)
    log("  DEMO 1: ABORT (rm a + echo hi > b leaves no trace)", Colors.CYAN + Colors.BOLD)
    log("=" * 65, Colors.CYAN)

    # 1. Setup initial state
    log("[1] Setting up initial state outside speculation...", Colors.BOLD)
    q.run("echo hello > a")
    q.run("rm b")
    out_a = q.run("cat a").strip()
    out_b = q.run("cat b").strip()
    log(f"    cat a -> {out_a}")
    log(f"    cat b -> {out_b}")
    assert "hello" in out_a, f"Expected file 'a' with 'hello', got {out_a}"
    assert "cannot open b" in out_b, f"Expected 'b' not to exist, got {out_b}"
    log("    Initial state verified: file 'a' exists, file 'b' absent.", Colors.GREEN)

    # 2. Open speculation
    log("[2] Opening speculation with xbegin...", Colors.BOLD)
    out_begin = q.run("xbegin")
    spec_id = out_begin.split()[-1]
    log(f"    $ xbegin -> speculation {spec_id}")
    assert spec_id.isdigit(), f"Invalid speculation id: {out_begin}"

    # 3. Perform speculative operations
    log(f"[3] Executing speculative operations inside speculation {spec_id}...", Colors.BOLD)
    log(f"    $ xrun {spec_id} sh demoscript")
    log("      (inside demoscript: 'rm a' and 'echo hi > b')")
    q.run(f"xrun {spec_id} sh demoscript")

    # 4. Check outside state while speculation is still open
    log(f"[4] Checking outside view while speculation {spec_id} is open...", Colors.BOLD)
    out_a_spec = q.run("cat a").strip()
    out_b_spec = q.run("cat b").strip()
    log(f"    Outside cat a -> {out_a_spec}")
    log(f"    Outside cat b -> {out_b_spec}")
    assert "hello" in out_a_spec, f"Isolation failure: 'a' was modified before commit: {out_a_spec}"
    assert "cannot open b" in out_b_spec, f"Isolation failure: 'b' is visible before commit: {out_b_spec}"
    log("    ISOLATION VERIFIED: uncommitted operations are completely invisible outside.", Colors.GREEN)

    # 5. Abort
    log(f"[5] Aborting speculation with xabort {spec_id}...", Colors.BOLD)
    out_abort = q.run(f"xabort {spec_id}").strip()
    log(f"    $ xabort {spec_id} -> {out_abort}")
    assert f"speculation {spec_id} aborted" in out_abort, f"Unexpected abort output: {out_abort}"

    # 6. Verify state after abort
    log("[6] Verifying state after abort...", Colors.BOLD)
    out_a_after = q.run("cat a").strip()
    out_b_after = q.run("cat b").strip()
    log(f"    cat a -> {out_a_after}")
    log(f"    cat b -> {out_b_after}")
    assert "hello" in out_a_after, f"Abort failure: 'a' missing or modified: {out_a_after}"
    assert "cannot open b" in out_b_after, f"Abort failure: 'b' was created: {out_b_after}"

    log("\n>>> PASS: Abort left NO TRACE! ('a' preserved, 'b' never created) <<<", Colors.GREEN + Colors.BOLD)
    return True


def run_commit_demo(q):
    log("\n" + "=" * 65, Colors.CYAN)
    log("  DEMO 2: COMMIT (rm a + echo hi > b appear both at once)", Colors.CYAN + Colors.BOLD)
    log("=" * 65, Colors.CYAN)

    # 1. Setup initial state
    log("[1] Setting up initial state outside speculation...", Colors.BOLD)
    q.run("echo hello > a")
    q.run("rm b")
    out_a = q.run("cat a").strip()
    out_b = q.run("cat b").strip()
    log(f"    cat a -> {out_a}")
    log(f"    cat b -> {out_b}")
    assert "hello" in out_a, f"Expected file 'a' with 'hello', got {out_a}"
    assert "cannot open b" in out_b, f"Expected 'b' not to exist, got {out_b}"
    log("    Initial state verified: file 'a' exists, file 'b' absent.", Colors.GREEN)

    # 2. Open speculation
    log("[2] Opening speculation with xbegin...", Colors.BOLD)
    out_begin = q.run("xbegin")
    spec_id = out_begin.split()[-1]
    log(f"    $ xbegin -> speculation {spec_id}")
    assert spec_id.isdigit(), f"Invalid speculation id: {out_begin}"

    # 3. Perform speculative operations
    log(f"[3] Executing speculative operations inside speculation {spec_id}...", Colors.BOLD)
    log(f"    $ xrun {spec_id} sh demoscript")
    log("      (inside demoscript: 'rm a' and 'echo hi > b')")
    q.run(f"xrun {spec_id} sh demoscript")

    # 4. Check outside state while speculation is still open
    log(f"[4] Checking outside view while speculation {spec_id} is open...", Colors.BOLD)
    out_a_spec = q.run("cat a").strip()
    out_b_spec = q.run("cat b").strip()
    log(f"    Outside cat a -> {out_a_spec}")
    log(f"    Outside cat b -> {out_b_spec}")
    assert "hello" in out_a_spec, f"Isolation failure: 'a' was modified before commit: {out_a_spec}"
    assert "cannot open b" in out_b_spec, f"Isolation failure: 'b' is visible before commit: {out_b_spec}"
    log("    ISOLATION VERIFIED: uncommitted operations are completely invisible outside.", Colors.GREEN)

    # 5. Commit
    log(f"[5] Committing speculation with xcommit {spec_id}...", Colors.BOLD)
    out_commit = q.run(f"xcommit {spec_id}").strip()
    log(f"    $ xcommit {spec_id} -> {out_commit}")
    assert f"speculation {spec_id} committed" in out_commit, f"Unexpected commit output: {out_commit}"

    # 6. Verify state after commit
    log("[6] Verifying state after commit...", Colors.BOLD)
    out_a_after = q.run("cat a").strip()
    out_b_after = q.run("cat b").strip()
    log(f"    cat a -> {out_a_after}")
    log(f"    cat b -> {out_b_after}")
    assert "cannot open a" in out_a_after, f"Commit failure: 'a' was not deleted: {out_a_after}"
    assert out_b_after == "hi", f"Commit failure: 'b' does not contain 'hi': {out_b_after}"

    log("\n>>> PASS: Commit made BOTH changes appear AT ONCE! ('a' deleted, 'b'='hi') <<<", Colors.GREEN + Colors.BOLD)
    return True


def run_in_guest_demospec(q):
    log("\n" + "=" * 65, Colors.CYAN)
    log("  DEMO 3: IN-GUEST C VERIFIER (user/demospec.c)", Colors.CYAN + Colors.BOLD)
    log("=" * 65, Colors.CYAN)
    log("Running in-guest test binary 'demospec'...", Colors.BOLD)
    out = q.run("demospec", timeout=60).strip()
    for line in out.splitlines():
        if "PASS:" in line or "All demo checks passed" in line:
            log(f"    {line}", Colors.GREEN)
        elif "FAIL:" in line or "error" in line.lower():
            log(f"    {line}", Colors.RED)
        else:
            log(f"    {line}")
    assert "All demo checks passed successfully." in out, f"demospec failed: {out}"
    log(">>> PASS: In-guest verification passed! <<<", Colors.GREEN + Colors.BOLD)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cpus", type=int, default=1, help="Number of CPUs for QEMU (default: 1)")
    parser.add_argument("--mode", choices=["all", "abort", "commit", "guest"], default="all",
                        help="Which demo phase to execute")
    args = parser.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    logfile = os.path.join(LOGDIR, f"demo_spec_{args.cpus}.log")

    log("Building xv6 kernel and fs.img...", Colors.BOLD)
    os.system("make -s kernel/kernel fs.img")

    # Backup fs.img
    fsimg = os.path.join(ROOT, "fs.img")
    saved = tempfile.NamedTemporaryFile(delete=False)
    saved.close()
    shutil.copyfile(fsimg, saved.name)

    log(f"Booting QEMU (CPUS={args.cpus}, log: {logfile})...", Colors.BOLD)
    q = base.Qemu(args.cpus, logfile)
    try:
        q.boot()
        log("Booted and logged in as admin successfully.", Colors.GREEN)

        if args.mode in ("all", "abort"):
            run_abort_demo(q)

        if args.mode in ("all", "commit"):
            run_commit_demo(q)

        if args.mode in ("all", "guest"):
            run_in_guest_demospec(q)

        log("\n" + "=" * 65, Colors.GREEN)
        log("  ALL DEMONSTRATION PHASES COMPLETED AND VERIFIED SUCCESSFULLY!", Colors.GREEN + Colors.BOLD)
        log("=" * 65 + "\n", Colors.GREEN)
    finally:
        q.quit()
        shutil.copyfile(saved.name, fsimg)
        os.unlink(saved.name)


if __name__ == "__main__":
    main()
