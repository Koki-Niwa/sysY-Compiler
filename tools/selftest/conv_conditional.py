#!/usr/bin/env python3
"""Conditional conv preheader must be declined without corrupting SSA."""

import os
import subprocess
import sys
import tempfile


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=30)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: conv_conditional.py COMPILER FIXTURE")
    compiler, fixture = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="sysy-conv-cond-") as temporary:
        base = os.path.join(temporary, "base.flat")
        opt = os.path.join(temporary, "opt.flat")
        result = run([compiler, fixture, "--from-flat", "--emit=flat-ir", "-o", base])
        if result.returncode != 0:
            raise AssertionError(f"input fixture rejected: {result.stderr}")
        result = run([compiler, fixture, "--from-flat", "--mem2reg", "--opt=s24c",
                      "--dump-opt-stats", "-o", opt])
        if result.returncode != 0:
            raise AssertionError(f"conditional preheader produced invalid IR: {result.stderr}")
        if "blocked-conv=" in result.stderr:
            raise AssertionError(f"conditional preheader was transformed: {result.stderr}")
        print("CONV_CONDITIONAL_NOCHANGE=1")


if __name__ == "__main__":
    main()
