#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
loopnorm_independent.py —— 轨 D 的**独立规范化引擎**

   与 `independent_loopnorm_check.py`（CLI + 比对 + 差异归因）分开的理由：
   §C4 的新文件 800 行上限。

   ★ 本文件是"第二份规范化器"的**本体**：它按 S05b prompt §三 的规则
     **独立**读 dump、独立产出规范化后的 dump。写它之前**没有读**
     `LoopNormalize.cpp` / `LoopAnalysis.cpp`；`.reference/` 从来没有读过。
     唯一依赖的"约定"是 §3.2 的 `continue` 消解规范形式
     （`if (!cond) { B }`）。
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import loopnorm_ir as L  # noqa: E402

# ============================================================================
# 极简可变 IR（与 dump 一一对应；**不复用 C++ 的任何东西**）
# ============================================================================
class N(object):
    """一个 Op。`regions` 里放 list[N]。"""
    __slots__ = ('kind', 'name', 'results', 'attrs', 'operands', 'regions',
                 'line', 'src', 'has_line', 'raw_core', 'src_line')

    def __init__(self, kind, line):
        self.kind = kind
        self.name = ''
        self.results = []
        self.attrs = []
        self.operands = []
        self.regions = []
        self.line = line
        self.has_line = False   # 原文里有没有 `@line`（C++ dump 对没有位置的 Op 不打印）
        self.raw_core = None    # ★ 原文里 `( … )` 的**逐字内容**（含结尾的 `)`）
        self.src_line = 0       # 源码行（**不是** dump 行；从 `@line N` 属性取）
        self.src = None         # 对应的解析结果（属性文本从它抄）

    @staticmethod
    def from_op(op):
        n = N(op.kind, op.line)
        n.name = op.name
        n.results = list(op.results)
        n.attrs = list(op.attrs)
        n.operands = list(op.operands)
        n.has_line = ('@line' in op.attrs)
        if n.has_line:
            i = op.attrs.index('@line')
            if i + 1 < len(op.attrs):
                try:
                    n.src_line = int(op.attrs[i + 1])
                except ValueError:
                    n.src_line = 0
        else:
            # `@line` 与操作数一起被放进 attrs；解析器把它们都列在 attrs 里
            for i, t in enumerate(op.attrs):
                if t == '@line' and i + 1 < len(op.attrs):
                    try:
                        n.src_line = int(op.attrs[i + 1])
                    except ValueError:
                        pass
        n.src = op
        for reg in op.regions:
            n.regions.append([N.from_op(x) for x in reg])
        return n


# ============================================================================
# 序列化（**与 StructuredDump.cpp 同一份格式**；见 prompt §四）
# ============================================================================
def op_line(op, names):
    if op.raw_core is not None:
        # 来自原文：逐字照抄，只把**结果名/操作数**按新编号替换
        txt = op.raw_core
        # 只替换 `%name` 记号（不碰 `ptr[i32]` 这类类型文本）
        out = []
        i = 0
        n = len(txt)
        while i < n:
            c = txt[i]
            if c == '"':                      # 字符串原样保留
                j = i + 1
                while j < n and txt[j] != '"':
                    j += 2 if txt[j] == '\\' else 1
                out.append(txt[i:j + 1])
                i = j + 1
                continue
            if c == '%':
                j = i + 1
                while j < n and (txt[j].isalnum() or txt[j] in '_.'):
                    j += 1
                k = txt[i:j]
                out.append(names.get(k, k))
                i = j
                continue
            out.append(c)
            i += 1
        return ''.join(out)
    parts = ['(' + op.kind]
    if op.kind in ('Call', 'GetGlobal'):
        parts.append('"%s"' % op.name)
    for r in op.results:
        parts.append(names.get(r, r))
    skip = 0
    if op.kind == 'Call':
        skip = 1
    elif op.kind == 'Module':
        parts.append('"%s"' % op.name)
        skip = len(op.attrs)
    elif op.kind == 'Func':
        parts.append('"%s"' % op.name)
        rest = op.attrs[1:]
        # `:ret T :param [T, ...]`
        body = []
        i = 0
        while i < len(rest):
            body.append(rest[i])
            i += 1
        parts.extend(body)
        skip = len(op.attrs)
    elif op.kind == 'GetGlobal':
        parts.extend(op.attrs[1:])
        skip = len(op.attrs)
    elif op.kind == 'GlobalVar':
        parts.append('"%s"' % op.name)
        parts.extend(op.attrs[1:])
        skip = len(op.attrs)
    elif op.kind == 'For':
        parts.append('"%s"' % op.name)
        skip = 1
    for t in op.attrs[skip:]:
        parts.append(t)
    for o in op.operands:
        parts.append(o)          # 新 Op：`fix()` 已经换成最终名（不再二次映射）
    if op.has_line:
        parts.append('@line')
        parts.append(str(op.src_line))
    return ' '.join(parts) + ')'


