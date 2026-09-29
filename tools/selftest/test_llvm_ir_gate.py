#!/usr/bin/env python3
"""Negative controls for the S07 LLVM gate's independent predicates."""

import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_llvm_ir as gate  # noqa: E402


GOOD = """define i32 @main() {
entry:
  %slot = alloca i32
  store i32 0, ptr %slot
  %v = load i32, ptr %slot
  ret i32 %v
}
"""


class LlvmGateTests(unittest.TestCase):
    def test_valid_module_is_accepted(self):
        self.assertEqual([], gate.validate_ir(GOOD))

    def test_non_entry_alloca_is_rejected(self):
        bad = GOOD.replace("  %v = load", "next:\n  %late = alloca i32\n  %v = load")
        self.assertTrue(any("alloca" in error for error in gate.validate_ir(bad)))

    def test_forbidden_attributes_and_opcode_are_rejected(self):
        bad = GOOD.replace("entry:", 'target datalayout = "bad"\nentry:')
        bad = bad.replace("  ret i32 %v", "  %v2 = freeze i32 %v\n  ret i32 %v2")
        errors = gate.validate_ir(bad)
        self.assertTrue(any("target" in error for error in errors))
        self.assertTrue(any("closed subset" in error for error in errors))

    def test_unsupported_intrinsic_declaration_is_rejected(self):
        bad = "declare void @llvm.evil(ptr)\n" + GOOD
        errors = gate.validate_ir(bad)
        self.assertTrue(any("intrinsic declaration outside subset" in error for error in errors))

    def test_inline_function_body_is_not_claimed_complete(self):
        bad = "define i32 @main(){ ret i32 0 }\n"
        errors = gate.validate_ir(bad)
        self.assertTrue(any("malformed function definition header" in error for error in errors))

    def test_unexpected_module_text_is_rejected(self):
        errors = gate.validate_ir("module asm \"unexpected\"\n" + GOOD)
        self.assertTrue(any("unexpected module-level text" in error for error in errors))

    def test_result_reconciliation_rejects_identity_swap(self):
        rows = [{"key": "a/f/x", "skip": "-"}, {"key": "a/f/t", "skip": "tensor"}]
        results = [{"key": "a/f/x", "status": "FRONTEND_REJECT"},
                   {"key": "a/f/t", "status": "PASS"}]
        errors = gate.reconcile(rows, results)
        self.assertGreaterEqual(len(errors), 2)

    def test_tensor_reject_requires_tensor_specific_frontend_diagnostic(self):
        self.assertRegex("x.sy:2:5: error: [E-UNDEF] 未定义的标识符 'tensor'",
                         gate.TENSOR_REJECTION)
        self.assertRegex("x.sy:1:1: error: expected a declaration, but got identifier 'tensor'",
                         gate.TENSOR_REJECTION)
        self.assertIsNone(gate.TENSOR_REJECTION.search(
            "x.sy:2:5: error: [E-PARSE] expected expression"))

    def test_manifest_cannot_label_a_non_tensor_source_as_tensor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "case.sy"
            source.write_text("int main(){ return 0; }\n", encoding="utf-8")
            manifest = root / "manifest.tsv"
            manifest.write_text("suite\tcat\tcase\tcase.sy\t-\t-\ttensor\n",
                                encoding="utf-8")
            _, errors = gate.read_manifest(manifest, root, expected_supported=0,
                                           expected_tensor=1)
            self.assertTrue(any("has no tensor token" in error for error in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
