#!/usr/bin/env python3
"""Execute genuinely unrolled IR and compare FlatExec observables."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod
from flat_exec_report import observable_same


def check_equivalence(binary, directory):
    subprocess.run([binary, str(directory)], check=True)
    expected = {"even", "odd", "step3", "odd_step3", "non_aligned_odd",
                "near_max", "near_min", "wide_step", "factor2"}
    actual = {p.name.removesuffix(".before.flat")
              for p in directory.glob("*.before.flat")}
    if actual != expected:
        raise AssertionError(f"missing/extra unroll fixtures: {actual}")
    for name in sorted(expected):
        before = (directory / (name + ".before.flat")).read_text()
        after = (directory / (name + ".after.flat")).read_text()
        if before == after:
            raise AssertionError(f"{name}: no transformation occurred")
        original = FlatExec(FlatMod(before)).run()
        transformed = FlatExec(FlatMod(after)).run()
        if not observable_same(original, transformed):
            raise AssertionError(f"{name}: {original} != {transformed}")
        evidence = {"case": name, "before": original, "after": transformed}
        print(json.dumps(evidence, sort_keys=True))
        (directory / (name + ".trace.json")).write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(f"UNROLL_EQUIVALENCE={len(expected)} FAILURES=0")


if __name__ == "__main__":
    if len(sys.argv) == 3:
        destination = Path(sys.argv[2])
        destination.mkdir(parents=True, exist_ok=True)
        check_equivalence(sys.argv[1], destination)
    elif len(sys.argv) == 2:
        with tempfile.TemporaryDirectory(prefix="sysy-unroll-") as temporary:
            check_equivalence(sys.argv[1], Path(temporary))
    else:
        raise SystemExit("usage: unroll_equivalence.py TEST_BINARY [ARTIFACT_DIR]")
