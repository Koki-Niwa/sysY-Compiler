#!/usr/bin/env python3
"""Independent S07 LLVM text gate.

The manifest is the identity authority.  Every ``skip=-`` entry must compile
with the compiler's default output, pass the closed LLVM subset checks, and
assemble with a warning-free ``llvm-as``.  Every ``skip=tensor`` entry must be
rejected by the frontend (exit 1, an actual diagnostic, and no output file).
"""

import argparse
import json
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import re
import subprocess
import sys
import tempfile


SUPPORTED = 490
TENSOR = 50
MAX_JOBS = 32
TIMEOUT = 30
SKIPS = {"-", "tensor"}
# Tensor is not a language keyword yet. Its fixtures must therefore reach the
# ordinary identifier diagnostic for the unsupported name; a random parser or
# backend failure is not evidence that this feature was rejected.
TENSOR_REJECTION = re.compile(
    r"error:\s*(?:\[E-UNDEF\].*未定义的标识符\s*'tensor'|"
    r"expected .*got identifier 'tensor')"
)
OPCODES = {
    "ret", "br", "unreachable", "alloca", "load", "store",
    "getelementptr", "add", "sub", "mul", "sdiv", "srem", "shl", "lshr",
    "ashr", "and", "or", "xor", "fadd", "fsub", "fmul", "fdiv", "fneg",
    "icmp", "fcmp", "sitofp", "fptosi", "fpext", "sext", "zext", "trunc",
    "bitcast", "call", "phi", "select",
}
INTRINSICS = {"llvm.memcpy", "llvm.memset"}
INTRINSIC_DECLARATIONS = {
    "llvm.memcpy.p0.p0.i64",
    "llvm.memset.p0.i64",
}
ASSIGN_RE = re.compile(
    r"^\s*%[-$A-Za-z0-9_.]+\s*=\s*(?:(?:tail|musttail|notail)\s+)?"
    r"([a-z][a-z0-9.]*)\b"
)
OP_RE = re.compile(r"^\s*(?:(?:tail|musttail|notail)\s+)?([a-z][a-z0-9.]*)\b")
LABEL_RE = re.compile(r"^\s*(?:[-$A-Za-z0-9_.]+):\s*(?:;.*)?$")
DEFINE_RE = re.compile(r"^\s*define\b")
DECLARE_RE = re.compile(r"^\s*declare\b")
DECLARE_NAME_RE = re.compile(
    r"^\s*declare\b.*?@(?P<name>[-$A-Za-z0-9_.]+)\s*\("
)
GLOBAL_RE = re.compile(
    r"^\s*@(?:[-$A-Za-z0-9_.]+|\"(?:[^\"\\]|\\.)*\")\s*=\s+global\b"
)


def _code_line(raw):
    """Remove a line comment without treating a semicolon in a string as one."""
    quote = False
    escaped = False
    for pos, char in enumerate(raw):
        if char == '"' and not escaped:
            quote = not quote
        if char == ";" and not quote:
            return raw[:pos]
        escaped = char == "\\" and not escaped
        if char != "\\":
            escaped = False
    return raw


