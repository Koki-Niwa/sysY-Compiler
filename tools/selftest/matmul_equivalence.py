#!/usr/bin/env python3
"""S24c matmul blocking: final memory/return must match, writes may reorder."""

import os
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod


def compile_flat(compiler, source, flags, output):
    proc = subprocess.run(
        [compiler, source, "--emit=flat-ir", "--mem2reg", *flags, "-o", output],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise AssertionError(proc.stderr)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: matmul_equivalence.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 200_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-matmul-") as temporary:
        base = os.path.join(temporary, "base.flat")
        opt = os.path.join(temporary, "opt.flat")
        compile_flat(compiler, source, [], base)
        compile_flat(compiler, source, ["--opt=s24c"], opt)
        before = FlatExec(FlatMod(open(base, encoding="utf-8").read())).run()
        after = FlatExec(FlatMod(open(opt, encoding="utf-8").read())).run()
        for key in ("out", "mem", "ret", "input_state"):
            if before[key] != after[key]:
                raise AssertionError(f"{key}: {before[key]} != {after[key]}")
        if before["effects"] == after["effects"]:
            raise AssertionError("matmul output stores were not reordered")
        print(f"MATMUL_EQUIVALENCE ret={before['ret']} mem={before['mem']} "
              f"effects_reordered=1")


if __name__ == "__main__":
    main()
