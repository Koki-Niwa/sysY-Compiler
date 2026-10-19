#!/usr/bin/env python3
"""F05 structural controls: one materialization path and one source list.

main.cpp may only parse CLI, choose artifacts and write output; stage
materialization belongs to CompilationSession. The unit runner must not
re-declare source lists that CMake already owns.
"""
import argparse
from pathlib import Path
import re

COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)
STAGE_CALL = re.compile(
    r'\b(?:parseAstText|parseStructuredModule|parseFlatModule|runSema|runInitLowering|'
    r'buildModule|flattenModule|normalizeLoops|hoistAllocas|runMem2Reg|'
    r'verifyFlatModule|verifyModule)\s*\(')
STAGE_TYPE = re.compile(r'\b(?:Lexer|Parser|Sema)\s+[A-Za-z_]\w*\s*[({;]')
SOURCE_LIST = re.compile(r'(?:^|\n)\s*(?:SUPPORT|FRONTEND|ANALYSIS|OPT|PASS_MANAGER)=\(', re.M)
CPP_PATH = re.compile(r'\$\{?(?:SRC|ROOT)\}?/.*\.cpp')
REQUIRED = {
    'src/main/CompilationSession.h': (
        ('class CompilationSession', 'missing session type'),
        ('const CompUnit* ast()', 'missing AST materialization accessor'),
        ('const Sema* sema()', 'missing Sema materialization accessor'),
        ('const InitPlan* initPlan()', 'missing InitPlan materialization accessor'),
        ('const sir::Op* structured(', 'missing structured materialization'),
        ('const flat::Module* flat(', 'missing flat materialization'),
    ),
    'src/main/CompilationSession.cpp': (
        ('bool CompilationSession::materializeAst()', 'missing single AST path'),
        ('bool CompilationSession::materializeSema()', 'missing single Sema path'),
        ('bool CompilationSession::materializeInitPlan()', 'missing single InitPlan path'),
        ('sir::buildModule(structured_.arena', 'missing structured construction'),
        ('flat::flattenModule(mod', 'missing flat construction'),
    ),
    'src/main/main.cpp': (
        ('CompilationSession session(', 'main must construct the session'),
        ('emitArtifact(session, opts)', 'main must dispatch through the session'),
    ),
    'CMakeLists.txt': (
        ('add_library(sysy_support', 'missing support component target'),
        ('add_library(sysy_frontend', 'missing frontend component target'),
        ('add_library(sysy_ir', 'missing ir component target'),
        ('add_library(sysy_analysis', 'missing analysis component target'),
        ('add_library(sysy_opt', 'missing opt component target'),
        ('add_library(sysy_structured', 'missing structured component target'),
        ('sysy_unit_test(test_mem2reg_pass sysy_opt)', 'unit tests must link components'),
    ),
    'tools/selftest/unit/run.sh': (
        ('cmake --build', 'runner must build CMake targets'),
        ('ctest', 'runner must execute registered tests'),
    ),
}


def check(root):
    errors = []
    for name, required in REQUIRED.items():
        path = root / name
        if not path.is_file():
            errors.append(f'{name}: missing F05 file')
            continue
        source = path.read_text(encoding='utf-8')
        clean = COMMENT.sub(lambda m: '\n' * m.group().count('\n'), source)
        for marker, reason in required:
            if marker not in clean:
                errors.append(f'{name}: {reason}')
        if name == 'src/main/main.cpp':
            for pattern, reason in ((STAGE_CALL, 'main must not own stage materialization'),
                                    (STAGE_TYPE, 'main must not construct stage parsers')):
                for match in pattern.finditer(clean):
                    line = clean.count('\n', 0, match.start()) + 1
                    errors.append(f'{name}:{line}: {reason}')
        elif name == 'tools/selftest/unit/run.sh':
            for pattern, reason in ((SOURCE_LIST, 'runner must not hand-copy a source list'),
                                    (CPP_PATH, 'runner must not name component sources')):
                for match in pattern.finditer(clean):
                    line = clean.count('\n', 0, match.start()) + 1
                    errors.append(f'{name}:{line}: {reason}')
    return errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    errors = check(Path(parser.parse_args().root).resolve())
    for error in errors:
        print('F05 boundary FAIL:', error)
    print('F05 session/build boundary:', 'FAIL' if errors else 'PASS')
    raise SystemExit(bool(errors))
