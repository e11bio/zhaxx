# SPDX-License-Identifier: MIT
"""
Offline tests for zhaxx-scan against the anonymized fixtures. No ZFS, no root:
generates synthetic zdb dumps, runs the scanner in --dumps mode, and checks the
findings and the emitted repair plan. Run: python3 tests/test_scan.py
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fixtures  # noqa: E402

SCAN = os.path.join(HERE, "..", "scan", "zhaxx-scan")

_fails = 0


def check(cond, msg):
    global _fails
    status = "ok  " if cond else "FAIL"
    print(f"  [{status}] {msg}")
    if not cond:
        _fails += 1


def run_scan(objs):
    with tempfile.TemporaryDirectory() as d:
        dumps = os.path.join(d, "dumps")
        fixtures.write_scenario(objs, dumps)
        jf = os.path.join(d, "report.json")
        pf = os.path.join(d, "plan.txt")
        r = subprocess.run(
            [sys.executable, SCAN, "--dumps", dumps, "--json", jf,
             "--plan", pf],
            capture_output=True, text=True)
        with open(jf) as f:
            report = json.load(f)
        return r, report


def plan_set(report):
    return set(report["plan"])


def test_scenario_a():
    print("Scenario A: orphaned clone with dead objset")
    r, report = run_scan(fixtures.SCENARIO_A)
    check(r.returncode == 1, "exit code 1 (findings present)")
    got = plan_set(report)
    want = fixtures.SCENARIO_A_PLAN
    for line in sorted(want):
        check(line in got, f"plan contains: {line}")
    extra = got - want
    check(not extra, f"no unexpected plan ops (extra={sorted(extra)})")

    # The dead objset object 673 must NOT be freed (unreadable -> type
    # unknown -> unsafe). Leaking it is the documented, safe residual.
    check(not any("obj=673" in line for line in got),
          "does NOT free the unreadable objset (obj 673)")
    check(any(f["object"] == 673 for f in report["findings"]),
          "flags obj 673 as the corruption signature")
    check(any("deadlock" in f["message"] for f in report["findings"]),
          "explains the errlog->txg_sync deadlock")


def test_scenario_b():
    print("Scenario B: errlog_last points at a recycled space map")
    r, report = run_scan(fixtures.SCENARIO_B)
    got = plan_set(report)
    check(got == fixtures.SCENARIO_B_PLAN,
          "detaches errlog_last, plans nothing else")
    check(not any(line.startswith("free") for line in got),
          "refuses to free the recycled object (type guard)")
    check(any("recycled" in f["message"] for f in report["findings"]),
          "explains the ID was recycled")


def test_clean_pool():
    print("Scenario C: a consistent pool yields no findings")
    clean = [
        fixtures.Obj(1, "object directory",
                     {"root_dataset": 32, "errlog_last": 0, "errlog_scrub": 0}),
        fixtures.Obj(32, "DSL directory",
                     {"head_dataset_obj": 55, "parent_dir_obj": 0,
                      "origin_obj": 0, "child_dir_zapobj": 34, "clones": 0}),
        fixtures.Obj(55, "DSL dataset",
                     {"prev_snap_obj": 0, "next_clones_obj": 0}),
        fixtures.Obj(34, "DSL directory child map",
                     {"$MOS": 35, "$FREE": 38, "data": 1792}),
        fixtures.Obj(1792, "DSL directory",
                     {"head_dataset_obj": 1793, "parent_dir_obj": 32,
                      "origin_obj": 0, "child_dir_zapobj": 1794, "clones": 0}),
        fixtures.Obj(1793, "DSL dataset",
                     {"prev_snap_obj": 0, "next_clones_obj": 0}),
        fixtures.Obj(1794, "DSL directory child map", {}),
    ]
    r, report = run_scan(clean)
    check(r.returncode == 0, "exit code 0 (clean)")
    check(not report["findings"], "no findings")
    check(not report["plan"], "empty plan")


def main():
    test_scenario_a()
    test_scenario_b()
    test_clean_pool()
    print()
    if _fails:
        print(f"FAILED: {_fails} check(s)")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
