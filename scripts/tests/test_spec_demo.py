#!/usr/bin/env python3
"""End-to-end test of the official demo script: speculative rm a + echo hi > b.

Verifies:
  - Abort leaves no trace: rm a and echo hi > b are undone, 'a' remains, 'b' is not created.
  - Commit makes both appear at once: 'a' is deleted and 'b' appears with 'hi'.
  - In-guest 'demospec' utility runs and passes all assertions.

Standard library only. Raw logs go to scripts/logs/spec_demo_<cpus>.log.
Exits 0 on success, non-zero on failure.
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGDIR = os.path.join(ROOT, "scripts", "logs")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_path_e2e as base


def check_demo(cpus):
    logfile = os.path.join(LOGDIR, f"spec_demo_{cpus}.log")
    q = base.Qemu(cpus, logfile)
    try:
        q.boot()

        # 1. Test in-guest demospec
        out = q.run("demospec")
        if "All demo checks passed successfully." not in out:
            print(f"[FAIL CPUS={cpus}] demospec binary failed:\n{out}")
            return False

        # 2. Test shell script demo with abort
        q.run("echo hello > a")
        q.run("rm b")
        out_b = q.run("xbegin")
        spec_id = out_b.split()[-1]
        q.run(f"xrun {spec_id} sh demoscript")

        # Outside view before abort
        if "hello" not in q.run("cat a"):
            print(f"[FAIL CPUS={cpus}] file 'a' disappeared before abort")
            return False
        if "cannot open b" not in q.run("cat b"):
            print(f"[FAIL CPUS={cpus}] file 'b' appeared before abort")
            return False

        q.run(f"xabort {spec_id}")

        # Outside view after abort
        if "hello" not in q.run("cat a"):
            print(f"[FAIL CPUS={cpus}] file 'a' missing after abort")
            return False
        if "cannot open b" not in q.run("cat b"):
            print(f"[FAIL CPUS={cpus}] file 'b' exists after abort")
            return False

        # 3. Test shell script demo with commit
        out_b = q.run("xbegin")
        spec_id = out_b.split()[-1]
        q.run(f"xrun {spec_id} sh demoscript")

        # Outside view before commit
        if "hello" not in q.run("cat a"):
            print(f"[FAIL CPUS={cpus}] file 'a' disappeared before commit")
            return False
        if "cannot open b" not in q.run("cat b"):
            print(f"[FAIL CPUS={cpus}] file 'b' appeared before commit")
            return False

        q.run(f"xcommit {spec_id}")

        # Outside view after commit
        if "cannot open a" not in q.run("cat a"):
            print(f"[FAIL CPUS={cpus}] file 'a' still exists after commit")
            return False
        if q.run("cat b").strip() != "hi":
            print(f"[FAIL CPUS={cpus}] file 'b' did not contain 'hi' after commit")
            return False

        return True
    finally:
        q.quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 3])
    args = parser.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    subprocess_env = dict(os.environ)

    # Rebuild
    os.system("make -s kernel/kernel fs.img")

    fsimg = os.path.join(ROOT, "fs.img")
    saved = tempfile.NamedTemporaryFile(delete=False)
    saved.close()
    shutil.copyfile(fsimg, saved.name)

    failed = 0
    try:
        for cpus in args.cpus:
            print(f"== CPUS={cpus}")
            try:
                ok = check_demo(cpus)
                print(f"{'PASS' if ok else 'FAIL'} demo_spec on CPUS={cpus}")
                if not ok:
                    failed += 1
            except Exception as e:
                print(f"FAIL CPUS={cpus} exception: {e}")
                failed += 1
            finally:
                shutil.copyfile(saved.name, fsimg)
    finally:
        os.unlink(saved.name)

    print(f"spec demo test: {'all passed' if failed == 0 else f'{failed} failed'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