def render(mod):
    """【后置】返回整棵树的文本（**与 C++ dump 逐字节可比**）。

    结果名规则（与 `StructuredDump.cpp` 的 `scanNames` 一致）：
    **函数内先序、每个结果一个递增序号**。★ 注意：改写会把条件 Region 的
    Op 搬到 `WhileOp` **之前** ⇒ 它们在最终先序里的位置**前移**了，所以
    **序号必须按最终树重算**（"保留原名"是错的 —— 实测差 7 个号）。
    """
    names = {}          # 原名 → 最终名
    counter = [0]
    cur = ['']

    def assign(ops):
        for op in ops:
            if op.kind == 'Func':
                cur[0] = op.name
                counter[0] = 0
            for k, r in enumerate(op.results):
                nm = '%%%s.%d' % (cur[0], counter[0])
                counter[0] += 1
                # ★ 只记映射，**不就地改 `op.results`**：新 Op 的占位名若改成一个
                #   "真名字"，可能与某个**原 Op 的旧名字**撞车（旧名字在后续
                #   会被映射到别的最终名）⇒ 打印时又被改一次（实测差 2 个号）。
                names[r] = nm
            for reg in op.regions:
                assign(reg)

    assign([mod])

    def fix(ops):
        """★ 只处理**新 Op**（`raw_core is None`）：把占位名/原名换成最终名。
        原 Op 的文本由 `op_line` 从 `raw_core` 逐字照抄 + 替换 `%name`，
        所以**不能**在这里动它的操作数（否则会被映射两次 —— 实测差 2 个号）。
        """
        for op in ops:
            if op.raw_core is None:
                op.operands = [names.get(o, o) if isinstance(o, str) else o for o in op.operands]
                # ★ `ForOp` 的 IV 名字（`attrs[0]`）是**字符串属性**，不是结果
                #   引用 ⇒ **不跟着结果重新编号**（C++ 把 pass 运行时的名字直接
                #   写进字符串；dump 的结果编号与它无关）。第一版把它一起映射了，
                #   于是每个 `For` 的 IV 名字都比 C++ 大 2。
                #   （`op.name` 只在它是新 Op 时用于打印，保持原样即可。）
            for reg in op.regions:
                fix(reg)
    fix([mod])

    out = []

    def emit(ops, depth):
        """缩进规则（与 `StructuredDump.cpp` 的脚本动作一致）：
        * 第一个 Region 的 `{` 跟在 Op 行尾；
        * **其余** Region 的 `{` 独占一行，缩进 = **拥有者**那一层；
        * 每个 Region 的 `}` 也缩进在**拥有者**那一层（不是内容层）。
          ⚠️ 第一版把 `{`/`}` 写成 `depth + 1`，结果每个循环的闭合括号都多缩进
             2 空格（只有 `cmp` 逐字节比才发现）。
        """
        for op in ops:
            head = '  ' * depth + op_line(op, names)
            if op.regions:
                head += ' {'
            out.append(head)
            for k, reg in enumerate(op.regions):
                if k > 0:
                    out.append('  ' * depth + '{')
                emit(reg, depth + 1)
                out.append('  ' * depth + '}')
    emit([mod], 0)
    return '\n'.join(out) + '\n'


# ============================================================================
# 规范化（**按 prompt §三 独立推导的实现**）
# ============================================================================
_PH = [0]


def ph(tag):
    """【后置】返回一个**全局唯一**的占位结果名（前缀 `\x00`）。

    ★ 为什么必须唯一：`render()` 用 `names[占位名] = 最终名` 来把新 Op 接进
      结果编号体系。若两个循环用同一个占位名（例如都叫 `\x00lower`），
      后一个会**覆盖**前一个的映射 ⇒ 两个都显示最后那个名字
      （实测：`f1` 的 `Load lower` 显示成 `%f2.5`）。
    """
    _PH[0] += 1
    return '\x00%s#%d' % (tag, _PH[0])


