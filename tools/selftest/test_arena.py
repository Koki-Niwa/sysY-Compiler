#!/usr/bin/env python3
"""Regression checks for the GCC verdict in arena.py."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import arena  # noqa: E402


class ArenaVerdictTest(unittest.TestCase):
    def run_arena(self, gcc_result, trace):
        output = io.StringIO()
        with mock.patch.object(sys, 'argv', ['arena.py', 'sample.sy']), \
                mock.patch.object(arena.os.path, 'exists', return_value=True), \
                mock.patch.object(arena.tempfile, 'mkdtemp', return_value='/tmp'), \
                mock.patch.object(arena, 'gcc_truth', return_value=gcc_result), \
                mock.patch.object(arena, 'emit_traces', return_value=(trace, trace)), \
                contextlib.redirect_stdout(output):
            rc = arena.main()
        return rc, output.getvalue()

    def test_both_lanes_agree_on_wrong_output_and_return(self):
        trace = {'ret': 1, 'out': ['7', 'c10'], 'mem': 'same', 'loops': []}
        rc, report = self.run_arena((0, '8\n'), trace)
        self.assertEqual(rc, 1)
        for lane in ('结构化', '平面'):
            self.assertIn('gcc/%s 差异 main 返回值' % lane, report)
            self.assertIn('gcc/%s 差异 输出缓冲' % lane, report)

    def test_main_return_uses_exit_status_width(self):
        trace = {'ret': 256, 'out': ['7', 'c10'], 'mem': 'same', 'loops': []}
        rc, report = self.run_arena((0, '7\n'), trace)
        self.assertEqual(rc, 0)
        self.assertIn('三方可观察行为一致', report)

    def test_array_output_reconstructs_sylib_format(self):
        trace = {'out': ['a3', '1', '-2', '3', 'fa2', 'f3fa00000',
                         'f43798000', 'c10', 'a0']}
        self.assertEqual(arena.stdout_of(trace),
                         '3: 1 -2 3\n2: 0x1.4p+0 0x1.f3p+7\n\n0:\n')

    def test_truncated_array_output_is_reported(self):
        with self.assertRaisesRegex(ValueError, '1000'):
            arena.stdout_of({'out': ['a1001']})

    def test_gcc_runtime_mixed_input_and_array_output(self):
        source = '''int main() {
    float f[8]; int a[8]; int local[3] = {1, -2, 3};
    float x = getfloat();
    int y = getint();
    int c = getch();
    int nf = getfarray(f);
    int ni = getarray(a);
    putfloat(x); putch(10);
    putint(y); putch(10);
    putint(c); putch(10);
    putfarray(nf, f);
    putarray(3, local);
    putarray(ni, a);
    return 0;
}'''
        with tempfile.TemporaryDirectory() as tmpdir:
            sy = Path(tmpdir) / 'mixed.sy'
            sy.write_text(source, encoding='ascii')
            rc, output = arena.gcc_truth(str(sy), 32, 12345, tmpdir)
        self.assertEqual(rc, 0)
        self.assertEqual(output, '0x1.4p+0\n10\n76\n'
                         '8: 0x1.4p+0 0x1.f3p+7 0x1.f28p+7 '
                         '0x1.8p-1 0x1.8p-1 0x1.8p-1 0x1.cp+0 0x1.2p+1\n'
                         '3: 1 -2 3\n'
                         '8: 10 -3 2 6 7 8 -3 5\n')


if __name__ == '__main__':
    unittest.main()
