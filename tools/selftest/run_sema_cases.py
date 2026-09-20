#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_sema_cases.py —— 轨 C：跑 `compiler/tests/sema/` 下的**最小对照用例集**

每个用例是一个目录：

    compiler/tests/sema/<编号>-<短名>/bad.sy    必须报错
    compiler/tests/sema/<编号>-<短名>/good.sy   与 bad 只差一处，必须通过
    compiler/tests/sema/<编号>-<短名>/expect    一行：`<编号> <行号>`

判据（phases/S03-sema.md §八）：

  * `bad.sy`：退出码 1；stderr 里出现的诊断编号**恰好**是期望的那一个
    （不多不少 —— "恰好"是有意的：多出来一个编号说明这条规则连带报了别的东西，
     那种噪声会让"诊断指错原因"变得无法察觉）；报该编号的**行号**与 `expect` 一致。
  * `good.sy`：退出码 0 且 stderr 无 `error:`。
  * **最小对照**：`bad.sy` 与 `good.sy` 只差**一处**（统一 diff 只有一个 hunk，
    且两侧各不超过 2 行）。这条是轨 C 的意义所在：若 good 改了别的东西，
    "bad 报错 / good 不报"就不能归因到那一处。

命令行（**关卡会用，签名不许改**）:
    run_sema_cases.py --compiler <可执行文件路径> [--dir DIR] [--filter GLOB]
                      [--verbose] [--jobs N]
