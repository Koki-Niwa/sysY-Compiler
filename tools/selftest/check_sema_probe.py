#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_sema_probe.py —— **证明 check_sema.py 的轨 B 不是"永远说 OK"**

S01 的教训（phases/S03-sema.md §七）：自校验脚本必须先证明它抓得住错。
所以这里从 `sema_bad/probe.sema`（由 `probe.sy` 真实编译出来的转储）出发，
做**四处定向的人为破坏**，每处对应 §七 的一条不变式，然后要求
`InvariantChecker` **逐条报红**。

四处破坏（都是"改一处、其余原样"）：
  #2  把 `f + i` 里那个 `(Cast :IntToFloat :float (LVal i))` 换成裸操作数
      ⇒ `+` 的两个子节点类型变成 float / int
  #3  把 `%` 的右操作数 `(IntLit 3 :int)` 改成浮点字面量
      ⇒ `%` 出现了 float 操作数（指令集里没有 frem）
  #10 把 `(Cast :ToBool :int` 的目标类型改成 `:float`
      ⇒ 目标类型与"种类推出的值类型"不一致
  #11 把 `while (f)` 的条件整个换成 `(LVal f :obj float :float)`
      ⇒ 条件值类型是 float，没有被 ToBool 包住

用法:
    check_sema_probe.py [--emit] [--root DIR]
        --emit   重新生成 sema_bad/bad-rule*.sema（只在改了 probe.sy 后需要）
退出码：0 = 四处破坏都被逐条抓到；1 = 有没抓到的；2 = 工具自身错误。
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_sema import InvariantChecker            # noqa: E402
from sema_dump_format import Reverter              # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BAD = os.path.join(HERE, 'sema_bad')


# ============================================================================
# 四处定向破坏（纯文本替换，每处都断言"确实换掉了"）
# ============================================================================
def _drop_cast(text, marker):
    """把含 `marker` 的那个 Cast 换成它的子节点（**严格照 §六 第 4 条**）。

    删掉 Cast 那一整行 + 子节点子树每行去掉 2 个前导空格 + 丢掉 Cast 自己的
    右括号。这里的 Cast 直接子节点恰好只有一行，所以"丢掉自己的右括号" =
    在子节点行尾那串 `)` 里去掉**紧跟子节点自身 `)` 之后**的那一个。
    """
    lines = text.split('\n')
    i = next(k for k, l in enumerate(lines) if marker in l)
    head = lines[i]
    indent = len(head) - len(head.lstrip(' '))
    assert head.strip().startswith('(Cast'), head
    child = lines[i + 1]
    assert child.startswith(' ' * (indent + 2)), (head, child)
    body = child[indent + 2:]
    depth, cut = 0, None
    for k, ch in enumerate(body):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                cut = k
                break
    assert cut is not None and body[cut + 1] == ')', body
    new_body = body[:cut + 1] + body[cut + 2:]
    lines[i] = None                       # 删掉 Cast 那一整行
    lines[i + 1] = ' ' * indent + new_body
    return '\n'.join(l for l in lines if l is not None)


def make_bad_rule02(text):
    """#2：删掉 `f + i` 里的 IntToFloat ⇒ `+` 两侧类型不同。"""
    out = _drop_cast(text, '(Cast :IntToFloat :float')
    assert 'Cast :IntToFloat' not in out
    return out


def make_bad_rule03(text):
    """#3：`%` 的右操作数改成浮点 ⇒ `%` 出现 float 操作数。"""
    out = text.replace('(IntLit 3 :int)', '(FloatLit 3.0 :float)', 1)
    assert out != text
    return out


def make_bad_rule10(text):
    """#10：`if (f)` 的 `(Cast :ToBool :int` 目标类型改成 `:float`。"""
    out = text.replace('(Cast :ToBool :int', '(Cast :ToBool :float', 1)
    assert out != text
    return out


def make_bad_rule11(text):
    """#11：`while (f)` 的条件整个换成 float 的 LVal（不再被 ToBool 包住）。"""
    lines = text.split('\n')
    i = next(k for k, l in enumerate(lines)
             if l.strip() == '(While' and 'Cast :ToBool' in lines[k + 1])
    head = lines[i]
    indent = len(head) - len(head.lstrip(' '))
    # 条件 = Cast 的子节点（下一行的再下一行）
    cond = lines[i + 2].strip()
    assert cond.startswith('(LVal f'), cond
    lines[i + 1] = ' ' * (indent + 2) + cond
    del lines[i + 2]
    return '\n'.join(lines)


