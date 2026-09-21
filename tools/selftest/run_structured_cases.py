#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_structured_cases.py —— 跑 S05 的用例集（`compiler/tests/structured/**`）。

用法（**命令行为固定契约**，关卡会照这个签名调用）：
    python3 run_structured_cases.py --compiler <路径> [--verbose]

判据（三类）
------------
1. `example/`：`example.sy` 的 `--emit=structured-ir` 输出必须与
   `example.emit-structured.txt` **逐字节相同**（**格式契约**；关卡另有 cmp -s）。
2. `golden/<编号>-<短名>/`：`good.sy` 的输出必须与 `expect` **逐字节相同**。
   ⚠️ `expect` 是**真实运行产出**的（见 `compiler/tests/structured/README.md`），
   不是手写 —— S03/S04 都因为"手写示例与实际输出不一致"被关卡抓过。
3. `min/<编号>-<短名>/`：`good.sy` 零诊断、`bad.sy` 的诊断编号集合必须与
   `expect` 一致（**空 expect = 零诊断**）。★ S05 不产生新诊断，`bad.sy` 的
   主要目的是验"**不许崩、不许静默产出空 IR**"：所有用例都必须产出
   **非空且能被读回**（轨 A 的读回路径）的结构化 IR，即使退出码是 1。

退出码：0 = 全过；1 = 有用例失败；2 = 工具自身错误（找不到目录/编译器）。
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
CASES = os.path.join(ROOT, 'compiler', 'tests', 'structured')

CODE_RE = re.compile(r'\[(E-[A-Z-]+)\]')


def run(compiler, args):
    return subprocess.run([compiler] + args, capture_output=True)


def emit(compiler, src, out):
    return run(compiler, [src, '--emit=structured-ir', '-o', out])


