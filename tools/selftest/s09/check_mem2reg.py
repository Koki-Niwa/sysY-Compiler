#!/usr/bin/env python3
"""S09's focused transformation gate and forged-statistic negative control."""

import argparse
import re
import subprocess
from pathlib import Path


PROMOTION = re.compile(
    r"(?m)^mem2reg promoted (?P<promoted>[1-9][0-9]*) allocas "
    r"generated-phis (?P<generated>[0-9]+) eliminated-phis (?P<eliminated>[0-9]+)$"
)
ALLOCA = re.compile(r"=\s*alloca\s+")
LOAD = re.compile(r"=\s*load\s+")
STORE = re.compile(r"\bstore\s+")
PHI = re.compile(r"\bphi\s+")
RETURN = re.compile(r"^\s*ret\s+\S+\s+\S+", re.MULTILINE)


def promotion_stats(stderr):
    match = PROMOTION.search(stderr)
    if match is None:
        return None
    return {key: int(value) for key, value in match.groupdict().items()}


def shape(text):
    return {
        "alloca": len(ALLOCA.findall(text)),
        "load": len(LOAD.findall(text)),
        "store": len(STORE.findall(text)),
        "phi": len(PHI.findall(text)),
    }


def transformation_errors(baseline, candidate, stats):
    before = shape(baseline)
    after = shape(candidate)
    errors = []
    if before["alloca"] == 0 or before["load"] == 0 or before["store"] == 0:
        errors.append(f"baseline lacks designated memory form: {before}")
    if baseline == candidate:
        errors.append("candidate Flat IR is byte-for-byte unchanged")
    if after["alloca"] != 0 or after["load"] != 0 or after["store"] != 0:
        errors.append(f"candidate retains alloca/load/store: {after}")
    if stats is None or stats["promoted"] <= 0:
        errors.append("promotion statistic is absent or zero")
    if after["phi"] == 0 and RETURN.search(candidate) is None:
        errors.append("candidate has neither phi nor a direct return value")
    return errors


def invoke(command, output):
    output.unlink(missing_ok=True)
    return subprocess.run(command + ["-o", str(output)], capture_output=True, text=True)


def report_failure(label, result):
    details = [f"{label} exited {result.returncode}"]
    if result.stderr.strip():
        details.append(result.stderr.strip())
    return details


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--shim", required=True, type=Path)
    parser.add_argument("--source", default=Path(__file__).with_name("promotable.flat"), type=Path)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args(argv)
    if not args.compiler.is_file() or (args.compiler.stat().st_mode & 0o111) == 0:
        print(f"S09 fixture: compiler is not executable: {args.compiler}")
        return 2
    if not args.shim.is_file() or not args.source.is_file():
        print("S09 fixture: shim or source is missing")
        return 2
    args.work.mkdir(parents=True, exist_ok=True)
    baseline_path = args.work / "baseline.flat"
    promoted_path = args.work / "promoted.flat"
    noop_path = args.work / "noop.flat"

    common = ["--from-flat", "--emit=flat-ir", "--dump-flat-stats", str(args.source)]
    baseline = invoke([str(args.compiler), *common], baseline_path)
    if baseline.returncode != 0:
        for line in report_failure("baseline compiler", baseline):
            print(line)
        return 1
    promoted = invoke([str(args.compiler), *common, "--mem2reg"], promoted_path)
    if promoted.returncode != 0:
        for line in report_failure("mem2reg compiler", promoted):
            print(line)
        return 1
    baseline_text = baseline_path.read_text(encoding="utf-8")
    promoted_text = promoted_path.read_text(encoding="utf-8")
    stats = promotion_stats(promoted.stderr)
    errors = transformation_errors(baseline_text, promoted_text, stats)
    if errors:
        print("S09 transformation FAIL")
        for error in errors:
            print(f"  {error}")
        return 1

    noop = invoke(
        ["bash", str(args.shim), "--from-flat", "--emit=flat-ir",
         "--dump-flat-stats", "--mem2reg", str(baseline_path)],
        noop_path,
    )
    if noop.returncode != 0:
        for line in report_failure("no-op shim", noop):
            print(line)
        return 1
    noop_text = noop_path.read_text(encoding="utf-8")
    forged = promotion_stats(noop.stderr)
    negative_errors = transformation_errors(baseline_text, noop_text, forged)
    if forged is None or forged["promoted"] <= 0 or not negative_errors:
        print("S09 negative control FAIL: forged-stat no-op was accepted")
        return 1

    before = shape(baseline_text)
    after = shape(promoted_text)
    print("S09 transformation PASS")
    print(f"S09_PROMOTED_ALLOCAS={stats['promoted']}")
    print(f"S09_BASELINE_MEMORY=alloca:{before['alloca']},load:{before['load']},store:{before['store']}")
    print(f"S09_PROMOTED_MEMORY=alloca:{after['alloca']},load:{after['load']},store:{after['store']},phi:{after['phi']}")
    print("S09 negative control PASS: unchanged IR with forged statistic rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
