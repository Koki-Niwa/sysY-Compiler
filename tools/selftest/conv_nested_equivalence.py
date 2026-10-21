#!/usr/bin/env python3
"""Nested-K 2-D conv blocking: full C oracle, final memory and return equality."""

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
    data = machine.globals["in"].mem
    weights = machine.globals["w"].mem
    rows = columns = 64
    width = 66
    values = []
    for i in range(rows):
        for j in range(columns):
            total = 0
            for ki in range(3):
                for kj in range(3):
                    total += (data.read_word(4 * ((i + ki) * width + j + kj), 4) *
                              weights.read_word(4 * (ki * 3 + kj), 4))
            values.append(total & 0xffffffff)
    return values


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: conv_nested_equivalence.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 50_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-conv-nested-") as temporary:
        base = os.path.join(temporary, "base.flat")
        opt = os.path.join(temporary, "opt.flat")
        compile_flat(compiler, source, [], base)
        stats = compile_flat(compiler, source, ["--opt=s24c", "--dump-opt-stats"], opt)
        if not re.search(r"^s24c conv-blocking blocked-conv=1$", stats, re.MULTILINE):
            raise AssertionError(f"no actual nested-conv transform: {stats}")
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
            raise AssertionError("nested conv output differs from arithmetic oracle")
        if before["effects"] == after["effects"]:
            raise AssertionError("nested conv output stores were not reordered")
        print(f"CONV_NESTED_EQUIVALENCE cells={len(expected_cells)} "
              f"ret={before['ret']} mem={before['mem']} effects_reordered=1")


if __name__ == "__main__":
    main()
