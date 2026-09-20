#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_ast_baseline.py —— 冻结 `--emit=ast` 输出的**基线指纹**。

为什么需要它
------------
S02 把 `--emit=ast` 的格式冻结成对外契约：S03 以后所有阶段都必须保证它对
490 个范围内用例的输出**逐字节不变**（新格式只能是**加法**，见 S03 的 §五/§六）。

问题在于"先存基线、改完再比"这套流程有个空子：**基线是开发 agent 自己在改动
前后生成的**，它完全可以在改完之后再生成基线，然后声称"没有回归"。
报告与核查分离的前提是**核查方持有不可篡改的基线**，所以基线必须由编排方
在阶段开始前算好并进版本库。

用法
----
    # 生成/刷新基线（只在编排方确认要冻结新格式时用）
    python3 check_ast_baseline.py --compiler <path> --write

    # 校验（默认行为；开发 agent 与关卡都跑这个）
    python3 check_ast_baseline.py --compiler <path>

退出码：0 = 全部一致；1 = 有不一致/缺失/多出；2 = 工具自身错误。

基线文件里的行格式：`<sha256>  <相对 tests/ 的路径>`
"""

import argparse
import concurrent.futures
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

# 与其余自校验脚本一致：范围外（含 @ / tensor）的用例本来就不产出 AST，不纳入基线。
# 判据与 TESTING-GUIDE §7 一致：**先去掉注释**再找关键词。
EXCLUDE_KEYWORDS = ('tensor', '@')


def strip_comments(text: str) -> str:
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i = min(i + 2, n)
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def in_scope(path: str) -> bool:
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            body = strip_comments(f.read())
    except OSError:
        return False
    return not any(k in body for k in EXCLUDE_KEYWORDS)


def ast_of(compiler: str, path: str, workdir: str):
    """返回 (sha256, 错误说明)。成功时错误说明为 None。"""
    fd, out = tempfile.mkstemp(suffix='.ast', dir=workdir)
    os.close(fd)
    try:
        p = subprocess.run([compiler, '--emit=ast', path, '-o', out],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        if p.returncode != 0:
            return None, 'exit=%d %s' % (p.returncode, p.stderr.decode('utf-8', 'replace')[:200])
        with open(out, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest(), None
    except subprocess.TimeoutExpired:
        return None, 'timeout'
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--root', default='/home/koki1/try')
    ap.add_argument('--write', action='store_true', help='刷新基线文件')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    tests = os.path.join(args.root, 'tests')
    baseline = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'baseline_ast_sha256.txt')
    files = sorted(glob.glob(os.path.join(tests, '**', '*.sy'), recursive=True))
    files = [f for f in files if in_scope(f)]
    if not files:
        print('找不到任何范围内用例（--root 对吗？）', file=sys.stderr)
        return 2

    rel = [os.path.relpath(f, tests) for f in files]
    with tempfile.TemporaryDirectory() as wd:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(lambda f: ast_of(args.compiler, f, wd), files))

    cur = {}
    bad = []
    for r, (h, err) in zip(rel, results):
        if h is None:
            bad.append((r, err))
        else:
            cur[r] = h

    if bad:
        print('✘ %d 个文件没能产出 AST：' % len(bad), file=sys.stderr)
        for r, e in bad[:10]:
            print('    %s: %s' % (r, e), file=sys.stderr)
        return 1

    if args.write:
        with open(baseline, 'w', encoding='utf-8') as f:
            for r in sorted(cur):
                f.write('%s  %s\n' % (cur[r], r))
        print('已写入基线：%s（%d 个文件）' % (baseline, len(cur)))
        return 0

    if not os.path.exists(baseline):
        print('✘ 基线文件不存在：%s（由编排方生成，不许自行生成）' % baseline, file=sys.stderr)
        return 2

    old = {}
    with open(baseline, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line:
                continue
            h, _, r = line.partition('  ')
            old[r] = h

    changed = sorted(r for r in old if r in cur and cur[r] != old[r])
    missing = sorted(r for r in old if r not in cur)
    added = sorted(r for r in cur if r not in old)

    print('基线文件 : %s' % baseline)
    print('基线用例 : %d' % len(old))
    print('本次用例 : %d' % len(cur))
    print('逐字节相同: %d' % (len(old) - len(changed) - len(missing)))
    if changed:
        print('✘ 输出变了 (%d): %s' % (len(changed), ', '.join(changed[:8])))
    if missing:
        print('✘ 基线里有、本次没产出 (%d): %s' % (len(missing), ', '.join(missing[:8])))
    if added:
        print('✘ 本次多出来的用例 (%d): %s' % (len(added), ', '.join(added[:8])))
    if changed or missing or added:
        return 1
    print('✔ `--emit=ast` 输出与冻结基线逐字节一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())