def validate_ir(text):
    """Return independent invariant errors for one LLVM module."""
    errors = []
    defines = 0
    in_function = False
    block = 0
    saw_label = False
    for number, raw in enumerate(text.splitlines(), 1):
        line = _code_line(raw).strip()
        if not line:
            continue
        if line.startswith("target triple") or line.startswith("target datalayout"):
            errors.append(f"line {number}: target triple/datalayout is forbidden")
        if re.search(r'"target-(?:cpu|features|abi|fpga|machine)"', line):
            errors.append(f"line {number}: target attribute is forbidden")
        if re.search(r"\b(?:inbounds|nsw|nuw)\b", line):
            errors.append(f"line {number}: inbounds/nsw/nuw is forbidden")

        if not in_function:
            if DEFINE_RE.match(line):
                defines += 1
                # A definition header is the only module-level construct that
                # opens a function.  Keep the checker conservative here: an
                # inline body or a missing opening brace must not be mistaken
                # for an independently complete definition.  llvm-as remains
                # the authority for the complete LLVM grammar.
                if not line.endswith("{") or "}" in line:
                    errors.append(f"line {number}: malformed function definition header")
                    continue
                in_function, block, saw_label = True, 0, False
                continue
            if DECLARE_RE.match(line):
                declaration = DECLARE_NAME_RE.match(line)
                if declaration is None:
                    errors.append(f"line {number}: malformed module declaration")
                else:
                    name = declaration.group("name")
                    if name.startswith("llvm.") and name not in INTRINSIC_DECLARATIONS:
                        errors.append(
                            f"line {number}: intrinsic declaration outside subset: {name}"
                        )
                continue
            if GLOBAL_RE.match(line):
                continue
            errors.append(f"line {number}: unexpected module-level text")
            continue
        if line == "}":
            in_function = False
            continue
        if LABEL_RE.match(line):
            if saw_label:
                block += 1
            saw_label = True
            continue

        match = ASSIGN_RE.match(line) if "=" in line else None
        opcode = match.group(1) if match else None
        if opcode is None:
            match = OP_RE.match(line)
            opcode = match.group(1) if match else None
        if opcode and opcode not in OPCODES:
            errors.append(f"line {number}: opcode outside closed subset: {opcode}")
        if opcode == "alloca" and block != 0:
            errors.append(f"line {number}: alloca is outside the entry block")
        if opcode == "call":
            for intrinsic in re.findall(r"@((?:llvm\.)[A-Za-z0-9_.]+)", line):
                if intrinsic.split(".", 2)[:2] == ["llvm", "memcpy"]:
                    intrinsic = "llvm.memcpy"
                elif intrinsic.split(".", 2)[:2] == ["llvm", "memset"]:
                    intrinsic = "llvm.memset"
                if intrinsic not in INTRINSICS:
                    errors.append(f"line {number}: intrinsic outside subset: {intrinsic}")
    if in_function:
        errors.append("unterminated function definition")
    if defines == 0:
        errors.append("module has no function definition")
    return errors


def read_manifest(path, root, expected_supported=SUPPORTED, expected_tensor=TENSOR):
    """Read and validate exact manifest identities, returning case dictionaries."""
    rows = []
    seen = set()
    errors = []
    try:
        stream = Path(path).open(encoding="utf-8")
    except OSError as exc:
        return [], [f"cannot read manifest: {exc}"]
    with stream:
        for number, raw in enumerate(stream, 1):
            if not raw.strip() or raw.lstrip().startswith("#"):
                continue
            fields = raw.rstrip("\n").split("\t")
            if len(fields) != 7:
                errors.append(f"manifest line {number}: expected 7 tab-separated fields")
                continue
            suite, category, name, source, input_path, output_path, skip = fields
            key = "/".join((suite, category, name))
            if key in seen:
                errors.append(f"manifest line {number}: duplicate identity {key}")
            seen.add(key)
            if skip not in SKIPS:
                errors.append(f"manifest line {number}: unspecified skip {skip!r} for {key}")
            source_path = (Path(root) / source).resolve()
            root_path = Path(root).resolve()
            try:
                source_path.relative_to(root_path)
            except ValueError:
                errors.append(f"manifest line {number}: source escapes root: {source}")
            if not source_path.is_file() or source_path.suffix != ".sy":
                errors.append(f"manifest line {number}: source missing or not .sy: {source}")
            elif skip == "tensor":
                try:
                    source_text = source_path.read_text(encoding="utf-8")
                except (OSError, UnicodeError) as exc:
                    errors.append(f"manifest line {number}: cannot read tensor source: {exc}")
                else:
                    if not re.search(r"\btensor\b", source_text):
                        errors.append(f"manifest line {number}: tensor exclusion source has no tensor token: {source}")
            rows.append({"key": key, "source": str(source_path), "skip": skip,
                         "suite": suite, "category": category, "name": name,
                         "input": input_path, "output": output_path})
    supported = sum(row["skip"] == "-" for row in rows)
    tensor = sum(row["skip"] == "tensor" for row in rows)
    if supported != expected_supported or tensor != expected_tensor:
        errors.append(f"manifest counts are supported={supported}, tensor={tensor}; "
                      f"expected {expected_supported}/{expected_tensor}")
    if len(rows) != supported + tensor:
        errors.append("manifest contains rows outside the supported/tensor identity sets")
    return rows, errors