class Failed(Exception):
    def __init__(self, reason):
        Exception.__init__(self, reason)
        self.reason = reason


def slot_of_load(by_name, tok):
    """`%x` 若是 `Load` 的结果，返回"被读的指针值"；否则 None。"""
    d = by_name.get(tok)
    if d is None or d.kind != 'Load' or len(d.operands) != 1:
        return None
    return d.operands[0]


def is_iv_increment(store, ivslot, by_name):
    """`store` 是不是 `iv = iv + 1`。"""
    if store.kind != 'Store' or len(store.operands) != 2:
        return False
    if store.operands[1] != ivslot:
        return False
    add = by_name.get(store.operands[0])
    if add is None or add.kind != 'AddI' or len(add.operands) != 2:
        return False
    a, b = add.operands
    lo = by_name.get(a)
    hi = by_name.get(b)
    def is_ld(x):
        return x is not None and x.kind == 'Load' and len(x.operands) == 1 and x.operands[0] == ivslot
    def one(x):
        return x is not None and x.kind == 'Int' and x.src is not None and x.src.attrs and \
            x.src.attrs[0] == '1'
    return (is_ld(lo) and one(hi)) or (is_ld(hi) and one(lo))


def index_by_name(ops, out):
    """把 `%name` → Op 收进 `out`（`ops` 可以是 Region 列表或 Op 列表）。"""
    for op in ops:
        if isinstance(op, list):
            index_by_name(op, out)
            continue
        for r in op.results:
            out[r] = op
        for reg in op.regions:
            index_by_name(reg, out)


PURE_KINDS = {
    'Int', 'Float', 'AddI', 'SubI', 'MulI', 'DivI', 'ModI', 'MinusI',
    'AddF', 'SubF', 'MulF', 'DivF', 'MinusF', 'Eq', 'Ne', 'Lt', 'Le', 'Gt', 'Ge',
    'I2F', 'F2I', 'Sext', 'Select', 'Bitcast', 'GetElementPtr', 'Alloca',
    # `GetGlobal` 是**纯定义**（全局对象的引用形式）：`bound = Load(GetGlobal)`
    # 的指针来源就是它。漏掉它会让"边界是全局"的循环全判非仿射
    # （实测：`while (i < N - 1)` 而 `N` 是全局常量）。
    'GetGlobal',
}


def prove_pure(tok, ivslot, written, by_name):
    seen = set()
    st = [tok]
    while st:
        t = st.pop()
        if t is None or t in seen:
            continue
        seen.add(t)
        op = by_name.get(t)
        if op is None:
            return False
        if op.kind == 'Load':
            base = op.operands[0] if op.operands else None
            baseop = by_name.get(base)
            if baseop is not None and baseop.kind == 'Alloca':
                if base == ivslot:
                    return False
                if base in written:
                    return False
            continue
        if op.kind not in PURE_KINDS:
            return False
        st.extend(op.operands)
    return True


def written_slots(ops, by_name):
    out = set()

    def v(o):
        if o.kind == 'Store' and len(o.operands) == 2:
            base = o.operands[1]
            op = by_name.get(base)
            while op is not None and op.kind == 'GetElementPtr':
                base = op.operands[0]
                op = by_name.get(base)
            if op is not None and op.kind == 'Alloca':
                out.add(base)
    L.walk(ops, lambda o, d: v(o))
    return out


def has_call(ops):
    found = [False]

    def v(o, d):
        if o.kind == 'Call':
            found[0] = True
    L.walk(ops, v)
    return found[0]


def bound_reads_local(tok, by_name):
    seen = set()
    st = [tok]
    while st:
        t = st.pop()
        if t is None or t in seen:
            continue
        seen.add(t)
        op = by_name.get(t)
        if op is None:
            return True
        if op.kind == 'Load' and op.operands:
            b = by_name.get(op.operands[0])
            if b is not None and b.kind == 'Alloca':
                return True
        st.extend(op.operands)
    return False


