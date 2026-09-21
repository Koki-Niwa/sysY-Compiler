#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_structured.py —— S05 的**独立检查器**（轨 A / B / C）。

用法（**命令行为固定契约**）：
    python3 check_structured.py --compiler <路径> [--jobs N] [--dir 语料目录]
                               [--action-limit N] [--verbose] [--probe]

它**只读编译器产出的文本**，不 import 任何 C++ 侧的东西（零第三方依赖）。
为什么必须独立：S04 的 `--probe` 抓的是"检查器空转"，而**同一个 bug 会同时
改坏生成与自检** —— 只有"从文本出发的第二份实现"才抓得住"理解错了"。

三条轨
------
* **轨 A**：`--emit=structured-ir` → `--from-structured --emit=structured-ir`
  → **逐字节相同**（490 个范围内文件）。它抓"打印器与读取器不互逆"。
* **轨 B**：读 dump 文本，检查六条结构性质：
    1. **I4** 每个 Region 恰好一个终结 Op、且在最后一行（模块 Region 无终结符）
    2. **I5** 控制流容器只有 `IfOp`/`WhileOp`/`ForOp`；模块 Region 只放
       `GlobalVar`/`GetGlobal`/`Func`，且全局都在函数之前
    3. **use-def 一致**：每个 `%name` 都能在**同一函数内**找到定义
    4. **指令集封闭**：用到的 OpKind 全部在冻结的集合里（多一个即违规）
    5. **终结符类型匹配**：`ReturnOp` 的操作数个数 == 函数返回类型（void ⇒ 0）
    6. **I6**：每个 `GetElementPtrOp` 都有类型/下标类型/亲和性标记
* **轨 C**：覆盖性（每个文件产出非空 IR、至少一个 Func）+ **Op 数 / AST 数
  比值分布**（比值为 0 或异常小 ⇒ 静默丢内容，S02 的教训）+ 铁律 1 形状抽查
  （`ReturnOp` 的操作数不是直接的 `IntOp`/`FloatOp`，除非源码真的写 `return 0;`）。

`--probe`：**反证**。把转储人为改坏（5 处），检查器必须逐条报红。
没有它就无法区分"检查器真的在工作"与"检查器空转"。

