#!/usr/bin/env python3
"""执行证据反证：身份、覆盖和汇总必须同时成立，测试含真实 shell 接线。"""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GATE = Path(__file__).resolve().parents[1] / 'gate'
sys.path.insert(0, str(GATE))
from check_execution import validate


def report_for(expected, all_skip=False):
    rows = [{'key': key, 'status': 'SKIP' if skip or all_skip else 'PASS',
             'skip_reason': 'tensor' if skip else ''} for key, skip in expected.items()]
    skipped = sum(r['status'] == 'SKIP' for r in rows)
    return {'schema_version': 1, 'target': 'x86',
            'compiler': {'revision': 'fixture', 'dirty': False,
                         'executable': '/fixture/compiler', 'executable_sha256': 'hash'},
            'manifest': {'path': '/fixture/manifest', 'sha256': 'hash'},
            'mode': {'target': 'x86', 'optimize': False,
                     'no_structured': False, 'boundary': 'source'},
            'toolchain': {
                'llvm': {'llvm_as': {'path': '/fixture/as', 'version': '18'},
                         'llc': {'path': '/fixture/llc', 'version': '18'}},
                'linker': {'path': '/fixture/clang', 'version': '18', 'dumpmachine': 'x86_64'},
                'execution': {'kind': 'native', 'runner': '/bin/uname',
                              'runner_version': 'kernel', 'real_target_performance': True},
                'target_config': {'triple': 'x86_64-linux-gnu',
                                  'probed': {'dumpmachine': 'x86_64'}}},
            'results': rows,
            'summary': {'total': len(rows), 'pass': len(rows) - skipped, 'skip': skipped,
                        'fail': 0, 'timeout': 0, 'format_diff': 0}}


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.expected = {'suite/cat/a': False, 'suite/cat/b': False, 'suite/cat/t': True}
        self.report = report_for(self.expected)

    def errors(self, report, **kwargs):
        return validate(report, self.expected, 'x86', required=2, excluded=1, **kwargs)

    def test_complete_positive(self):
        self.assertEqual([], self.errors(self.report))

    def test_all_skips_and_nonzero_runner(self):
        self.assertTrue(self.errors(report_for(self.expected, all_skip=True)))
        self.assertTrue(self.errors(self.report, runner_rc=1))

    def test_bad_statuses(self):
        for status in ('SKIP', 'TIMEOUT', 'COMPILE_FAIL', 'BUILD_FAIL', 'WRONG_OUTPUT',
                       'ERROR', 'CANCELLED', None):
            with self.subTest(status=status):
                data = copy.deepcopy(self.report)
                data['results'][0]['status'] = status
                self.assertTrue(self.errors(data))

    def test_missing_duplicate_unknown_identity(self):
        data = copy.deepcopy(self.report)
        data['results'].pop()
        self.assertTrue(self.errors(data))
        for key in ('suite/cat/b', 'unknown'):
            data = copy.deepcopy(self.report)
            data['results'][0]['key'] = key
            self.assertTrue(self.errors(data))

    def test_summary_target_and_malformed(self):
        for field, value in [('summary', {}), ('target', 'aarch64'), ('results', None)]:
            data = copy.deepcopy(self.report)
            data[field] = value
            self.assertTrue(self.errors(data))
        self.assertTrue(self.errors(None))
        self.assertTrue(validate(self.report, self.expected, 'x86'))  # 非 490/50 范围

    def test_revision_manifest_toolchain_and_mode_negative_controls(self):
        for path, value in (
                (('compiler', 'revision'), 'wrong'),
                (('compiler', 'dirty'), True),
                (('manifest', 'sha256'), 'wrong'),
                (('toolchain', 'llvm'), {}),
                (('mode', 'no_structured'), True),
                (('mode', 'boundary'), 'fake')):
            with self.subTest(path=path):
                data = copy.deepcopy(self.report)
                data[path[0]][path[1]] = value
                self.assertTrue(self.errors(data, revision='fixture', manifest_sha256='hash'))
        self.assertEqual([], self.errors(self.report, revision='fixture', manifest_sha256='hash'))

    def test_no_structured_requires_two_compiler_calls(self):
        data = copy.deepcopy(self.report)
        data['mode']['no_structured'] = True
        data['mode']['boundary'] = 'from-flat'
        self.assertTrue(self.errors(data, no_structured=True))
        for row in data['results']:
            if row['status'] == 'PASS':
                row['compile_invocations'] = [
                    ['compiler', 'source.sy', '--emit=flat-ir'],
                    ['compiler', 'case.flat', '--from-flat', '--emit=llvm-ir']]
        self.assertEqual([], self.errors(data, no_structured=True))
        data['results'][0]['compile_invocations'].pop()
        self.assertTrue(self.errors(data, no_structured=True))

    def test_documented_format_pass_keeps_explicit_counter(self):
        self.report['results'][0]['status'] = 'PASS(FORMAT_C1)'
        self.report['summary']['format_diff'] = 1
        self.assertEqual([], self.errors(self.report))

    def test_malformed_serialized_flat_is_rejected(self):
        compiler = GATE.parents[1] / 'build' / 'compiler'
        if not compiler.exists():
            self.skipTest('compiler has not been built')
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / 'corrupt.flat'
            output = Path(directory) / 'invalid.ll'
            bad.write_text('this is not a valid flat module\n')
            result = subprocess.run([str(compiler), str(bad), '--from-flat',
                                     '--emit=llvm-ir', '-o', str(output)],
                                    capture_output=True, timeout=15)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(output.exists())

    def test_real_execution_wrapper_rejects_all_skip_for_full_and_toy(self):
        expected = {f'suite/cat/{n}': n >= 490 for n in range(540)}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'manifest.tsv').write_text(''.join(
                '\t'.join([*key.split('/'), 'case.sy', '-', 'case.out',
                           'tensor' if skip else '-']) + '\n' for key, skip in expected.items()))
            fixture = root / 'fixture.json'
            runner = root / 'run_tests.sh'
            runner.write_text('''#!/usr/bin/env bash
while [ $# -gt 0 ]; do
  if [ "$1" = --json ]; then cp "$FIXTURE" "$2"; exit 0; fi
  shift
done
exit 2
''')
            runner.chmod(0o755)
            for flag in ('E2E_X86', 'TOY_OK'):
                for all_skip in (False, True):
                    with self.subTest(flag=flag, all_skip=all_skip):
                        data = report_for(expected, all_skip)
                        data['compiler']['revision'] = subprocess.check_output(
                            ['git', '-C', str(GATE.parents[1]), 'rev-parse', 'HEAD'],
                            text=True).strip()
                        data['manifest']['sha256'] = hashlib.sha256(
                            (root / 'manifest.tsv').read_bytes()).hexdigest()
                        fixture.write_text(json.dumps(data))
                        command = '''
FAIL=0; CAN_IR=0; E2E_X86=0; TOY_OK=0
hdr(){ :; }; c_skip(){ :; }; c_ok(){ :; }; c_bad(){ FAIL=$((FAIL+1)); }
source "$GATE/execution.sh"
run_execution "$FLAG" x86 fixture --toy-backend
printf 'RESULT %s %s\n' "${!FLAG}" "$FAIL"
'''
                        env = dict(os.environ, GATE=str(GATE), TOOLS=str(root), TESTS=str(root),
                                   WORK=str(root), FIXTURE=str(fixture), FLAG=flag)
                        result = subprocess.run(['bash', '-c', command], env=env,
                                                capture_output=True, text=True, timeout=20)
                        self.assertEqual(0, result.returncode, result.stderr)
                        self.assertIn('RESULT 0 1' if all_skip else 'RESULT 1 0', result.stdout)

    def test_s07_stage_does_not_run_s11_execution_gate(self):
        command = '''
FAIL=0; CAN_IR=1; STAGE=S07
hdr(){ :; }; c_skip(){ echo "SKIP $1"; }; c_ok(){ :; }; c_bad(){ FAIL=$((FAIL+1)); }
source "$GATE/stages.sh"
source "$GATE/execution.sh"
printf 'RESULT %s\n' "$FAIL"
'''
        env = dict(os.environ, GATE=str(GATE), TOOLS='/no-runner', TESTS='/no-tests',
                   WORK='/no-work')
        result = subprocess.run(['bash', '-c', command], env=env,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn('阶段 S07 不要求 S11', result.stdout)
        self.assertIn('RESULT 0', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
