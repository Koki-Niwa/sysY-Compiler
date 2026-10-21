#!/usr/bin/env python3
"""Static IR-size proxy for opt-in s24/s25 pipelines. Not target runtime."""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from flat_mod import FlatMod  # noqa: E402


def compile_counts(compiler, source, flags, timeout):
    fd, out = tempfile.mkstemp(suffix=".flat")
    os.close(fd)
    try:
        proc = subprocess.run([compiler, source, "--emit=flat-ir", "--mem2reg",
                               *flags, "-o", out],
                              capture_output=True, text=True, timeout=timeout)
        if proc.returncode != 0:
            return None
        with open(out, encoding="utf-8") as handle:
            mod = FlatMod(handle.read())
        blocks = sum(len(blocks) for blocks, _params, _captures in mod.funcs.values())
        insts = sum(len(insts) for blocks, _params, _captures in mod.funcs.values()
                    for insts in blocks.values())
        loops = sum(len(fn_loops) for fn_loops in mod.loops.values())
        return blocks, insts, loops
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def one_file(compiler, source, flags, timeout):
    base = compile_counts(compiler, source, [], timeout)
    opt = compile_counts(compiler, source, flags, timeout)
    return base, opt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    here = os.path.dirname(os.path.abspath(__file__))
    compiler_dir = os.path.dirname(os.path.dirname(here))
    workspace = os.path.dirname(compiler_dir)
    ap.add_argument("--dir", default=os.path.join(workspace, "tests"))
    ap.add_argument("--flags", default="--opt=s24")
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
    total = [0, 0, 0]
    opt = [0, 0, 0]
    comparable = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for base, changed in ex.map(
                lambda path: one_file(args.compiler, path, flags, args.timeout),
                files):
            if base is None or changed is None:
                continue
            comparable += 1
            for i in range(3):
                total[i] += base[i]
                opt[i] += changed[i]
    print(f"OPT_METRICS flags={' '.join(flags)} files={comparable}")
    for name, a, b in zip(("blocks", "insts", "loop-headers"), total, opt):
        delta = b - a
        pct = (100.0 * delta / a) if a else 0.0
        print(f"{name}: {a} -> {b} ({delta:+d}, {pct:+.2f}%)")


if __name__ == "__main__":
    main()