def _run(command, timeout):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        return exc


def _tool_version(path, timeout):
    """Return a short, stable version string for a report field."""
    result = _run([path, "--version"], timeout)
    if isinstance(result, subprocess.TimeoutExpired):
        return "TIMEOUT"
    output = ((result.stdout or "") + (result.stderr or "")).strip()
    first_line = output.splitlines()[0].strip() if output else ""
    if first_line:
        return first_line[:400]
    return f"unavailable (rc={result.returncode})"


def run_case(case, compiler, llvm_as, work, timeout):
    index = case["index"]
    case_dir = Path(work) / f"case-{index:04d}"
    case_dir.mkdir(parents=True, exist_ok=True)
    output = case_dir / "module.ll"
    bitcode = case_dir / "module.bc"
    output.unlink(missing_ok=True)
    bitcode.unlink(missing_ok=True)
    base = {"key": case["key"], "source": case["source"], "expected": case["skip"],
            "compiler_returncode": None, "llvm_as_returncode": None}
    result = _run([compiler, case["source"], "-o", str(output)], timeout)
    if isinstance(result, subprocess.TimeoutExpired):
        return {**base, "status": "TIMEOUT", "detail": "compiler timeout"}
    base["compiler_returncode"] = result.returncode
    if case["skip"] == "tensor":
        diagnostic = (result.stderr or "").strip()
        base["frontend_diagnostic"] = diagnostic[-4000:]
        if result.returncode == 1 and TENSOR_REJECTION.search(diagnostic) and not output.exists():
            return {**base, "status": "FRONTEND_REJECT", "returncode": 1}
        detail = ("expected tensor-specific frontend rejection (rc=1, E-UNDEF for tensor, "
                  f"no .ll), got rc={result.returncode}")
        return {**base, "status": "FAIL", "detail": detail,
                "stderr": diagnostic[-800:]}
    if result.returncode != 0 or not output.is_file() or output.stat().st_size == 0:
        return {**base, "status": "FAIL", "detail": "supported case did not produce .ll",
                "returncode": result.returncode, "stderr": (result.stderr or "")[-800:]}
    try:
        text = output.read_text(encoding="utf-8")
    except OSError as exc:
        return {**base, "status": "FAIL", "detail": f"cannot read .ll: {exc}"}
    errors = validate_ir(text)
    assembled = _run([llvm_as, str(output), "-o", str(bitcode)], timeout)
    if isinstance(assembled, subprocess.TimeoutExpired):
        errors.append("llvm-as timeout")
    elif assembled.returncode != 0:
        base["llvm_as_returncode"] = assembled.returncode
        errors.append(f"llvm-as failed: {(assembled.stderr or '').strip()[-500:]}")
    else:
        base["llvm_as_returncode"] = assembled.returncode
        if (assembled.stderr or "").strip():
            errors.append(f"llvm-as emitted a warning: {assembled.stderr.strip()[-500:]}")
        elif not bitcode.is_file():
            errors.append("llvm-as returned success without creating bitcode")
    if errors:
        return {**base, "status": "FAIL", "detail": "; ".join(errors[:8])}
    return {**base, "status": "PASS"}


def reconcile(rows, results):
    """Check exact identity and status/count reconciliation."""
    expected = {row["key"]: row["skip"] for row in rows}
    actual = {result.get("key"): result for result in results}
    errors = []
    if len(results) != len(rows) or set(actual) != set(expected):
        errors.append("per-case results do not cover the exact manifest identities")
    if len(actual) != len(results):
        errors.append("per-case results contain duplicate identities")
    for key, skip in expected.items():
        result = actual.get(key)
        if result is None:
            continue
        wanted = "PASS" if skip == "-" else "FRONTEND_REJECT"
        if result.get("status") != wanted:
            errors.append(f"{key}: expected {wanted}, got {result.get('status')}")
    pass_count = sum(r.get("status") == "PASS" for r in results)
    reject_count = sum(r.get("status") == "FRONTEND_REJECT" for r in results)
    if pass_count != sum(row["skip"] == "-" for row in rows):
        errors.append(f"supported result count {pass_count} does not match manifest")
    if reject_count != sum(row["skip"] == "tensor" for row in rows):
        errors.append(f"frontend rejection count {reject_count} does not match manifest")
    return errors


