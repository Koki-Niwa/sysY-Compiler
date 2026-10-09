"""S11 measurement/comparison deterministic positive and negative controls."""
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from compare_execution_reports import compare
from _run_tests import Case, evaluate, build_argparser
from _execution_evidence import probe
from gate.check_execution import validate


def fixture(speed=1.0):
    rows = []
    for key, value in [('a/c/one', 2.0), ('b/c/two', 4.0)]:
        samples = [value * speed - 0.1, value * speed, value * speed + 0.1]
        rows.append({'key': key, 'status': 'PASS', 'source_sha256': 'source-'+key,
                     'stdin_sha256': 'stdin-'+key, 'expected_sha256': 'expected-'+key,
                     'executable_bytes': 100, 'run_protocol': {'warmup_count': 1, 'repeat_count': 3},
                     'run_samples_seconds': samples, 'run_median_seconds': statistics.median(samples),
                     'run_spread': {'kind': 'mad', 'seconds': statistics.median(abs(v-statistics.median(samples)) for v in samples)}})
    return {'schema_version': 1, 'target': 'x86', 'compiler': {'revision': 'abc', 'executable_sha256': 'xyz'},
            'manifest': {'sha256': 'manifest'}, 'toolchain': {'llvm': 'v18', 'execution': 'native'},
            'mode': {'optimize': False}, 'summary': {'fail': 0, 'timeout': 0},
            'measurement': {'warmup_count': 1, 'repeat_count': 3, 'statistic': 'median',
                            'spread_kind': 'mad', 'metric_scope': 'native-host',
                            'target_hardware_runtime': 'not-product-target', 'real_target_performance': False},
            'results': rows}


class ComparisonTests(unittest.TestCase):
    def test_known_math(self):
        result = compare(fixture(), fixture(.5))
        self.assertEqual(result['case_count'], 2)
        self.assertAlmostEqual(result['aggregate']['geomean_speedup'], 2)
        self.assertAlmostEqual(result['aggregate']['worst_speedup'], 2)
        self.assertAlmostEqual(result['per_case'][0]['speedup'], 2)

    def test_fail_closed(self):
        cases = [
            lambda r: r.update(target='riscv64'),
            lambda r: r['manifest'].update(sha256='different'),
            lambda r: r['results'][0].update(key='different'),
            lambda r: r['results'][0].update(source_sha256='different'),
            lambda r: r['results'][0].update(stdin_sha256='different'),
            lambda r: r['results'][0].update(status='WRONG_OUTPUT'),
            lambda r: r['measurement'].update(repeat_count=2),
            lambda r: r['results'][0].pop('run_samples_seconds'),
            lambda r: r['toolchain'].update(llvm='different'),
        ]
        for mutate in cases:
            candidate = fixture()
            mutate(candidate)
            with self.subTest(mutation=str(mutate)):
                with self.assertRaises(ValueError):
                    compare(fixture(), candidate)

    def test_qemu_label(self):
        a, b = fixture(), fixture(.5)
        for r in (a, b):
            r['target'] = 'aarch64'
            r['measurement']['metric_scope'] = 'qemu-proxy'
            r['measurement']['target_hardware_runtime'] = 'unmeasured'
        self.assertEqual(compare(a, b)['metric_scope'], 'qemu-proxy')
        self.assertFalse(compare(a, b)['real_target_performance'])


class RunnerTests(unittest.TestCase):
    def test_repeat_single_and_mad(self):
        self.assertEqual(statistics.median([0.2]), 0.2)
        self.assertEqual(statistics.median([abs(x-0.2) for x in [0.2]]), 0)
        self.assertEqual(statistics.median([1, 2, 5]), 2)
        self.assertEqual(statistics.median([abs(x-2) for x in [1, 2, 5]]), 1)

    def test_warmup_and_repeats_validate_every_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'case.ll').write_text('define i32 @main() { ret i32 0 }\n')
            (root/'a.sy').write_text('ok')
            (root/'a.out').write_bytes(b'0\n0\n')
            case = Case('s', 'performance', 'a', 'a.sy', '-', 'a.out', '')
            args = build_argparser().parse_args(['--root', temp, '--no-compile-step',
                        '--warmup', '1', '--repeat', '3'])
            calls = []
            def fake(cmd, cwd, stdin_path=None, timeout=60, env=None):
                if cmd[0] in ('./prog', 'qemu-aarch64', 'qemu-riscv64'):
                    calls.append(cmd)
                    return (0, b'0\n', '', False)
                return (0, b'', '', False)
            with patch('_run_tests.run_cmd', side_effect=fake), patch('_run_tests.os.path.getsize', return_value=100):
                result = evaluate(case, args, temp, '/usr/bin')
            self.assertEqual(result['status'], 'PASS')
            self.assertEqual(len(calls), 4)
            self.assertEqual(len(result['run_samples_seconds']), 3)
            self.assertGreaterEqual(result['run_spread']['seconds'], 0)
            self.assertEqual(result['run_spread']['kind'], 'mad')

    def test_repeat_wrong_output_or_timeout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'case.ll').write_text('define i32 @main() { ret i32 0 }\n')
            (root/'a.sy').write_text('ok')
            (root/'a.out').write_bytes(b'0\n0\n')
            case = Case('s', 'performance', 'a', 'a.sy', '-', 'a.out', '')
            args = build_argparser().parse_args(['--root', temp, '--no-compile-step',
                         '--warmup', '1', '--repeat', '3'])
            for failure, expected in [((0, b'bad', '', False), 'WRONG_OUTPUT'),
                                       ((None, b'', '', True), 'TIMEOUT')]:
                calls = [0]
                def fake(cmd, cwd, stdin_path=None, timeout=60, env=None):
                    if cmd[0] == './prog':
                        calls[0] += 1
                        return failure if calls[0] == 3 else (0, b'0\n', '', False)
                    return (0, b'', '', False)
                with patch('_run_tests.run_cmd', side_effect=fake), patch('_run_tests.os.path.getsize', return_value=100):
                    result = evaluate(case, args, temp, '/usr/bin')
                self.assertEqual(result['status'], expected)
                self.assertEqual(calls[0], 3)

    def test_probe_timeout(self):
        import subprocess
        with patch('_execution_evidence.subprocess.run', side_effect=subprocess.TimeoutExpired(['x'], 15)):
            with self.assertRaisesRegex(RuntimeError, 'toolchain probe failed'):
                probe(['x'])


if __name__ == '__main__':
    unittest.main()
