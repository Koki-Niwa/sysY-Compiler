#!/usr/bin/env python3
"""Aggregate opt-in s24/s25 pass counters over a SysY corpus."""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile


def one_file(compiler, path, flags, timeout):
    fd, out = tempfile.mkstemp(suffix=".flat")
    os.close(fd)
    try:
        proc = subprocess.run(
            [compiler, path, "--emit=flat-ir", "--mem2reg", *flags,
             "--dump-opt-stats", "-o", out],
            capture_output=True, text=True, timeout=timeout)
        stats = [line for line in proc.stderr.splitlines()
                 if line.startswith(("s24 ", "s24b ", "s25 "))]
        return proc.returncode == 0, stats
    except subprocess.TimeoutExpired:
        return False, []
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    here = os.path.dirname(os.path.abspath(__file__))
    compiler_dir = os.path.dirname(os.path.dirname(here))
    workspace = os.path.dirname(compiler_dir)
    ap.add_argument("--dir", default=os.path.join(workspace, "tests"))
    ap.add_argument("--flags", default="--opt=s24",
                    help="space-separated flags, e.g. '--opt=s24 --opt=s25'")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    files = []
    for dirpath, dirnames, filenames in os.walk(args.dir):
        dirnames[:] = [d for d in dirnames if d not in (".git", "__pycache__")]
        files += [os.path.join(dirpath, f) for f in sorted(filenames)
                  if f.endswith(".sy")]
    files.sort()
    flags = args.flags.split()
    counters, changed, ok = {}, 0, 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for success, stats in ex.map(
                lambda path: one_file(args.compiler, path, flags, args.timeout),
                files):
            if not success:
                continue
            ok += 1
            if stats:
                changed += 1
            for line in stats:
                parts = line.split()
                name = parts[0] + " " + parts[1] + " " + parts[2].split("=")[0]
                counters[name] = counters.get(name, 0) + int(parts[-1].split("=")[-1])
    print(f"OPT_STATS flags={' '.join(flags)} files={len(files)} compiled={ok} "
          f"with_stats={changed}")
    for name in sorted(counters):
        print(f"{name}={counters[name]}")


if __name__ == "__main__":
    main()