def execute(args):
    root = Path(args.root).resolve()
    rows, errors = read_manifest(args.manifest, root, args.expected_supported,
                                 args.expected_tensor)
    if errors:
        return 1, {"manifest_errors": errors, "results": []}
    compiler = str(Path(args.compiler).resolve())
    llvm_as = str(Path(args.llvm_as).resolve())
    if not os.access(compiler, os.X_OK):
        return 1, {"setup_errors": [f"compiler is not executable: {compiler}"], "results": []}
    if not os.access(llvm_as, os.X_OK):
        return 1, {"setup_errors": [f"llvm-as is not executable: {llvm_as}"], "results": []}
    jobs = max(1, min(int(args.jobs), MAX_JOBS))
    temporary = None
    if args.work:
        work = Path(args.work).resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="llvm_ir_gate_")
        work = Path(temporary.name)
    compiler_version = _tool_version(compiler, args.timeout)
    llvm_as_version = _tool_version(llvm_as, args.timeout)
    cases = [dict(row, index=index) for index, row in enumerate(rows)]
    results = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_case, case, compiler, llvm_as, str(work), args.timeout)
                   for case in cases]
        for future in as_completed(futures):
            results.append(future.result())
    results.sort(key=lambda result: result["key"])
    errors = reconcile(rows, results)
    report = {"manifest": str(Path(args.manifest).resolve()), "compiler": compiler,
              "compiler_version": compiler_version, "llvm_as": llvm_as,
              "llvm_as_version": llvm_as_version, "jobs": jobs, "results": results,
              "summary": {"total": len(results),
                          "pass": sum(r["status"] == "PASS" for r in results),
                          "frontend_reject": sum(r["status"] == "FRONTEND_REJECT" for r in results),
                          "fail": sum(r["status"] == "FAIL" for r in results),
                          "timeout": sum(r["status"] == "TIMEOUT" for r in results)},
              "errors": errors}
    if temporary is not None:
        temporary.cleanup()
    return (0 if not errors else 1), report


def main(argv=None):
    here = Path(__file__).resolve()
    default_root = here.parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(default_root))
    parser.add_argument("--manifest", default=None)
    parser.add_argument("--compiler", default=str(default_root / "compiler/build/compiler"))
    parser.add_argument("--llvm-as", default=os.environ.get("LLVM_AS", "/usr/lib/llvm-18/bin/llvm-as"))
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=float, default=TIMEOUT)
    parser.add_argument("--work", default=None)
    parser.add_argument("--report", default=None)
    parser.add_argument("--expected-supported", type=int, default=SUPPORTED)
    parser.add_argument("--expected-tensor", type=int, default=TENSOR)
    args = parser.parse_args(argv)
    if args.manifest is None:
        args.manifest = str(Path(args.root) / "tests/manifest.tsv")
    try:
        rc, report = execute(args)
    except (OSError, ValueError) as exc:
        rc, report = 1, {"setup_errors": [str(exc)], "results": []}
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                     encoding="utf-8")
    summary = report.get("summary")
    if summary:
        print("LLVM manifest: total={total} pass={pass} frontend_reject={frontend_reject} "
              "fail={fail} timeout={timeout}".format(**summary))
    for error in report.get("manifest_errors", []) + report.get("setup_errors", []) + report.get("errors", []):
        print("FAIL: " + error, file=sys.stderr)
    for result in report.get("results", []):
        if result.get("status") in {"FAIL", "TIMEOUT"}:
            print("FAIL: {}: {}".format(result["key"], result.get("detail", "unknown")),
                  file=sys.stderr)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
