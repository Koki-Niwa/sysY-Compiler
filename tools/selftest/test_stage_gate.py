#!/usr/bin/env python3
"""阶段不允许借跳过变绿；无需执行慢速语料即可复现。"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

GATE = Path(__file__).resolve().parents[1] / 'gate'
FLAGS = ('CAN_SEMA CAN_INITPLAN CAN_SIR CAN_NORM CAN_FLAT CAN_IR IR_VALID S08_OK MEM2REG_OK '
         'INTERP_OK E2E_X86 E2E_AARCH64 E2E_RISCV64 E2E_NO_STRUCT '
         'DIFF_OK TOY_OK').split()


class StageTests(unittest.TestCase):
    def run_stage(self, stage, missing=()):
        env = dict(os.environ, GATE=str(GATE), STAGE=stage)
        env.update({key: '0' if key in missing else '1' for key in FLAGS})
        return subprocess.run(['bash', '-c', '''
FAIL=0
c_bad(){ echo "$*"; FAIL=$((FAIL+1)); }
source "$GATE/stages.sh"
validate_stage "$STAGE" || exit 2
check_stage "$STAGE"
[ "$FAIL" = 0 ]
'''], env=env, capture_output=True, text=True)

    def test_s06_does_not_require_future_capabilities(self):
        self.assertEqual(0, self.run_stage('S06', FLAGS[5:]).returncode)

    def test_s07_requires_real_ir_and_validation(self):
        for missing in ('CAN_IR', 'IR_VALID'):
            result = self.run_stage('S07', [missing])
            self.assertEqual(1, result.returncode)
            self.assertIn(missing, result.stdout)

    def test_s11_requires_all_execution_paths(self):
        for missing in ('E2E_X86', 'E2E_AARCH64', 'E2E_RISCV64', 'E2E_NO_STRUCT'):
            self.assertEqual(1, self.run_stage('S11', [missing]).returncode)

    def test_s10a_requires_interpreter_evidence_and_keeps_old_alias(self):
        result = self.run_stage('S10a', ['INTERP_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('INTERP_OK', result.stdout)
        self.assertEqual(1, self.run_stage('S10', ['INTERP_OK']).returncode)
        self.assertEqual(0, self.run_stage('S10').returncode)

    def test_s08_requires_public_flat_verifier_matrix(self):
        result = self.run_stage('S08', ['S08_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('S08_OK', result.stdout)
        self.assertEqual(0, self.run_stage('S08').returncode)

    def test_s09_requires_mem2reg_behavior_evidence_only_from_s09(self):
        self.assertEqual(0, self.run_stage('S08', ['MEM2REG_OK']).returncode)
        result = self.run_stage('S09', ['MEM2REG_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('MEM2REG_OK', result.stdout)
        self.assertEqual(0, self.run_stage('S09').returncode)

    def test_s10b_requires_s11_and_differential_evidence(self):
        result = self.run_stage('S10b', ['DIFF_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('DIFF_OK', result.stdout)
        result = self.run_stage('S10b', ['INTERP_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('INTERP_OK', result.stdout)
        result = self.run_stage('S10b', ['E2E_AARCH64'])
        self.assertEqual(1, result.returncode)
        self.assertIn('E2E_AARCH64', result.stdout)
        result = self.run_stage('S10b', ['TOY_OK'])
        self.assertEqual(1, result.returncode)
        self.assertIn('TOY_OK', result.stdout)

    def test_dependency_closure_keeps_s10a_independent_and_orders_s11b(self):
        for missing in ('INTERP_OK', 'DIFF_OK', 'TOY_OK'):
            self.assertEqual(0, self.run_stage('S12', [missing]).returncode,
                             f'S12 unexpectedly requires {missing}')
        command = '''
source "$GATE/stages.sh"
stage_requires S10a S03 && stage_requires S10a S04
test "$?" -ne 0 && exit 1
stage_requires S10a S08 && exit 1
stage_requires S10a S09 && exit 1
stage_requires S10b S11b || exit 1
stage_requires S11b S10b && exit 1
stage_requires S12 S11 || exit 1
stage_requires S12 S10a && exit 1
stage_requires S12 S10b && exit 1
stage_requires S12 S11b && exit 1
true
'''
        result = subprocess.run(['bash', '-c', command], env=dict(os.environ, GATE=str(GATE)),
                                capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)

    def test_unknown_and_cycle_dependencies_are_hard_failures(self):
        unknown = subprocess.run(['bash', '-c', '''
source "$GATE/stages.sh"
STAGE_DEPS[S09]="S08 S_UNKNOWN"
validate_stage_graph
'''], env=dict(os.environ, GATE=str(GATE)), capture_output=True, text=True)
        self.assertNotEqual(0, unknown.returncode)
        self.assertIn('未知阶段依赖', unknown.stderr)

        cycle = subprocess.run(['bash', '-c', '''
source "$GATE/stages.sh"
STAGE_DEPS[S09]="S09"
validate_stage_graph
'''], env=dict(os.environ, GATE=str(GATE)), capture_output=True, text=True)
        self.assertNotEqual(0, cycle.returncode)
        self.assertIn('阶段依赖环', cycle.stderr)

        missing = subprocess.run(['bash', '-c', '''
source "$GATE/stages.sh"
unset 'STAGE_DEPS[S09]'
validate_stage_graph
'''], env=dict(os.environ, GATE=str(GATE)), capture_output=True, text=True)
        self.assertNotEqual(0, missing.returncode)
        self.assertIn('阶段缺少依赖注册', missing.stderr)

    def test_stage_checks_do_not_fall_back_to_numeric_stage_index(self):
        env = dict(os.environ, GATE=str(GATE), STAGE='S10a')
        env.update({key: '1' for key in FLAGS})
        result = subprocess.run(['bash', '-c', '''
FAIL=0
c_bad(){ FAIL=$((FAIL+1)); }
source "$GATE/stages.sh"
stage_index(){ echo "numeric fallback called" >&2; return 99; }
validate_stage "$STAGE" || exit 2
check_stage "$STAGE"
[ "$FAIL" = 0 ]
'''], env=env, capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn('numeric fallback called', result.stderr)

    def test_stage_query_walks_only_the_requested_closure(self):
        result = subprocess.run(['bash', '-c', '''
source "$GATE/stages.sh"
validate_stage_graph(){ echo "full graph validation called" >&2; return 99; }
stage_requires S09 S08
'''], env=dict(os.environ, GATE=str(GATE)), capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn('full graph validation called', result.stderr)

    def test_s11b_and_s10b_require_toy(self):
        self.assertEqual(0, self.run_stage('S11', ['TOY_OK']).returncode)
        for stage in ('S11b', 'S10b'):
            self.assertEqual(1, self.run_stage(stage, ['TOY_OK']).returncode)

    def test_unknown_stage_rejected(self):
        self.assertEqual(2, self.run_stage('S99').returncode)

    def test_quick_summary_is_not_acceptance(self):
        # 执行协调器实际的最终报告片段，避免只检索字符串。
        coordinator = (GATE.parent / 'verify.sh').read_text()
        summary = coordinator[coordinator.index('if [ "$FAIL" != 0 ]; then'):]
        for quick, fail in [('1', '0'), ('1', '1')]:
            result = subprocess.run(['bash', '-c', summary], capture_output=True, text=True,
                                    env=dict(os.environ, QUICK=quick, FAIL=fail))
            if fail == '0':
                self.assertEqual(0, result.returncode)
                self.assertIn('NOT FULL ACCEPTANCE', result.stdout)
            else:
                self.assertEqual(1, result.returncode)

    def test_verify_workspaces_are_filesystem_unique_and_isolated(self):
        tools = GATE.parent
        verify = (tools / 'verify.sh').read_text()
        self.assertNotIn('verify_$$', verify)
        self.assertIn('source "$TOOLS/verify_workspace.sh"', verify)
        helper = tools / 'verify_workspace.sh'
        self.assertIn('mktemp -d', helper.read_text())
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, TMPDIR=tmp)
            script = f'''source "{helper}"
create_verify_workspace || exit
first="$VERIFY_WORK"
create_verify_workspace || exit
second="$VERIFY_WORK"
test "$first" != "$second" || exit 3
rm -rf -- "$first"
test -d "$second" || exit 4
printf '%s\\n%s\\n' "$first" "$second"
'''
            result = subprocess.run(['bash', '-c', script], env=env, capture_output=True,
                                    text=True, check=True)
            first, second = result.stdout.splitlines()
            self.assertEqual(1, len(list(Path(tmp).iterdir())))
            self.assertFalse(Path(first).exists())
            self.assertTrue(Path(second).is_dir())


if __name__ == '__main__':
    unittest.main(verbosity=2)