CASES = [
    ('bad-rule02.sema', 2, make_bad_rule02,
     '把 (Cast :IntToFloat :float (LVal i)) 换成裸操作数 ⇒ `+` 两侧类型不同'),
    ('bad-rule03.sema', 3, make_bad_rule03,
     '把 `%` 的右操作数改成浮点 ⇒ `%` 出现 float 操作数'),
    ('bad-rule10.sema', 10, make_bad_rule10,
     '把 (Cast :ToBool :int 的目标类型改成 :float ⇒ 与种类推出的值类型不符'),
    ('bad-rule11.sema', 11, make_bad_rule11,
     '把 while 的条件换成 float 的 LVal ⇒ 条件没有被 ToBool 包住'),
]


# ============================================================================
# 跑检查器
# ============================================================================
def scan(text):
    chk = InvariantChecker()
    for line in text.splitlines(keepends=True):
        chk.feed(line)
    chk.finish()
    return chk.violations


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emit', action='store_true', help='重新生成 bad-rule*.sema')
    args = ap.parse_args()

    good_path = os.path.join(BAD, 'probe.sema')
    if not os.path.exists(good_path):
        print('找不到 %s（先用编译器生成：compiler probe.sy --emit=sema -o probe.sema）'
              % good_path, file=sys.stderr)
        return 2
    with open(good_path, encoding='utf-8') as f:
        good = f.read()

    if args.emit:
        for name, rule, fn, _desc in CASES:
            with open(os.path.join(BAD, name), 'w', encoding='utf-8') as f:
                f.write(fn(good))
        print('已生成 %d 份坏输入到 %s' % (len(CASES), BAD))

    failures = 0

    # ① 未经破坏的转储必须**零违规**（否则下面的"抓到了"没有意义）
    v = scan(good)
    if v:
        print('✘ probe.sema（未破坏）本应零违规，实际有 %d 条：' % len(v))
        for r, ln, m in v[:5]:
            print('    #%d 行%d %s' % (r, ln, m))
        failures += 1
    else:
        print('✔ probe.sema（未破坏）零违规')

    # ② 还原器也要在同一份文本上成立（probe.sy 的 ast 转储在旁边）
    ast_path = os.path.join(BAD, 'probe.emit-ast.txt')
    if os.path.exists(ast_path):
        with open(ast_path, encoding='utf-8') as f:
            want = f.read()
        rev = Reverter()
        got = []
        for line in good.splitlines(keepends=True):
            o = rev.feed(line)
            if o:
                got.append(o)
        got.append(rev.finish())
        got = ''.join(got)
        if got == want:
            print('✔ probe.sema 去注解还原后与 probe.emit-ast.txt 逐字节相同')
        else:
            print('✘ probe.sema 去注解还原结果不一致')
            gl, wl = got.split('\n'), want.split('\n')
            for i in range(max(len(gl), len(wl))):
                a = gl[i] if i < len(gl) else '<EOF>'
                b = wl[i] if i < len(wl) else '<EOF>'
                if a != b:
                    print('    首个不同：第 %d 行\n      got: %r\n      exp: %r'
                          % (i + 1, a, b))
                    break
            failures += 1
    else:
        print('! 没有 %s，跳过还原器自检' % ast_path)

    # ③ 四处破坏必须逐条报红
    for name, rule, _fn, desc in CASES:
        path = os.path.join(BAD, name)
        if not os.path.exists(path):
            print('✘ 缺少坏输入 %s（用 --emit 生成）' % name)
            failures += 1
            continue
        with open(path, encoding='utf-8') as f:
            text = f.read()
        viol = scan(text)
        hit = [x for x in viol if x[0] == rule]
        print('─' * 74)
        print('%s  期望违反 #%d —— %s' % (name, rule, desc))
        if hit:
            print('  ✔ 抓到了 %d 条 #%d，实际报错原文：' % (len(hit), rule))
            for r, ln, m in hit[:3]:
                print('      [轨B #%d] 第 %d 行：%s' % (r, ln, m))
        else:
            print('  ✘ **没抓到** #%d！实际违规：%s'
                  % (rule, sorted({x[0] for x in viol}) or '无'))
            failures += 1

    print('=' * 74)
    if failures:
        print('判定：✘ 探针有 %d 项失败' % failures)
        return 1
    print('判定：✔ 检查器不是"永远说 OK"：未破坏的零违规、四处破坏逐条报红')
    return 0


if __name__ == '__main__':
    sys.exit(main())
