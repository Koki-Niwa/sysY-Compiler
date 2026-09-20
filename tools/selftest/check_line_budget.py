#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_line_budget.py —— 让 `SESSION-PLAN.md` §C4 的"单文件行数上限"**真的可执行**。

为什么需要它
------------
§C4 一直写着"单一职责，单文件 ≤ 600 行……超 600 行必须拆分"，
但**从来没有任何东西检查过它**，于是它既没被遵守也没被拦下：
S02 收尾时 `AstPrinter.cpp` 1135 行、`Parser.cpp` 1010 行，两份报告都没提这件事。
"写了标准但不检查"比没有标准更糟——它会让后面每个会话都学会无视这一节。

规则（与 §C4 同步，改这里必须同时改文档）
------------------------------------------
* `compiler/src/**`（**产品代码**）：**硬上限 600 行**，无豁免。
* `compiler/tools/**`（**开发/验证脚本**）：**棘轮**
    - 已记录在 `line_ratchet.txt` 里的文件：**只许变短，不许变长**；
    - 未记录的新文件：不得超过 `--new-limit`（默认 800）。
  理由：测试与独立验证脚本的"长"往往来自断言/用例的数量，拆开更难对照；
  但也没有理由让它们继续长。

用法
----
    python3 check_line_budget.py --root /home/koki1/try            # 校验（默认）
    python3 check_line_budget.py --root /home/koki1/try --update   # 刷新棘轮（**编排方专用**）

退出码：0 = 合规；1 = 有超限；2 = 工具自身错误。
"""

import argparse
import os
import sys

SRC_HARD_LIMIT = 600
SRC_EXTS = ('.h', '.cpp')
TOOLS_EXTS = ('.py', '.sh', '.c', '.cpp', '.h')


def count_lines(path: str) -> int:
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return sum(1 for _ in f)
    except OSError:
        return -1


def collect(root: str, sub: str, exts) -> dict:
    base = os.path.join(root, sub)
    out = {}
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in ('build', '__pycache__', '.git')]
        for fn in filenames:
            if not fn.endswith(exts):
                continue
            p = os.path.join(dirpath, fn)
            out[os.path.relpath(p, root)] = count_lines(p)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='/home/koki1/try')
    ap.add_argument('--update', action='store_true', help='刷新棘轮文件（编排方专用）')
    ap.add_argument('--new-limit', type=int, default=800)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    root = args.root
    if not os.path.isdir(os.path.join(root, 'compiler')):
        print('找不到 %s/compiler（--root 对吗？）' % root, file=sys.stderr)
        return 2

    src = collect(root, 'compiler/src', SRC_EXTS)
    tools = collect(root, 'compiler/tools', TOOLS_EXTS)
    ratchet_path = os.path.join(root, 'compiler/tools/selftest/line_ratchet.txt')

    if args.update:
        with open(ratchet_path, 'w', encoding='utf-8') as f:
            f.write('# 开发/验证脚本的行数棘轮（由编排方用 --update 刷新）\n')
            f.write('# 格式：<行数>  <相对工作区根的路径>\n')
            f.write('# 规则：只许变短，不许变长；不在表里的新文件不得超过 %d 行。\n' % args.new_limit)
            for k in sorted(tools):
                f.write('%d  %s\n' % (tools[k], k))
        print('已写入棘轮：%s（%d 个文件）' % (ratchet_path, len(tools)))
        return 0

    # ── 产品代码：硬上限 ──
    over = sorted((v, k) for k, v in src.items() if v > SRC_HARD_LIMIT)
    print('产品代码 (compiler/src): %d 个文件，上限 %d 行' % (len(src), SRC_HARD_LIMIT))
    for v, k in over:
        print('  ✘ %-46s %4d 行（超 %d）' % (k, v, v - SRC_HARD_LIMIT))

    # ── 工具脚本：棘轮 ──
    recorded = {}
    if os.path.exists(ratchet_path):
        with open(ratchet_path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                n, _, p = line.partition('  ')
                recorded[p] = int(n)
    else:
        print('  ! 棘轮文件不存在：%s' % ratchet_path, file=sys.stderr)

    grew, newbig = [], []
    for k, v in sorted(tools.items()):
        if k in recorded:
            if v > recorded[k]:
                grew.append((v, recorded[k], k))
        elif v > args.new_limit:
            newbig.append((v, k))

    print('工具脚本 (compiler/tools): %d 个文件（已记录 %d），棘轮' % (len(tools), len(recorded)))
    for v, old, k in grew:
        print('  ✘ %-46s %4d 行（棘轮 %d，长了 %d）' % (k, v, old, v - old))
    for v, k in newbig:
        print('  ✘ %-46s %4d 行（新文件超过 %d）' % (k, v, args.new_limit))

    if over or grew or newbig:
        print('判定：✘ 违反 §C4（要么拆分，要么由编排方明确改标准）')
        return 1
    print('判定：✔ 行数预算合规')
    return 0


if __name__ == '__main__':
    sys.exit(main())
