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


def resolve_compiler_root(root):
    """Accept a compiler checkout or its containing workspace."""
    root = Path(root).resolve()
    if (root / "src").is_dir():
        return root
    nested = root / "compiler"
    if (nested / "src").is_dir():
        return nested
    return None


def check(root):
    source_root = resolve_compiler_root(root)
    violations = []
    if source_root is None:
        return [f"compiler source root not found under {Path(root).resolve()}"]
    for folder in ("opt", "analysis"):
        directory = source_root / "src" / folder
        if not directory.is_dir():
            violations.append(f"{directory.relative_to(source_root)}: source directory missing")
            continue
        files = [file for file in sorted(directory.rglob("*"))
                 if file.suffix in (".cpp", ".h", ".inc") and file.is_file()]
        if not files:
            violations.append(f"{directory.relative_to(source_root)}: source directory is empty")
            continue
        for file in files:
            text = file.read_text(encoding="utf-8")
            clean = COMMENTS.sub(lambda match: "\n" * match.group().count("\n"), text)
            for match in RAW.finditer(clean):
                line = clean.count("\n", 0, match.start()) + 1
                violations.append(f"{file.relative_to(source_root)}:{line}: {match.group()}")
    return violations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=str(Path(__file__).resolve().parents[2]),
        help="compiler checkout or containing workspace (default: this script's compiler root)")
    args = parser.parse_args()
    bad = check(Path(args.root).resolve())
    for violation in bad:
        print("IR mutation boundary:", violation)
    if bad:
        print(f"FAIL: {len(bad)} boundary violations")
        return 1
    print("PASS: optimizer/analysis raw mutation boundary")
    return 0


if __name__ == "__main__":
    sys.exit(main())
