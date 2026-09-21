#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_loopnorm_cases.py —— 跑 S05b 的用例集（`compiler/tests/loopnorm/**`）

用法（**命令行为固定契约**，关卡会照这个签名调用）：
    python3 run_loopnorm_cases.py --compiler <路径> [--verbose]

判据（四类）
------------
1. `example/`：`example.sy` 的 `--normalize --emit=structured-ir` 输出必须与
   `example.emit-structured.txt` **逐字节相同**（**格式契约**；关卡另有 cmp -s）。
   再加两条：
     * **幂等**：把产物 `--from-structured --normalize --emit=structured-ir`
       读回再跑，必须**逐字节相同**（§C3：`run(run(X)) == run(X)`）；
     * **冻结**：**不带** `--normalize` 的产物不得含 `For`（S05 契约不变）。
2. `golden/<编号>-<短名>/`：`case.sy` 的 `--normalize --emit=structured-ir`
   输出必须与 `expect` **逐字节相同**。`expect` 是**真实运行产出**
   （见 `README.md`），不是手写。
3. `min/<编号>-<短名>/`：`good.sy` / `bad.sy` **只差一处**；要求
   `ForOp` 数 **good ≥ bad**（"该规范的规范化、不该规范的保留"），且两者都
   零诊断、都能往返。
4. 全部用例：`--normalize` 之后**不变量全过**（用 `check_loopnorm` 的检查器）。

退出码：0 = 全过；1 = 有用例失败；2 = 工具自身错误（找不到目录/编译器）。
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CASES = os.path.join(ROOT, 'compiler', 'tests', 'loopnorm')
sys.path.insert(0, HERE)

import loopnorm_ir as L            # noqa: E402
import check_loopnorm as CK        # noqa: E402

CODE_RE = re.compile(r'\[(E-[A-Z-]+)\]')


def run(compiler, args):
    return subprocess.run([compiler] + args, capture_output=True)


