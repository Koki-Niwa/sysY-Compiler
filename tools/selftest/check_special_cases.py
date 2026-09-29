#!/usr/bin/env python3
"""Find function-name and benchmark-name special cases in product code."""

import argparse
from collections import Counter
from pathlib import Path
import re


SPECIAL_NAME = re.compile(
    r'"(getint|getch|getfloat|getarray|getfarray|putint|putch|putfloat|'
    r'putarray|putfarray|putf|matmul|transpose|fft|shuffle|crypto|huffman|sl1)"'
)
CATALOG_FILES = {"support/RuntimeLib.h"}

# putf is the one deliberate backend ABI exception: its C variadic signature
# cannot be represented by flat Function parameters. Keep the allowed uses
# local and exact so unrelated name-based behavior remains rejected.
ALLOWED_ABI_LINES = {
    ("ir/LLVMTextEmitter.cpp", 'if (name == "putf") {'),
    ("ir/LLVMTextEmitter.cpp", 'if (fn->name() == "putf") continue;  // The closed ABI contract is variadic.'),
    ("ir/LLVMTextEmitterValidation.inc", 'if (!f->isDeclaration() && f->name() == "putf") {'),
    ("ir/LLVMTextEmitterValidation.inc", 'if (f->isDeclaration() && f->name() == "putf" &&'),
    ("ir/LLVMTextEmitterValidation.inc", 'if (inst->op() == Opcode::Call && inst->callee() == "putf") {'),
    ("ir/LLVMTextEmitterValidation.inc", 'if (user->op() != Opcode::Call || user->callee() != "putf" || argument == 0) {'),
}


def violations(source_root):
    source_root = Path(source_root).resolve()
    if not source_root.is_dir():
        return [f"product source directory does not exist: {source_root}"]
    errors = []
    allowed_counts = Counter()
    for path in sorted(source_root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(source_root).as_posix()
        if relative in CATALOG_FILES:
            continue
        try:
            lines = path.read_bytes().splitlines()
        except OSError as exc:
            errors.append(f"{path}: cannot read product source: {exc}")
            continue
        for number, raw_line in enumerate(lines, 1):
            line = raw_line.decode("utf-8", errors="replace")
            if not SPECIAL_NAME.search(line):
                continue
            allowed_key = (relative, line.strip())
            if allowed_key in ALLOWED_ABI_LINES:
                allowed_counts[allowed_key] += 1
                if allowed_counts[allowed_key] > 1:
                    errors.append(f"{path}:{number}: duplicate allowlisted putf ABI rule")
                continue
            errors.append(f"{path}:{number}: {line.strip()}")
    for allowed_key in sorted(ALLOWED_ABI_LINES):
        if allowed_counts[allowed_key] != 1:
            errors.append(f"allowlisted putf ABI rule occurs {allowed_counts[allowed_key]} times: "
                          f"{allowed_key[0]}: {allowed_key[1]}")
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    root = Path(parser.parse_args().root).resolve()
    source = root / "src"
    if not source.is_dir():
        source = root / "compiler" / "src"
    errors = violations(source)
    for error in errors:
        print(error)
    print("Special-case scan: " + ("FAIL" if errors else "PASS"))
    raise SystemExit(bool(errors))
