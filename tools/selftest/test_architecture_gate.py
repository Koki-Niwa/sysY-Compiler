#!/usr/bin/env python3
"""架构与行数判据的正例/反例，全部在临时仓库中运行。"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from check_architecture import check


class ArchitectureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, name, content=''):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def test_valid_dag_and_exact_bridges(self):
        self.put('src/support/Base.h')
        self.put('src/frontend/Ast.h', '#include "support/Base.h"\n')
        for bridge in ('IRGenInternal.h', 'IRGenImpl.h'):
            self.put('src/structured/' + bridge, '#include "frontend/Ast.h"\n')
        self.assertEqual([], check(self.root)[0])

    def test_similar_bridge_name_does_not_allow_reverse_dependency(self):
        self.put('src/frontend/Ast.h')
        self.put('src/structured/IRGenPretend.h', '#include "frontend/Ast.h"\n')
        self.assertTrue(any('禁止依赖方向' in e for e in check(self.root)[0]))

    def test_support_cannot_depend_on_ir(self):
        self.put('src/ir/Type.h')
        self.put('src/support/Base.h', '#include "ir/Type.h"\n')
        self.assertTrue(any('support -> ir' in e for e in check(self.root)[0]))

    def test_angle_project_include_and_system_header(self):
        self.put('src/support/Base.h', '#include <vector>\n')
        self.put('src/ir/Type.h', '#include <support/Base.h>\n')
        self.assertEqual([], check(self.root)[0])

    def test_angle_reverse_dependency(self):
        self.put('src/frontend/Ast.h')
        self.put('src/support/Base.h', '#include <frontend/Ast.h>\n')
        self.assertTrue(any('support -> frontend' in e for e in check(self.root)[0]))

    def test_angle_cycle(self):
        self.put('src/ir/A.h', '#include <ir/B.h>\n')
        self.put('src/ir/B.h', '#include <ir/A.h>\n')
        self.assertTrue(any('循环依赖' in e for e in check(self.root)[0]))

    def test_local_cycle(self):
        self.put('src/ir/A.h', '#include "B.h"\n')
        self.put('src/ir/B.h', '#include "A.h"\n')
        self.assertTrue(any('循环依赖' in e for e in check(self.root)[0]))

    def test_unresolved_local_include(self):
        self.put('src/ir/A.h', '#include "missing.h"\n')
        self.assertTrue(any('不存在' in e for e in check(self.root)[0]))

    def budget(self):
        return subprocess.run([sys.executable, str(Path(__file__).with_name('check_line_budget.py')),
                               '--root', str(self.root)], capture_output=True, text=True)

    def test_inc_600_passes_601_fails(self):
        self.put('src/ir/Part.inc', '\n' * 600)
        self.put('tools/selftest/line_ratchet.txt')
        self.assertEqual(0, self.budget().returncode)
        self.put('src/ir/Part.inc', '\n' * 601)
        result = self.budget()
        self.assertEqual(1, result.returncode)
        self.assertIn('Part.inc', result.stdout)

    def test_gate_cap_cannot_be_overridden_by_ratchet(self):
        self.put('src/ir/Type.h')
        for name in ('verify.sh', 'gate/large.sh'):
            self.put('tools/' + name, '\n' * 601)
        self.put('tools/selftest/line_ratchet.txt',
                 '842  compiler/tools/verify.sh\n842  compiler/tools/gate/large.sh\n')
        result = self.budget()
        self.assertEqual(1, result.returncode)
        self.assertIn('硬上限 600', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
