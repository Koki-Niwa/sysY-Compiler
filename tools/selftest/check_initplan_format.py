#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_initplan_format.py —— `--emit=initplan` 的**关卡侧**检查（S04 起）。

为什么要由关卡自己检查，而不是只调开发方的脚本
------------------------------------------------
S03 的教训：开发方的检查器再强，也**不能证明格式契约本身没被理解偏**。
S03 的转储格式曾经出现"例子与规则文字自相矛盾"，那次是关卡里的样例对比抓出来的。
这一关的产物（初始化计划）没有可对齐的历史基线，所以关卡必须**自己**守住三件事：

  ① **结构**：转储是不是 `(InitPlan …)`，`RuntimeLib` 头在不在，
     `:zero` / `:data` / `:actions` 三种形态的拼写对不对；
  ② **规模**：490 个文件的转储**总量** < 20 MB、单文件 < 2 MB。
     语料里有 216,000,000 个元素的全局数组（`sl1-3` 各两个），
     任何"逐元素物化"的实现都会在这里爆掉；
  ③ **策略**：零初始化不产生数据；`int a[4096] = {1}` 的动作数 ≤ 3。
     这两条是 SESSION-PLAN 给 S04 的验收原文，必须**可机械判定**。

语义正确性（填对没填对）由开发方的 `check_initplan.py` 负责——
它要独立实现一遍初始化语义并模拟计划。**关卡不重复那件事，只守住上面三条。**

用法:
    check_initplan_format.py --compiler <路径> [--root DIR] [--jobs N]
