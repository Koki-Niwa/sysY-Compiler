#!/usr/bin/env python3
"""S24c-c 1-D stencil: actual reorder, full output oracle and memory equality."""

import os
import re
import subprocess
import sys
import tempfile

from flat_exec import FlatExec, FlatMod


def compile_flat(compiler, source, flags, output):
    proc = subprocess.run(
        [compiler, source, "--emit=flat-ir", "--mem2reg", *flags, "-o", output],
        capture_output=True, text=True, timeout=30)
    if proc.returncode != 0:
        raise AssertionError(proc.stderr)
    return proc.stderr


def expected(machine):
    inputs = machine.globals["in"].mem
    weights = machine.globals["w"].mem
    rows, columns = 64, 64
    return [sum(inputs.read_word(4 * (j + k), 4) * weights.read_word(4 * k, 4)
                for k in range(9)) & 0xffffffff
            for _ in range(rows) for j in range(columns)]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: stencil_equivalence.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 50_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-stencil-") as temporary:
        base = os.path.join(temporary, "base.flat")
        opt = os.path.join(temporary, "opt.flat")
        compile_flat(compiler, source, [], base)
        stats = compile_flat(compiler, source, ["--opt=s24c", "--dump-opt-stats"], opt)
        if not re.search(r"^s24c matmul-blocking blocked-matmul=1$", stats, re.MULTILINE):
            raise AssertionError(f"no actual stencil transform: {stats}")
        before_machine = FlatExec(FlatMod(open(base, encoding="utf-8").read()))
        after_machine = FlatExec(FlatMod(open(opt, encoding="utf-8").read()))
        expected_cells = expected(before_machine)
        before, after = before_machine.run(), after_machine.run()
        for key in ("out", "mem", "ret", "input_state"):
            if before[key] != after[key]:
                raise AssertionError(f"{key}: {before[key]} != {after[key]}")
        actual = [before_machine.globals["out"].mem.read_word(4 * index, 4)
                  for index in range(len(expected_cells))]
        if actual != expected_cells:
            raise AssertionError("base output differs from stencil oracle")
        if before["effects"] == after["effects"]:
            raise AssertionError("stencil output stores were not reordered")
        print(f"STENCIL_EQUIVALENCE cells={len(expected_cells)} ret={before['ret']} "
              f"mem={before['mem']} effects_reordered=1")


if __name__ == "__main__":
    main()