# ── ① continue 消解 ──────────────────────────────────────────────────────
#   ★ 规范形式（prompt §3.2 的字面）：`continue` 归一化为 `if (!cond) { B }`
#     的**嵌套**（B = continue 之后要跳过的一切）。落地：
#       body = [ P1…, (If C) { A…, (Break) } { (Yield) }, REST… ]
#         ↓
#       body = [ P1…, (If C) { A…, (Yield) } { (Yield) },
#                (Int 0), (Eq C 0), (If (Eq C 0)) { (Yield) } { REST… }, (Yield) ]
#     反转条件用 **`Eq C 0`**（不交换比较谓词 —— 那属于算术改写）。
#   ★ 为什么必须"把 REST 包起来"而不是只把 `Break` 换成 `(Yield)`：
#     `YieldOp` 在**分支**里的语义是"本分支结束 ⇒ 回到拥有者的下一条语句"，
#     而不是"跳到循环头"（否则 IRGen 给普通 `if (c) { A }` 追加的 `(Yield)`
#     会变成"跳过 `if` 之后的语句"）。⇒ 要表达"跳过 REST"，REST 必须被搬进
#     一个受 `!C` 保护的分支里。
#     这是一个**语义 bug**：第一版只把 `Break` 换成 `(Yield)`，于是
#     `if (i < j) { j = j + 1; continue; } swap…` 会照样执行 `swap`
#     （`transpose0.sy`）。轨 D 抓不到（两份实现同一个误解），**轨 E 才抓得到**。

def check_continues(region):
    """【后置】只读：这个体的 `continue` 是否**可以**按规范形式消解。"""
    for i, op in enumerate(region):
        if op.kind == 'Break':
            # ★ **体顶层的 `BreakOp` 一律拒绝**：`break` 与"尾部 continue"在
            #   IRGen 里都是 `BreakOp`（形状完全一样）⇒ 不可判定 ⇒ 保守
            #   （prompt §3.3："含 break 的 while 一律不规范化"）。
            return False
        if op.kind in ('While', 'For'):
            # ★ 内层循环体里还有 `BreakOp` ⇒ 说明内层**没被规范化**（真 `break`）
            #   ⇒ 内层升不成 `ForOp`，而外层 `ForOp` 的体内会留着那个 `BreakOp`
            #   ⇒ 违反 I3（"`ForOp` 体内无 break"）⇒ 外层也不许规范化。
            #   （漏掉这条会让**内层失败的循环把外层也带坏** —— 实测
            #     `19_search.sy`：两份实现只有这一条不一致。）
            if len(op.regions) > 1 and has_break_anywhere(op.regions[1]):
                return False
            continue
        if op.kind != 'If':
            continue
        a, b = op.regions[0], op.regions[1]
        ab = bool(a) and a[-1].kind == 'Break'
        bb = bool(b) and b[-1].kind == 'Break'
        if ab and bb:
            return False               # 两条路径都 continue：不处理
        if ab or bb:
            continue                   # 末尾 Break：交给 resolve
        if has_break_anywhere(a) or has_break_anywhere(b):
            return False               # 非末尾 Break：保守拒绝
    return True


