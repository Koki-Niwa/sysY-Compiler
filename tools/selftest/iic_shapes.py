#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iic_shapes.py —— 独立实现的**读取层形状辅助**（C6/C7/C8 共用）。

★ 本文件里的每一处都**只影响读取（形状）**，不参与任何语义推导：
  * `obj_of` / `obj_of_ir`：值的包装类型 ↔ 对象类型
      （`ir_of` 把数组包成 `ptr[…]`，而 `Alloca`/`global`/`gep` 要对象类型）
  * `attribute_diffs` / `print_bad`：差异 hunk 逐段配对归因 + 全量打印
      （不能用"集合差"：移位会造出几百种假类别；只取两段里第一个真正
        不同的行当归因。全量打印是必须的 —— 打印出来的清单极易被当成全集。）
"""

import re


def obj_of(t):
    """变量槽的**对象类型**（`Alloca` 的 `:type`、`GEP` 的元素类型用它）。

    ★ 这一行只影响读取（形状），不影响语义推导：
      `ir_of` 给的是**值的包装类型**（数组值在 IR 里是一根指针：
      `int[3] -> ptr[[3 x i32]]`），而 `Alloca`/`GEP` 的第一个记号是
      **对象类型本身**，要**递归**展开、不带指针包装：
        int[3]     -> [3 x i32]
        int[3][4]  -> [3 x [4 x i32]]
      实测（`int loc[3]` 那种用例）：
        独立实现 `(Alloca %main.0 ptr[[3 x i32]] @line 4)`
        C++      `(Alloca %main.0 [3 x i32]      @line 4)`
      ⇒ 96 个文件里最大的那一类差异。
    """
    base, dims = t
    if not dims:
        return 'i32' if base == 'int' else 'f32'
    return '[%d x %s]' % (dims[0] if dims[0] is not None else 0,
                          obj_of((base, dims[1:])))


def obj_of_ir(t):
    """IR 文本的"值类型 → 对象类型"（递归）。★ 只影响读取（形状），不改语义。

    `ir_of` 给的是值的包装类型（数组被包成 `ptr[…]`），而 `Alloca`/`global`/
    `gep` 的记号要对象类型：`ptr[[10 x ptr[[10 x i32]]]]` -> `[10 x [10 x i32]]`。
    """
    t = t.strip()
    if t.startswith('ptr[') and t.endswith(']'):
        return obj_of_ir(t[4:-1])
    if t.startswith('[') and t.endswith(']'):
        inner = t[1:-1]
        pos = inner.find(' x ')
        if pos > 0:
            return '[%s x %s]' % (inner[:pos], obj_of_ir(inner[pos + 3:]))
    return t


def attribute_diffs(bad, norm=None):
    """差异 hunk 逐段配对归因 → [(minus, plus, 次数)]（降序）。

    ★ 不能用"集合差"：**移位**会把无关行配成对、造出几百种假类别（实测
      286 类里绝大多数是假的）。只取两段里**第一个真正不同的行**当归因。
    """
    import re as _re

    def _n(x):
        x = _re.sub(r'%[A-Za-z0-9_.]+', '%N', x)
        return _re.sub(r'\d+', 'N', x)

    tally = {}
    for _p, d in bad:
        lines = [l for l in d.split('\n') if l[:1] in '+-' and not l.startswith(('+++', '---'))]
        i = 0
        while i < len(lines):
            if lines[i][0] != '-':
                i += 1
                continue
            j = i
            while j < len(lines) and lines[j][0] == '-':
                j += 1
            k = j
            while k < len(lines) and lines[k][0] == '+':
                k += 1
            m = [_n(x[1:].strip()) for x in lines[i:j]]
            q = [_n(x[1:].strip()) for x in lines[j:k]]
            first = None
            for idx in range(max(len(m), len(q))):
                a = m[idx] if idx < len(m) else '<段结束>'
                b = q[idx] if idx < len(q) else '<段结束>'
                if a != b:
                    first = (a[:70], b[:70])
                    break
            if first:
                tally[first] = tally.get(first, 0) + 1
            i = max(k, i + 1)
    return sorted(tally.items(), key=lambda kv: -kv[1])


def print_bad(bad, list_diffs, head=3, top=40):
    """打印差异：默认前 `head` 个；`list_diffs=True` 时印**全部**并归因。

    ★ 必须能"印全部"：原来只印 `bad[:3]`，而**打印出来的清单**极易被当成
      全集（S06 实测把 10 当成 76，见 TESTING-GUIDE"数字只认摘要行"）。
    """
    show = bad if list_diffs else bad[:head]
    for p, d in show:
        print('-' * 72)
        print('差异文件: %s' % p)
        print(d)
    if not list_diffs:
        return
    tally = attribute_diffs(bad)
    print('=' * 72)
    print('差异行归因（%d 个文件、%d 类）：' % (len(bad), len(tally)))
    for (x, y), n in tally[:top]:
        print('  %5d  - %s' % (n, x))
        print('         + %s' % y)
