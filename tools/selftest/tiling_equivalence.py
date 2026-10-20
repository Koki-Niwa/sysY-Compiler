#!/usr/bin/env python3
"""Execute genuinely transformed IR, checking every FlatExec observable."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod
from flat_exec_report import observable_same


def check_equivalence(binary, directory):
    # The producer verifies both modules, requires changed + tiled-loops == 1
    # for each stride case, and checks unsupported cases remain unmodified.
    subprocess.run([binary, str(directory)], check=True)
    expected = {"nested", "step1", "step3", "tail", "near_max",
                "near_min", "wide_distance"}
    actual = {p.name.removesuffix(".before.flat")
              for p in directory.glob("*.before.flat")}
    if actual != expected:
        raise AssertionError(f"missing/extra equivalence fixtures: {actual}")
    for name in sorted(expected):
        before = (directory / (name + ".before.flat")).read_text()
        after = (directory / (name + ".after.flat")).read_text()
        if before == after:
            raise AssertionError(f"{name}: no transformation occurred")
        original = FlatExec(FlatMod(before)).run()
        transformed = FlatExec(FlatMod(after)).run()
        if not observable_same(original, transformed):
            raise AssertionError(f"{name}: {original} != {transformed}")
        # An independent arithmetic oracle makes the original step-3
        # counterexample explicit, in addition to comparing the two executions.
        if name == "step3" and original["ret"] != 6048:
            raise AssertionError(f"step3 oracle: {original}")
        evidence = {"case": name, "before": original, "after": transformed}
        print(json.dumps(evidence, sort_keys=True))
        (directory / (name + ".trace.json")).write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(f"TILING_EQUIVALENCE={len(expected)} FAILURES=0")


if __name__ == "__main__":
    if len(sys.argv) == 3:
        destination = Path(sys.argv[2])
        destination.mkdir(parents=True, exist_ok=True)
        check_equivalence(sys.argv[1], destination)
    elif len(sys.argv) == 2:
        with tempfile.TemporaryDirectory(prefix="sysy-tiling-") as temporary:
            check_equivalence(sys.argv[1], Path(temporary))
    else:
        raise SystemExit("usage: tiling_equivalence.py TEST_BINARY [ARTIFACT_DIR]")
