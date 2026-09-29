#!/usr/bin/env python3
"""Positive/negative controls for the function-name special-case policy."""

from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_special_cases import ALLOWED_ABI_LINES, violations


def seed_allowed_abi(root):
    for relative, line in ALLOWED_ABI_LINES:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as stream:
            stream.write(line + "\n")


class SpecialCaseTests(unittest.TestCase):
    def test_documented_putf_abi_sites_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed_allowed_abi(root)
            self.assertEqual([], violations(root))

    def test_unrelated_function_name_special_case_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed_allowed_abi(root)
            path = root / "ir/SomePass.cpp"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('if (name == "matmul") {\n')
            self.assertEqual(1, len(violations(root)))

    def test_new_source_extensions_are_scanned(self):
        for extension in (".cc", ".c", ".cxx"):
            with self.subTest(extension=extension), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                seed_allowed_abi(root)
                path = root / ("ir/Pass" + extension)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('if (name == "matmul") {\n')
                self.assertEqual(1, len(violations(root)))

    def test_only_runtime_catalog_file_is_exempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed_allowed_abi(root)
            catalog = root / "support/RuntimeLib.h"
            catalog.parent.mkdir(parents=True)
            catalog.write_text('"putint"\n')
            helper = root / "support/SignatureHelper.cpp"
            helper.write_text('if (name == "matmul") {\n')
            self.assertEqual(1, len(violations(root)))

    def test_putf_exception_does_not_allow_other_call_sites(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed_allowed_abi(root)
            path = root / "ir/LLVMTextEmitter.cpp"
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a", encoding="utf-8") as stream:
                stream.write('if (callee == "putf") {\n')
            self.assertGreaterEqual(len(violations(root)), 1)

    def test_duplicate_allowlisted_line_is_rejected(self):
        allowed = 'if (name == "putf") {\n'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            seed_allowed_abi(root)
            path = root / "ir/LLVMTextEmitter.cpp"
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a", encoding="utf-8") as stream:
                stream.write(allowed)
            errors = violations(root)
            self.assertTrue(any("duplicate allowlisted" in error for error in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