def resolve_continues(body, loop_src_line=0):
    """【后置】按规范形式改写体的 `continue`（**只在判定通过后调用**）。
    `loop_src_line` = 该 `WhileOp` 的源码行（补 `(Yield)` 时用它——与 C++ 的
    `loop_->loc` 一致）。
    """
    stats = {'wrapped': 0, 'tail': 0}
    # 体尾的尾部 continue
    if body and body[-1].kind == 'Break':
        y = N('Yield', body[-1].line)
        y.has_line = True
        y.src_line = body[-1].src_line
        body[-1] = y
        stats['tail'] += 1
    while True:
        k = None
        which = 0
        for i in range(len(body) - 1, -1, -1):
            op = body[i]
            if op.kind != 'If':
                continue
            a, b = op.regions[0], op.regions[1]
            ab = bool(a) and a[-1].kind == 'Break'
            bb = bool(b) and b[-1].kind == 'Break'
            if not ab and not bb:
                continue
            k, which = i, (0 if ab else 1)
            break
        if k is None:
            break
        target = body[k]
        br = target.regions[which]
        y = N('Yield', br[-1].line)
        y.has_line = True
        y.src_line = br[-1].src_line
        br[-1] = y
        stats['tail'] += 1
        rest = body[k + 1:]
        if not rest:
            break
        cond = target.operands[0] if target.operands else None
        pz = ph('zero')
        pc = ph('cond')
        zero = N('Int', target.line)
        zero.attrs = ['0']
        zero.results = [pz]
        zero.has_line = True
        zero.src_line = target.src_line
        neg = N('Eq', target.line)
        neg.operands = [cond, pz]
        neg.results = [pc]
        neg.has_line = True
        neg.src_line = target.src_line
        yl = N('Yield', target.line)
        yl.has_line = True
        yl.src_line = target.src_line
        wrapper = N('If', target.line)
        wrapper.operands = [pc]
        # ★ 规范形式 = `if (!C) { REST }` ⇒ **REST 进 then**、else 只留 `(Yield)`。
        #   （条件用的是 `Eq C 0` = `!C`；若把 REST 放进 else 就等价于
        #    `if (C) { REST }`，**语义正好反了** —— 只有轨 E 抓得到。）
        wrapper.regions = [rest, [yl]]
        wrapper.has_line = True
        wrapper.src_line = target.src_line
        resolve_region_tail(rest)
        body[k + 1:] = [zero, neg, wrapper]
        stats['wrapped'] += 1
    # 包装后体的最后一条是 `IfOp` ⇒ 补一个 `(Yield)`（I4）
    if not body or body[-1].kind not in L.TERMINATORS:
        y = N('Yield', body[-1].line if body else 0)
        y.has_line = True
        y.src_line = loop_src_line
        body.append(y)
    return stats


def resolve_region_tail(region):
    """被搬进包装分支的 REST：只做"分支末尾 `Break` → `(Yield)`"这一层。"""
    for op in region:
        if op.kind != 'If':
            continue
        for k in range(len(op.regions)):
            sub = op.regions[k]
            if sub and sub[-1].kind == 'Break':
                y = N('Yield', sub[-1].line)
                y.has_line = True
                y.src_line = sub[-1].src_line
                sub[-1] = y
    if region and region[-1].kind == 'Break':
        y = N('Yield', region[-1].line)
        y.has_line = True
        y.src_line = region[-1].src_line
        region[-1] = y


# ── ④ 每条路径恰好 +1 ────────────────────────────────────────────────────
def direct_increments(region, ivslot, by_name):
    n = 0
    for op in region:
        if op.kind == 'Break':
            continue
        if is_iv_increment(op, ivslot, by_name):
            n += 1
            continue
        if op.kind != 'If':
            continue
        a, b = op.regions[0], op.regions[1]
        if not a or not b:
            return -1
        ab = a[-1].kind == 'Break'
        bb = b[-1].kind == 'Break'
        if ab and bb:
            return -1
        if ab or bb:
            continue
        ca = direct_increments(a, ivslot, by_name)
        cb = direct_increments(b, ivslot, by_name)
        if ca < 0 or cb < 0 or ca != cb:
            return -1
        # 分支整体是内层循环 ⇒ 它的自增不算本层
        def ends_loop(reg):
            last = reg[-2] if len(reg) >= 2 and reg[-1].kind in ('Yield', 'Break') else reg[-1]
            return last.kind in ('While', 'For')
        if ends_loop(a) or ends_loop(b):
            continue
        n += ca
    return n


def continue_branches_ok(region, ivslot, by_name):
    for op in region:
        if op.kind == 'Break':
            c = sum(1 for x in region if is_iv_increment(x, ivslot, by_name))
            if c != 1:
                return False
            continue
        if op.kind != 'If':
            continue
        for sub in op.regions:
            if not sub:
                continue
            if sub[-1].kind == 'Break':
                c = sum(1 for x in sub if is_iv_increment(x, ivslot, by_name))
                if c != 1:
                    return False
                continue
            last = sub[-2] if len(sub) >= 2 and sub[-1].kind in ('Yield', 'Break') else sub[-1]
            if last.kind in ('While', 'For'):
                continue
            if not continue_branches_ok(sub, ivslot, by_name):
                return False
    return True


def has_break_anywhere(region):
    for op in region:
        if op.kind == 'Break':
            return True
        for reg in op.regions:
            if has_break_anywhere(reg):
                return True
    return False


