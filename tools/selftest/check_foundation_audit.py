#!/usr/bin/env python3
"""S16b structural controls: one dominator implementation, no pass rebuilds."""
import argparse
from pathlib import Path
import re

COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)
DOM_ALLOWED = {
    'src/ir/FlatVerifier.h', 'src/ir/FlatVerifier.cpp', 'src/ir/DominatorTree.cpp',
    'src/analysis/DominatorAnalysis.h', 'src/analysis/DominatorAnalysis.cpp',
}
ANALYSES = {
    'src/analysis/ReachabilityAnalysis.h': 'ReachabilityAnalysis',
    'src/analysis/CallGraph.h': 'CallGraphAnalysis',
    'src/analysis/FunctionSummary.h': 'SccSummaryAnalysis',
    'src/analysis/DominatorAnalysis.h': 'DominatorAnalysis',
    'src/analysis/LoopAnalysis.h': 'LoopInfoAnalysis',
    'src/analysis/AliasAnalysis.h': 'AliasAnalysis',
    'src/analysis/BranchProbabilityAnalysis.h': 'BranchProbabilityAnalysis',
    'src/analysis/BlockFrequencyAnalysis.h': 'BlockFrequencyAnalysis',
    'src/analysis/ValueFacts.h': 'ValueFactsAnalysis',
}


def check(root):
    errors = []
    for path in sorted((root / 'src').rglob('*')):
        if not path.is_file() or path.suffix not in ('.h', '.cpp', '.inc'):
            continue
        rel = path.relative_to(root).as_posix()
        source = path.read_text(encoding='utf-8')
        clean = COMMENT.sub(lambda m: '\n' * m.group().count('\n'), source)
        if 'DominatorTree' in clean and rel not in DOM_ALLOWED:
            errors.append(f'{rel}: DominatorTree outside the shared ir implementation')
        if rel.startswith('src/ir/'):
            # Check includes on the raw text: the comment/string stripper would
            # erase the quoted header name.
            for match in re.finditer(r'#include\s*"analysis/', source):
                line = source.count('\n', 0, match.start()) + 1
                errors.append(f'{rel}:{line}: ir must not include analysis')
        if rel.startswith(('src/opt/', 'src/analysis/')) and \
                re.search(r'\b(?:rebuildUseDef|rebuildCFG)\s*\(', clean):
            line = clean[:re.search(r'\b(?:rebuildUseDef|rebuildCFG)\s*\(', clean).start()].count('\n') + 1
            errors.append(f'{rel}:{line}: manual rebuild is not allowed in analyses/passes')
    for rel, analysis in ANALYSES.items():
        path = root / rel
        if not path.is_file():
            errors.append(f'{rel}: missing analysis header')
            continue
        clean = COMMENT.sub(lambda m: '\n' * m.group().count('\n'),
                            path.read_text(encoding='utf-8'))
        if analysis not in clean:
            errors.append(f'{rel}: missing {analysis}')
        if 'revisionDependencies()' not in clean:
            errors.append(f'{rel}: {analysis} must declare revision dependencies')
        if 'AnalysisScope scope' not in clean:
            errors.append(f'{rel}: {analysis} must declare its scope')
    return errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    errors = check(Path(parser.parse_args().root).resolve())
    for error in errors:
        print('S16b audit FAIL:', error)
    print('S16b foundation audit:', 'FAIL' if errors else 'PASS')
    raise SystemExit(bool(errors))