退出码：0 = 全过；1 = 有用例失败；2 = 工具自身错误。
"""

import argparse
import concurrent.futures
import difflib
import glob as globmod
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIR = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'sema'))

# `file:line:col: error: [E-XXXX] message`
DIAG_RE = re.compile(r':(\d+):(\d+): error: \[(E-[A-Z0-9-]+)\]')
ERROR_RE = re.compile(r'error:')


def read_expect(path):
    with open(path, encoding='utf-8') as f:
        txt = f.read().strip()
    parts = txt.split()
    if len(parts) != 2:
        raise ValueError('expect 文件应为 `<编号> <行号>`，实际是 %r' % txt)
    return parts[0], int(parts[1])


def minimal_contrast(bad_src, good_src):
    """返回 (是否最小对照, 说明)。"""
    bl = bad_src.splitlines(keepends=True)
    gl = good_src.splitlines(keepends=True)
    ops = [o for o in difflib.SequenceMatcher(None, bl, gl).get_opcodes()
           if o[0] != 'equal']
    if not ops:
        return False, 'bad.sy 与 good.sy 完全相同'
    if len(ops) > 1:
        return False, '相差 %d 处（应为 1 处）' % len(ops)
    tag, i1, i2, j1, j2 = ops[0]
    if tag == 'replace' and (i2 - i1) <= 2 and (j2 - j1) <= 2:
        return True, ''
    if tag in ('insert', 'delete') and (i2 - i1) <= 2 and (j2 - j1) <= 2:
        return True, ''
    return False, 'diff hunk 太大：%s %d 行 -> %d 行' % (tag, i2 - i1, j2 - j1)


class CaseResult:
    def __init__(self, name):
        self.name = name
        self.failures = []
        self.notes = []

    @property
    def ok(self):
        return not self.failures


def run_case(compiler, base, name, timeout):
    d = os.path.join(base, name)
    r = CaseResult(name)
    bad = os.path.join(d, 'bad.sy')
    good = os.path.join(d, 'good.sy')
    exp = os.path.join(d, 'expect')
    for p in (bad, good, exp):
        if not os.path.exists(p):
            r.failures.append('缺少文件 %s' % os.path.basename(p))
            return r
    try:
        code, line = read_expect(exp)
    except ValueError as e:
        r.failures.append(str(e))
        return r

    with open(bad, encoding='utf-8') as f:
        bad_src = f.read()
    with open(good, encoding='utf-8') as f:
        good_src = f.read()
    ok_min, why = minimal_contrast(bad_src, good_src)
    if not ok_min:
        r.failures.append('不是最小对照：%s' % why)

    # ── bad.sy ────────────────────────────────────────────────────────────
    pb = subprocess.run([compiler, bad, '--emit=sema', '-o', os.devnull],
                        capture_output=True, timeout=timeout)
    errb = pb.stderr.decode('utf-8', 'replace')
    r.notes.append('bad stderr: %s' % (errb.strip().split('\n')[0] if errb.strip() else '(空)'))
    if pb.returncode != 1:
        r.failures.append('bad.sy 的退出码是 %d，应为 1' % pb.returncode)
    codes = sorted({m.group(3) for m in DIAG_RE.finditer(errb)})
    if codes != [code]:
        r.failures.append('bad.sy 报的编号是 %s，期望恰好是 [%s]'
                          % (codes or '（无）', code))
    lines = sorted({int(m.group(1)) for m in DIAG_RE.finditer(errb)
                    if m.group(3) == code})
    if lines != [line]:
        r.failures.append('bad.sy 报 %s 的行号是 %s，期望是 [%d]'
                          % (code, lines or '（无）', line))

    # ── good.sy ───────────────────────────────────────────────────────────
    pg = subprocess.run([compiler, good, '--emit=sema', '-o', os.devnull],
                        capture_output=True, timeout=timeout)
    errg = pg.stderr.decode('utf-8', 'replace')
    if pg.returncode != 0:
        r.failures.append('good.sy 的退出码是 %d，应为 0；stderr: %s'
                          % (pg.returncode, errg.strip().split('\n')[0]))
    if ERROR_RE.search(errg):
        r.failures.append('good.sy 的 stderr 里有 error:')
    return r


def main():
    ap = argparse.ArgumentParser(description='S03 轨 C：最小对照用例集')
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--dir', default=DEFAULT_DIR)
    ap.add_argument('--filter', default=None)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--timeout', type=float, default=60.0)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    if not os.path.isdir(args.dir):
        print('找不到用例目录：%s' % args.dir, file=sys.stderr)
        return 2
    names = sorted(n for n in os.listdir(args.dir)
                   if os.path.isdir(os.path.join(args.dir, n)))
    if args.filter:
        names = [n for n in names if globmod.fnmatch.fnmatch(n, args.filter)]
    if not names:
        print('用例目录里没有用例：%s' % args.dir, file=sys.stderr)
        return 2

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(lambda n: run_case(compiler, args.dir, n, args.timeout),
                              names))
    results.sort(key=lambda r: r.name)

    codes_seen = set()
    for r in results:
        if r.ok:
            if args.verbose:
                print('✔ %-34s %s' % (r.name, '；'.join(r.notes)))
            else:
                print('✔ %s' % r.name)
        else:
            print('✘ %s' % r.name)
            for n in r.notes:
                print('    %s' % n)
            for f in r.failures:
                print('    ✘ %s' % f)
        d = os.path.join(args.dir, r.name, 'expect')
        if os.path.exists(d):
            try:
                codes_seen.add(read_expect(d)[0])
            except ValueError:
                pass

    bad = [r for r in results if not r.ok]
    print('=' * 78)
    print('  用例组数           : %d（通过 %d，失败 %d）'
          % (len(results), len(results) - len(bad), len(bad)))
    print('  覆盖的诊断编号     : %d 个 —— %s' % (len(codes_seen), ' '.join(sorted(codes_seen))))
    if len(codes_seen) < 21:
        missing = {'E-UNDEF', 'E-REDEF', 'E-CONST-ASSIGN', 'E-NOT-VAR', 'E-NOT-ARRAY',
                   'E-ARRAY-RANK', 'E-INDEX-TYPE', 'E-ARRAY-DIM', 'E-TYPE', 'E-ARGC',
                   'E-ARGTYPE', 'E-CALL-NONFUNC', 'E-VOID-VALUE', 'E-RET-MISSING',
                   'E-RET-VALUE', 'E-NOT-CONTEXT', 'E-MOD-FLOAT', 'E-CONST-INIT',
                   'E-INIT-SHAPE', 'E-MAIN', 'E-CALL-VARARGS'} - codes_seen
        print('  ✘ 未覆盖的编号     : %s' % ' '.join(sorted(missing)))
        bad = bad or ['missing-codes']
    if bad:
        print('判定：✘ 轨 C 未通过')
        return 1
    print('判定：✔ 轨 C 全部通过（%d 组最小对照，覆盖 21 个编号）' % len(results))
    return 0


if __name__ == '__main__':
    sys.exit(main())
