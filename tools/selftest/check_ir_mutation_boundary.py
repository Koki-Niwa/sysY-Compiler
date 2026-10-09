#!/usr/bin/env python3
"""F01: no direct live-IR construction/repair primitives in normal optimizers."""
import argparse
from pathlib import Path
import re
import sys

# Only src/opt and (future) src/analysis: readers and builders are different.
RAW = re.compile(
    r"(?:->|\.)\s*(?:setOperand|clearOperands|addSucc|setSucc|clearSuccs|"
    r"addInst|insertInst|removeInst|replaceAllUsesWith)\s*\("
    r"|\b(?:rebuildCFG|rebuildUseDef)\s*\("
)
COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)


def check(root):
    violations = []
    for folder in ("opt", "analysis"):
        directory = root / "src" / folder
        if not directory.is_dir():
            continue
        for file in sorted(directory.rglob("*")):
            if file.suffix not in (".cpp", ".h", ".inc"):
                continue
            text = file.read_text(encoding="utf-8")
            clean = COMMENTS.sub(lambda match: "\n" * match.group().count("\n"), text)
            for match in RAW.finditer(clean):
                line = clean.count("\n", 0, match.start()) + 1
                violations.append(f"{file.relative_to(root)}:{line}: {match.group()}")
    return violations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    args = parser.parse_args()
    bad = check(Path(args.root).resolve())
    for violation in bad:
        print("IR mutation boundary:", violation)
    if bad:
        print(f"FAIL: {len(bad)} raw mutation calls")
        return 1
    print("PASS: optimizer/analysis raw mutation boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
