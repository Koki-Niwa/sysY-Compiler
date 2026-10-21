#!/usr/bin/env python3
"""Matmul blocking: actual rewrites, full memory equality and arithmetic oracle."""

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


def matrix_shape(mod, name):
    match = re.fullmatch(r"\[(\d+) x \[(\d+) x i32\]\]", mod.globals[name][0])
    if not match:
        raise AssertionError(f"{name}: expected a two-dimensional i32 matrix")
    return tuple(map(int, match.groups()))


def expected_matrix(machine):
    """Compute C directly from the initialized arrays, independently of IR loops."""
    rows, reduction = matrix_shape(machine.mod, "A")
    reduction_b, columns = matrix_shape(machine.mod, "B")
    if reduction != reduction_b or matrix_shape(machine.mod, "C") != (rows, columns):
        raise AssertionError("incompatible fixture matrix dimensions")
    a = machine.globals["A"].mem
    b = machine.globals["B"].mem
    return [sum(a.read_word(4 * (i * reduction + k), 4) *
                b.read_word(4 * (k * columns + j), 4)
                for k in range(reduction)) & 0xffffffff
            for i in range(rows) for j in range(columns)]


def check_case(compiler, source, directory, label):
    base = os.path.join(directory, label + ".base.flat")
    opt = os.path.join(directory, label + ".opt.flat")
    compile_flat(compiler, source, [], base)
    stats = compile_flat(compiler, source, ["--opt=s24c", "--dump-opt-stats"], opt)
    if not re.search(r"^s24c matmul-blocking blocked-matmul=1$", stats, re.MULTILINE):
        raise AssertionError(f"{label}: no actual matmul transformation: {stats}")
    with open(base, encoding="utf-8") as stream:
        before_machine = FlatExec(FlatMod(stream.read()))
    with open(opt, encoding="utf-8") as stream:
        after_machine = FlatExec(FlatMod(stream.read()))
    expected = expected_matrix(before_machine)
    before, after = before_machine.run(), after_machine.run()
    for key in ("out", "mem", "ret", "input_state"):
        if before[key] != after[key]:
            raise AssertionError(f"{label} {key}: {before[key]} != {after[key]}")
    for name, machine in (("base", before_machine), ("blocked", after_machine)):
        actual = [machine.globals["C"].mem.read_word(4 * index, 4)
                  for index in range(len(expected))]
        if actual != expected:
            raise AssertionError(f"{label} {name}: C differs from arithmetic oracle")
    if before["effects"] == after["effects"]:
        raise AssertionError(f"{label}: output stores were not reordered")
    print(f"MATMUL_EQUIVALENCE case={label} cells={len(expected)} "
          f"ret={before['ret']} mem={before['mem']} effects_reordered=1")


def rectangular_fixture(rows, columns, reduction):
    def initializer(n, m, value):
        return "{" + ",".join("{" + ",".join(str(value(i, j)) for j in range(m)) + "}"
                              for i in range(n)) + "}"
    a = initializer(rows, reduction, lambda i, k: (i * 3 + k * 7) % 17 - 8)
    b = initializer(reduction, columns, lambda k, j: (k * 5 + j * 2) % 13 - 6)
    c = initializer(rows, columns, lambda i, j: -999)
    return f"""int A[{rows}][{reduction}] = {a};
int B[{reduction}][{columns}] = {b};
int C[{rows}][{columns}] = {c};
int main() {{
  int i = 0;
  while (i < {rows}) {{
    int j = 0;
    while (j < {columns}) {{
      int k = 0;
      int s = 0;
      while (k < {reduction}) {{
        s = s + A[i][k] * B[k][j];
        k = k + 1;
      }}
      C[i][j] = s;
      j = j + 1;
    }}
    i = i + 1;
  }}
  return C[0][0] + C[{rows - 1}][{columns - 1}];
}}
"""


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: matmul_equivalence.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 200_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-matmul-") as temporary:
        check_case(compiler, source, temporary, "square64")
        # Non-square output, independent K extent, and tails on both tile axes.
        for rows, columns, reduction in ((65, 67, 7), (67, 65, 5)):
            label = f"{rows}x{columns}x{reduction}"
            fixture = os.path.join(temporary, label + ".sy")
            with open(fixture, "w", encoding="utf-8") as stream:
                stream.write(rectangular_fixture(rows, columns, reduction))
            check_case(compiler, fixture, temporary, label)


if __name__ == "__main__":
    main()
