#!/usr/bin/env python3
"""Write an atomic, revision-bound S10a evidence manifest.

No manifest means NOT_RUN.  RUNNING and FAIL are durable non-success states;
PASS is accepted only when the completed driver log contains every required
summary and negative-control marker.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


VALID_STATES = {"RUNNING", "FAIL", "PASS"}
SEED = 0x5A10A2026


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            value.update(chunk)
    return value.hexdigest()


def git_text(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root / "compiler"), *args],
        check=True, capture_output=True, text=True,
    )
    return result.stdout.strip()


def validate_pass(run_log: Path, run_err: Path) -> None:
    text = run_log.read_text(encoding="utf-8")
    summaries = [line for line in text.splitlines() if line.startswith("S10a fixed=")]
    if len(summaries) != 1:
        raise ValueError("PASS report requires exactly one S10a summary")
    summary = summaries[0]
    required_summary = (
        "S10a fixed=115/115 repeat=115/115 generated=32/32",
        f"seed={SEED}", "replay=PASS", "oracle=s10a-recipe-oracle-v1",
        "generated-negative-control=PASS",
        "constraints=PASS", "hash-vectors=PASS", "limits=PASS",
        "expression-limit=PASS", "statement-limit=PASS",
        "total-allocation-limit=PASS", "large-array-limit=PASS",
        "invalid-input=PASS", "invalid-ast=PASS", "uninitialized=PASS",
        "missing-return=PASS", "negative-control=PASS",
    )
    missing = [marker for marker in required_summary if marker not in summary]
    if missing:
        raise ValueError("PASS summary is missing required markers: " + ", ".join(missing))
    generated = re.findall(
        r"^S10a generated-case index=([0-9]+) source-hash=([0-9]+) expected=(-?[0-9]+)$",
        text, re.MULTILINE,
    )
    if len(generated) != 32 or {int(item[0]) for item in generated} != set(range(32)):
        raise ValueError("PASS report requires one generated-case record for each index 0..31")
    features = (
        "arith", "float", "branch", "loop", "break", "continue", "call",
        "recursion", "array", "init", "scope", "global", "short", "io",
        "input", "cast", "runtime", "timing", "wrap", "ub-normalized",
        "boundary",
    )
    for feature in features:
        if re.search(rf"^  {re.escape(feature)}=[1-9][0-9]*$", text, re.MULTILINE) is None:
            raise ValueError(f"PASS report lacks nonzero coverage for {feature}")
    if text.splitlines().count("S10a PASS") != 1:
        raise ValueError("PASS report requires exactly one final S10a PASS verdict")
    if run_err.read_bytes():
        raise ValueError("PASS report requires empty run.err")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", required=True, choices=sorted(VALID_STATES))
    parser.add_argument("--report", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--reason", default="")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    work = Path(args.work).resolve()
    report = Path(args.report).resolve()
    run_log, run_err = work / "run.out", work / "run.err"
    status = git_text(root, "status", "--short")
    effective_state = args.state
    reason = args.reason
    if args.state == "PASS":
        try:
            validate_pass(run_log, run_err)
        except (OSError, ValueError) as error:
            effective_state = "FAIL"
            reason = f"invalid PASS evidence: {error}"
        if status:
            effective_state = "FAIL"
            reason = "PASS evidence requires a clean compiler source revision"

    tracked_sources = sorted(
        list((root / "compiler/tools/selftest/interpreter").glob("s10a_*.[ch]*"))
        + list((root / "compiler/src/interpreter").glob("Interpreter*.[ch]*"))
    )
    artifact_candidates = [work / "build.log", run_log, run_err, work / "s10a"]
    artifacts = {
        str(path.relative_to(root) if path.is_relative_to(root) else path): digest(path)
        for path in artifact_candidates + tracked_sources if path.is_file()
    }
    payload = {
        "schema": "sysy.s10a.evidence.v1",
        "state": effective_state,
        "reason": reason,
        "source_revision": git_text(root, "rev-parse", "HEAD"),
        "source_dirty": bool(status),
        "seed": SEED,
        "fixed_cases": 115,
        "generated_cases": 32,
        "oracle": "s10a-recipe-oracle-v1",
        "work_directory": str(work),
        "artifacts_sha256": artifacts,
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=report.name + ".", dir=report.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(payload, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary, report)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return 0 if effective_state == args.state else 1


if __name__ == "__main__":
    raise SystemExit(main())
