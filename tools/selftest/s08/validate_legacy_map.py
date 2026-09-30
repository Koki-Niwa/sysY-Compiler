#!/usr/bin/env python3
"""Check that S08's replacement map still names every public legacy bad case."""
import ast
import re
import sys
from pathlib import Path


def bad_case_names(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "BAD_CASES"
            for target in node.targets
        ):
            return [ast.literal_eval(item.elts[0]) for item in node.value.elts]
    raise ValueError(f"BAD_CASES assignment not found: {path}")


def mapped_names_and_replacements(path):
    source = path.read_text(encoding="utf-8")
    match = re.search(r"const LegacyMap kLegacy\[\] = \{(.*?)\n\};", source, re.S)
    if match is None:
        raise ValueError(f"kLegacy table not found: {path}")
    rows = re.findall(
        r'\{"([^\"]+)",\s*"([^\"]+)",\s*"([^\"]+)",\s*"([^\"]+)",',
        match.group(1),
    )
    if not rows:
        raise ValueError(f"kLegacy table has no rows: {path}")

    case_match = re.search(r"const MatrixCase cases\[\] = \{(.*?)\n  \};", source, re.S)
    if case_match is None:
        raise ValueError(f"MatrixCase table not found: {path}")
    case_names = set(re.findall(r'\{"([^\"]+)",\s*"(?:ACCEPT|V[1-6])",', case_match.group(1)))
    return [row[0] for row in rows], [row[3] for row in rows], case_names


def main():
    matrix = Path(sys.argv[1])
    legacy = Path(sys.argv[2])
    expected = bad_case_names(legacy)
    names, replacements, cases = mapped_names_and_replacements(matrix)
    failures = []
    if names != expected:
        failures.append(f"legacy identities differ: expected={expected!r}, mapped={names!r}")
    for legacy_name, replacement in zip(names, replacements):
        if replacement not in cases:
            failures.append(f"{legacy_name}: replacement fixture missing: {replacement}")
    if failures:
        print("S08 legacy mapping FAIL")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(f"S08 legacy mapping PASS: {len(names)} identities and replacement fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
