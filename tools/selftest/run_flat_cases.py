#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_flat_cases.py —— 跑 `compiler/tests/flat/**` 的用例集（金样例 + 最小对照）

用法（**命令行为固定契约**，关卡照这个签名调用）：
    python3 run_flat_cases.py --compiler <路径> [--verbose] [--update]

判据：结构化产物与历史 expect 逐字节相同且往返幂等；平面产物真正由
`--emit=flat-ir` 生成，经 `--from-flat` 往返幂等，并与 gcc 比较输出和
main 返回值。两种编译均要求 rc=0、stderr 零诊断。

`min/<编号>-<短名>/` 另加一条：`good.sy` / `bad.sy` 必须**只差一处** ——
用 difflib 的统一 diff 数 hunk，**恰好 1 个**（`diff -u` 同一口径）。

`--update` 只刷新结构化 expect；平面行为仍须通过 gcc 判据。

退出码：0 = 全过；1 = 有用例失败；2 = 工具自身错误（找不到编译器/用例树）。
"""

import argparse
import difflib
import os
import subprocess
import sys

import flat_case_checks

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CASES = os.path.join(ROOT, 'compiler', 'tests', 'flat')

# 历史 expect 是结构化 IR；平面 IR 的检查在 flat_case_checks.py。
EMIT_ARGS = ['--emit=structured-ir', '--normalize']
ROUNDTRIP_ARGS = ['--from-structured', '--normalize', '--emit=structured-ir']

# 用例数量下限（少于此数说明语料被删过，算失败）
MIN_GOLDEN = 14
MIN_PAIRS = 8


def run(compiler, args):
    return subprocess.run([compiler] + args, capture_output=True)


def diagnose(r):
    """把 stderr 压成一行：stderr 非空本身就是失败，内容只用于报告。"""
    text = r.stderr.decode('utf-8', 'replace').strip()
    return text.splitlines()[0] if text else ''


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def check_source(compiler, sy, expect, stem, update):
    """一份源码的三条判据。

    返回 (problems, info)：problems 为空即通过；info 供 --verbose 与报告用。
    临时文件与源码**同目录**（相对路径做键，见 S05b 的教训：basename 会撞车），
    跑完即删。
    """
    problems = []
    info = ''
    cdir = os.path.dirname(sy)
    got = os.path.join(cdir, '.' + stem + '.got')
    rt = os.path.join(cdir, '.' + stem + '.rt')
    try:
        r = run(compiler, [sy] + EMIT_ARGS + ['-o', got])
        if r.returncode != 0:
            return ['编译失败 rc=%d %s' % (r.returncode, diagnose(r))], ''
        if r.stderr.strip():
            problems.append('stderr 有诊断：%s' % diagnose(r))
        if not os.path.exists(got):
            return problems + ['编译器没产出文件'], ''
        data = read_bytes(got)
        info = '%d 字节' % len(data)

        if not update and not os.path.exists(expect):
            problems.append('缺 %s' % os.path.basename(expect))
        elif not update:
            want = read_bytes(expect)
            if want != data:
                problems.append('与 %s 不同（want %d / got %d 字节）'
                                % (os.path.basename(expect), len(want), len(data)))

        # 往返：读回再 dump 必须逐字节相同（幂等 / 轨 A 的前身）
        r2 = run(compiler, [got] + ROUNDTRIP_ARGS + ['-o', rt])
        if r2.returncode != 0:
            problems.append('往返失败 rc=%d %s' % (r2.returncode, diagnose(r2)))
        elif r2.stderr.strip():
            problems.append('往返 stderr 有诊断：%s' % diagnose(r2))
        elif not os.path.exists(rt):
            problems.append('往返没产出文件')
        elif read_bytes(rt) != data:
            problems.append('往返与产物不同（%d / %d 字节）'
                            % (os.path.getsize(rt), len(data)))
        flat_problems, flat_size = flat_case_checks.check(compiler, sy)
        problems.extend(flat_problems)
        info += '；flat %d 字节' % flat_size
        if update and not problems:
            with open(expect, 'wb') as f:
                f.write(data)
    finally:
        for p in (got, rt):
            if os.path.exists(p):
                os.remove(p)
    return problems, info


def diff_hunks(a_path, b_path):
    """按 `diff -u` 的口径数 (hunk 数, 改动行数)。零第三方依赖。"""
    with open(a_path, encoding='utf-8') as f:
        a = f.read().splitlines()
    with open(b_path, encoding='utf-8') as f:
        b = f.read().splitlines()
    hunks = changed = 0
    for line in difflib.unified_diff(a, b, lineterm=''):
        if line.startswith('@@'):
            hunks += 1
        elif line[:1] in ('+', '-') and not line.startswith(('+++', '---')):
            changed += 1
    return hunks, changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True, help='编译器可执行文件路径')
    ap.add_argument('--verbose', action='store_true', help='打印每条判据的细节')
    ap.add_argument('--update', action='store_true',
                    help='（**维护者专用**）用真实运行产出刷新全部 expect')
    ap.add_argument('--dir', default=CASES, help='用例树根目录（默认 compiler/tests/flat）')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    if not os.path.isdir(args.dir):
        print('找不到用例树：%s' % args.dir, file=sys.stderr)
        return 2
    gdir = os.path.join(args.dir, 'golden')
    mdir = os.path.join(args.dir, 'min')
    for d in (gdir, mdir):
        if not os.path.isdir(d):
            print('用例树缺子目录：%s' % d, file=sys.stderr)
            return 2

    npass = nfail = 0

    def report(ok, msg):
        """每个用例一行（通过也打）；计进总账。"""
        nonlocal npass, nfail
        if ok:
            npass += 1
            print('  ✔ %s' % msg)
        else:
            nfail += 1
            print('  ✘ %s' % msg)

    mode = '（--update：刷新结构化 expect）' if args.update else '（结构化 expect + flat 往返 + gcc）'
    print('== flat 用例集：structured-ir + flat-ir %s ==' % mode)
    print('   编译器：%s' % compiler)

    # ── 1. golden/ ─────────────────────────────────────────────────────────
    print('== golden/（金样例：case.sy + expect）==')
    gnames = sorted(n for n in os.listdir(gdir) if os.path.isdir(os.path.join(gdir, n)))
    if len(gnames) < MIN_GOLDEN:
        report(False, '金样例不足 %d 个（实际 %d）' % (MIN_GOLDEN, len(gnames)))
    for name in gnames:
        cdir = os.path.join(gdir, name)
        sy = os.path.join(cdir, 'case.sy')
        expect = os.path.join(cdir, 'expect')
        if not os.path.exists(sy):
            report(False, 'golden/%s: 缺 case.sy' % name)
            continue
        problems, info = check_source(compiler, sy, expect, 'case', args.update)
        if problems:
            report(False, 'golden/%s: %s' % (name, '；'.join(problems)))
            continue
        extra = '（--update：已写 expect，%s）' % info if args.update else '（%s）' % info
        report(True, 'golden/%s%s' % (name, extra))
        if args.verbose:
            print('      结构化 expect/往返、flat 往返、flat 与 gcc 行为均通过')

    # ── 2. min/ ────────────────────────────────────────────────────────────
    print('== min/（最小对照：good.sy / bad.sy 只差一处 + expect）==')
    mnames = sorted(n for n in os.listdir(mdir) if os.path.isdir(os.path.join(mdir, n)))
    if len(mnames) < MIN_PAIRS:
        report(False, '最小对照不足 %d 组（实际 %d）' % (MIN_PAIRS, len(mnames)))
    for name in mnames:
        cdir = os.path.join(mdir, name)
        good = os.path.join(cdir, 'good.sy')
        bad = os.path.join(cdir, 'bad.sy')
        if not (os.path.exists(good) and os.path.exists(bad)):
            report(False, 'min/%s: 缺 good.sy 或 bad.sy' % name)
            continue
        problems = []
        infos = []
        for which in ('good', 'bad'):
            sy = os.path.join(cdir, which + '.sy')
            expect = os.path.join(cdir, which + '.expect')
            p, info = check_source(compiler, sy, expect, which, args.update)
            problems += ['%s: %s' % (which, x) for x in p]
            infos.append('%s=%s' % (which, info))
        # 只差一处：统一 diff 恰好 1 个 hunk（与 `diff -u` 同口径）
        hunks, changed = diff_hunks(good, bad)
        if hunks != 1:
            problems.append('good/bad 有 %d 个 hunk（要求恰好 1）' % hunks)
        if problems:
            report(False, 'min/%s: %s' % (name, '；'.join(problems)))
            continue
        extra = '--update：已写 good/bad.expect，%s' % '，'.join(infos) \
            if args.update else '，'.join(infos)
        report(True, 'min/%s（1 hunk / %d 改动行；%s）' % (name, changed, extra))
        if args.verbose:
            print('      good 与 bad 各：结构化 expect/往返、flat 往返、flat 与 gcc 行为均通过')

    # ── 3. 汇总 ────────────────────────────────────────────────────────────
    print('== 结果：%s ==' % ('全过' if nfail == 0 else '有失败'))
    print('   （用例 %d 个：golden %d、min %d 组；判据失败 %d 条）'
          % (len(gnames) + len(mnames), len(gnames), len(mnames), nfail))
    return 0 if nfail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
