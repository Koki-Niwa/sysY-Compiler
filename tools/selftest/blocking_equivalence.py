#!/usr/bin/env python3
"""Execute a genuinely blocked reduction; final memory/return must match."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod


def check_equivalence(binary, directory):
    subprocess.run([binary, str(directory)], check=True)
    expected = {"reduction", "partial", "map", "array_reduction", "nested"}
    actual = {p.name.removesuffix(".before.flat")
              for p in directory.glob("*.before.flat")}
    if actual != expected:
        raise AssertionError(f"missing/extra blocking fixtures: {actual}")
    for name in sorted(expected):
        before = (directory / (name + ".before.flat")).read_text()
        after = (directory / (name + ".after.flat")).read_text()
        if before == after:
            raise AssertionError(f"{name}: no transformation occurred")
        original = FlatExec(FlatMod(before)).run()
        transformed = FlatExec(FlatMod(after)).run()
        for key in ("mem", "ret", "out", "input_state"):
            if original[key] != transformed[key]:
                raise AssertionError(
                    f"{name}: {key} differs: {original[key]} != {transformed[key]}")
        # The blocked order must actually reorder the reduction's global writes.
        if original["effects"] == transformed["effects"]:
            raise AssertionError(f"{name}: reduction was not reordered")
        evidence = {"case": name, "before": original, "after": transformed}
        print(json.dumps(evidence, sort_keys=True))
        (directory / (name + ".trace.json")).write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(f"BLOCKING_EQUIVALENCE={len(expected)} FAILURES=0")


if __name__ == "__main__":
    if len(sys.argv) == 3:
        destination = Path(sys.argv[2])
        destination.mkdir(parents=True, exist_ok=True)
        check_equivalence(sys.argv[1], destination)
    elif len(sys.argv) == 2:
        with tempfile.TemporaryDirectory(prefix="sysy-blocking-") as temporary:
            check_equivalence(sys.argv[1], Path(temporary))
    else:
        raise SystemExit("usage: blocking_equivalence.py TEST_BINARY [ARTIFACT_DIR]")
