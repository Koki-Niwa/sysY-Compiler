#!/usr/bin/env python3
"""架构负控使用临时仓库；F04 默认根另从 compiler/workspace 目录验证。"""
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

    def test_interpreter_may_read_ast_and_support_but_not_ir(self):
        self.put('src/support/SourceLoc.h')
        self.put('src/frontend/Ast.h', '#include "support/SourceLoc.h"\n')
        self.put('src/interpreter/Interpreter.h',
                 '#include "frontend/Ast.h"\n#include "support/SourceLoc.h"\n')
        self.assertEqual([], check(self.root)[0])
        self.put('src/interpreter/Interpreter.h', '#include "ir/Module.h"\n')
        self.put('src/ir/Module.h')
        self.assertTrue(any('禁止依赖方向 interpreter -> ir' in e
                            for e in check(self.root)[0]))

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


    def test_f02_analysis_dependency_direction(self):
        self.put('src/ir/Module.h')
        self.put('src/support/Base.h')
        self.put('src/analysis/Manager.h',
                 '#include "ir/Module.h"\n#include "support/Base.h"\n')
        self.put('src/opt/Consumer.h', '#include "analysis/Manager.h"\n')
        self.assertEqual([], check(self.root)[0])
        for target in ('frontend', 'opt', 'structured'):
            self.put('src/' + target + '/Bad.h')
            self.put('src/analysis/Manager.h',
                     '#include "' + target + '/Bad.h"\n')
            self.assertTrue(any('analysis -> ' + target in e
                                for e in check(self.root)[0]))
        self.put('src/analysis/Manager.h', '#include "ir/Module.h"\n')
        self.put('src/ir/Module.h', '#include "analysis/Manager.h"\n')
        self.assertTrue(any('ir -> analysis' in e for e in check(self.root)[0]))
        self.put('src/ir/Module.h', '')
        self.put('src/support/Base.h', '#include "analysis/Manager.h"\n')
        self.assertTrue(any('support -> analysis' in e for e in check(self.root)[0]))

    def mem2reg_boundary(self):
        return subprocess.run(
            [sys.executable, str(Path(__file__).with_name('check_mem2reg_pass_boundary.py')),
             '--root', str(self.root)],
            cwd=self.root, capture_output=True, text=True)

    def test_f04_mem2reg_boundary_default_root_ignores_cwd(self):
        checker = Path(__file__).with_name('check_mem2reg_pass_boundary.py').resolve()
        compiler_root = checker.parents[2]
        for cwd in (compiler_root, compiler_root.parent):
            with self.subTest(cwd=cwd):
                result = subprocess.run([sys.executable, str(checker)], cwd=cwd,
                                        capture_output=True, text=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn('F04 Mem2Reg pass boundary: PASS', result.stdout)

    def test_f04_mem2reg_pass_boundary_and_negative_controls(self):
        from check_mem2reg_pass_boundary import check as check_mem2reg
        self.put('src/opt/Mem2Reg.h',
                 'class Mem2RegPass : public FunctionPass { '
                 'PassResult run(Function& f, PassContext& c); '
                 'const Mem2RegResult& aggregate() const; };\n'
                 'enum class Mem2RegStatus { Failed };')
        self.put('src/main/FlatPipeline.cpp',
                 'flat::runMem2Reg(module, options); return false;')
        self.put('src/opt/Mem2RegAnalysis.cpp', '// analysis helper\n')
        pass_region = (
            'context.analyses().get<ReachabilityAnalysis>(function);\n'
            'context.mutator(); context.cfg();\n'
            'replaceUses(function, mutator, ssa);\n'
            'detached;\n'
            'context.mutator().summary().changed();\n'
        )
        wrapper = (
            'Mem2RegResult runMem2Reg(Module& module, const Mem2RegOptions&) {\n'
            'manager.run(module);\n'
            'Mem2RegStatus::Failed; return {}; }\n'
        )
        baseline = pass_region + wrapper
        self.put('src/opt/Mem2Reg.cpp', baseline)
        self.assertEqual([], check_mem2reg(self.root))
        self.assertEqual(0, self.mem2reg_boundary().returncode)
        negatives = [
            ('AnalysisManager rogue(module);', 'private stack'),
            ('IRMutator rogue(module);', 'private stack'),
            ('CFGEditor rogue(mutator);', 'private stack'),
            ('auto rogue = AnalysisManager(module);', 'private constructed'),
            ('auto rogue = new AnalysisManager(module);', 'private heap'),
            ('auto rogue = std::make_unique<CFGEditor>(mutator);', 'smart-pointer'),
            ('for (auto* f : module.functions()) {}', 'module-wide'),
            ('FlatPassManager nested;', 'nested pass manager'),
            ('verifyFlatModule(module);', 'internal per-invocation verification'),
            ('dumpModule(module);', 'internal per-invocation dump'),
            ('std::chrono::steady_clock::now();', 'internal per-invocation timing'),
        ]
        for injection, expected in negatives:
            self.put('src/opt/Mem2Reg.cpp', pass_region + injection + wrapper)
            self.assertTrue(any(expected in error for error in check_mem2reg(self.root)),
                            injection)
            cli = self.mem2reg_boundary()
            self.assertEqual(1, cli.returncode, injection)
            self.assertIn(expected, cli.stdout, injection)
        self.put('src/opt/Mem2Reg.cpp',
                 baseline.replace('manager.run(module);', 'legacyOptimize(module);'))
        self.assertTrue(any('FlatPassManager' in error
                            for error in check_mem2reg(self.root)))
        self.assertEqual(1, self.mem2reg_boundary().returncode)
        self.put('src/opt/Mem2Reg.cpp',
                 baseline.replace('context.analyses().get<ReachabilityAnalysis>(function);',
                                  'otherAnalyses.get<ReachabilityAnalysis>(function);'))
        self.assertTrue(any('shared F02' in error
                            for error in check_mem2reg(self.root)))
        self.assertEqual(1, self.mem2reg_boundary().returncode)
        self.put('src/opt/Mem2Reg.cpp', baseline)
        self.put('src/opt/Mem2RegAnalysis.cpp',
                 'AnalysisManager privateManager(module);')
        self.assertTrue(any('private' in error
                            for error in check_mem2reg(self.root)))
        self.assertEqual(1, self.mem2reg_boundary().returncode)
        self.put('src/opt/Mem2RegAnalysis.cpp', '// analysis helper\n')
        self.put('src/main/FlatPipeline.cpp', 'legacyOptimize(module);')
        self.assertTrue(any('production' in error
                            for error in check_mem2reg(self.root)))
        self.assertEqual(1, self.mem2reg_boundary().returncode)

    def test_f05_session_and_source_list_boundaries(self):
        from check_f05_session_boundary import check as check_f05
        self.put('src/main/CompilationSession.h',
                 'class CompilationSession { const CompUnit* ast(); '
                 'const Sema* sema(); const InitPlan* initPlan(); '
                 'const sir::Op* structured(; const flat::Module* flat(; };')
        self.put('src/main/CompilationSession.cpp',
                 'bool CompilationSession::materializeAst() {}\n'
                 'bool CompilationSession::materializeSema() {}\n'
                 'bool CompilationSession::materializeInitPlan() {}\n'
                 'sir::buildModule(structured_.arena; flat::flattenModule(mod;')
        self.put('src/main/main.cpp',
                 'CompilationSession session(src, diags); emitArtifact(session, opts);')
        self.put('CMakeLists.txt',
                 'add_library(sysy_support\nadd_library(sysy_frontend\n'
                 'add_library(sysy_ir\nadd_library(sysy_analysis\n'
                 'add_library(sysy_opt\nadd_library(sysy_structured\n'
                 'sysy_unit_test(test_mem2reg_pass sysy_opt)')
        self.put('tools/selftest/unit/run.sh',
                 '#!/bin/bash\ncmake --build build\nctest\n')
        self.assertEqual([], check_f05(self.root))
        self.put('src/main/main.cpp',
                 'CompilationSession session(src, diags);\n'
                 'Lexer lexer(src, diags); emitArtifact(session, opts);')
        self.assertTrue(any('stage parsers' in error
                            for error in check_f05(self.root)))
        self.put('src/main/main.cpp',
                 'CompilationSession session(src, diags); emitArtifact(session, opts);')
        self.put('tools/selftest/unit/run.sh',
                 '#!/bin/bash\nSUPPORT=(src/support/SourceFile.cpp)\nctest\n')
        self.assertTrue(any('hand-copy' in error for error in check_f05(self.root)))
        self.put('tools/selftest/unit/run.sh',
                 '#!/bin/bash\ncmake --build build\nctest\n')
        self.put('CMakeLists.txt',
                 'add_library(sysy_support\nadd_library(sysy_frontend\n'
                 'add_library(sysy_ir\nadd_library(sysy_analysis\n'
                 'add_library(sysy_opt\n')
        self.assertTrue(any('structured component' in error
                            for error in check_f05(self.root)))

    def test_s16b_foundation_audit_boundaries(self):
        from check_foundation_audit import ANALYSES, check as check_audit
        for rel, analysis in ANALYSES.items():
            self.put(rel, f'struct {analysis} {{ '
                     'static constexpr AnalysisScope scope = AnalysisScope::Function; '
                     'static constexpr MutationKind revisionDependencies(); };')
        self.put('src/opt/Good.cpp', '// no rebuilds here\n')
        self.assertEqual([], check_audit(self.root))
        self.put('src/opt/Bad.cpp', 'void f(){ m.rebuildCFG(); }\n')
        self.assertTrue(any('manual rebuild' in error for error in check_audit(self.root)))
        self.put('src/opt/Bad.cpp')
        self.put('src/opt/Dom.cpp', 'DominatorTree tree(function);\n')
        self.assertTrue(any('DominatorTree outside' in error
                            for error in check_audit(self.root)))
        self.put('src/opt/Dom.cpp')
        self.put('src/ir/Bad.cpp', '#include "analysis/AnalysisManager.h"\n')
        self.assertTrue(any('ir must not include analysis' in error
                            for error in check_audit(self.root)))

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