退出码：0 = 全过；1 = 有违反；2 = 工具自身错误 / 能力未实现。
"""

import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile

TOTAL_BUDGET = 20 * 1024 * 1024      # 490 个文件合计
SINGLE_BUDGET = 2 * 1024 * 1024      # 单文件
RUNTIME_FUNCS = 13                   # 见 prompt §3.4
EXCLUDE = ('tensor', '@')


def strip_comments(text: str) -> str:
    out, i, n = [], 0, len(text)
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
            return not any(k in strip_comments(f.read()) for k in EXCLUDE)
    except OSError:
        return False


def run(compiler, src, workdir, tag):
    out = os.path.join(workdir, tag + '.txt')
    p = subprocess.run([compiler, '--emit=initplan', src, '-o', out],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    body = ''
    if os.path.exists(out):
        with open(out, 'r', encoding='utf-8', errors='replace') as f:
            body = f.read()
    return p.returncode, body, p.stderr.decode('utf-8', 'replace')


def check_structure(text, problems):
    """结构检查：拼写、配平、三种形态。返回 (globals, locals) 计数。"""
    if not text.startswith('(InitPlan'):
        problems.append('转储不是以 `(InitPlan` 开头，实际前 40 字节：%r' % text[:40])
        return 0, 0
    if text.count('(') != text.count(')'):
        problems.append('括号不配平：%d 个 `(` / %d 个 `)`'
                        % (text.count('('), text.count(')')))
    if '(RuntimeLib' not in text:
        problems.append('缺少 `(RuntimeLib …)` 头（prompt §4.3 要求逐字节复用 --emit=sema 的 13 行）')
    n_rt = len(re.findall(r'^\(RuntimeFunc ', text, re.M)) or text.count('(RuntimeFunc ')
    if n_rt != RUNTIME_FUNCS:
        problems.append('RuntimeLib 里有 %d 个运行时函数，应为 %d 个' % (n_rt, RUNTIME_FUNCS))
    # `:zero` 必须是自闭合的：`(Global x :t int[2] :zero)`，不许再挂子节点
    for m in re.finditer(r'\(Global (\S+) :t ([^\s:]+(?:\[\d*\])*) :zero(.*)$', text, re.M):
        if m.group(3).strip() not in ('', ')'):
            problems.append('`%s` 的 `:zero` 后面还有内容：%r' % (m.group(1), m.group(3)[:40]))
    gl = len(re.findall(r'\(Global ', text))
    lo = len(re.findall(r'\(Local ', text))
    return gl, lo


def check_data_sparse(text, problems):
    """`:data` 里不许出现零值（prompt §4.2：零就地记账，非零才展开）。"""
    for m in re.finditer(r'\(Global (\S+) :t [^\n]*:data\n((?:    \([^)]*\)[^\n]*\n)*)', text):
        for pair in re.finditer(r'\((\d+) (\S+?)\)', m.group(2)):
            v = pair.group(2)
            if v in ('0', '0.0', '-0'):
                problems.append('`%s` 的 `:data` 里列出了零值（偏移 %s）——'
                                '非零元素才展开，零要就地记账' % (m.group(1), pair.group(1)))


def actions_of(text, name):
    """数出某个 Local 的动作条数（只数动作行的开头）。"""
    m = re.search(r'\(Local ([^\s]*)' + re.escape(name) + r' :t [^\n]*:actions\n', text)
    if m is None:
        m = re.search(r'\(Local \S*' + re.escape(name) + r' :t [^\n]*:actions\n', text)
    if m is None:
        return None
    seg = text[m.end():]
    end = seg.find('\n    (Local ')
    seg = seg if end < 0 else seg[:end]
    return len(re.findall(r'^\s+\((Zero|StoreConst|StoreExpr|MemcpyConst) ', seg, re.M))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--root', default='/home/koki1/try')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    problems = []
    tests = os.path.join(args.root, 'tests')
    with tempfile.TemporaryDirectory(dir=args.root, prefix='.initplan_probe_') as wd:
        # ── 能力探测：真正的信号是输出以 `(InitPlan` 开头（不能只看退出码）──
        probe = os.path.join(wd, 'probe.sy')
        with open(probe, 'w') as f:
            f.write('int main(){ return 0; }\n')
        rc, body, err = run(args.compiler, probe, wd, 'probe')
        if rc != 0 or not body.startswith('(InitPlan'):
            print('  ! `--emit=initplan` 尚未实现（rc=%d）—— 跳过' % rc)
            return 2
        check_structure(body, problems)

        # ── ① 策略：零初始化不产生数据 ──
        z = os.path.join(wd, 'zero.sy')
        with open(z, 'w') as f:
            f.write('int big[10000000] = {};\nint big2[10000000];\nint main(){ return 0; }\n')
        rc, body, err = run(args.compiler, z, wd, 'zero')
        if rc != 0:
            problems.append('零初始化的大数组编译失败：rc=%d %s' % (rc, err[:120]))
        else:
            if len(body) > 1024 * 1024:
                problems.append('零初始化的大数组产生了 %d 字节的转储（应只有 `:zero`）' % len(body))
            for nm in ('big', 'big2'):
                if not re.search(r'\(Global ' + nm + r' :t [^\n]*:zero', body):
                    problems.append('`%s` 是零初始化的大数组，却没有被记成 `:zero`' % nm)

        # ── ② 策略：`int a[4096] = {1}` 动作数 ≤ 3；无初始化器 → 0 动作 ──
        a = os.path.join(wd, 'a.sy')
        with open(a, 'w') as f:
            f.write('int main(){ int a[4096] = {1}; int b[4096]; return a[0]+b[0]; }\n')
        rc, body, err = run(args.compiler, a, wd, 'a')
        if rc != 0:
            problems.append('探针 a[4096] 编译失败：rc=%d %s' % (rc, err[:120]))
        else:
            n = actions_of(body, 'a')
            if n is None:
                problems.append('转储里找不到局部对象 `a` 的 `:actions`')
            elif n > 3:
                problems.append('`int a[4096] = {1}` 产生了 %d 条动作（预算 ≤ 3）——'
                                '语义是"全零 + 首元素 1"，不许逐元素展开' % n)
            elif args.verbose:
                print('    a[4096]={1} 动作数 = %d ✔' % n)
            nb = actions_of(body, 'b')
            if nb not in (0, None):
                problems.append('`int b[4096];`（无初始化器）产生了 %d 条动作，应为 0 ——'
                                '未初始化的局部变量不插零填充' % nb)

        # ── ③ 规模：490 个范围内文件的转储总量 ──
        files = sorted(f for f in glob.glob(os.path.join(tests, '**', '*.sy'), recursive=True)
                       if in_scope(f))
        if not files:
            print('找不到语料（--root 对吗？）', file=sys.stderr)
            return 2

        def one(path):
            # ⚠️ 每个文件必须有自己的输出路径。早先这里所有线程共用一个 `x.txt`，
            #    于是并发互相覆盖 —— 测出来的"转储总量"在 1.1~2.1 MB 之间乱跳、
            #    "最大文件"每次都是另一个（还报出过 0.39 MB 的 `05_param_name.sy`，
            #    那种小程序不可能有几十万字节的转储）。**并发的测量代码必须有自己的槽位。**
            fd, out = tempfile.mkstemp(dir=wd, suffix='.txt')
            os.close(fd)
            try:
                p = subprocess.run([args.compiler, '--emit=initplan', path, '-o', out],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300)
                size = os.path.getsize(out) if os.path.exists(out) else 0
            finally:
                try:
                    os.unlink(out)
                except OSError:
                    pass
            return os.path.relpath(path, tests), p.returncode, size

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            res = list(ex.map(one, files))

        failed = [r for r in res if r[1] != 0]
        total = sum(r[2] for r in res)
        biggest = max(res, key=lambda r: r[2])
        print('  范围内用例       : %d' % len(res))
        print('  转储总量         : %.2f MB（预算 %.0f MB）' % (total / 1048576.0, TOTAL_BUDGET / 1048576.0))
        print('  单文件最大       : %s（%.2f MB，预算 %.0f MB）'
              % (biggest[0], biggest[2] / 1048576.0, SINGLE_BUDGET / 1048576.0))
        if failed:
            problems.append('%d 个文件 `--emit=initplan` 退出码非 0，例如 %s'
                            % (len(failed), ', '.join(r[0] for r in failed[:3])))
        if total > TOTAL_BUDGET:
            problems.append('转储总量 %.2f MB 超出预算 %.0f MB —— 几乎肯定是把零初始化物化了'
                            % (total / 1048576.0, TOTAL_BUDGET / 1048576.0))
        if biggest[2] > SINGLE_BUDGET:
            problems.append('单文件最大 %.2f MB 超出预算 %.0f MB（%s）'
                            % (biggest[2] / 1048576.0, SINGLE_BUDGET / 1048576.0, biggest[0]))

    for p in problems:
        print('  ✘ %s' % p)
    if problems:
        print('判定：✘ %d 项违反（格式 / 策略 / 规模）' % len(problems))
        return 1
    print('判定：✔ 初始化计划的结构、策略与规模都合规')
    return 0


if __name__ == '__main__':
    sys.exit(main())