def codes_of(stderr: bytes):
    return sorted(set(CODE_RE.findall(stderr.decode('utf-8', 'replace'))))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True, help='编译器可执行文件路径')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--dir', default=CASES, help='用例根目录（默认 compiler/tests/structured）')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    if not os.path.isdir(args.dir):
        print('找不到用例目录：%s' % args.dir, file=sys.stderr)
        return 2

    tmp = os.path.join(ROOT, '.work', 'structured_cases')
    os.makedirs(tmp, exist_ok=True)
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

    # ── 1. example/：格式契约 ──────────────────────────────────────────────
    print('== example/（格式契约，逐字节）==')
    d = os.path.join(args.dir, 'example')
    if not os.path.isdir(d):
        print('  ✘ 缺少 example/ 目录')
        nfail += 1
    else:
        sy = os.path.join(d, 'example.sy')
        want = os.path.join(d, 'example.emit-structured.txt')
        got = os.path.join(tmp, 'example.out')
        r = emit(compiler, sy, got)
        if not os.path.exists(want):
            report(False, 'example/example.emit-structured.txt 不存在')
        elif r.returncode != 0:
            report(False, 'example.sy 编译失败 rc=%d %s' % (r.returncode, r.stderr[:200]))
        else:
            a = open(want, 'rb').read()
            b = open(got, 'rb').read()
            if a == b:
                report(True, 'example 与契约逐字节相同（%d 字节）' % len(a))
            else:
                report(False, 'example 与契约不同（want %d 字节 / got %d 字节）' % (len(a), len(b)))
        # 轨 A：读回再 dump 必须逐字节相同
        rt = os.path.join(tmp, 'example.rt')
        r2 = run(compiler, [got, '--from-structured', '--emit=structured-ir', '-o', rt])
        report(r2.returncode == 0 and open(got, 'rb').read() == open(rt, 'rb').read(),
               'example 往返逐字节相同')

    # ── 2. golden/ ─────────────────────────────────────────────────────────
    print('== golden/（金样例：真实产出，逐字节）==')
    gdir = os.path.join(args.dir, 'golden')
    names = sorted(os.listdir(gdir)) if os.path.isdir(gdir) else []
    if not names:
        print('  ✘ 没有金样例')
        nfail += 1
    for name in names:
        d = os.path.join(gdir, name)
        if not os.path.isdir(d):
            continue
        sy, want = os.path.join(d, 'good.sy'), os.path.join(d, 'expect')
        if not (os.path.exists(sy) and os.path.exists(want)):
            report(False, '%s: 缺 good.sy 或 expect' % name)
            continue
        got = os.path.join(tmp, 'g_%s.out' % name)
        r = emit(compiler, sy, got)
        if r.returncode != 0:
            report(False, '%s: 编译失败 rc=%d' % (name, r.returncode))
            continue
        ok = open(want, 'rb').read() == open(got, 'rb').read()
        # 顺带跑轨 A（往返）
        rt = os.path.join(tmp, 'g_%s.rt' % name)
        r2 = run(compiler, [got, '--from-structured', '--emit=structured-ir', '-o', rt])
        rt_ok = r2.returncode == 0 and open(got, 'rb').read() == open(rt, 'rb').read()
        report(ok and rt_ok, '%s: 与 expect %s / 往返 %s' %
               (name, '相同' if ok else '**不同**', '相同' if rt_ok else '**不同**'))

    # ── 3. min/ ────────────────────────────────────────────────────────────
    print('== min/（最小对照：诊断编号 + 不许崩/不许空 IR）==')
    mdir = os.path.join(args.dir, 'min')
    names = sorted(os.listdir(mdir)) if os.path.isdir(mdir) else []
    if not names:
        print('  ✘ 没有最小对照')
        nfail += 1
    for name in names:
        d = os.path.join(mdir, name)
        if not os.path.isdir(d):
            continue
        exp_path = os.path.join(d, 'expect')
        # expect：**一行若干编号**（`E-UNDEF E-TYPE`）或**空文件**（= 零诊断）。
        #   S03/S04 的 `expect` 是"编号 行号"两列（如 `E-UNDEF 3`），
        #   S05 **不产生新诊断** ⇒ 这里只关心**编号集合**，行号由 S03 的
        #   `run_sema_cases.py` 守（那是它的契约，本脚本不重复）。
        want_codes = (sorted(set(re.findall(r'E-[A-Z-]+', open(exp_path).read())))
                      if os.path.exists(exp_path) else [])
        # good：必须零诊断、非空 IR、能往返
        g = os.path.join(d, 'good.sy')
        og = os.path.join(tmp, 'm_%s.good' % name)
        r = emit(compiler, g, og) if os.path.exists(g) else None
        if r is None:
            report(False, '%s: 缺 good.sy' % name)
        elif r.returncode != 0 or codes_of(r.stderr):
            report(False, '%s: good.sy 不该有诊断（rc=%d codes=%s）' %
                   (name, r.returncode, codes_of(r.stderr)))
        elif os.path.getsize(og) == 0:
            report(False, '%s: good.sy 产出了**空 IR**' % name)
        else:
            rt = os.path.join(tmp, 'm_%s.good.rt' % name)
            r2 = run(compiler, [og, '--from-structured', '--emit=structured-ir', '-o', rt])
            report(r2.returncode == 0 and open(og, 'rb').read() == open(rt, 'rb').read(),
                   '%s: good 零诊断 / 非空 / 往返相同' % name)
        # bad：诊断编号必须与 expect 一致；**必须仍然产出非空合法 IR**（即使 rc=1）
        b = os.path.join(d, 'bad.sy')
        if not os.path.exists(b):
            report(False, '%s: 缺 bad.sy' % name)
            continue
        ob = os.path.join(tmp, 'm_%s.bad' % name)
        r = emit(compiler, b, ob)
        got_codes = codes_of(r.stderr)
        if r.returncode not in (0, 1):
            report(False, '%s: bad.sy 崩溃/异常退出（rc=%d）' % (name, r.returncode))
        elif got_codes != want_codes:
            report(False, '%s: bad.sy 诊断编号不符（want=%s got=%s）' %
                   (name, want_codes, got_codes))
        elif os.path.getsize(ob) == 0:
            report(False, '%s: bad.sy 产出了**空 IR**（静默丢弃）' % name)
        else:
            rt = os.path.join(tmp, 'm_%s.bad.rt' % name)
            r2 = run(compiler, [ob, '--from-structured', '--emit=structured-ir', '-o', rt])
            report(r2.returncode == 0 and open(ob, 'rb').read() == open(rt, 'rb').read(),
                   '%s: bad 诊断编号一致 / 非空 / 可读回' % name)

    print('=' * 62)
    print('用例结果：通过 %d，失败 %d' % (npass, nfail))
    print('判定：%s' % ('✔ 全部通过' if nfail == 0 else '✘ 有失败'))
    return 0 if nfail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
