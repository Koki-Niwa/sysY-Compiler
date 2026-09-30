#!/usr/bin/env python3
"""检查产品本地字面量 include 的方向与文件/模块环；不声称覆盖动态依赖。

--root 可为工作区或独立 compiler 仓库。规则以本文件为可执行权威，
新增层须同时更新 docs/ARCHITECTURE.md 和反例测试。
"""
import argparse
from pathlib import Path
import re

ALLOWED = {
    'support': set(), 'frontend': {'support'}, 'ir': {'support'},
    'interpreter': {'frontend', 'support'},
    'structured': {'ir', 'support'},
    'main': {'support', 'frontend', 'ir', 'structured', 'analysis', 'opt'},
    'analysis': {'support', 'ir', 'structured'},
    'opt': {'support', 'ir', 'structured', 'analysis'},
}
BRIDGES = {'structured/IRGenInternal.h', 'structured/IRGenImpl.h'}
EXTENSIONS = {'.h', '.cpp', '.inc'}


def cycle_paths(graph):
    seen, active, result = set(), [], []
    def visit(node):
        if node in active:
            result.append(active[active.index(node):] + [node])
            return
        if node in seen:
            return
        seen.add(node)
        active.append(node)
        for target in sorted(graph.get(node, ())):
            visit(target)
        active.pop()
    for node in sorted(graph):
        visit(node)
    return result


def check(root):
    root = Path(root).resolve()
    src = root / 'src'
    if not src.is_dir():
        src = root / 'compiler/src'
    files = sorted(p for p in src.rglob('*') if p.suffix in EXTENSIONS and p.is_file())
    if not files:
        return ['产品源码为空或不存在: ' + str(src)], {}
    graph = {p.relative_to(src).as_posix(): set() for p in files}
    modules, problems = {}, []
    for path in files:
        key = path.relative_to(src).as_posix()
        module = key.split('/')[0]
        if module not in ALLOWED:
            problems.append(f'{key}: 未登记模块 {module}')
        modules.setdefault(module, set())
        # 保留换行便于诊断；去块注释后仅匹配行首预处理指令。
        text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m[0].count('\n'),
                      path.read_text(), flags=re.S)
        for number, line in enumerate(text.splitlines(), 1):
            match = re.match(r'\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', line)
            if not match:
                continue
            quoted = match[1] is not None
            name = match[1] if quoted else match[2]
            candidates = (path.parent / name, src / name) if quoted else (src / name,)
            target = next((p.resolve() for p in candidates
                           if p.is_file()), None)
            if target is None and not quoted and name.split('/')[0] not in ALLOWED:
                continue  # 系统/外部头；已知项目模块不能靠角括号规避缺失检查。
            if target is None or not target.is_relative_to(src.resolve()):
                problems.append(f'{key}:{number} -> {name}: 本地 include 不存在或越出 src')
                continue
            dest = target.relative_to(src.resolve()).as_posix()
            if dest not in graph:
                problems.append(f'{key}:{number} -> {dest}: 产品片段扩展名未纳入检查')
                continue
            graph[key].add(dest)
            lower = dest.split('/')[0]
            if lower != module:
                modules[module].add(lower)
                bridge = key in BRIDGES and lower == 'frontend'
                if lower not in ALLOWED.get(module, set()) and not bridge:
                    problems.append(f'{key}:{number} -> {dest}: 禁止依赖方向 {module} -> {lower}')
    for label, edges in [('文件', graph), ('模块', modules)]:
        problems.extend(label + '循环依赖: ' + ' -> '.join(c) for c in cycle_paths(edges))
    return problems, modules


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    errors, modules = check(parser.parse_args().root)
    for module, targets in sorted(modules.items()):
        print(module + ' -> ' + ', '.join(sorted(targets)))
    for error in errors:
        print('FAIL: ' + error)
    print('Architecture: ' + ('FAIL' if errors else 'PASS (literal local includes)'))
    raise SystemExit(bool(errors))
