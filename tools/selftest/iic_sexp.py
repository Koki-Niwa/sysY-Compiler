#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""iic_sexp —— `independent_irgen_check` 的**辅助层**：S-表达式与 IR 类型。
拆出来是为了让主文件满足 §C4 的 800 行上限（单一职责：这里只做格式与类型）。"""
import re
import struct                       # ★ 必须有：本文件的 `f32`/`f32_bits` 用它做位级转换。
#   （拆分时漏了这一行 ⇒ 只有**浮点**语料会 `NameError: name 'struct' is not defined`，
#    整数语料全绿 ⇒ 这一类"拆分引入的静默缺陷"正是轨 D 才能暴露的。）


# ========================= 一、S-表达式与类型 =========================
_TOK = re.compile(r'\(|\)|"[^"]*"|[^\s()]+')
class Node(list):
    """S-表达式节点：就是 list，但可以挂 line 属性（@line 的来源）。"""
    __slots__ = ('line',)
def sexpr(text):
    root, stack = [], [Node()]
    for t in _TOK.findall(text):
        if t == '(':
            n = Node(); stack[-1].append(n); stack.append(n)
        elif t == ')':
            if len(stack) > 1:
                stack.pop()
        else:
            stack[-1].append(t)
    return stack[0]
def subs(n):
    return [x for x in n[1:] if isinstance(x, list)]
def one(n):
    s = subs(n); return s[0] if s else None
def stext(n):
    """S-表达式节点 → 一行文本（把 InitPlan 的表达式对回 Sema 树用）。迭代实现：
    语料里有 ~4000 层嵌套表达式，递归版会爆栈。"""
    out, stack = [], [(0, n)]
    while stack:
        k, x = stack.pop()
        if k == 1:
            out.append(x)
        elif isinstance(x, list):
            out.append('('); stack.append((1, ')'))
            for i, c in enumerate(reversed(x)):
                stack.append((0, c))
                if i != len(x) - 1:
                    stack.append((1, ' '))
        else:
            out.append(x)
    return ''.join(out)
def initval_exprs(n, out):
    """InitVal 子树的表达式根（源码顺序）。"""
    for c in subs(n):
        initval_exprs(c, out) if c[0] == 'InitVal' else out.append(c)
    return out
# SysY 类型 = (base, dims)：int[2][3] -> ('int', (2,3))；int[] -> ('int', (None,))
# IR 类型（dump 写法，由 example.emit-structured.txt 的真实输出确定）：
#   int->i32  float->f32  T[]->ptr[IR(T)]  T[N]->ptr[[N x IR(T)]]
#   int[3][4] -> ptr[[3 x ptr[[4 x i32]]]]（数组元素仍是数组时其"值类型"是指针）
_TY = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)((?:\[\d*\])*)$')
def parse_ty(s):
    m = _TY.match(s)
    if not m:
        raise ValueError('bad type: %r' % s)
    return (m.group(1), tuple(int(d) if d else None for d in re.findall(r'\[(\d*)\]', m.group(2))))
def ir_of(t):
    base, dims = t
    if base == 'void':
        return 'void'
    if not dims:
        return 'i32' if base == 'int' else 'f32'
    inner = ir_of((base, dims[1:])); return 'ptr[%s]' % inner if dims[0] is None else 'ptr[[%d x %s]]' % (dims[0], inner)
def elem_of(t):
    return (t[0], t[1][1:])
def is_scalar(t):
    return not t[1]
def n_leaf(t):
    n = 1
    for d in t[1]:
        n *= d
    return n
def sizeof_ir(irt):
    return 8 if irt.startswith('ptr[') else 4
def wrap32(v):
    v &= 0xFFFFFFFF; return v - 0x100000000 if v >= 0x80000000 else v
def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]
def f32_bits(v):
    return struct.unpack('<i', struct.pack('<f', f32(v)))[0]
def int_lit(s):
    """C 风格字面量：0x 十六进制、0 开头八进制、其余十进制。
    不能直接用 `int(s, 0)`：它对 "070" 抛错，会把 070 当 70（实测 C++ 给 56）。
    """
    s = s.strip(); neg = s.startswith('-')
    if neg:
        s = s[1:]
    v = int(s, 16) if s[:2].lower() == '0x' else ( int(s, 8) if len(s) > 1 and s[0] == '0' else int(s)); return -v if neg else v
def parse_fval(s):
    s = s.strip(); return float.fromhex(s) if s.startswith(('0x', '-0x', '+0x')) else float(s)
def fmt_float(v):
    """C 的 %a 风格（去尾零）；NaN→nan，±∞→inf/-inf。"""
    v = f32(v)
    if v != v:
        return 'nan'
    if v in (float('inf'), float('-inf')):
        return 'inf' if v > 0 else '-inf'
    mant, _, exp = float.hex(v).partition('p'); neg = mant.startswith('-')
    ip, _, fp = (mant[1:] if neg else mant)[2:].partition('.'); fp = fp.rstrip('0')
    return ('-' if neg else '') + '0x' + ip + ('.' + fp if fp else '') + 'p' + exp


_ALLOCA_RE = re.compile(r'^\s*\(Alloca ')


def _split_alloca(lines):
    """【后置】返回 (去掉 Alloca 行后的行列表, 排序后的 Alloca 行列表)。"""
    rest, alloca = [], []
    for x in lines:
        (alloca if _ALLOCA_RE.match(x) else rest).append(x)
    return rest, sorted(alloca)


def same_up_to_alloca_position(a, b):
    """【后置】两份转储是否"**等价**"：Alloca 的多重集相同、且**其余行逐字节相同**。

    【依据（这是判据的细化，不是实现让步）】
      * prompt §五.1 只要求"alloca 在**函数的入口 Region**"（本来就是为了 S05b 的
        提升"位置不动"与后端的栈帧布局），§六 的格式规则也**没有**规定
        `Alloca` 在入口 Region 内的**相对位置**；
      * `Alloca` 之间没有任何数据依赖，位置调换不改变语义、也不影响后端布局；
      * 因此"两个变量各自的 `Alloca` 与它的初始化是否相邻"属于**未规定的实现细节**，
        逐字节比对把它当成了契约 ⇒ 在这一项上**比语义要求更严**。
    ⚠️ 放宽的**只有 Alloca 的位置**：其余任何一行不同仍然算差异，
       并且**两边的 Alloca 多重集必须完全相同**（少一个/多一个/类型不同都算错）。
    """
    if a == b:
        return True
    ra, aa = _split_alloca(a)
    rb, ab = _split_alloca(b)
    return aa == ab and ra == rb


def unified(expected, actual, ctx=5):
    import difflib
    d = list(difflib.unified_diff(expected, actual, 'independent(独立实现)',
                                  'compiler(C++ 实现)', lineterm='', n=ctx))
    return '\n'.join(d[:120])


