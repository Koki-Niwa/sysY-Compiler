#!/usr/bin/env python3
"""Nested-K conv must be declined, not miscompiled by the output-reduction pass."""

import os
import re
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod
from matmul_equivalence import compile_flat


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: unsupported_conv.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 50_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-conv-nochange-") as temporary:
        base = os.path.join(temporary, "base.flat")
        opt = os.path.join(temporary, "opt.flat")
        compile_flat(compiler, source, [], base)
        stats = compile_flat(compiler, source, ["--opt=s24c", "--dump-opt-stats"], opt)
        if re.search(r"blocked-(matmul|output-reductions)=", stats):
            raise AssertionError(f"nested-K conv was unexpectedly transformed: {stats}")
        # --opt=s24c still runs DCE/canonicalization; only blocking must decline.
        before = FlatExec(FlatMod(open(base, encoding="utf-8").read())).run()
        after = FlatExec(FlatMod(open(opt, encoding="utf-8").read())).run()
        for key in ("out", "mem", "ret", "input_state", "effects"):
            if before[key] != after[key]:
                raise AssertionError(f"{key}: {before[key]} != {after[key]}")
        print(f"UNSUPPORTED_CONV_NOCHANGE ret={before['ret']} mem={before['mem']}")


if __name__ == "__main__":
    main()
