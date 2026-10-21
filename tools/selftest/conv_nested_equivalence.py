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


def expected(machine, kernel_h, kernel_w, width):
    data = machine.globals["in"].mem
    weights = machine.globals["w"].mem
    rows = columns = 64
    values = []
    for i in range(rows):
        for j in range(columns):
            total = 0
            for ki in range(kernel_h):
                for kj in range(kernel_w):
                    total += (data.read_word(4 * ((i + ki) * width + j + kj), 4) *
                              weights.read_word(4 * (ki * kernel_w + kj), 4))
            values.append(total & 0xffffffff)
    return values


def rectangular_fixture(kernel_h, kernel_w):
    width = 64 + kernel_w - 1
    height = 64 + kernel_h - 1
    data = [((i * 7 + 3) % 19 - 9) for i in range(width * height)]
    weights = [((i * 5 + 2) % 13 - 6) for i in range(kernel_h * kernel_w)]
    return (f"int in[{len(data)}] = {{{','.join(map(str, data))}}};\n"
            f"int w[{len(weights)}] = {{{','.join(map(str, weights))}}};\n"
            "int out[64][64];\n"
            "int main() {\n"
            "  int i = 0;\n"
            "  while (i < 64) {\n"
            "    int j = 0;\n"
            "    while (j < 64) {\n"
            "      int acc = 0;\n"
            "      int ki = 0;\n"
            f"      while (ki < {kernel_h}) {{\n"
            "        int kj = 0;\n"
            f"        while (kj < {kernel_w}) {{\n"
            f"          acc = acc + in[(i + ki) * {width} + j + kj] * w[ki * {kernel_w} + kj];\n"
            "          kj = kj + 1;\n"
            "        }\n"
            "        ki = ki + 1;\n"
            "      }\n"
            "      out[i][j] = acc;\n"
            "      j = j + 1;\n"
            "    }\n"
            "    i = i + 1;\n"
            "  }\n"
            "  return out[3][4] + out[31][63];\n"
            "}\n")


def check_case(compiler, source, kernel_h, kernel_w, width, label, temporary):
    base = os.path.join(temporary, label + ".base.flat")
    opt = os.path.join(temporary, label + ".opt.flat")
    compile_flat(compiler, source, [], base)
    stats = compile_flat(compiler, source, ["--opt=s24c", "--dump-opt-stats"], opt)
    if not re.search(r"^s24c conv-blocking blocked-conv=1$", stats, re.MULTILINE):
        raise AssertionError(f"{label}: no actual nested-conv transform: {stats}")
    before_machine = FlatExec(FlatMod(open(base, encoding="utf-8").read()))
    after_machine = FlatExec(FlatMod(open(opt, encoding="utf-8").read()))
    expected_cells = expected(before_machine, kernel_h, kernel_w, width)
    before, after = before_machine.run(), after_machine.run()
    for key in ("out", "mem", "ret", "input_state"):
        if before[key] != after[key]:
            raise AssertionError(f"{label} {key}: {before[key]} != {after[key]}")
    actual = [before_machine.globals["out"].mem.read_word(4 * index, 4)
              for index in range(len(expected_cells))]
    if actual != expected_cells:
        raise AssertionError(f"{label}: output differs from arithmetic oracle")
    if before["effects"] == after["effects"]:
        raise AssertionError(f"{label}: output stores were not reordered")
    print(f"CONV_NESTED_EQUIVALENCE case={label} cells={len(expected_cells)} "
          f"ret={before['ret']} mem={before['mem']} effects_reordered=1")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: conv_nested_equivalence.py COMPILER FIXTURE")
    compiler, source = sys.argv[1], sys.argv[2]
    FlatExec.MAX_STEPS = 50_000_000
    with tempfile.TemporaryDirectory(prefix="sysy-conv-nested-") as temporary:
        check_case(compiler, source, 3, 3, 66, "square3x3", temporary)
        rect = os.path.join(temporary, "rect5x7.sy")
        with open(rect, "w", encoding="utf-8") as stream:
            stream.write(rectangular_fixture(5, 7))
        check_case(compiler, rect, 5, 7, 70, "rect5x7", temporary)


if __name__ == "__main__":
    main()
