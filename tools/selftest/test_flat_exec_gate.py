"""Regression tests for false green verdicts in the S06 behavior gate."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.selftest import exec_core
from tools.selftest.flat_exec import one_file
from tools.selftest.flat_exec_report import observable_same


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOL = HERE / 'flat_exec.py'
SOURCE = ROOT / 'tests/final_arm/functional/00_main.sy'


class FlatExecGateTests(unittest.TestCase):
    def test_overwritten_global_write_changes_effect_trace(self):
        def trace(writes):
            machine = exec_core.Machine(1)
            memory = exec_core.Mem(4, 'g')
            pointer = exec_core.Ptr(memory, 0)
            machine.globals['g'] = pointer
            for value in writes:
                machine.store_word(pointer, 0, 4, value)
            return machine.finish_trace(0)

        early_write, no_early_write = trace([1, 2]), trace([2])
        self.assertEqual(early_write['mem'], no_early_write['mem'])
        self.assertFalse(observable_same(early_write, no_early_write))

    def test_memcpy_clears_destination_when_source_is_sparse_zero(self):
        machine = exec_core.Machine(1)
        dst_mem, src_mem = exec_core.Mem(4, 'g'), exec_core.Mem(4, 'src')
        dst, src = exec_core.Ptr(dst_mem, 0), exec_core.Ptr(src_mem, 0)
        machine.globals['g'] = dst
        machine.store_word(dst, 0, 4, 7)
        before = machine.finish_trace(0)['effects']
        machine.runtime('llvm.memcpy', [dst, src, 4, 4], None)
        self.assertEqual(machine.load_word(dst, 0, 4), 0)
        self.assertNotEqual(machine.finish_trace(0)['effects'], before)

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

    def test_mem2reg_mode_passes_explicit_opt_in_to_flat_compiler(self):
        failure = subprocess.CompletedProcess([], 1, b'', b'compile failed')
        with tempfile.TemporaryDirectory() as tmp, patch(
                'tools.selftest.flat_exec.subprocess.run', return_value=failure) as run:
            one_file('/fake/compiler', str(SOURCE), tmp, 1, mem2reg=True)
        self.assertEqual(run.call_count, 2)
        self.assertIn('--mem2reg', run.call_args_list[1].args[0])

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
        self.assertEqual(result.returncode, 0)
        self.assertIn('仅循环计数口径不同:', result.stdout)
        self.assertNotIn('Traceback', result.stderr)


if __name__ == '__main__':
    unittest.main()
