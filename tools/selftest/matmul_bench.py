#!/usr/bin/env python3
"""Fixed s24c pipeline comparison on the native host through Clang -O0 (backend proxy).

Times only calls into generated code, excluding process startup and validation.
Every warmup/sample must produce the entire expected C matrix and return value.
The s24c pipeline includes canonicalization and DCE as well as blocking; this
comparison does not isolate the blocking pass. This is neither an A53/BOOM
measurement nor the compiler's own backend runtime.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time

from flat_exec import FlatExec, FlatMod
from matmul_equivalence import compile_flat, expected_matrix, rectangular_fixture


def checked(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          timeout=60, **kwargs)


def build(compiler, clang, source, directory, label, flag):
    llvm = directory / (label + ".ll")
    library = directory / (label + ".so")
    assembly = directory / (label + ".s")
    rss_file = directory / (label + ".rss")
    command = [compiler, str(source), "--emit=llvm-ir", "--mem2reg",
               "--dump-opt-stats", *flag, "-o", str(llvm)]
    started = time.perf_counter()
    result = checked(["/usr/bin/time", "-f", "%M", "-o", str(rss_file), *command])
    compile_seconds = time.perf_counter() - started
    if flag and "s24c matmul-blocking blocked-matmul=1\n" not in result.stderr:
        raise AssertionError("no matmul transform in measured optimized binary")
    started = time.perf_counter()
    checked([clang, "-O0", "-fPIC", "-shared", "-Wl,-Bsymbolic",
             str(llvm), "-o", str(library)])
    backend_seconds = time.perf_counter() - started
    checked([clang, "-O0", "-fPIC", "-S", str(llvm), "-o", str(assembly)])
    loaded = ctypes.CDLL(str(library), mode=os.RTLD_LOCAL)
    loaded.main.argtypes = []
    loaded.main.restype = ctypes.c_int32
    return loaded, {
        "compiler_seconds": compile_seconds,
        "compiler_peak_rss_kib": int(rss_file.read_text().strip()),
        "backend_seconds": backend_seconds,
        "library_bytes": library.stat().st_size,
        "assembly_bytes": assembly.stat().st_size,
        "llvm_sha256": hashlib.sha256(llvm.read_bytes()).hexdigest(),
    }


def sample(library, expected, expected_return):
    c = (ctypes.c_uint32 * len(expected)).in_dll(library, "C")
    # Poison output before every run so a skipped tail cannot inherit a prior
    # correct result. Initialization and checking are outside the timed region.
    for index, value in enumerate(expected):
        c[index] = value ^ 0xffffffff
    started = time.perf_counter_ns()
    result = library.main()
    seconds = (time.perf_counter_ns() - started) / 1e9
    if result != expected_return or list(c) != expected:
        raise AssertionError("native output differs from full arithmetic oracle")
    return seconds


def benchmark(compiler, clang, directory, shape, warmups, repeats):
    rows, columns, reduction = shape
    label = f"{rows}x{columns}x{reduction}"
    source = directory / (label + ".sy")
    source.write_text(rectangular_fixture(*shape), encoding="utf-8")
    flat = directory / (label + ".flat")
    compile_flat(compiler, str(source), [], str(flat))
    machine = FlatExec(FlatMod(flat.read_text(encoding="utf-8")))
    expected = expected_matrix(machine)
    expected_return = ctypes.c_int32(expected[0] + expected[-1]).value
    base, base_info = build(compiler, clang, source, directory, label + ".base", [])
    opt, opt_info = build(compiler, clang, source, directory, label + ".s24c", ["--opt=s24c"])
    times = {"base": [], "s24c": []}
    for iteration in range(warmups + repeats):
        # Alternate pass-on/off order to reduce systematic ordering bias.
        pair = (("base", base), ("s24c", opt))
        if iteration % 2:
            pair = pair[::-1]
        for name, library in pair:
            elapsed = sample(library, expected, expected_return)
            if iteration >= warmups:
                times[name].append(elapsed)
    for name, info in (("base", base_info), ("s24c", opt_info)):
        values = times[name]
        info.update(samples_seconds=values, median_seconds=statistics.median(values),
                    min_seconds=min(values), max_seconds=max(values),
                    stdev_seconds=statistics.pstdev(values))
    return dict(case=label, input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                correctness="all C cells and return match arithmetic oracle on every run",
                warmups=warmups, repeats=repeats, base=base_info, s24c=opt_info,
                delta_percent=(opt_info["median_seconds"] / base_info["median_seconds"] - 1) * 100)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=11)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.warmups < 1 or args.repeats < 3:
        parser.error("at least one warmup and three samples are required")
    report = dict(metric="native-host Clang -O0 proxy; not A53/BOOM or own backend",
                  comparison="mem2reg vs mem2reg+s24c (canonicalization, DCE, blocking)",
                  compiler_sha256=hashlib.sha256(Path(args.compiler).read_bytes()).hexdigest(),
                  host=platform.platform(), machine=platform.machine(),
                  clang=checked([args.clang, "--version"]).stdout.splitlines()[0],
                  backend_flags=["-O0", "-fPIC", "-shared", "-Wl,-Bsymbolic"],
                  skipped=0, timeouts=0, cases=[])
    with tempfile.TemporaryDirectory(prefix="sysy-matmul-bench-") as temporary:
        for shape in ((64, 64, 64), (65, 67, 7), (67, 65, 5)):
            result = benchmark(args.compiler, args.clang, Path(temporary), shape,
                               args.warmups, args.repeats)
            report["cases"].append(result)
            print(f"{result['case']}: delta={result['delta_percent']:+.2f}% oracle=PASS")
    ratios = [case["base"]["median_seconds"] / case["s24c"]["median_seconds"]
              for case in report["cases"]]
    report["geomean_speedup"] = statistics.geometric_mean(ratios)
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
