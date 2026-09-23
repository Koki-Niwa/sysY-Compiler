"""Regression tests for false green verdicts in the S06 behavior gate."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.selftest import exec_core


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOL = HERE / 'flat_exec.py'
SOURCE = ROOT / 'tests/final_arm/functional/00_main.sy'


class FlatExecGateTests(unittest.TestCase):
    def test_f32_overflow_saturates_to_infinity(self):
        self.assertEqual(exec_core.bits_of_f32(10 ** 1000), 0x7f800000)
        self.assertEqual(exec_core.bits_of_f32(-(10 ** 1000)), 0xff800000)

    def test_both_lanes_rejecting_valid_source_is_failure(self):
        result = subprocess.run([sys.executable, str(TOOL), '--compiler',
                                 '/bin/false', '--dir', str(SOURCE.parent),
                                 '--limit', '1', '--min-files', '0'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('工具、编译或解析错误', result.stdout)

    def test_success_without_output_cannot_reuse_stale_ir(self):
        result = subprocess.run([sys.executable, str(TOOL), '--compiler',
                                 '/bin/true', '--show-trace', str(SOURCE)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('状态: error', result.stdout)

    def test_default_difference_report_does_not_crash(self):
        source = ROOT / 'tests/final_arm/h_functional/26_scope4.sy'
        compiler = ROOT / 'compiler/build/compiler'
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / '26.sy').symlink_to(source)
            result = subprocess.run([sys.executable, str(TOOL), '--compiler',
                                     str(compiler), '--dir', tmp,
                                     '--min-files', '0'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('轨迹不同:', result.stdout)
        self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
