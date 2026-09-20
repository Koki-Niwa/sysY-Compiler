#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
initplan_dump.py —— `--emit=initplan` 转储的**解析 + 模拟**（S04）

被 `check_initplan.py` 复用。这里**不 import 编译器的任何代码**：
格式知识来自 prompt §4.3 的契约（"偏移(字节) 值，按偏移升序，只列非零"、
`:zero` 与 `:data` 互斥、局部是 `(Zero|StoreConst|StoreExpr|MemcpyConst …)`）。

模拟的语义：
  * `Zero(off,n)`        → buf[off:off+n] = 0
  * `StoreConst(off,v)`  → 4 字节写入
  * `MemcpyConst(off,…)` → 从 off 起连续写
  * `StoreExpr(off,_)`   → 该 4 字节标记为"未知"（局部初始化器可引用变量）
并记录"每个字节被哪些动作覆盖"，供 check 侧的**覆盖性判据**使用。
"""

import re
import struct

from initplan_sem import FLT, INT, f32, i32, parse_int_literal

RE_TYPE = re.compile(r'^([a-z]+)((?:\[\d*\])*)$')
RE_NUM = re.compile(r'^-?(?:0[xX][0-9a-fA-F]*(?:\.[0-9a-fA-F]*)?[pP][+-]?\d+'
                    r'|\d+\.\d*(?:[eE][+-]?\d+)?'
                    r'|\d+(?:[eE][+-]?\d+)?'
                    r'|0[xX][0-9a-fA-F]+)$')
RE_INT = re.compile(r'^-?\d+$')

# 记号切分：`(` / `)` / 其余非空白串。格式契约是 S-表达式，所以**括号是结构**，
# 缩进与换行只是排版 —— 按记号流解析比按行解析稳得多（S04 实测：按行解析在
# `(偏移 值))` 这种"一行里既写值又收尾"的写法上必错）。
TOKEN_RE = re.compile(r'[()]|[^\s()]+')


def tokenize_plan(text):
    return TOKEN_RE.findall(text)


def parse_type(s):
    """`int[3][2]` → ('int', [3, 2])；`int` → ('int', [])；非法 ⇒ None。"""
    m = RE_TYPE.match(s or '')
    if not m:
        return None
    dims = [int(x) if x else None for x in re.findall(r'\[([0-9]*)\]', m.group(2))]
    return (m.group(1), dims)


def count_of_type(t):
    n = 1
    for d in t[1]:
        if d is None:
            return None
        n *= d
    return n


def parse_const(tok, elem):
    if elem == 'float':
        v = float.fromhex(tok) if ('x' in tok or 'p' in tok) else float(tok)
        return (FLT, f32(v))
    return (INT, parse_int_literal(tok))


def is_callable_expr(ts, k):
    """【后置】ts[k:] 的头部是不是一个"可调用的表达式节点的头"。
    只认 `--emit=sema` 的那几种节点头 —— 别的都是格式违规。"""
    if k >= len(ts) or ts[k] != '(':
        return False
    if k + 1 >= len(ts):
        return False
    head = ts[k + 1]
    if head in ('LVal', 'Call', 'Cast'):
        return True
    if head in ('IntLit', 'FloatLit'):
        return True
    return head in ('+', '-', '*', '/', '%', '<', '>', '<=', '>=', '==', '!=',
                    '&&', '||', '!')


def parse_plan(text):
    """→ {'objs': {name: [{'type','kind','pairs','acts'}, …]}}。

    ★ 同名遮蔽 ⇒ 同一个名字可能有**多条**记录（按源文件出现顺序）。

    **记号级**解析（缩进/换行不参与判定，契约只规定记号与顺序）。
    每个节点的括号都**显式**校验，所以"形状不对"会立刻报"转储无法解析"，
    而不是静默读错 —— 反证（4 份人为改坏）依赖这条。
    """
    ts = tokenize_plan(text)
    n = len(ts)
    if n < 2 or ts[0] != '(' or ts[1] != 'InitPlan':
        raise ValueError('第一行不是 (InitPlan')

    k = 0
    depth = 0

    def expect_close(j, what):
        if j >= n or ts[j] != ')':
            raise ValueError('%s 没有收尾的 `)`（看到 %r）'
                             % (what, ts[j] if j < n else '<EOF>'))
        return j + 1

    def skip_expr(j):
        if j >= n or ts[j] != '(':
            raise ValueError('表达式不以 `(` 开头：%r' % (ts[j] if j < n else '<EOF>'))
        d = 0
        while j < n:
            if ts[j] == '(':
                d += 1
            elif ts[j] == ')':
                d -= 1
                if d == 0:
                    return j + 1
            j += 1
        raise ValueError('表达式没有收尾')

    objs = {}
    k = 2                       # 跳过 `( InitPlan`
    depth = 1
    cur = None
    cur_global = False
    cur_depth = 0
    rt_depth = -1               # >= 0 ⇒ 正在跳过 `(RuntimeLib …)`
    while k < n:
        tok = ts[k]

        # ── 跳过 RuntimeLib 那一整块（它不属于任何对象）──────────────────
        if rt_depth >= 0:
            if tok == '(':
                depth += 1
            elif tok == ')':
                depth -= 1
                if depth <= rt_depth:
                    rt_depth = -1
            k += 1
            continue
        if tok == 'RuntimeLib':
            # 它的 `(` 已经在上面把 depth 加过 1，所以这一层的"外面"深度是
            # depth-1；配对的 `)` 会把 depth 降回它 ⇒ 那时才停止跳过。
            rt_depth = depth - 1
            k += 1
            continue
        if tok == 'Func':
            cur = None
            k += 2          # `( Func <函数名>`：函数名不是对象
            continue
        if tok in ('RuntimeFunc', 'Param', 'InitPlan'):
            cur = None
            k += 1
            continue

        # ── 括号 ────────────────────────────────────────────────────────
        if tok == '(':
            depth += 1
            k += 1
            continue
        if tok == ')':
            depth -= 1
            if cur is not None and depth < cur_depth:
                cur = None
            k += 1
            continue

        # ── 对象头：`( Global 名 :t 类型 :zero|:data` ────────────────────
        if tok == 'Global':
            if k + 4 >= n or ts[k + 2] != ':t' or not ts[k + 4].startswith(':'):
                raise ValueError('Global 的头形状不对：%r' % ts[k:k + 6])
            kind = ts[k + 4][1:]
            if kind not in ('zero', 'data'):
                raise ValueError('Global 的记号 %r 不是 :zero/:data' % ts[k + 4])
            cur = {'type': ts[k + 3], 'kind': kind, 'pairs': [], 'acts': []}
            objs.setdefault(ts[k + 1], []).append(cur)
            cur_global = True
            cur_depth = depth
            k += 5
            continue
        if tok == 'Local':
            if k + 4 >= n or ts[k + 2] != ':t' or ts[k + 4] != ':actions':
                raise ValueError('Local 的头形状不对：%r' % ts[k:k + 6])
            cur = {'type': ts[k + 3], 'kind': 'actions', 'pairs': [], 'acts': []}
            objs.setdefault(ts[k + 1], []).append(cur)
            cur_global = False
            cur_depth = depth
            k += 5
            continue

        if cur is None:
            raise ValueError('记号 %r 出现在对象之外' % tok)

        # ── 全局 `:data`：`( 偏移 值 )` ─────────────────────────────────
        if cur_global and cur['kind'] == 'data':
            # 每一对是 `( 偏移 值 )`。走到这里时 `(` 已经被上面的括号分支吃掉，
            # 所以 `tok` 就是偏移本身（S04 实测踩过：在这里再要求 `(` 会读错）。
            off, val = tok, ts[k + 1] if k + 1 < n else ''
            if not RE_INT.match(off):
                raise ValueError(':data 的偏移 %r 不是十进制无符号数' % off)
            k = expect_close(k + 2, ':data 的 (偏移 值)')
            cur['pairs'].append((int(off), val))
            continue

        # ── 局部动作 ────────────────────────────────────────────────────
        if tok == 'Zero':
            if not (RE_INT.match(ts[k + 1]) and RE_INT.match(ts[k + 2])):
                raise ValueError('Zero 的参数不是整数：%r %r' % (ts[k + 1], ts[k + 2]))
            cur['acts'].append(('Zero', int(ts[k + 1]), int(ts[k + 2])))
            k = expect_close(k + 3, 'Zero')
            continue
        if tok == 'StoreConst':
            off, cty, val = ts[k + 1], ts[k + 2], ts[k + 3]
            if not RE_INT.match(off) or not cty.startswith(':') or not RE_NUM.match(val):
                raise ValueError('StoreConst 的形状不对：%r %r %r' % (off, cty, val))
            cur['acts'].append(('StoreConst', int(off), parse_const(val, cty[1:])))
            k = expect_close(k + 4, 'StoreConst')
            continue
        if tok == 'StoreExpr':
            off, cty = ts[k + 1], ts[k + 2]
            if not RE_INT.match(off) or not cty.startswith(':'):
                raise ValueError('StoreExpr 的形状不对：%r %r' % (off, cty))
            if not is_expr_head(ts, k + 3):
                raise ValueError('StoreExpr 的子节点不是表达式节点头：%r'
                                 % (ts[k + 3] if k + 3 < n else '<EOF>'))
            j = skip_expr(k + 3)
            cur['acts'].append(('StoreExpr', int(off)))
            k = expect_close(j, 'StoreExpr')
            continue
        if tok == 'MemcpyConst':
            off, cty = ts[k + 1], ts[k + 2]
            if not RE_INT.match(off) or not cty.startswith(':'):
                raise ValueError('MemcpyConst 的形状不对：%r %r' % (off, cty))
            elem = cty[1:]
            j = k + 3
            vals = []
            while j < n and ts[j] != ')':
                if not RE_NUM.match(ts[j]):
                    raise ValueError('MemcpyConst 的值 %r 不是数字常量' % ts[j])
                vals.append(parse_const(ts[j], elem))
                j += 1
            if j >= n:
                raise ValueError('MemcpyConst 没有收尾')
            if not vals:
                raise ValueError('MemcpyConst 至少要有一个值')
            cur['acts'].append(('MemcpyConst', int(off), vals))
            k = j + 1
            continue
        raise ValueError('无法识别的记号 %r' % tok)
    return {'objs': objs}


def is_expr_head(ts, k):
    """【后置】ts[k:] 的头部是不是 `--emit=sema` 表达式节点的开头。"""
    if k + 1 >= len(ts) or ts[k] != '(':
        return False
    head = ts[k + 1]
    if head in ('LVal', 'Call', 'Cast', 'IntLit', 'FloatLit'):
        return True
    return head in ('+', '-', '*', '/', '%', '<', '>', '<=', '>=', '==', '!=',
                    '&&', '||', '!')


class Sim(object):
    """把一个对象的动作序列模拟成**稀疏**字节状态。

    ★ 为什么是稀疏的（S04 实测）：语料里有 `int a[30000010]`、
      `int buffer[50000000]` 这种巨型数组。稠密 bytearray（4 字节/元素）
      对 50M 元素就是 **200 MB/对象**，检查器会吃光内存并超时；
      而它们的初始化计划其实只有 1 条 `Zero`。⇒ 只记"被写过"的元素：
        `cells[off] = 4 字节位模式`（off 是元素起始字节偏移）
        `unknown` = 被 StoreExpr 覆盖的元素集合（值是运行期才知道的）

    【不变式】没出现在 `cells` 里的元素，语义上就是 0（这正是"未写到的隐式
    初始化为 0"）。覆盖性检查要靠 `cells`/`unknown` 的键集合与"应该被写"
    的位置集合比对，而不是靠"每个字节有没有被碰过"。
    """

    def __init__(self, elem, nbytes):
        self.elem = elem
        self.nbytes = nbytes
        self.cells = {}          # 元素字节偏移 -> 32 位位模式（**非零**值）
        self.unknown = set()     # 元素字节偏移（值编译期不知道）
        # 被任何动作写过的**区间**（元素字节偏移，含写 0），按起点排序且互不相交。
        # ★ 为什么是区间而不是集合：`Zero(0, 200000000)` 有 5×10^7 个元素，
        #   逐元素 add 会在一个合法程序上就超时（S04 实测）。区间表示是 O(1)。
        self.written = []
        self.actions = 0

    def at(self, off):
        return self.cells.get(off, 0)

    def _mark(self, lo, hi):
        """把 [lo,hi) 记入"已写过"，与已有区间合并（O(k log k)，k 很小）。"""
        if hi <= lo:
            return
        self.written.append((lo, hi))
        self.written.sort()
        merged = []
        for a, b in self.written:
            if merged and a <= merged[-1][1]:
                if b > merged[-1][1]:
                    merged[-1] = (merged[-1][0], b)
            else:
                merged.append((a, b))
        self.written = merged

    def is_written(self, off):
        """【后置】off 处是否被某个动作写过。区间有序 ⇒ 二分。"""
        import bisect
        i = bisect.bisect_right(self.written, (off, float('inf'))) - 1
        return i >= 0 and self.written[i][0] <= off < self.written[i][1]

    def unwritten_ranges(self, nbytes):
        """【后置】[0, nbytes) 里**没被任何动作写过**的区间列表（用于覆盖性判据）。

        ★ 为什么需要它：稀疏表示下"没写过"= 0，于是"某条 `Zero` 被改短"
          这种破坏在**值比对**里完全看不出来（剩下的位置本来就是 0）。
          覆盖性判据要求"该被写的位置必须被写过"，用区间减法做，**不扫全表**。
        """
        gaps = []
        cur = 0
        for a, b in self.written:
            if a > cur:
                gaps.append((cur, a))
            if b > cur:
                cur = b
        if cur < nbytes:
            gaps.append((cur, nbytes))
        return gaps

    def _put(self, off, ev, viol):
        if off < 0 or off + 4 > self.nbytes:
            viol.append('写偏移 %d 超出对象大小 %d' % (off, self.nbytes))
            return
        k, v = ev
        bits = (i32(v) & 0xFFFFFFFF) if k == INT else \
            struct.unpack('<I', struct.pack('<f', f32(v)))[0]
        # 写 0 与"没写过"在**语义**上等价（未写到的隐式初始化为 0），
        # 但"写过"这件事本身要记下来 —— 覆盖性判据靠 `written`。
        if bits == 0:
            self.cells.pop(off, None)
        else:
            self.cells[off] = bits
        self._mark(off, off + 4)
        self.unknown.discard(off)

    def run(self, acts, viol):
        for a in acts:
            self.actions += 1
            kind = a[0]
            if kind == 'Zero':
                off, n = a[1], a[2]
                if n <= 0:
                    viol.append('Zero 的字节数是 %d（必须 > 0）' % n)
                    continue
                if off < 0 or off + n > self.nbytes:
                    viol.append('Zero(%d,%d) 超出对象大小 %d' % (off, n, self.nbytes))
                    n = max(0, self.nbytes - off)
                # 只删"本来记着的格子"，不按 n 循环（n 可能是 2 亿字节）。
                for o in [o for o in self.cells if off <= o < off + n]:
                    del self.cells[o]
                for o in [o for o in self.unknown if off <= o < off + n]:
                    self.unknown.discard(o)
                self._mark(off, off + n)
            elif kind == 'StoreConst':
                self._put(a[1], a[2], viol)
            elif kind == 'MemcpyConst':
                for j, ev in enumerate(a[2]):
                    self._put(a[1] + 4 * j, ev, viol)
            elif kind == 'StoreExpr':
                off = a[1]
                if off < 0 or off + 4 > self.nbytes:
                    viol.append('StoreExpr 偏移 %d 超出对象大小 %d' % (off, self.nbytes))
                    continue
                self.cells.pop(off, None)
                self.unknown.add(off)
                self._mark(off, off + 4)
            else:
                viol.append('未知动作 %s' % kind)
        return self.actions