def strip_all_increments(region, ivslot, by_name):
    out = []
    for op in region:
        if op.kind == 'Break':
            out.append(op)
            continue
        if is_iv_increment(op, ivslot, by_name):
            continue
        if op.kind == 'If':
            for k in range(len(op.regions)):
                op.regions[k] = strip_all_increments(op.regions[k], ivslot, by_name)
        out.append(op)
    return out


def normalize_func(fn, stats, gidx=None):
    """对一个函数做规范化（就地改）。返回转换数。"""
    built = [0]
    guard = [0]

    def walk_regions(ops):
        # ★ **内层先处理**（与 C++ 侧一致）：内层规范化会把 `WhileOp` 变成
        #   `ForOp`，外层"看到的内层形状"因此改变 —— 若外层先判、内层后改，
        #   `run(run(X)) != run(X)`（实测：`transpose2.sy` 第一次没规范化、
        #   第二次规范化了）。⇒ 递归进体**之后**再判本层。
        for op in list(ops):
            if op.kind != 'While':
                for reg in op.regions:
                    walk_regions(reg)
                continue
            condR, bodyR = op.regions[0], op.regions[1]
            walk_regions(bodyR)
            walk_regions(condR)
            idx = ops.index(op)
            # ① continue 消解（**只读判定**；改写推迟到全部判定通过之后）
            if not check_continues(bodyR):
                stats['skip']['has-break'] += 1
                walk_regions(bodyR)
                continue
            # ★ 索引必须是**整个函数**的：`ivslot` 是入口 Region 里的
            #   `Alloca` 结果，而条件/体只引用它（定义不在它们里面）。
            #   （第一版只索引 cond/body ⇒ 找不到 `Alloca` ⇒ 所有循环都判
            #     "IV 未识别"，一个都没规范化。）
            # ★ 索引必须包含**模块级**定义（`GetGlobal` 的结果 `%.N`）。
            #   漏掉它们会让"边界是全局"的循环判错（实测：`while (i < n)`
            #   而 `n` 是全局 ⇒ 边界读 `%.0` 在索引里找不到 ⇒ 误判"读本地槽"）。
            by_name = dict(gidx) if gidx else {}
            index_by_name(fn.regions, by_name)
            # ② IV 识别
            term = condR[-1] if condR else None
            cmp_op = None
            if term is not None and term.kind == 'Yield' and term.operands:
                cmp_op = by_name.get(term.operands[0])
            if cmp_op is None or cmp_op.kind != 'Lt' or len(cmp_op.operands) != 2:
                stats['skip']['cond'] += 1
                walk_regions(bodyR)
                continue
            ivslot = None
            upper = None
            for side in (0, 1):
                cand = slot_of_load(by_name, cmp_op.operands[side])
                if cand is None:
                    continue
                base = by_name.get(cand)
                if base is not None and base.kind == 'Alloca':
                    ivslot = cand
                    upper = cmp_op.operands[1 - side]
                    break
            if ivslot is None:
                stats['skip']['no-iv'] += 1
                walk_regions(bodyR)
                continue
            # ③ 边界纯性
            written = written_slots(bodyR, by_name)
            if not prove_pure(upper, ivslot, written, by_name):
                stats['skip']['nonaffine'] += 1
                walk_regions(bodyR)
                continue
            if has_call(bodyR) and bound_reads_local(upper, by_name):
                stats['skip']['call'] += 1
                walk_regions(bodyR)
                continue
            # ④ 每条路径 +1
            if direct_increments(bodyR, ivslot, by_name) != 1 or \
               not continue_branches_ok(bodyR, ivslot, by_name):
                stats['skip']['step'] += 1
                walk_regions(bodyR)
                continue
            # ⑤ 构造 ForOp
            cond_ops = [x for x in condR if x.kind not in L.TERMINATORS]
            lower = N('Load', op.line)
            lower.attrs = ['i32']
            lower.operands = [ivslot]
            pl = ph('lower')
            lower.results = [pl]
            lower.has_line = True
            lower.src_line = op.src_line
            step = N('Int', op.line)
            step.attrs = ['1']
            ps = ph('step')
            step.results = [ps]
            step.has_line = True
            step.src_line = op.src_line
            # `ForOp` 的 IV 名字 = 条件里那条 `Load iv` 的结果名
            #   （C++ 用 `dumpResultName(fnOp, ivLoad)`；这里直接用结果名，
            #    由 `fix()` 统一改名）
            ivname = ''
            for x in condR:
                if x.kind == 'Load' and x.operands and x.operands[0] == ivslot and x.results:
                    ivname = x.results[0]
                    break
            forop = N('For', op.line)
            forop.has_line = True
            forop.src_line = op.src_line
            forop.name = ivname
            forop.operands = [ivslot, pl, upper, ps]
            forop.regions = [bodyR]
            # ★ 判定通过 ⇒ 此刻才改写：先按规范形式消解 continue，
            #   再摘掉体内全部 IV 自增（`ForOp` 的 step 对每条路径生效）
            rst = resolve_continues(bodyR, op.src_line)
            stats['continues'] += rst['wrapped']
            bodyR[:] = strip_all_increments(bodyR, ivslot, by_name)
            ops[idx:idx + 1] = cond_ops + [lower, step, forop]
            guard[0] += 1
            built[0] += 1
    for reg in fn.regions:
        walk_regions(reg)
    return built[0]


