#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sema_dump_format.py —— `--emit=sema` 转储的**格式层**（S03；被 check_sema.py 复用）

这里放的是"与编译器实现完全独立"的三件事：

  ① 独立实现的**去注释**（判定哪些用例在范围内 / 范围外）；
  ② **去注解还原器**（phases/S03-sema.md §六 第 4 条）：把 `--emit=sema`
     的文本按三步还原成 `--emit=ast` 的字节流。**流式**：边剥注解边喂
     SHA-256，绝不先拼出第二个 128 MB 字符串；
  ③ 转储里的**类型字符串工具**（`int` / `float[2][3]` / `int[]` …）。

为什么要单独一个文件：`check_sema.py` 的职责是"扫语料 + 判 16 条不变式"，
而格式层是纯文本变换，两者可以分开测试与复用（探针脚本也要用还原器）。
**这里不 import 编译器的任何代码** —— 独立性的全部意义就在这里。
"""

import re

# ============================================================================
# 一、语法表（**与 AstSexpFormat.h / Ast.h 的节点种类一一对应**）
# ============================================================================
ARITH = ('+', '-', '*', '/', '%')
REL = ('<', '>', '<=', '>=', '==', '!=')
LOGIC = ('&&', '||')
OPS = set(ARITH) | set(REL) | set(LOGIC) | {'!'}
# 无额外头记号的语句/结构节点（节点头 = `(` + 节点名）
PLAIN = {
    'CompUnit', 'params', 'Block', 'InitVal', 'Dim', '=',
    'ExprStmt', 'If', 'Else', 'While', 'Break', 'Continue', 'Return',
}
# 运行时库的 13 个函数（顺序 = sylib.h，也是转储的打印顺序）
RT_ORDER = ['getint', 'getch', 'getarray', 'getfloat', 'getfarray', 'putint',
            'putch', 'putarray', 'putfloat', 'putfarray', 'putf',
            'starttime', 'stoptime']
CAST_IN = {'IntToFloat': ('float', 'int'), 'FloatToInt': ('int', 'float'),
           'ToBool': ('int', 'float')}   # kind -> (目标类型, 子节点类型)


# ============================================================================
# 二、独立实现的"去注释"（**不复用编译器的任何代码**；与 check_parser.py 同源）
# ============================================================================
def strip_comments(text):
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


def out_of_scope_reason(text):
    code = strip_comments(text)
    reasons = []
    if '@' in code:
        reasons.append('含 `@`')
    if re.search(r'\btensor\b', code):
        reasons.append('含 `tensor`')
    return ' 且 '.join(reasons) if reasons else None


# ============================================================================
# 三、去注解还原器（§六 第 4 条；**流式**，边剥注解边喂 SHA-256）
#
#   三步（顺序不能换，README 里写过为什么）：
#     ① 删掉开头的 `(RuntimeLib ...)` 整块（**按括号配平删**，不按缩进猜）；
#     ② **先**处理 `Cast`：`(Cast :<种类> :<目标类型> <子节点>)` → `<子节点>`。
#        `Cast` 是**一个节点**：它独占一行、子节点在下一行缩进 +2，
#        所以"换成子节点" = 删掉 Cast 那一整行 + 子树的每一行去掉 2 个前导空格
#        + 丢掉 Cast 自己的那个右括号。
#     ③ **再**删记号：` :t <类型>`、` :obj <类型>`、以及表达式节点的值类型记号。
#        ⚠️ `(Decl :const :int` 与 `(FuncDef f :float` 上的类型记号是 S02 原有的，
#           **不删** —— 所以必须"结构感知"（按节点头的形状决定删几个 token）。
# ============================================================================
_TOKEN_RE = re.compile(r'[()]|[^\s()]+')


class Reverter:
    """把 `--emit=sema` 的行流还原成 `--emit=ast` 的字节流。"""

    def __init__(self):
        self.depth = 0
        self.cast_levels = []      # 每个"已丢弃的 Cast"记录它 '(' 之前的深度
        self.in_header = True
        self.header_depth = 0
        self.saw_header = False

    # ── ① RuntimeLib 头：按括号配平整块丢弃 ──────────────────────────────
    def _header(self, line):
        self.header_depth += line.count('(') - line.count(')')
        if '(' in line:
            self.saw_header = True
        if self.header_depth == 0:
            self.in_header = False
        return None

    # ── ② + ③ 一行正文 ────────────────────────────────────────────────────
    def _body(self, line):
        nl = '\n' if line.endswith('\n') else ''
        raw = line[:-1] if nl else line
        stripped = raw.lstrip(' ')
        indent = len(raw) - len(stripped)
        if not stripped:
            return line

        # ② Cast 的节点头独占一行（子节点必定在下一行）⇒ 整行丢掉
        if stripped.startswith('(Cast ') and ')' not in stripped:
            self.cast_levels.append(self.depth)
            self.depth += 1
            return None

        # 在 Cast 内部 ⇒ 本行缩进少 2（每个还开着的 Cast 一层）
        dedent = 2 * len(self.cast_levels)
        out_indent = max(0, indent - dedent)

        delete = []          # 要删掉的字符区间（半开区间）
        # ── Cast 的右括号：深度回到 Cast 打开前的那一刻 ⇒ 丢掉这个 ')' ──
        if self.cast_levels:
            d = self.depth
            for idx, ch in enumerate(stripped):
                if ch == '(':
                    d += 1
                elif ch == ')':
                    if self.cast_levels and self.cast_levels[-1] == d - 1:
                        self.cast_levels.pop()
                        delete.append((idx, idx + 1))
                    d -= 1
            self.depth = d
        else:
            self.depth += stripped.count('(') - stripped.count(')')

        # ── ③ 结构感知地删记号 ──
        toks = [(m.start(), m.end(), m.group(0)) for m in _TOKEN_RE.finditer(stripped)]
        drop_from = _annotation_start(toks)
        if drop_from is not None:
            start = toks[drop_from][0]
            end = toks[drop_from][1]
            # 连续删到该节点头部结束（`:t T` / `:obj T :T` / `:T`）：
            # `:` 开头的记号本身要删；紧跟在 `:t` / `:obj` 后面的类型记号
            # 不以 `:` 开头，也要删；遇到 `(` / `)` 就停。
            j = drop_from
            while j < len(toks) and toks[j][2].startswith(':'):
                end = toks[j][1]
                j += 1
                if j < len(toks) and not toks[j][2].startswith(':') \
                        and toks[j][2] not in ('(', ')'):
                    end = toks[j][1]
                    j += 1
            delete.append((max(0, start - 1), end))

        if not delete:
            return ' ' * out_indent + stripped + nl
        delete.sort()
        merged = []
        for a, b in delete:
            if merged and a <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], b))
            else:
                merged.append((a, b))
        pieces, prev = [], 0
        for a, b in merged:
            pieces.append(stripped[prev:a])
            prev = b
        pieces.append(stripped[prev:])
        return ' ' * out_indent + ''.join(pieces) + nl

    def feed(self, line):
        if self.in_header:
            return self._header(line)
        return self._body(line)

    def finish(self):
        # 头没被删掉（例如输入不是合法的 sema 转储）⇒ 报出来，别静默
        if self.in_header and self.saw_header:
            raise ValueError('revert: RuntimeLib 头括号不配平')
        return ''


def _annotation_start(toks):
    r"""返回本行**第一个要删的记号**在 toks 里的下标；None = 本行没有要删的。

    toks 是 `[()]|[^\s()]+` 的分词结果，所以 `toks[0]` 恒为 `(`，
    `toks[1]` 是节点头自带的记号（节点名 / 运算符）。
    节点头的形状与 AstSexpLayout.h 的打印代码一一对应：

        (IntLit <原文> :T          → 删下标 3
        (<op> :T                   → 删下标 2
        (Call <名字> :T            → 删下标 3
        (LVal <名字> :obj T :T     → 删下标 3（`:obj T` 与值类型记号一起删）
        (VarDef|Param <名字> :t T  → 删下标 3
        (Decl ... / (FuncDef ...   → 不删（S02 原有的类型记号）
        其余结构/语句节点           → 不删
    """
    if len(toks) < 2 or toks[0][2] != '(':
        return None
    sym = toks[1][2]
    if sym in PLAIN or sym in ('Decl', 'FuncDef'):
        return None
    if sym in OPS:
        return 2
    if sym in ('IntLit', 'FloatLit', 'Call', 'LVal', 'VarDef', 'Param'):
        return 3
    return None


# ============================================================================
# 四、类型字符串工具（转储里的拼写：int / float / void / int[2] / float[2][3] / int[]）
# ============================================================================
_TYPE_RE = re.compile(r'^(int|float|void)((?:\[[0-9]*\])*)$')


def parse_type(s):
    """→ (base, dims)；dims 的元素是 int 或 None（`[]` 未知）。非法返回 None。"""
    m = _TYPE_RE.match(s or '')
    if not m:
        return None
    base = m.group(1)
    dims = []
    for part in re.findall(r'\[([0-9]*)\]', m.group(2)):
        dims.append(None if part == '' else int(part))
    return base, dims


def ty_rank(t):
    return len(t[1])


def ty_elem(t):
    return t[0]


def ty_drop(t, k):
    return (t[0], t[1][k:])


def ty_text(t):
    if t is None:
        return '?'
    return t[0] + ''.join('[]' if d is None else '[%d]' % d for d in t[1])


def ty_scalar(t):
    return t is not None and len(t[1]) == 0 and t[0] != 'void'


