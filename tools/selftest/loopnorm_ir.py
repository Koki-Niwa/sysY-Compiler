#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
loopnorm_ir.py —— 结构化 IR 转储（`--emit=structured-ir`）的**极简解析器**

用途：S05b 的三个独立工具共用同一份"读 dump"的实现：
  * `check_loopnorm.py`            —— 轨 B/C/F（不变式 + 覆盖率 + 零改动 + alloca 位置）
  * `independent_loopnorm_check.py` —— 轨 D（**独立实现的第二份规范化器**）
  * `loopnorm_exec.py`             —— 轨 E（受限执行器）

★ 为什么"独立实现"可以共用这一个文件：解析器只回答"文本长什么样"，
  **不含任何规范化语义**。轨 D 的独立性体现在**规范化规则**的实现上
  （它按 prompt §三 自己推导，不读 `LoopNormalize.cpp`）。

解析模型（与 C++ 侧 `StructuredReader.cpp` 同一份规格）
------------------------------------------------------
* 一行一个 Op：`(Kind [名字] [结果...] [属性...] [操作数...] @line N)`，
  行尾可能带 `{`（第一个子 Region 从这里开始）。
* 缩进 2 空格/层；`{` 独占一行表示**该 Op 的下一个**子 Region；`}` 收尾。
* **结果名的位置按 OpKind 固定**：`Call`/`GetGlobal` 的名字在结果之前，
  `Module`/`Func`/`GlobalVar` 的名字在结果之后；`For` 的 IV 名字在结果之后、
  操作数之前（S05b 定稿）。
* 操作数一律是 `%name`；**常量也是 Op、也有名字**（不内联）。