def hoist_allocas(mod):
    """把每个函数的 `AllocaOp` 提到入口 Region 最前面（**只在真的需要时**）。"""
    for fn in L.funcs(mod):
        entry = fn.regions[0]
        direct = [i for i, o in enumerate(entry) if o.kind == 'Alloca']
        total = []

        def v(o, d):
            if o.kind == 'Alloca':
                total.append(o)
        for op in entry:
            if op.kind == 'Alloca':
                continue
            for reg in op.regions:
                L.walk(reg, v)
        if not total:
            continue
        allocas = [entry[i] for i in direct] + total
        rest = [o for o in entry if o.kind != 'Alloca']
        entry[:] = allocas + rest
        # 递归摘除深层副本
        def strip(reg):
            out = []
            for o in reg:
                if o.kind == 'Alloca':
                    continue
                for k in range(len(o.regions)):
                    o.regions[k] = strip(o.regions[k])
                out.append(o)
            return out
        for op in entry:
            for k in range(len(op.regions)):
                op.regions[k] = strip(op.regions[k])


def attach_raw(mod, raw_text):
    """把每一个"来自原文"的 Op 的 `raw_core` 填上（逐字保留原文片段）。

    ★ 为什么必须逐字保留：C++ 的 dump 对 `Func`/`GlobalVar` 这类头部有**很具体**
      的排版（`:param []` 里没有空格、`Module` 不带 `@line`…）。用解析后的
      记号**重新拼**这些行极易差一个空格。**没被改写的 Op 直接照抄原文**
      既最省事又最忠实（本实现的改写只**新增** Op 与**删除** Op，
      从不修改既有 Op 的文本）。
    """
    lines = raw_text.split('\n')
    # Op 行号 → 原文片段（`( … )`，不含行尾的 `{`）
    by_line = {}
    for raw in lines:
        s = raw.strip()
        if not s.startswith('('):
            continue
        core = s[:-1].rstrip() if s.endswith('{') else s
        if core.endswith(')'):
            by_line[len(by_line)] = core
    # 按先序把 raw_core 贴回（行号可能会重复，所以按**顺序**配）
    seq = []
    for raw in lines:
        s = raw.strip()
        if not s.startswith('('):
            continue
        core = s[:-1].rstrip() if s.endswith('{') else s
        if core.endswith(')'):
            seq.append(core)

    it = [0]

    def walk(ops):
        for op in ops:
            if it[0] < len(seq):
                op.raw_core = seq[it[0]]
            it[0] += 1
            for reg in op.regions:
                walk(reg)
    walk([mod])
    return mod


def normalize_dump(text):
    """【后置】返回规范化后的 dump 文本（与 C++ 的产物可比）。"""
    modop, names, fnret = L.parse_dump(text)
    mod = N.from_op(modop)
    attach_raw(mod, text)
    stats = {'continues': 0, 'skip': {'cond': 0, 'no-iv': 0, 'nonaffine': 0,
                                      'step': 0, 'has-break': 0, 'call': 0},
             'pending': []}
    gidx = {}
    index_by_name(L.module_region(mod), gidx)
    for fn in L.funcs(mod):
        normalize_func(fn, stats, gidx)
    hoist_allocas(mod)
    return render(mod), stats


