#!/usr/bin/env python3
"""AArch64 QEMU wall-clock proxy for opt-in passes. NOT A53/BOOM hardware."""

import argparse
import os
import statistics
import subprocess
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(HERE, "benchmarks")


def build(compiler, source, flags):
    directory = tempfile.mkdtemp(prefix="sysy-proxy-")
    ll = os.path.join(directory, "m.ll")
    exe = os.path.join(directory, "m.a64")
    proc = subprocess.run([compiler, source, "--emit=llvm-ir", "--mem2reg",
                           *flags, "-o", ll], capture_output=True, text=True)
    if proc.returncode != 0:
        return None, None
    proc = subprocess.run(["clang", "--target=aarch64-linux-gnu", "-O0",
                           "-static", ll, "-o", exe],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None, None
    return directory, exe


def run(exe, reps):
    times, returns = [], []
    for _ in range(reps):
        start = time.perf_counter()
        proc = subprocess.run(["qemu-aarch64", exe], capture_output=True)
        times.append(time.perf_counter() - start)
        returns.append(proc.returncode)
    return statistics.median(times), returns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--reps", type=int, default=5)
    args = ap.parse_args()
    cases = (("unroll", os.path.join(BENCH, "unroll.sy"), "--opt=s25"),
             ("blocking", os.path.join(BENCH, "blocking.sy"), "--opt=s24b"))
    print("QEMU_AARCH64_PROXY (not A53/BOOM hardware)")
    for name, source, flag in cases:
        base_dir, base = build(args.compiler, source, [])
        opt_dir, opt = build(args.compiler, source, [flag])
        if base is None or opt is None:
            print(f"{name}: build failed")
            continue
        base_t, base_r = run(base, args.reps)
        opt_t, opt_r = run(opt, args.reps)
        if len(set(base_r + opt_r)) != 1:
            print(f"{name}: return differs {base_r} vs {opt_r}")
            continue
        delta = (opt_t - base_t) / base_t * 100.0
        print(f"{name}: base={base_t*1000:.2f}ms opt={opt_t*1000:.2f}ms "
              f"delta={delta:+.2f}% return={base_r[0]}")


if __name__ == "__main__":
    main()