def nfor(text):
    try:
        mod, _n, _f = L.parse_dump(text)
    except L.ParseError:
        return -1
    return len(L.find_all([mod], 'For'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True, help='编译器可执行文件路径')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--dir', default=CASES)
    ap.add_argument('--update', action='store_true',
                    help='（**编排方专用**）用真实输出刷新 expect/*.txt')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    if not os.path.isdir(args.dir):
        print('找不到用例目录：%s' % args.dir, file=sys.stderr)
        return 2

    npass = nfail = 0

    def report(ok, msg):
        nonlocal npass, nfail
        if ok:
            npass += 1
            if args.verbose:
                print('  ✔ %s' % msg)
        else:
            nfail += 1
            print('  ✘ %s' % msg)

    # ── 1. example/ ────────────────────────────────────────────────────────
    print('== example/（格式契约 + 幂等 + 冻结，逐字节）==')
    d = os.path.join(args.dir, 'example')
    sy = os.path.join(d, 'example.sy')
    want = os.path.join(d, 'example.emit-structured.txt')
    got = os.path.join(d, '.example.got')
    raw = os.path.join(d, '.example.raw')
    rt = os.path.join(d, '.example.rt')
    r = run(compiler, [sy, '--normalize', '--emit=structured-ir', '-o', got])
    if args.update:
        if r.returncode == 0:
            with open(want, 'wb') as f:
                f.write(open(got, 'rb').read())
            print('  （--update：已刷新 example.emit-structured.txt）')
        else:
            print('  ✘ 编译失败，未刷新：%s' % r.stderr[:200])
            return 1
    if not os.path.exists(want):
        report(False, 'example/example.emit-structured.txt 不存在')
    elif r.returncode != 0:
        report(False, 'example.sy 编译失败 rc=%d %s' % (r.returncode, r.stderr[:200]))
    else:
        a = open(want, 'rb').read()
        b = open(got, 'rb').read()
        report(a == b, 'example 与契约逐字节相同（%d 字节）' % len(a) if a == b
               else 'example 与契约不同（want %d / got %d 字节）' % (len(a), len(b)))
        # 幂等：读回再规范化必须逐字节相同
        r2 = run(compiler, [got, '--from-structured', '--normalize',
                            '--emit=structured-ir', '-o', rt])
        same = (r2.returncode == 0 and
                open(got, 'rb').read() == open(rt, 'rb').read())
        report(same, '幂等：读回再规范化逐字节相同（run(run(X)) == run(X)）')
    # 冻结：不带开关时不得出现 ForOp
    r3 = run(compiler, [sy, '--emit=structured-ir', '-o', raw])
    if r3.returncode == 0:
        txt = open(raw, encoding='utf-8').read()
        report(nfor(txt) == 0, '冻结：不带 --normalize 时 0 个 ForOp（S05 契约不变）')
        # 不变量：规范化后全过
        try:
            mod, names, _f = L.parse_dump(open(got, encoding='utf-8').read())
            bad = CK.check_invariants(mod, names)
            report(not bad, '规范化后的 example 不变量全过' if not bad else str(bad[:2]))
        except L.ParseError as e:
            report(False, 'ParseError: %s' % e)
    for p in (got, rt, raw):
        if os.path.exists(p):
            os.remove(p)

    # ── 2. golden/ ─────────────────────────────────────────────────────────
    print('== golden/（金样例：真实产出，逐字节）==')
    gdir = os.path.join(args.dir, 'golden')
    names = sorted(os.listdir(gdir)) if os.path.isdir(gdir) else []
    if not names:
        print('  ✘ 没有金样例')
        nfail += 1
    for name in names:
        cdir = os.path.join(gdir, name)
        if not os.path.isdir(cdir):
            continue
        sy = os.path.join(cdir, 'case.sy')
        want = os.path.join(cdir, 'expect')
        got = os.path.join(cdir, '.got')
        r = run(compiler, [sy, '--normalize', '--emit=structured-ir', '-o', got])
        if args.update:
            if r.returncode == 0:
                with open(want, 'wb') as f:
                    f.write(open(got, 'rb').read())
                print('  （--update：已刷新 %s/expect）' % name)
            else:
                print('  ✘ %s 编译失败（rc=%d），未刷新' % (name, r.returncode))
            if os.path.exists(got):
                os.remove(got)
            continue
        if not os.path.exists(want):
            report(False, '%s: 缺 expect' % name)
            continue
        if r.returncode != 0:
            report(False, '%s: rc=%d %s' % (name, r.returncode, r.stderr[:160]))
            if os.path.exists(got):
                os.remove(got)
            continue
        a = open(want, 'rb').read()
        b = open(got, 'rb').read()
        report(a == b, '%s（%d 字节）' % (name, len(a)) if a == b
               else '%s: 与 expect 不同（want %d / got %d 字节）' % (name, len(a), len(b)))
        # 幂等 + 不变量
        rt = os.path.join(cdir, '.rt')
        r2 = run(compiler, [got, '--from-structured', '--normalize',
                            '--emit=structured-ir', '-o', rt])
        report(r2.returncode == 0 and open(got, 'rb').read() == open(rt, 'rb').read(),
               '%s: 幂等（读回再规范化逐字节相同）' % name)
        try:
            mod, nm, _f = L.parse_dump(open(got, encoding='utf-8').read())
            bad = CK.check_invariants(mod, nm)
            report(not bad, '%s: 不变量全过' % name if not bad else '%s: %s' % (name, bad[:2]))
        except L.ParseError as e:
            report(False, '%s: ParseError %s' % (name, e))
        for p in (got, rt):
            if os.path.exists(p):
                os.remove(p)

    # ── 3. min/ 对照 ───────────────────────────────────────────────────────
    print('== min/（最小对照：只差一处，good 的 ForOp 数 ≥ bad）==')
    mdir = os.path.join(args.dir, 'min')
    mnames = sorted(os.listdir(mdir)) if os.path.isdir(mdir) else []
    if len(mnames) < 8:
        print('  ✘ 最小对照不足 8 组（实际 %d）' % len(mnames))
        nfail += 1
    for name in mnames:
        cdir = os.path.join(mdir, name)
        if not os.path.isdir(cdir):
            continue
        counts = {}
        for which in ('good', 'bad'):
            sy = os.path.join(cdir, which + '.sy')
            got = os.path.join(cdir, '.' + which + '.got')
            r = run(compiler, [sy, '--normalize', '--emit=structured-ir', '-o', got])
            if r.returncode != 0:
                report(False, '%s/%s: rc=%d %s' % (name, which, r.returncode, r.stderr[:120]))
                counts[which] = -1
                continue
            txt = open(got, encoding='utf-8').read()
            counts[which] = nfor(txt)
            try:
                mod, nm, _f = L.parse_dump(txt)
                bad = CK.check_invariants(mod, nm)
                if bad:
                    report(False, '%s/%s: 不变量违反 %s' % (name, which, bad[:2]))
            except L.ParseError as e:
                report(False, '%s/%s: ParseError %s' % (name, which, e))
            if os.path.exists(got):
                os.remove(got)
        if counts.get('good', -1) >= 0 and counts.get('bad', -1) >= 0:
            report(counts['good'] >= counts['bad'],
                   '%s：good ForOp=%d ≥ bad ForOp=%d'
                   % (name, counts['good'], counts['bad']))
        # 两个文件必须**只差一处**：用 difflib 对齐后数"改动行数"
        #   （逐行 zip 是错的：删掉一行之后所有后续行都会"看起来不同"）
        import difflib
        g = open(os.path.join(cdir, 'good.sy'), encoding='utf-8').read().split('\n')
        b = open(os.path.join(cdir, 'bad.sy'), encoding='utf-8').read().split('\n')
        changed = 0
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, g, b).get_opcodes():
            if tag != 'equal':
                changed += (i2 - i1) + (j2 - j1)
        report(changed <= 2, '%s：good/bad 只差 %d 行' % (name, changed))

    print('== 结果：%d 通过 / %d 失败 ==' % (npass, nfail))
    return 0 if nfail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
