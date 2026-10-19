#!/usr/bin/env python3
"""F04 structural controls: Mem2Reg cannot reintroduce a private IR/AM session."""
import argparse
from pathlib import Path
import re

COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)
SESSION = r'(?:AnalysisManager|IRMutator|CFGEditor)'
FORBIDDEN = (
    (re.compile(r'\b' + SESSION + r'\s+(?![&*])\w+\s*[\({]'),
     'private stack analysis/mutation session'),
    (re.compile(r'\b' + SESSION + r'\s+(?![&*])\w+\s*='),
     'private value analysis/mutation session'),
    (re.compile(r'=\s*' + SESSION + r'\s*[\({]'),
     'private constructed analysis/mutation session'),
    (re.compile(r'\bnew\s+' + SESSION + r'\b'),
     'private heap analysis/mutation session'),
    (re.compile(r'\bmake_(?:unique|shared)\s*<\s*' + SESSION + r'\b'),
     'private smart-pointer analysis/mutation session'),
    (re.compile(r'\bmodule\s*\.\s*functions\s*\('),
     'module-wide function scan inside Mem2Reg'),
)
# The pass (everything before the module compatibility wrapper) must not own a
# second pipeline, verifier, dump, or clock. The wrapper owns those once.
PASS_REGION_FORBIDDEN = (
    (re.compile(r'\bFlatPassManager\b'), 'nested pass manager inside the pass'),
    (re.compile(r'\bverifyFlatModule\s*\('), 'internal per-invocation verification'),
    (re.compile(r'\bdumpModule\s*\('), 'internal per-invocation dump'),
    (re.compile(r'\bchrono\b'), 'internal per-invocation timing'),
)
ANALYSIS_FORBIDDEN = FORBIDDEN + (
    (re.compile(r'\bverifyFlatModule\s*\('), 'analysis code must not run the flat verifier'),
    (re.compile(r'\bdumpModule\s*\('), 'analysis code must not dump IR'),
)
REQUIRED = {
    'src/opt/Mem2Reg.h': (
        ('public FunctionPass', 'missing formal FunctionPass inheritance'),
        ('PassResult run(Function&', 'missing per-Function run'),
        ('const Mem2RegResult& aggregate() const',
         'aggregate must be pass-owned, not an external raw pointer'),
        ('Failed', 'failed pipelines need an explicit status'),
    ),
    'src/opt/Mem2Reg.cpp': (
        ('context.analyses().get<ReachabilityAnalysis>(function)',
         'must request shared F02 reachability per candidate'),
        ('context.mutator()', 'must use PassContext mutator'),
        ('context.cfg()', 'must use PassContext CFG editor'),
        ('replaceUses(function, mutator, ssa)',
         'replacement must be Function-local'),
        ('Mem2RegStatus::Failed',
         'module wrapper must report Failed, never Changed, on pipeline failure'),
        ('manager.run(module)', 'module compatibility wrapper must use FlatPassManager'),
        ('context.mutator().summary().changed()',
         'per-Function status must use actual tracked mutations'),
        ('detached', 'scratch phis must be discarded on planning failure'),
    ),
    'src/main/FlatPipeline.cpp': (
        ('flat::runMem2Reg(module, options)',
         'production must use the formal-manager-backed wrapper'),
        ('return false;', 'production must propagate mem2reg failure'),
    ),
}


def line_of(text, offset):
    return text.count('\n', 0, offset) + 1


def check(root):
    errors = []
    sources = dict(REQUIRED)
    sources.setdefault('src/opt/Mem2RegAnalysis.cpp', ())
    for name, required in sources.items():
        path = root / name
        if not path.is_file():
            errors.append(f'{name}: missing F04 product source')
            continue
        source = path.read_text(encoding='utf-8')
        clean = COMMENT.sub(lambda m: '\n' * m.group().count('\n'), source)
        for marker, reason in required:
            if marker not in clean:
                errors.append(f'{name}: {reason}')
        if name == 'src/opt/Mem2Reg.cpp':
            wrapper = clean.find('Mem2RegResult runMem2Reg(')
            pass_region = clean if wrapper < 0 else clean[:wrapper]
            if wrapper < 0:
                errors.append(f'{name}: module wrapper entry point not found')
            for pattern, reason in FORBIDDEN:
                for match in pattern.finditer(pass_region):
                    errors.append(f'{name}:{line_of(clean, match.start())}: {reason}')
            for pattern, reason in PASS_REGION_FORBIDDEN:
                for match in pattern.finditer(pass_region):
                    errors.append(f'{name}:{line_of(clean, match.start())}: {reason}')
        elif name == 'src/opt/Mem2RegAnalysis.cpp':
            for pattern, reason in ANALYSIS_FORBIDDEN:
                for match in pattern.finditer(clean):
                    errors.append(f'{name}:{line_of(clean, match.start())}: {reason}')
    return errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    errors = check(Path(parser.parse_args().root).resolve())
    for error in errors:
        print('F04 boundary FAIL:', error)
    print('F04 Mem2Reg pass boundary:', 'FAIL' if errors else 'PASS')
    raise SystemExit(bool(errors))
