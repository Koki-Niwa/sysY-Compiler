#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
initplan_sem.py —— **初始化语义的独立实现**（S04；被 check_initplan.py 复用）

这里放三件事，都**不依赖被测编译器的任何代码**：

  ① 期望值的表示与归一化（int 回绕 / float 单精度 / "编译期不知道"）
  ② 常量表达式的**独立求值**（字面量进制、`+ - * / %`、`/0` 的归一化）
  ③ C11 §6.7.9 的"当前对象"模型：把 `InitVal` 树展平成**行主序的元素值表**

规范依据（`docs/sysy_lang.txt`）：
  * §3 ConstDef 3 —— 数组定义的语义"the same as in C"
  * §3 ConstDef 6 —— 七个例子（`{{1,2},{3,4},{5,6}}` / `{1,2,{3,4},5,6}` /
    `{{1,2},{3},{5}}` / `{{},{3,4},5,6}` …）把填充规则钉死
  * §3 ConstDef 6.3 —— 未写到的元素隐式初始化为 0
  * §3 ConstDef 8 —— 整型数组不许有浮点元素；浮点数组可以有整型
  * §3 Initial Values 3 —— 局部未初始化 = 值不确定；全局未初始化 = 0

★ 为什么"语义"必须自己写一遍：语法树可以复用 S02 的独立 parser（它已经在
  格式层与 C++ 逐字节一致），但**语义**一旦复用就等于"用编译器验证编译器"。
