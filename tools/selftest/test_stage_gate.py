#!/usr/bin/env python3
"""阶段不允许借跳过变绿；无需执行慢速语料即可复现。"""
from pathlib import Path
import os
import subprocess
import unittest

GATE = Path(__file__).resolve().parents[1] / 'gate'
FLAGS = ('CAN_SEMA CAN_INITPLAN CAN_SIR CAN_NORM CAN_FLAT CAN_IR IR_VALID '
         'E2E_X86 E2E_AARCH64 E2E_RISCV64 E2E_NO_STRUCT TOY_OK').split()


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

    def test_s11b_and_later_require_toy(self):
        self.assertEqual(0, self.run_stage('S11', ['TOY_OK']).returncode)
        for stage in ('S11b', 'S12', 'S28'):
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
