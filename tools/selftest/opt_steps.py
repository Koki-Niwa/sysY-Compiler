#!/usr/bin/env python3
"""Dynamic FlatExec step proxy for opt-in pipelines. Not target runtime."""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from flat_exec import FlatExec  # noqa: E402
from flat_mod import FlatMod  # noqa: E402
from flat_exec_report import observable_same  # noqa: E402


def compile_run(compiler, source, flags, seed, timeout):
    fd, out = tempfile.mkstemp(suffix=".flat")
    os.close(fd)
    try:
        proc = subprocess.run([compiler, source, "--emit=flat-ir", "--mem2reg",
                               *flags, "-o", out],
                              capture_output=True, text=True, timeout=timeout)
        if proc.returncode != 0:
            return None
        with open(out, encoding="utf-8") as handle:
            text = handle.read()
        machine = FlatExec(FlatMod(text), seed=seed)
        trace = machine.run()
        return machine.steps, trace
    except (subprocess.TimeoutExpired, OSError, Exception):
        return None
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def one_file(compiler, source, flags, seed, timeout):
    return (compile_run(compiler, source, [], seed, timeout),
            compile_run(compiler, source, flags, seed, timeout))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    here = os.path.dirname(os.path.abspath(__file__))
    compiler_dir = os.path.dirname(os.path.dirname(here))
    workspace = os.path.dirname(compiler_dir)
    ap.add_argument("--dir", default=os.path.join(workspace, "tests"))
    ap.add_argument("--flags", default="--opt=s24")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    files = []
    for dirpath, dirnames, filenames in os.walk(args.dir):
        dirnames[:] = [d for d in dirnames if d not in (".git", "__pycache__")]
        files += [os.path.join(dirpath, f) for f in sorted(filenames)
                  if f.endswith(".sy")]
    files.sort()
    if args.limit:
        files = files[:args.limit]
    flags = args.flags.split()
    base_steps = opt_steps = 0
    comparable = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for base, opt in ex.map(
                lambda path: one_file(args.compiler, path, flags, args.seed,
                                      args.timeout), files):
            if base is None or opt is None or not observable_same(base[1], opt[1]):
                continue
            comparable += 1
            base_steps += base[0]
            opt_steps += opt[0]
    delta = opt_steps - base_steps
    pct = (100.0 * delta / base_steps) if base_steps else 0.0
    print(f"OPT_STEPS flags={' '.join(flags)} files={comparable} "
          f"base_steps={base_steps} opt_steps={opt_steps} "
          f"delta={delta:+d} ({pct:+.2f}%)")


if __name__ == "__main__":
    main()