退出码：0 = 全过；1 = 有失败；2 = 工具自身错误。
"""

import argparse
import math
import os
import random
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

# ── 冻结的指令集（prompt §4.1 / 设计文档 §1.2）──────────────────────────────
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
# 明确**不许出现**的（SysY 无移位/位运算；实测 0 次）
FORBIDDEN = {'AndI', 'OrI', 'XorI', 'LShift', 'RShift', 'Shl', 'LShr', 'AShr'}
TERMINATORS = {'Yield', 'Break', 'Return', 'Goto', 'Unreachable'}
CF_CONTAINERS = {'If', 'While', 'For'}
VOID_CALLS = {'llvm.memcpy', 'llvm.memset',
              '_sysy_starttime', '_sysy_stoptime'}
RUNTIME_VOID = {'putint', 'putch', 'putfloat', 'putarray', 'putfarray'}
RUNTIME_SIG = {
    'getint': ('i32', 0), 'getch': ('i32', 0), 'getarray': ('i32', 1),
    'getfloat': ('f32', 0), 'getfarray': ('i32', 1), 'putint': ('void', 1),
    'putch': ('void', 1), 'putarray': ('void', 2), 'putfloat': ('void', 1),
    'putfarray': ('void', 2), 'starttime': ('void', 0), 'stoptime': ('void', 0),
}

LINE_RE = re.compile(r'^(\s*)\((.*)\)\s*\{\s*$|^(\s*)\((.*)\)\s*$')
RESULT_RE = re.compile(r'%([A-Za-z_][A-Za-z0-9_]*|)\.(\d+)')
NAME_AT_START = re.compile(r'^%([^\s()]+)')

# ── 每个 OpKind 的"结果个数"（与 C++ 侧独立写一遍；这是刻意的重复）─────────
def expected_results(kind: str, callee: str, fnret: dict) -> int:
    if kind in ('Store', 'Return', 'Goto', 'Yield', 'Break', 'Unreachable',
                'Func', 'Module', 'GlobalVar', 'If', 'While', 'For'):
        return 0
    if kind == 'Call':
        if callee in fnret:
            return 0 if fnret[callee] == 'void' else 1
        if callee in VOID_CALLS or callee in RUNTIME_VOID:
            return 0
        if callee in RUNTIME_SIG:
            return 0 if RUNTIME_SIG[callee][0] == 'void' else 1
        return 1
    return 1


def is_void_type(t: str) -> bool:
    return t == 'void'


class ParseError(Exception):
    pass


# ============================================================================
# 极简 S-表达式解析（只认我们自己的 dump 形态；**不实现通用 sexp**）
# ============================================================================
class Op:
    __slots__ = ('kind', 'results', 'attrs', 'operands', 'regions', 'line', 'name')

    def __init__(self, kind, line):
        self.kind = kind
        self.results = []      # [name, ...]（`%f.0`）
        self.attrs = []        # 记号列表（不解释语义）
        self.operands = []     # [name, ...]
        self.regions = []      # [[Op, ...], ...]
        self.line = line
        self.name = ''         # Call/GetGlobal/Module/Func/GlobalVar 的名字


def tokenize(text: str):
    """把一行括号内的内容切成记号（字符串保持完整）。"""
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
        i = j
    return toks


def parse_dump(text: str):
    """【前置】text 是 `--emit=structured-ir` 的产物（可能畸形）。
    【后置】返回 (root_ops, name->Op, fnret)。畸形输入抛 ParseError。

    ── 解析模型：一个"打开着的 Region"栈（**无状态机**）─────────────────────
      栈元素 = `(region, owner)`；`owner` 是**拥有这个 Region 的 Op**
      （模块 Region 的 owner 为 None）。
        * 一行 Op ⇒ 加进 `stack[-1].region`
        * 行尾的 `{` ⇒ 这是"刚解析的那个 Op"的第一个子 Region ⇒ 压栈
        * 单独一行的 `{` ⇒ 这是**当前 owner 的下一个**子 Region ⇒ 压栈
        * `}` ⇒ 弹栈（回到 owner 那一层）
      ⚠️ 关键是"单独一行的 `{`"要挂到 **owner** 上，而不是"最近那个 Op" ——
      第一版把它挂到"最近那个 Op"（一个已被 `}` 关掉的 Op），于是 `IfOp` 的
      第二个 Region 挂错、随后报"孤立的 `{`"（实测）。
    """
    lines = text.split('\n')
    root = []
    stack = [(root, None)]
    names = {}
    fnret = {}
    # `active` = "还可能接收下一个 Region 的那个 Op"。
    #   单独一行的 `{` 要挂到**它**上，而不是"栈顶的 owner" ——
    #   多 Region 的 Op（`IfOp`）在它的第一个 Region 关掉之后，栈顶的 owner
    #   已经是 `Func` 了，而那个 `{` 仍然属于 `IfOp`（实测踩过）。
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
            # 关掉一层之后，**刚关掉的那个 Region 的 owner** 可能还要接下一个
            #   Region（`IfOp` 的 then 关掉之后就是 else）—— 所以 active 记的是
            #   "刚关掉的那个 Region 属于谁"，**不是**栈顶的 owner（实测踩过）。
            active = owner
            continue
        if s == '{':
            owner = active if (active is not None and
                               len(active.regions) < n_regions_of(active)) else stack[-1][1]
            if owner is None or len(owner.regions) >= n_regions_of(owner):
                raise ParseError('line %d: 孤立的 `{`（没有等待 Region 的 Op）' % ln_no)
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
        toks = tokenize(core[1:-1])
        if not toks:
            raise ParseError('line %d: 空 Op' % ln_no)
        kind = toks[0]
        op = Op(kind, ln_no)
        i = 1
        # ★ 结果名的位置**按 OpKind 固定**（与 C++ 侧同一份规格）：
        #   `Call`/`GetGlobal` 的名字在结果**之前**；其余 Op 的结果紧跟 OpKind。
        #   `Func`/`Module`/`GlobalVar` 的名字在结果**之后**（它们没有结果）。
        #   少了这条判据，`(GetGlobal "g" %.0 :type ...)` 的 `%.0` 会被当成
        #   结果名（而它其实是操作数），`Func` 签名也就登记不上（实测）。
        if kind in ('Call', 'GetGlobal'):
            if i < len(toks) and toks[i].startswith('"'):
                op.name = toks[i][1:]
                i += 1
        # 登记函数签名（`Func` 只带 0 结果 ⇒ 这里读签名，结果个数不受影响）
        if kind == 'Func':
            for idx in range(1, len(toks)):
                if toks[idx] == ':ret' and idx + 1 < len(toks):
                    op.name = toks[1][1:] if len(toks) > 1 and toks[1].startswith('"') else op.name
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
                    'Call', 'GetGlobal', 'Module', 'Func', 'GlobalVar'):
                op.name = t[1:]
        for t in rest:
            if t.startswith('%'):
                op.operands.append(t)
        stack[-1][0].append(op)
        for rname in op.results:
            names[rname] = op
        active = None
        if open_brace:
            reg = []
            op.regions.append(reg)
            stack.append((reg, op))
        if n_regions_of(op) > len(op.regions):
            active = op   # 它的下一个 Region 由"单独一行的 `{`"开
    if len(stack) != 1:
        raise ParseError('Region 没有闭合（还有 %d 层）' % (len(stack) - 1))
    return root, names, fnret


def n_regions_of(op) -> int:
    """【后置】这个 Op 按规格应当有几个子 Region（用于"单独一行的 `{`"归属判定）。"""
    if op.kind in ('Module', 'Func', 'For'):
        return 1
    if op.kind in ('While', 'If'):
        return 2
    return 0


def new_names(func: str, n: int):
    return ['%%%s.%d' % (func, i) for i in range(n)]


# ============================================================================
# 轨 B：结构检查
# ============================================================================
def check_structure(root, names, fnret):
    """【后置】返回违反列表（每条是字符串）。"""
    bad = []
    kinds_used = set()

    # ④ 指令集封闭 + 明确禁止的 Op
    def walk_ops(ops):
        for op in ops:
            kinds_used.add(op.kind)
            if op.kind in FORBIDDEN:
                bad.append('line %d: 出现了禁止的 Op `%s`（SysY 无移位/位运算）'
                           % (op.line, op.kind))
            elif op.kind not in OPKINDS:
                bad.append('line %d: OpKind `%s` 不在冻结的指令集里' % (op.line, op.kind))
            for reg in op.regions:
                walk_ops(reg)

    walk_ops(root)

    # 模块 Region 的结构（I5 + 全局在函数之前）
    #   ⚠️ `root` 里是**顶层 Op**（只有一个 `Module`）；真正的模块内容在
    #      `root[0].regions[0]` 里。第一版把 `root` 当成模块 Region，
    #      于是 `Module` 自己被判成"模块 Region 里出现 Module"（实测）。
    saw_func = False
    mod_region = root[0].regions[0] if (len(root) == 1 and root[0].regions) else root
    for op in mod_region:
        if op.kind in ('GlobalVar', 'GetGlobal'):
            if saw_func:
                bad.append('line %d: 全局相关的 Op 出现在 Func 之后' % op.line)
        elif op.kind == 'Func':
            saw_func = True
        else:
            bad.append('line %d: 模块 Region 里出现 `%s`' % (op.line, op.kind))

    # I4 / I5：每个 Region 恰好一个终结 Op 且在最后一行
    def check_region(reg, owner_kind, role, owner_line):
        terms = [i for i, op in enumerate(reg) if op.kind in TERMINATORS]
        if owner_kind != 'Module':
            if len(terms) == 0:
                bad.append('line %d: %s 没有终结 Op' % (owner_line, role))
            elif len(terms) > 1:
                bad.append('line %d: %s 有 %d 个终结 Op（必须恰好 1 个）'
                           % (owner_line, role, len(terms)))
            if terms and terms[-1] != len(reg) - 1:
                bad.append('line %d: %s 的终结 Op 不在最后一行' % (owner_line, role))
        for op in reg:
            # I5：**只有** If/While/For 是"控制流容器"；`Module`/`Func` 是
            #   顶层结构（各带 1 个 Region），不算控制流容器 —— 别把它们误判。
            if op.regions and op.kind not in CF_CONTAINERS and op.kind not in ('Module', 'Func'):
                bad.append('line %d: 非控制流容器 `%s` 带了子 Region' % (op.line, op.kind))
            if op.kind == 'While' and len(op.regions) != 2:
                bad.append('line %d: WhileOp 必须有 2 个 Region' % op.line)
            if op.kind == 'If' and len(op.regions) != 2:
                bad.append('line %d: IfOp 必须有 2 个 Region' % op.line)
            if op.kind in ('While', 'For', 'If') and op.results:
                bad.append('line %d: 控制流容器 `%s` 不该有结果' % (op.line, op.kind))
            for i, sub in enumerate(op.regions):
                check_region(sub, op.kind, '%s 的第 %d 个 Region' % (op.kind, i), op.line)

    for op in root:
        for i, sub in enumerate(op.regions):
            check_region(sub, op.kind, '%s 的第 %d 个 Region' % (op.kind, i), op.line)

    # ③ use-def：每个 `%name` 必须在同一函数内定义
    def check_usedef(ops, fn):
        for op in ops:
            for nm in op.operands:
                if nm not in names:
                    bad.append('line %d: 操作数 `%s` 没有定义' % (op.line, nm))
                elif not (nm.startswith('%' + fn + '.') or nm.startswith('%.')):
                    # `%.N` 是**模块级结果**（`GetGlobalOp` 产生的全局引用形式），
                    # 任何函数都可以引用它 —— 不算跨函数。
                    bad.append('line %d: 操作数 `%s` 跨函数引用（当前函数 %s）'
                               % (op.line, nm, fn))
            for reg in op.regions:
                check_usedef(reg, fn)

    # ⚠️ 遍历的是**模块 Region**（`mod_region`），不是 `root` —— `root` 里只有
    #    一个 `Module`，它的内容全在 `regions[0]` 里。第一版漏了这一点，于是
    #    use-def 与终结符检查**从来没被执行过**（探针"没报红"暴露了它）。
    for fop in mod_region:
        if fop.kind == 'Func':
            for reg in fop.regions:
                check_usedef(reg, fop.name)

    # ⑤ 终结符类型匹配 + I6（GEP 标记）
    def check_returns(ops, ret):
        for op in ops:
            if op.kind == 'Return':
                want = 0 if is_void_type(ret) else 1
                if len(op.operands) != want:
                    bad.append('line %d: ReturnOp 有 %d 个操作数（返回类型 %s 要求 %d）'
                               % (op.line, len(op.operands), ret, want))
            if op.kind == 'GetElementPtr':
                types = [t for t in op.attrs if t in ('i32', 'i64', 'f32')
                         or t.startswith('ptr[') or t.startswith('[')]
                if len(types) < 2:
                    bad.append('line %d: GetElementPtrOp 缺少类型标记' % op.line)
                #   ⚠️ 亲和性标记是"第二个类型属性**之后**的第一个整数" ——
                #      不能笼统取"最后一个整数"：`[6 x i32]` 里的 `6` 也是整数
                #      （实测把它当成了亲和性标记，报"实际 6"）。
                aff = None
                seen_type = 0
                ti = 0
                while ti < len(op.attrs):
                    t = op.attrs[ti]
                    if t == 'ptr' or t == '[' or t in ('i32', 'i64', 'f32'):
                        # 跳过一整段类型（`ptr[...]` 或 `[N x T]`）
                        depth = 0
                        if t == 'ptr' and ti + 1 < len(op.attrs) and op.attrs[ti + 1] == '[':
                            ti += 2
                            depth = 1
                            while ti < len(op.attrs) and depth:
                                if op.attrs[ti] == '[':
                                    depth += 1
                                elif op.attrs[ti] == ']':
                                    depth -= 1
                                ti += 1
                        elif t == '[':
                            depth = 1
                            ti += 1
                            while ti < len(op.attrs) and depth:
                                if op.attrs[ti] == '[':
                                    depth += 1
                                elif op.attrs[ti] == ']':
                                    depth -= 1
                                ti += 1
                        else:
                            ti += 1
                        seen_type += 1
                        if seen_type == 2 and ti < len(op.attrs) and \
                                re.fullmatch(r'-?\d+', op.attrs[ti]):
                            aff = op.attrs[ti]
                        continue
                    ti += 1
                if aff is None:
                    bad.append('line %d: GetElementPtrOp 缺少亲和性标记（0/1）' % op.line)
                elif aff not in ('0', '1'):
                    bad.append('line %d: 亲和性标记只能是 0/1（实际 %s）' % (op.line, aff))
            for reg in op.regions:
                check_returns(reg, ret)

    for fop in mod_region:
        if fop.kind == 'Func':
            rty = fnret.get(fop.name, 'i32')
            for reg in fop.regions:
                check_returns(reg, rty)

    # Call 的结果个数：由被调函数决定
    def check_calls(ops):
        for op in ops:
            if op.kind == 'Call':
                want = expected_results('Call', op.name, fnret)
                if len(op.results) != want:
                    bad.append('line %d: Call `%s` 有 %d 个结果（应为 %d）'
                               % (op.line, op.name, len(op.results), want))
            for reg in op.regions:
                check_calls(reg)

    check_calls(root)
    return bad, kinds_used


# ============================================================================
# 轨 C：覆盖性 / 规模 / 铁律 1 形状
# ============================================================================
def count_ops(root):
    n = 0
    for op in root:
        n += 1
        for reg in op.regions:
            n += count_ops(reg)
    return n


def count_kind(ops, kind):
    n = 0
    for op in ops:
        if op.kind == kind:
            n += 1
        for reg in op.regions:
            n += count_kind(reg, kind)
    return n


def count_ast_nodes(sema_text: str) -> int:
    """`--emit=sema` 的节点行数（每行一个节点）。"""
    return sum(1 for ln in sema_text.split('\n') if ln.strip().startswith('('))


def check_track_c_one(job):
    """一个文件：产出非空 IR、至少一个 Func、Op/AST 比值、铁律 1 形状。"""
    compiler, path, sy = job
    ir_path = sy + '.sir'
    sema_path = sy + '.sema'
    r1 = subprocess.run([compiler, path, '--emit=structured-ir', '-o', ir_path],
                        capture_output=True)
    r2 = subprocess.run([compiler, path, '--emit=sema', '-o', sema_path],
                        capture_output=True)
    if r1.returncode != 0:
        return (path, None, 'rc=%d %s' % (r1.returncode, r1.stderr.decode()[:150]))
    try:
        text = open(ir_path, encoding='utf-8', errors='replace').read()
        root, names, fnret = parse_dump(text)
    except ParseError as e:
        return (path, None, '解析失败：%s' % e)
    except OSError as e:
        return (path, None, '读文件失败：%s' % e)
    nop = count_ops(root)
    nfunc = count_kind(root, 'Func')   # `Func` 只在模块 Region 出现，递归等价
    nast = count_ast_nodes(open(sema_path, encoding='utf-8', errors='replace').read()) \
        if r2.returncode == 0 else 1
    ratio = (nop / nast) if nast else float('inf')
    # 铁律 1 形状：ReturnOp 的操作数不是直接的 Int/Float **定义**（除非源码真写 return 0;）
    direct_const = []
    def walk(ops):
        for op in ops:
            if op.kind == 'Return' and op.operands:
                owner = names.get(op.operands[0])
                if owner is not None and owner.kind in ('Int', 'Float'):
                    direct_const.append(op.line)
            for reg in op.regions:
                walk(reg)
    walk(root)
    return (path, dict(nop=nop, nfunc=nfunc, nast=nast, ratio=ratio,
                       direct_const=direct_const, size=len(text)), None)


# ============================================================================
# 反证（--probe）：把转储人为改坏，检查器必须报红
#
#   ★ 为什么必须有它：**同一个 bug 会同时改坏"生成"与"自检"** —— 检查器与
#     被检查对象一起错，检查就永远绿。`--probe` 是"检查器真的在工作"的唯一证据。
#
#   实现方式：**逐行替换**（不用正则大范围替换）—— 正则很容易"看起来改了、
#   其实匹配到别处"（第一版就踩了这个坑：6 个探针里有 3 个实际没改动到目标行，
#   于是"没报红"被误判成"检查器漏检"）。
# ============================================================================
def probe(compiler, sy, tmpdir):
    """【后置】返回 [(改坏方式, 是否被抓到), ...]。每种改法对应一条不变式。"""
    good = os.path.join(tmpdir, 'probe.sir')
    subprocess.run([compiler, sy, '--emit=structured-ir', '-o', good], capture_output=True)
    base = open(good, encoding='utf-8').read().split('\n')

    def edit(pred, make_new):
        """把**第一行**满足 pred 的行换成 make_new(line)；返回新文本（或 None）。"""
        out = list(base)
        for i, ln in enumerate(out):
            if pred(ln):
                out[i] = make_new(ln)
                return '\n'.join(out)
        return None

    variants = []
    # ① I4：删掉一个终结 Op（Region 就少了终结符）
    def drop_yield(lines):
        out = list(lines)
        for i, ln in enumerate(out):
            if ln.strip().startswith('(Yield'):
                del out[i]
                return '\n'.join(out)
        return None
    variants.append(('删掉一个 Yield（I4：Region 缺终结 Op）', drop_yield(base)))
    # ② I4：在终结 Op 之后塞一条指令（终结符不在最后一行）
    def after_yield(lines):
        out = list(lines)
        for i, ln in enumerate(out):
            if ln.strip().startswith('(Yield'):
                indent = ln[:len(ln) - len(ln.lstrip())]
                out.insert(i + 1, indent + '(Int %bogus.999 1)')
                return '\n'.join(out)
        return None
    variants.append(('终结 Op 之后塞指令（I4：不在最后一行）', after_yield(base)))
    # ③ 指令集封闭：插入一个禁止的 Op
    def insert_forbidden(lines):
        out = list(lines)
        for i, ln in enumerate(out):
            if '(Alloca ' in ln:
                indent = ln[:len(ln) - len(ln.lstrip())]
                out.insert(i, indent + '(AndI %bogus.998 i32 i32 1)')
                return '\n'.join(out)
        return None
    variants.append(('插入 `AndI`（指令集封闭）', insert_forbidden(base)))
    # ④ use-def：把一个操作数改成未定义的名字（挑一个**带 `%` 操作数**的非终结行）
    def bad_operand(lines):
        #   ⚠️ 必须**只替换那个记号本身**，不能"重新拼一行" ——
        #      重新拼会把 `ptr[i32]` 拆成 `ptr [ i32 ]`（tokenize 的记号化），
        #      于是 `ptr` 丢了参数类型、被当成"非函数名"的调用 ⇒ 检查器静默
        #      跳过 Call 检查（实测：探针"没报红"其实是**探针自己写坏了文本**）。
        out = list(lines)
        for i, ln in enumerate(out):
            st = ln.strip()
            if st.startswith('(Store') or st.startswith('(Load'):
                toks = tokenize(st[1:-1])
                for j, t in enumerate(toks):
                    if t.startswith('%') and j > 0:
                        out[i] = ln.replace(t, '%nosuch.12345', 1)
                        return '\n'.join(out)
        return None
    variants.append(('操作数改成未定义名（use-def）', bad_operand(base)))
    # ⑤ I6：删掉 GEP 的亲和性标记
    def drop_affinity(lines):
        out = list(lines)
        for i, ln in enumerate(out):
            if '(GetElementPtr ' in ln and ' i64 0 ' in ln:
                out[i] = ln.replace(' i64 0 ', ' i64 ', 1)
                return '\n'.join(out)
        return None
    variants.append(('删掉 GEP 的亲和性标记（I6）', drop_affinity(base)))
    # ⑥ 终结符类型匹配：给一个 **0 操作数**的 ReturnOp 加一个操作数
    def add_return_operand(lines):
        #   找一个**非 void 函数**里单独一行的 `(Return ...)`，把它变成两个操作数。
        out = list(lines)
        ret_is_void = True
        for i, ln in enumerate(out):
            if '(Func ' in ln and ':ret void' in ln:
                ret_is_void = True
            elif '(Func ' in ln and ':ret ' in ln:
                ret_is_void = False
            elif '(Return' in ln and not ret_is_void:
                out[i] = ln.replace('(Return', '(Return %bogus.997', 1)
                return '\n'.join(out)
        return None
    variants.append(('给 0 操作数的 ReturnOp 加操作数（终结符匹配）', add_return_operand(base)))

    result = []
    for label, text in variants:
        if text is None:
            result.append((label + '（**探针没找到目标行**）', False))
            continue
        try:
            root, names, fnret = parse_dump(text)
            bad, _ = check_structure(root, names, fnret)
        except ParseError as e:
            bad = ['ParseError: %s' % e]
        result.append((label, len(bad) > 0))
    return result


# ============================================================================
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--limit', type=int, default=0, help='只跑前 N 个文件（调试用）')
    ap.add_argument('--action-limit', type=int, default=0, help='（兼容 S04 签名，忽略）')
    ap.add_argument('--probe', action='store_true', help='跑反证（检查器必须抓得住）')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--sample', type=int, default=20, help='铁律 1 形状抽查的文件数')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2

    files = []
    for dirpath, _, names in os.walk(args.dir):
        for n in sorted(names):
            if n.endswith('.sy'):
                p = os.path.join(dirpath, n)
                try:
                    if re.search(r'\btensor\b', open(p, encoding='utf-8', errors='replace').read()):
                        continue
                except OSError:
                    continue
                files.append(p)
    files.sort()
    if args.limit:
        files = files[:args.limit]
    if not files:
        print('没有找到可跑的 .sy', file=sys.stderr)
        return 2

    tmpdir = os.path.join(ROOT, '.work', 'check_structured')
    os.makedirs(tmpdir, exist_ok=True)
    print('语料：%d 个范围内文件（已排除含 `tensor` 的）' % len(files))
    fails = 0

    # ── 轨 A：往返逐字节 ───────────────────────────────────────────────────
    print('\n== 轨 A：dump → 读回 → 再 dump 逐字节相同 ==')
    def rt(job):
        path, a, b = job
        r1 = subprocess.run([compiler, path, '--emit=structured-ir', '-o', a], capture_output=True)
        if r1.returncode != 0:
            return (path, 'emit rc=%d' % r1.returncode)
        r2 = subprocess.run([compiler, a, '--from-structured', '--emit=structured-ir', '-o', b],
                            capture_output=True)
        if r2.returncode != 0:
            return (path, 'read rc=%d %s' % (r2.returncode, r2.stderr.decode()[:120]))
        if open(a, 'rb').read() != open(b, 'rb').read():
            return (path, '往返不相同')
        return (path, None)

    jobs = [(p, os.path.join(tmpdir, 'a%d.sir' % i), os.path.join(tmpdir, 'b%d.sir' % i))
            for i, p in enumerate(files)]
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(rt, jobs))
    rt_bad = [r for r in results if r[1]]
    for p, why in rt_bad[:10]:
        print('  ✘ %s: %s' % (p, why))
    print('  通过 %d / %d' % (len(results) - len(rt_bad), len(results)))
    if rt_bad:
        fails += 1

    # ── 轨 B：结构检查 ─────────────────────────────────────────────────────
    print('\n== 轨 B：六条结构性质 ==')
    all_kinds = set()
    b_bad = 0
    for i, p in enumerate(files):
        ir = os.path.join(tmpdir, 'a%d.sir' % files.index(p) if False else jobs[files.index(p)][1])
        ir = jobs[files.index(p)][1]
        try:
            root, names, fnret = parse_dump(open(ir, encoding='utf-8', errors='replace').read())
        except (ParseError, OSError) as e:
            print('  ✘ %s: 解析失败 %s' % (p, e))
            b_bad += 1
            continue
        bad, kinds = check_structure(root, names, fnret)
        all_kinds |= kinds
        if bad:
            b_bad += 1
            if b_bad <= 5:
                print('  ✘ %s: %d 条违反' % (p, len(bad)))
                for x in bad[:3]:
                    print('      %s' % x)
    print('  通过 %d / %d（解析 + 六条检查）' % (len(files) - b_bad, len(files)))
    print('  用到的 OpKind：%d 个 —— %s' % (len(all_kinds), ' '.join(sorted(all_kinds))))
    extra = all_kinds - OPKINDS
    if extra:
        print('  ✘ 指令集不封闭：多出 %s' % sorted(extra))
        fails += 1
    if b_bad:
        fails += 1

    # ── 轨 C：覆盖性 / 规模 / 铁律 1 ───────────────────────────────────────
    print('\n== 轨 C：覆盖性 + Op 数分布 + 铁律 1 形状抽查 ==')
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        cres = list(ex.map(check_track_c_one,
                           [(compiler, p, os.path.join(tmpdir, 'c%d' % i))
                            for i, p in enumerate(files)]))
    empty, nofunc, ratios, cfail = [], [], [], []
    direct_hits = []
    for path, info, err in cres:
        if err:
            cfail.append((path, err))
            continue
        if info['nop'] == 0:
            empty.append(path)
        if info['nfunc'] == 0:
            nofunc.append(path)
        ratios.append((info['ratio'], info['nop'], info['nast'], path))
        if info['direct_const']:
            direct_hits.append((path, info['direct_const']))
    print('  非空 IR：%d / %d（空：%d）' % (len(files) - len(empty), len(files), len(empty)))
    print('  至少一个 FuncOp：%d / %d（缺：%d）' % (len(files) - len(nofunc), len(files), len(nofunc)))
    if cfail:
        print('  ✘ 失败 %d 个：' % len(cfail))
        for p, e in cfail[:5]:
            print('      %s: %s' % (p, e))
        fails += 1
    if empty or nofunc:
        for p in (empty + nofunc)[:5]:
            print('  ✘ %s' % p)
        fails += 1
    if ratios:
        rs = sorted(r[0] for r in ratios)
        med = statistics.median(rs)
        print('  Op 数 / AST 节点数：min=%.3f median=%.3f max=%.3f（%d 个文件）'
              % (rs[0], med, rs[-1], len(rs)))
        tiny = [r for r in ratios if r[0] < 0.2]
        print('  比值 < 0.2 的文件：%d 个' % len(tiny))
        for r in tiny[:5]:
            print('      %.3f  ops=%d ast=%d  %s' % (r[0], r[1], r[2], r[3]))
        if not ratios:
            fails += 1
    # 铁律 1 形状抽查：**确定性抽样**（排序后等距取），不用随机数
    step = max(1, len(files) // max(1, args.sample))
    sample = files[::step][:args.sample]
    print('  铁律 1 形状抽查 %d 个文件（等距抽样，确定性）：' % len(sample))
    hit = 0
    for path, lines_ in direct_hits:
        if path in sample:
            hit += 1
            print('    ⚠ %s: line %s 的 Return 直接用了常量定义' % (path, lines_[:3]))
    print('    ReturnOp 直接引用常量定义的：%d 处（源码真写 `return 0;` 时才合法；'
          '这类我们**不判失败**，只报出来供人工核对）' % len(direct_hits))

    # ── 反证 ───────────────────────────────────────────────────────────────
    if args.probe:
        print('\n== 反证（--probe）：把转储改坏，检查器必须抓得住 ==')
        sy = os.path.join(ROOT, 'compiler', 'tests', 'structured', 'example', 'example.sy')
        if not os.path.exists(sy):
            sy = files[0]
        pr = probe(compiler, sy, tmpdir)
        for label, caught in pr:
            print('  %s %s' % ('✔' if caught else '✘', label))
            if not caught:
                fails += 1

    # ── 清理临时转储（490 × 2 份 ≈ 300 MB；跑完就删，别留在工作区里）────────
    for _, a, b in jobs:
        for p in (a, b):
            try:
                os.remove(p)
            except OSError:
                pass
    for i in range(len(files)):
        for suf in ('.sir', '.sema'):
            try:
                os.remove(os.path.join(tmpdir, 'c%d%s' % (i, suf)))
            except OSError:
                pass

    print('\n' + '=' * 66)
    print('判定：%s' % ('✔ 轨 A/B/C 全部通过' if fails == 0 else '✘ 有 %d 组失败' % fails))
    return 0 if fails == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