"""

import struct

# ============================================================================
# 0. 期望值：三态
# ============================================================================
INT = 'i'      # ('i', int)
FLT = 'f'      # ('f', float)
# None = "编译期不知道"（初始化器引用了变量 / 非 const 对象 / 函数调用）


def i32(v):
    """32 位二进制补码回绕（规范 §3 的 int 语义；不产生 UB）。"""
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v >= 0x80000000 else v


def f32(v):
    """按 IEEE-754 单精度归一。"""
    return struct.unpack('<f', struct.pack('<f', v))[0]


def to_float(v):
    """C 的 int → float（单精度舍入）。"""
    return f32(float(v))


def to_int(v):
    """C 的 float → int（向零截断）；NaN/越界按饱和（与 satFptosi 一致）。"""
    if v != v:
        return 0
    if v >= 2147483648.0:
        return 2147483647
    if v < -2147483648.0:
        return -2147483648
    return int(v)


def bits_of(ev):
    """期望值的 4 字节位模式（与模拟出来的字节直接比）。None ⇒ None。"""
    if ev is None:
        return None
    k, v = ev
    if k == INT:
        return i32(v) & 0xFFFFFFFF
    return struct.unpack('<I', struct.pack('<f', f32(v)))[0]


def zero_of(elem):
    return (FLT, 0.0) if elem == 'float' else (INT, 0)


def normalize(ev, elem):
    """归一化到目标元素类型：浮点数组里的整型提升；整型数组里的浮点**不**转换
    （规范 §3 ConstDef 8 的不对称规则；那种写法本来就非法）。"""
    if ev is None:
        return None
    if elem == 'float' and ev[0] == INT:
        return (FLT, to_float(ev[1]))
    if elem == 'int' and ev[0] == FLT:
        return (INT, to_int(ev[1]))
    return ev


def parse_int_literal(text):
    """C 的整数常量语法：十进制 / 八进制 / 十六进制。**独立实现**。"""
    t = text.strip()
    if not t:
        raise ValueError('empty integer literal')
    neg = False
    if t[0] in '+-':
        neg = t[0] == '-'
        t = t[1:]
    if t[:2].lower() == '0x':
        v = int(t[2:], 16)
    elif len(t) > 1 and t[0] == '0':
        v = int(t[1:], 8)
    else:
        v = int(t, 10)
    v = i32(v)
    return i32(-v) if neg else v


# ============================================================================
# 1. 常量表达式求值（独立实现）
# ============================================================================
def arith(op, a, b):
    if a is None or b is None:
        return None
    if op == '%':
        if a[0] != INT or b[0] != INT:
            return None
        if b[1] == 0:
            return (INT, 0)                       # 归一化：x%0 → 0
        q = abs(a[1]) % abs(b[1])
        return (INT, i32(q if a[1] >= 0 else -q))
    isf = (a[0] == FLT) or (b[0] == FLT)
    if op == '/' and not isf:
        if b[1] == 0:
            return (INT, 0)                       # 归一化：x/0 → 0
        q = abs(a[1]) // abs(b[1])
        return (INT, i32(q if (a[1] < 0) == (b[1] < 0) else -q))
    x = to_float(a[1]) if isf else a[1]
    y = to_float(b[1]) if isf else b[1]
    if op == '+':
        r = x + y
    elif op == '-':
        r = x - y
    elif op == '*':
        r = x * y
    elif op == '/':
        if y == 0.0:
            nan = float('nan')
            r = nan if x == 0 else (float('inf') if x > 0 else float('-inf'))
        else:
            r = x / y
    else:
        return None
    return (FLT, f32(r)) if isf else (INT, i32(r))


def eval_expr(node, scopes):
    """【后置】返回期望值（('i',v) / ('f',v) / None）。不抛异常。

    只处理常量折叠需要的部分（规范 `ConstExp -> AddExp`）：
    字面量、LVal（标量或全常量下标的数组元素）、一元 `+ -`、二元 `+ - * / %`。
    关系/相等/逻辑/取反**不属于** ConstExp ⇒ 一律返回 None。
    """
    if node is None:
        return None
    head = getattr(node, 'head', '')
    tag = head.split(' ', 1)[0]
    kid = getattr(node, 'kids', [])

    if tag == 'InitVal':
        # `InitVal` 是"标量形式"的包装（`= <表达式>` 与 `{ <表达式> }` 都是它）：
        # 展平阶段会把整个 InitVal 节点交给本函数，所以这里要剥掉这层包装。
        # `{}`（没有子节点）不是标量 ⇒ None。
        if len(kid) == 1:
            return eval_expr(kid[0], scopes)
        return None
    if tag == 'IntLit':
        try:
            return (INT, parse_int_literal(head.split(' ', 1)[1]))
        except (ValueError, IndexError):
            return None
    if tag == 'FloatLit':
        try:
            tok = head.split(' ', 1)[1]
            # ★ 十六进制浮点（`0x1.921fb6p+1`）**必须**走 float.fromhex：
            #   `float('0x1.921fb6p+1')` 抛 ValueError，于是合法用例被误判成
            #   "不是常量"（S04 实测：语料 `95_float.sy` 的 PI_HEX / HEX2 / EVAL2）。
            v = float.fromhex(tok) if ('x' in tok or 'X' in tok or 'p' in tok) else float(tok)
            return (FLT, f32(v))
        except (ValueError, IndexError):
            return None
    if tag == 'LVal':
        name = head.split(' ', 1)[1] if ' ' in head else ''
        if not kid:
            v = lookup(scopes, name)
            # ★ 作用域里存的是"对象描述"（`{'shape':…, 'flat':[…]}`）而不是值：
            #   标量常量要取它的第 0 个元素。直接 `return v` 会把 dict 当成
            #   期望值流传下去，下游 `ev[0]` 立刻 KeyError（S04 实测踩过，
            #   语料里 5 个文件因此报"检查器异常"）。
            if isinstance(v, dict):
                fl = v.get('flat')
                return fl[0] if fl else None
            return v
        idx = []
        for k in kid:
            ev = eval_expr(k, scopes)
            if ev is None or ev[0] != INT:
                return None
            idx.append(ev[1])
        obj = lookup(scopes, name)
        if not isinstance(obj, dict):
            return None
        shape = obj.get('shape')
        if shape is None or len(idx) != len(shape):
            return None
        off = 0
        for d, ix in zip(shape, idx):
            if ix < 0 or ix >= d:
                return None
            off = off * d + ix
        return obj['flat'][off] if off < len(obj['flat']) else None
    if tag in ('+', '-'):
        if len(kid) == 1:                          # 一元
            a = eval_expr(kid[0], scopes)
            if a is None:
                return None
            if tag == '-':
                return (FLT, f32(-a[1])) if a[0] == FLT else (INT, i32(-a[1]))
            return a
        return arith(tag, eval_expr(kid[0], scopes), eval_expr(kid[1], scopes))
    if tag in ('*', '/', '%'):
        return arith(tag, eval_expr(kid[0], scopes), eval_expr(kid[1], scopes))
    return None


def lookup(scopes, name):
    for sc in reversed(scopes):
        if name in sc:
            return sc[name]
    return None


# ============================================================================
# 2. 期望扁平值表（C11 §6.7.9 的"当前对象"模型）
# ============================================================================
class FlatObj(object):
    """一个对象的期望值：`shape`（每一维长度；标量是 []）、`elem`、
    `flat`（行主序的元素值表，元素是三态之一）、`has_init`、`global_`、`label`。"""

    __slots__ = ('label', 'shape', 'elem', 'flat', 'has_init', 'global_', 'assigned')

    def __init__(self, label, shape, elem, has_init, global_):
        self.label = label
        self.shape = shape
        self.elem = elem
        self.has_init = has_init
        self.global_ = global_
        self.flat = [zero_of(elem)] * count_of(shape)
        # 被"显式写到"的下标集合（**稀疏**）。检查器要按"期望非零的位置"做
        # 覆盖性判据，而 `flat` 对 `int a[30000010]` 有 3×10^7 项 ——
        # 逐项扫要十几秒（S04 实测），所以这里单独记一份小集合。
        self.assigned = set()


def count_of(shape):
    n = 1
    for d in shape:
        n *= d
    return n


def is_braced(node):
    """独立 parser 的 `parse_initval`：花括号形式 ⇒ 子节点全是 `InitVal`；
    标量形式 ⇒ 只有一个**表达式**子节点。`{}` 没有子节点（也是花括号形式）。"""
    if node is None:
        return False
    kids = getattr(node, 'kids', None)
    if kids is None:
        return False
    for k in kids:
        if getattr(k, 'head', '').split(' ', 1)[0] != 'InitVal':
            return False
    return True


def fill(obj, init, scopes):
    """把 `init`（`InitVal` 节点）按 C 语义写进 obj.flat。

    ── 模型（C11 §6.7.9 的"当前对象"）─────────────────────────────────────
    与编译器 `InitLowering.cpp` 的 `flattenInto` 是**同一套模型**，这里按同一
    份规则独立重写（换语言、换数据结构，规则不许变）。

      记号：`dim[k]` = 第 k 维长度；
            `span[k]` = 第 k 层**一个对象**占几个元素 = dim[k]·…·dim[r-1]
                        （`span[r]` = 1 = 一个标量）。
      对象 = `(level, base)`：第 level 层的子对象，起点是扁平下标 `base`。
      `level == r` 表示**标量**对象（容量 1）；`level < r` 时它的第 e 个子对象
      是 `(level+1, base + e·span[level+1])`、容量 `span[level+1]`。
      只有两个动作：

        ① 花括号组作用在某个对象上，把组里的孩子**依次**摊给它的子对象：
             孩子是表达式 ⇒ 从当前子对象**借用**（见 ②）；
             孩子是花括号 ⇒ 作用在当前子对象上。
           `level == r`（标量对象）容量 1 ⇒ 只认**第一个**孩子：是表达式就写它，
           是花括号就剥掉首孩子继续（`{{{{1}}}}` 任意深）。
        ② 借用（C11 §6.7.9p20 的 "Otherwise" 子句：only enough initializers
           from the list are taken to account for the elements of the
           subaggregate）：当前子对象是**聚合**、而下一个孩子是**裸表达式**时，
           不新开花括号作用域，而是拿外层流的剩余孩子按行主序把这个子对象填满；
           填满（`e == dim[level]`）或流用尽就回到外层，外层从用掉的位置继续。

      ★ 这两条同时解释了三类最难的情形（只按"游标对齐/嵌套深度"选层必错一类）：
          `int[3][2] = {1,2,{3},5,6}`      ⇒ 1 2 3 0 5 6（`1,2` 借满 a[0]）
          `int[2][2] = {1,{2},3}`          ⇒ 1 2 3 0（`{2}` 落在**标量** a[0][1]）
          `int[2][3][4] = {1,2,3,4,{5},{}}` ⇒ 1 2 3 4 5 0…（`1..4` 借到 a[0][0]）
        （`.work/s04/proto_model.py` 把六种"对齐/最深/最浅"变体都试过，无一全过。）
      ★ 组内元素多于该子对象容量时**丢弃**多余（不是溢写到下一个子对象）：
        clang -std=c99 实测 `int[2][2] = {{1,2,3,4}}` 是 1 2 0 0。

    ── 为什么用显式栈而不是递归 ────────────────────────────────────────────
      ① 与 C++ 实现**逐帧对应**（帧 = `(孩子流, level, base, e, i, implicit)`），
         两边可以逐行对照，改哪边都能立刻发现不对称；
      ② 免疫病态深嵌套：孩子帧的 level 恒等于父帧 level+1（到 r 为止）
         ⇒ **栈深 ≤ r+1**；超出秩的深链（`{{{{…}}}}` 十万层）由 `assign_scalar`
         用 while 剥掉、根本不进栈。递归写法则会跟着**嵌套深度**走
         （Python 默认递归上限 1000），而嵌套深度不受 shape 的秩约束。
    """
    total = count_of(obj.shape)
    r = len(obj.shape)
    if total <= 0:
        return
    obj.flat = [zero_of(obj.elem)] * total
    if init is None:
        return

    # 根不是花括号组（标量形式 `= <表达式>`）：整个对象就是一个标量，写 0 号槽。
    if not is_braced(init):
        obj.flat[0] = normalize(eval_expr(init, scopes), obj.elem)
        obj.assigned.add(0)
        return
    if r <= 0:
        return          # 标量对象 + 花括号组：Sema 已报 E-INIT-SHAPE，保守留全 0

    # span[k] = 第 k 层**一个对象**占几个元素 = ∏_{j>=k} dim[j]（span[r] = 1）。
    #   ⚠️ 这里最容易错的是**差一级**：span[k] 必须是"从 k 维到最内层"的乘积。
    #   写成"从 k+1 维起"（或把组的跨度写成"从该维起的元素数"）会让
    #   `{{1,2},{3,4}}` 这种最基本的用例整张期望值表错位。
    dim = list(obj.shape)
    span = [1] * (r + 1)
    for k in range(r - 1, -1, -1):
        span[k] = span[k + 1] * dim[k]

    def write(pos, node):
        """把一个标量孩子写进槽：越界忽略；值按**元素类型**归一化
        （整型字面量给 float 数组 ⇒ ('f', 3.0)）。"""
        if 0 <= pos < total:
            obj.flat[pos] = normalize(eval_expr(node, scopes), obj.elem)
            obj.assigned.add(pos)

    def assign_scalar(group, pos):
        """花括号组作用在**标量**对象上：只认第一个孩子；首孩子还是组就继续剥
        （while，不递归）。`{}` ⇒ 什么都不写（隐式 0）。"""
        g = group
        while True:
            kids = getattr(g, 'kids', [])
            if not kids:
                return
            first = kids[0]
            if not is_braced(first):
                write(pos, first)
                return
            g = first

    # 显式工作栈。帧 = [src, level, base, e, i, implicit]
    #   src       孩子流 = src.kids
    #   level     本对象所在层
    #   base      本对象起点（扁平下标）
    #   e         对象内"下一个要填的子对象"下标
    #   i         流里"下一个孩子"下标
    #   implicit  True = 借用帧（没有花括号）
    stack = [[init, 0, 0, 0, 0, False]]
    while stack:
        cur = stack[-1]
        src, level, base, e, i, implicit = cur
        kids = getattr(src, 'kids', [])
        if i >= len(kids) or e >= dim[level]:
            # 本对象填满（或流用尽）⇒ 弹帧，把"消费到哪儿"回填给父帧。
            stack.pop()
            if stack:
                parent = stack[-1]
                if implicit:
                    parent[4] = i       # 借用：父帧采纳子帧用掉的流位置
                else:
                    parent[4] += 1      # 花括号组 = 流里的一个孩子
                parent[3] += 1
            continue
        child = kids[i]
        if child is None:               # 残缺树（Sema 报错后）：跳过
            cur[4] += 1
            continue
        nl = level + 1
        nb = base + e * span[nl]
        if not is_braced(child):
            if nl == r:                 # 目标是标量：写在游标处，然后前进一格
                write(nb, child)
                cur[4] += 1
                cur[3] += 1
            else:
                # 借用：**不**推进父帧的 i/e —— 子帧弹栈时统一回填（借用 ⇒ 采纳
                # 子帧的 i；花括号 ⇒ i++、e++）。推进两次会让 `{{1,2},{3,4}}`
                # 只填完第一行就以为整个数组填满了。
                stack.append([src, nl, nb, 0, i, True])
            continue
        if nl == r:                     # 花括号作用在标量位置上（本组就此用完）
            assign_scalar(child, nb)
            cur[4] += 1
            cur[3] += 1
            continue
        stack.append([child, nl, nb, 0, 0, False])   # 花括号作用在聚合子对象上


# ============================================================================
# 3. 从语法树里收集对象（全局 + 每个函数的局部）
# ============================================================================
class Extract(object):
    """按语法树收集 `FlatObj`。

    ★ 作用域是**真的一层一层管的**（S04 实测：`53_scope2.sy` 里同名变量被
      内层重新定义，遮蔽关系判错会让期望值整体错位；`54_hidden_var.sy` /
      `70_dijkstra.sy` 这类"块内声明"的文件也会被误判成"未初始化"）。
      规则与 `docs/sysy_lang.txt` §3 Block 2 一致：**块建立作用域**，
      块内的同名声明遮蔽块外，作用域从定义点起、到块尾止。

    ⚠️ 但**不**做完整的符号表：这个模块的职责是产出**期望值**（未知不算错），
      所以查不到的名字一律当作"编译期不知道"（`None`），绝不猜。
    """

    def __init__(self, tree):
        self.tree = tree
        self.objs = []
        # 作用域栈：底两层是"全局层"与"当前函数的参数层"，每进一个 Block 压一层。
        # 查不到 = None（编译期不知道）。
        self.scopes = [{}]

    def run(self):
        for it in self._items(self.tree):
            tag = it.head.split(' ', 1)[0]
            if tag == 'FuncDef':
                self._func(it)
            elif tag == 'Decl':
                self._decl(it, global_=True, fname='')
        return self.objs

    def _items(self, tree):
        return tree.kids if tree.head.split(' ', 1)[0] == 'CompUnit' else []

    def _func(self, fd):
        parts = fd.head.split()
        fname = parts[1] if len(parts) > 1 else '?'
        params = {}
        for p in fd.kids:
            if p.head.split(' ', 1)[0] == 'params':
                for q in p.kids:
                    qn = q.head.split()
                    if len(qn) > 1:
                        params[qn[1]] = None       # 形参：编译期不知道
        saved = self.scopes
        self.scopes = [saved[0], params]           # [全局层, 参数层]
        for b in fd.kids:
            if b.head.split(' ', 1)[0] == 'Block':
                self._stmts(b, fname)
        self.scopes = saved

    def _stmts(self, block, fname):
        self.scopes.append({})                     # 块 = 一层作用域
        for it in block.kids:
            tag = it.head.split(' ', 1)[0]
            if tag == 'Decl':
                self._decl(it, global_=False, fname=fname)
            elif tag == 'Block':
                self._stmts(it, fname)
            elif tag in ('If', 'While', 'Else'):
                for kid in it.kids:
                    # `(Else (Block …))`：else 分支自己一层作用域
                    if kid.head.split(' ', 1)[0] == 'Block':
                        self._stmts(kid, fname)
        self.scopes.pop()

    def _decl(self, d, global_, fname):
        is_const = ':const' in d.head
        # ⚠️ 基类型在 `Decl` 的头部：`Decl :int` / `Decl :const :float`。
        #    写成"看第二个 token"在 `Decl :float` 上恰好也能对，但在
        #    `Decl :const :float` 上会判成 int（S04 实测踩过：`float b[2] = {1,2}`
        #    被当成整型数组，期望值成了 0x00000001 而转储是 0x3f800000）。
        head_tokens = d.head.split()
        elem = 'float' if ':float' in head_tokens else 'int'
        for vd in d.kids:
            if vd.head.split(' ', 1)[0] != 'VarDef':
                continue
            vparts = vd.head.split()
            name = vparts[1]
            shape = []
            init = None
            ok = True
            for k in vd.kids:
                tk = k.head.split(' ', 1)[0]
                if tk == 'Dim':
                    ev = eval_expr(k.kids[0], self.scopes) if k.kids else None
                    if not ev or ev[0] != INT or ev[1] <= 0:
                        ok = False
                        break
                    shape.append(ev[1])
                elif tk == 'InitVal':
                    init = k
            if not ok:
                continue                        # 维度非法：S03 已报错，这里不猜
            label = name if global_ else '%s/%s' % (fname, name)
            obj = FlatObj(label, shape, elem, init is not None, global_)
            if init is not None:
                fill(obj, init, self.scopes)
            self.objs.append(obj)
            # 声明点之后这个名字才可见（规范 §3 Block 2）
            self.scopes[-1][name] = {'shape': shape, 'flat': obj.flat} if is_const else None