退出码约定见各调用方。本文件**不打印任何东西**（纯库）。
"""

import re

# ── 冻结的指令集（S05 的 `OPKINDS`，逐条照抄；多一个就是违规）─────────────
OPKINDS = {
    'Module', 'GlobalVar', 'Func', 'GetArg', 'Return', 'Call',
    'For', 'While', 'If', 'Goto', 'Yield', 'Break',
    'Alloca', 'Load', 'Store', 'GetElementPtr', 'GetGlobal', 'Bitcast',
    'AddI', 'SubI', 'MulI', 'DivI', 'ModI', 'MinusI',
    'AddF', 'SubF', 'MulF', 'DivF', 'MinusF',
    'Eq', 'Ne', 'Lt', 'Le', 'Gt', 'Ge',
    'I2F', 'F2I', 'Sext',
    'Int', 'Float',
    'Select', 'Phi', 'Unreachable',
}
FORBIDDEN = {'AndI', 'OrI', 'XorI', 'LShift', 'RShift', 'Shl', 'LShr', 'AShr'}
TERMINATORS = {'Yield', 'Break', 'Return', 'Goto', 'Unreachable'}
CF_CONTAINERS = {'If', 'While', 'For'}
# ⚠️ 计时函数的名字是 `_sysy` + `starttime`（**没有**中间的下划线）——
#    IRGen 的发射规则是 `"_sysy" + callee`（见 IRGen.cpp 的 `genCall`）。
#    写成 `_sysy_starttime` 会让解析器把它的**行号实参**当成"结果名"，
#    于是后面所有结果编号整体错位 1（实测：`03_sort1.sy` 差 1 个号）。
VOID_CALLS = {'llvm.memcpy', 'llvm.memset', '_sysystarttime', '_sysystoptime',
              '_sysy_starttime', '_sysy_stoptime'}
RUNTIME_VOID = {'putint', 'putch', 'putfloat', 'putarray', 'putfarray'}
RUNTIME_SIG = {
    'getint': ('i32', 0), 'getch': ('i32', 0), 'getarray': ('i32', 1),
    'getfloat': ('f32', 0), 'getfarray': ('i32', 0), 'putint': ('void', 1),
    'putch': ('void', 1), 'putarray': ('void', 2), 'putfloat': ('void', 1),
    'putfarray': ('void', 2), 'starttime': ('void', 0), 'stoptime': ('void', 0),
}


class ParseError(Exception):
    pass


class Op(object):
    __slots__ = ('kind', 'results', 'attrs', 'operands', 'regions', 'line', 'name',
                 'parent', 'owner_region')

    def __init__(self, kind, line):
        self.kind = kind
        self.results = []     # ['%f.0', ...]
        self.attrs = []       # 记号（不解释语义）
        self.operands = []    # ['%f.0', ...]
        self.regions = []     # [[Op, ...], ...]
        self.line = line
        self.name = ''        # Call/GetGlobal/Module/Func/GlobalVar/For 的名字
        self.parent = None    # 拥有它的 Op（模块级为 None）
        self.owner_region = None

    def __repr__(self):
        return '<%s@%d>' % (self.kind, self.line)


def tokenize(text):
    """把一行括号内的内容切成记号（字符串/类型保持成一个记号）。"""
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in ' \t':
            i += 1
            continue
        if c in '(){}[]=,':
            toks.append(c)
            i += 1
            continue
        if c == '"':
            j = i + 1
            buf = []
            while j < n:
                if text[j] == '\\' and j + 1 < n:
                    buf.append(text[j + 1])
                    j += 2
                    continue
                if text[j] == '"':
                    break
                buf.append(text[j])
                j += 1
            toks.append('"' + ''.join(buf))
            i = j + 1
            continue
        j = i
        while j < n and text[j] not in ' \t(){}[]=,':
            j += 1
        toks.append(text[i:j])
        i = j        # ★ 必须推进；漏掉这一行会让 tokenize 死循环（实测踩过）
    return toks


def merge_type_tokens(toks):
    """把 `[` … `]` 之间的记号合并成**一个**类型记号。

    ★ 为什么需要：`ptr[[3 x i32]]` / `[2 x [3 x i32]]` 里的括号会被 `tokenize`
      拆成 `ptr [ [ 3 x i32 ] ]`，于是"`GetElementPtr` 的第 3 个属性是 0/1"
      这类**按下标取属性**的判据全部错位（实测：`check_loopnorm.py` 的 I6
      检查报了 316 个假红，把全体带数组的文件都判成违规）。
      C++ 侧的读取器有真正的类型解析器，所以没有这个问题。
    """
    out = []
    i = 0
    while i < len(toks):
        if toks[i] == '[':
            depth = 0
            j = i
            buf = []
            while j < len(toks):
                t = toks[j]
                if t == '[':
                    depth += 1
                elif t == ']':
                    depth -= 1
                buf.append(t)
                j += 1
                if depth == 0:
                    break
            out.append(' '.join(buf).replace('[ ', '[').replace(' ]', ']'))
            i = j
            continue
        out.append(toks[i])
        i += 1
    # 再把 `ptr` + `[i32]` 粘成一个记号（`ptr[i32]` / `ptr[[3 x i32]]`）
    merged = []
    i = 0
    while i < len(out):
        if (i + 1 < len(out) and out[i + 1].startswith('[') and
                re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', out[i] or '')):
            merged.append(out[i] + out[i + 1])
            i += 2
            continue
        merged.append(out[i])
        i += 1
    return merged


def n_regions_of(op):
    """按规格应当有几个子 Region。"""
    if op.kind in ('Module', 'Func', 'For'):
        return 1
    if op.kind in ('While', 'If'):
        return 2
    return 0


def expected_results(kind, callee, fnret):
    """每个 OpKind 的结果个数（与 C++ 侧独立写一遍；这是刻意的重复）。"""
    if kind in ('Store', 'Return', 'Goto', 'Yield', 'Break', 'Unreachable',
                'Func', 'Module', 'GlobalVar', 'If', 'While', 'For'):
        return 0
    if kind == 'Call':
        if callee in fnret:
            return 0 if fnret[callee] == 'void' else 1
        if callee in VOID_CALLS or callee in RUNTIME_VOID:
            return 0
        if callee.startswith('_sysy'):   # 计时函数（`_sysy<name>` 前缀规则）
            return 0
        if callee in RUNTIME_SIG:
            return 0 if RUNTIME_SIG[callee][0] == 'void' else 1
        return 1
    return 1


def parse_dump(text):
    """【前置】text 是 `--emit=structured-ir` 的产物（可能畸形）。
    【后置】返回 (module_op, names, fnret)。畸形输入抛 ParseError。
    """
    lines = text.split('\n')
    root = []
    stack = [(root, None)]
    names = {}
    fnret = {}
    active = None
    for ln_no, raw in enumerate(lines, 1):
        s = raw.strip()
        if not s:
            continue
        if s == '}':
            if len(stack) <= 1:
                raise ParseError('line %d: 多余的 `}`' % ln_no)
            owner = stack[-1][1]
            stack.pop()
            active = owner
            continue
        if s == '{':
            owner = active if (active is not None and
                               len(active.regions) < n_regions_of(active)) else stack[-1][1]
            if owner is None or len(owner.regions) >= n_regions_of(owner):
                raise ParseError('line %d: 孤立的 `{`' % ln_no)
            reg = []
            owner.regions.append(reg)
            stack.append((reg, owner))
            continue
        if not s.startswith('('):
            raise ParseError('line %d: 不是 Op 行：%s' % (ln_no, s[:60]))
        open_brace = s.endswith('{')
        core = s[:-1].rstrip() if open_brace else s
        if not core.endswith(')'):
            raise ParseError('line %d: 缺少 `)`：%s' % (ln_no, s[:60]))
        toks = merge_type_tokens(tokenize(core[1:-1]))
        if not toks:
            raise ParseError('line %d: 空 Op' % ln_no)
        kind = toks[0]
        op = Op(kind, ln_no)
        op.parent = stack[-1][1]
        op.owner_region = stack[-1][0]
        i = 1
        if kind in ('Call', 'GetGlobal'):
            if i < len(toks) and toks[i].startswith('"'):
                op.name = toks[i][1:]
                i += 1
        if kind == 'Func':
            for idx in range(1, len(toks)):
                if toks[idx] == ':ret' and idx + 1 < len(toks):
                    if len(toks) > 1 and toks[1].startswith('"'):
                        op.name = toks[1][1:]
                    fnret[op.name] = toks[idx + 1]
        nres = expected_results(kind, op.name, fnret)
        for _ in range(nres):
            if i < len(toks) and toks[i].startswith('%'):
                op.results.append(toks[i])
                i += 1
        rest = toks[i:]
        op.attrs = rest[:]
        for t in rest:
            if t.startswith('"') and op.name == '' and kind in (
                    'Call', 'GetGlobal', 'Module', 'Func', 'GlobalVar', 'For'):
                # 字符串记号形如 `"main`（左引号在，右引号在 tokenize 时被吃掉）
                op.name = t[1:]
        for t in rest:
            if t.startswith('%'):
                op.operands.append(t)
        stack[-1][0].append(op)
        for rname in op.results:
            names[rname] = op
        if open_brace:
            reg = []
            op.regions.append(reg)
            stack.append((reg, op))
        if n_regions_of(op) > len(op.regions):
            active = op
    if len(stack) != 1:
        raise ParseError('Region 没有闭合（还有 %d 层）' % (len(stack) - 1))
    if len(root) != 1 or root[0].kind != 'Module':
        raise ParseError('根不是唯一的 ModuleOp')
    return root[0], names, fnret


# ── 便捷访问器 ────────────────────────────────────────────────────────────
def module_region(mod):
    return mod.regions[0] if mod.regions else []


def funcs(mod):
    return [op for op in module_region(mod) if op.kind == 'Func']


def walk(ops, visit, depth=0):
    """先序访问（Python 递归版；用例集规模小，够用）。"""
    for op in ops:
        visit(op, depth)
        for reg in op.regions:
            walk(reg, visit, depth + 1)


def find_all(ops, kind):
    out = []

    def v(op, _d):
        if op.kind == kind:
            out.append(op)
    walk(ops, v)
    return out


def indent_of_line(lines, idx):
    raw = lines[idx]
    return len(raw) - len(raw.lstrip())


def group_dump(text):
    """把 dump 按"函数"切块：返回 [(函数名, 文本)]，模块级部分归入 ('<module>', 文本)。

    用途：轨 C 的"不触发的用例逐字节零改动"要**按函数**比较，
    这样能定位到是哪个函数的哪个循环没规范化。
    """
    lines = text.split('\n')
    out = []
    cur_name = '<module>'
    cur = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        st = ln.strip()
        if st.startswith('(Func '):
            if cur:
                out.append((cur_name, '\n'.join(cur)))
            cur_name = tokenize(st[1:st.rfind(')')])[1][1:] if '"' in st else '?'
            cur = [ln]
            i += 1
            continue
        cur.append(ln)
        i += 1
    if cur:
        out.append((cur_name, '\n'.join(cur)))
    return out
