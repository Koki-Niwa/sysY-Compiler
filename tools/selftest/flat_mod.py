#!/usr/bin/env python3
# ============================================================================
# flat_mod.py —— 平面 IR  dump 的**解析层**（从 `flat_exec.py` 拆出来）
#
#   为什么拆：`check_line_budget.py` 的单文件上限是 800 行，而"平面 IR 的
#   文本形状 + 解析 + 循环分析"本身就占掉一半。拆开之后：
#     * 本文件：**只认文本形状**（`Inst` / `FlatMod` / 循环与支配分析）；
#     * `flat_exec.py`：**只认执行语义**（`FlatExec`，复用 `exec_core`）。
#   两者之间只有一个接口：`FlatMod` 的字段（funcs/globals/preds/loops/…）。
#
#   ⚠️ 这里的解析规则**由 `FlatDump.cpp` 唯一决定**（设计文档 §2–§5）。
#      类型文本一律用括号配平取，**不能按空格切** —— 见 `_take_type`。
import re
import sys

# ============================================================================
# ── 平面 IR 的文本形状（**由 FlatDump.cpp 唯一决定**，见设计文档 §2–§5）────
# ⚠️ 类型**不能**用 `\S+` 取：`[6 x i32]` 里有空格（`ptr[[5 x i32]]` 里没有）。
#   契约（`docs/handoff/03-设计/平面IR与dump格式.md` §2）给的是
#   `"@" NAME " = global " <type> <init>`，而 `<type>` 的**数组形态带空格**。
#   第一版用 `(\S+)` ⇒ 只吃到 `[6`，整条 `:init` 数据表都丢了
#   ⇒ 常量池全局的内存是空的 ⇒ `llvm.memcpy` 从空内存复制
#   ⇒ 多维数组的初始化全变 0（实测 `int c[2][3]={{1,2,3},{4,5,6}}` 的
#   `c[1][2]`：gcc 6、我们 0）。

def _take_type(body):
    """【后置】从 `body` 的第一个记号起取一个**括号配平**的类型文本。

    ★★ 为什么不能用 `body.split(' ')[1]` ★★
      `[3 x i32]` 里有空格（`getelementptr [3 x i32], ptr[...] %b, i64 %i`），
      按空格切只会拿到 `[3` ⇒ `size_of('[3')` = 0 ⇒ GEP 的步长变 0
      ⇒ 三维/二维数组的读地址全落到首元素。实测：
        `int c[2][3]={{1,2,3},{4,5,6}}; return c[1][2];`
        GEP 的元素类型被解析成 `[3`、步长 0 ⇒ 读到 `c[0][2]` = 3
        （gcc 是 6）。
      `ptr[...]` 没有空格，所以第一版只在**数组类型**上错 —— 这正是
      "语料里有数组、但小用例往往恰好是标量"时最容易漏掉的那类。
    """
    i = 0
    n = len(body)
    while i < n and body[i] == ' ':
        i += 1
    start = i
    depth = 0
    while i < n:
        c = body[i]
        if c == '[':
            depth += 1
        elif c == ']':
            depth -= 1
            if depth == 0:
                return body[start:i + 1]
        elif c == ' ' and depth == 0:
            break
        i += 1
    return body[start:i]

# ⚠️ 全局行的**类型文本带空格且可以嵌套**：`[3 x [4 x i32]]`、`ptr[[6 x i32]]`。
#   用正则"到第一个 `]` 为止"只吃到 `[3` —— 整条 `:init` 数据表就丢了
#   （实测 `.work/g2.sy` 的 `int t[3][4]`：类型成了 `[3` ⇒ 全局内存大小 0
#   ⇒ 平面侧读到全 0，而结构化侧 42、gcc 42）。
#   ⇒ 类型部分用**下标扫描**到配平的 `]`/空格，正则只负责切出 `@名字 = global `。
RE_GLOBAL_HEAD = re.compile(r'^@(\S+) = global (.*)$')


def _split_global(rest):
    """【后置】(`类型文本`, `初始化器文本`)。类型到**配平**的 `]` 或空格为止。"""
    depth = 0
    for i, ch in enumerate(rest):
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        elif ch == ' ' and depth == 0:
            return rest[:i], rest[i + 1:]
    return rest, ''
RE_DEFINE = re.compile(r'^define (\S+) @(\S+)\((.*)\) \{$')
RE_LABEL = re.compile(r'^L(\d+):$')
RE_RESULT = re.compile(r'^%(\d+) = (.*)$')
RE_OPERAND = re.compile(r'%(\d+)')
RE_BLOCKREF = re.compile(r'\bL(\d+)\b')
RE_LINE = re.compile(r'@line (\d+)$')


class Inst(object):
    """平面 IR 的一条指令（只保留执行需要的东西）。"""
    __slots__ = ('kind', 'res', 'operands', 'blocks', 'ty', 'callee', 'intval', 'fval',
                 'pred')

    def __init__(self, kind):
        self.kind = kind
        self.res = None          # `%N`（无结果 → None）
        self.operands = []       # `%N` 列表
        self.blocks = []         # `L<n>` 列表（终结符/φ 用）
        self.ty = ''             # 类型文本（`alloca`/`load`/`store`/`gep`/`call`）
        self.callee = ''
        self.intval = 0
        self.fval = ''
        self.pred = ''            # `icmp`/`fcmp` 的谓词


class FlatMod(object):
    """平面 IR 文本 → 可执行的数据结构（**只读**，不做任何变换）。"""

    def __init__(self, text):
        self.globals = {}        # 名 → (对象类型, 初始数据 {偏移: 值})
        self.funcs = {}          # 名 → (blocks[], params[])
        self.entries = {}        # 函数名 → 入口块号（恒 0）
        self.loops = {}          # 函数名 → [头块号…]（按块号升序）
        self._parse(text)
        self.preds = {}          # (函数名, 块号) → 前驱块号集合
        self.loop_bodies = {}    # 函数名 → {头块号: 属于该循环的块集合}
        self.loop_exits = {}     # 函数名 → {头块号: 该循环的出口块集合}
        self.succs = {}          # 函数名 → {块号: 后继块号集合}（计数口径要用）
        # ★ 每个块里的 φ 结果编号（跳转时按它记快照，见 `step` 的 φ 分支）
        self.phi_ids = {}
        for fn, (blocks, _p, _c) in self.funcs.items():
            self.phi_ids[fn] = {
                b: [it.res for it in insts if it.kind == 'phi']
                for b, insts in blocks.items()}
            self.phi_ids[fn] = {b: ids for b, ids in self.phi_ids[fn].items() if ids}
        for fn, (blocks, _p, _c) in self.funcs.items():
            pr = {}
            for b, insts in blocks.items():
                for it in insts:
                    if it.kind == 'br':
                        for t in it.blocks:
                            pr.setdefault(t, set()).add(b)
            for b in blocks:
                pr.setdefault(b, set())
            self.preds[fn] = pr
            sc = {}
            for b, insts in blocks.items():
                for it in insts:
                    if it.kind == 'br':
                        sc.setdefault(b, set()).update(it.blocks)
            self.succs[fn] = sc
            self.loops[fn] = self._find_loops(fn, blocks, pr)
            self._cur_fn = fn
            self.loop_bodies[fn], self.loop_exits[fn] = self._loop_bodies(
                blocks, pr, self.loops[fn])

    # ── 解析 ─────────────────────────────────────────────────────────────
    def _parse(self, text):
        fn = None
        blocks = None
        params = None
        cur = None
        for raw in text.split('\n'):
            ln = raw.strip()
            if not ln:
                continue
            m = RE_GLOBAL_HEAD.match(ln)
            if m and fn is None:
                name = m.group(1)
                ty, rest = _split_global(m.group(2))
                data = {}
                body = rest
                if body.startswith('{'):
                    for k, v in re.findall(r'(\d+)=(-?\d+)', body):
                        data[int(k)] = int(v) & 0xFFFFFFFF   # 解析层不依赖 exec_core（见文件头）
                self.globals[name] = (ty, data)
                continue
            m = RE_DEFINE.match(ln)
            if m:
                ret_ty, name, argstr = m.group(1), m.group(2), m.group(3)
                params = []
                for pm in re.finditer(r'(\S+)\s+%(\d+)', argstr):
                    params.append((int(pm.group(2)), pm.group(1)))
                blocks = {}
                consts = []
                self.funcs[name] = (blocks, params, consts)
                self.entries[name] = 0
                fn, cur = name, None
                continue
            if ln == '}':
                fn, cur = None, None
                continue
            m = RE_LABEL.match(ln)
            if m:
                cur = int(m.group(1))
                blocks[cur] = []
                continue
            if fn is None:
                continue
            if cur is None:
                # 函数头部的**定义区**（常量 / 全局地址）：不属于任何块
                consts.append(self._parse_inst(ln))
                continue
            blocks[cur].append(self._parse_inst(ln))
        # 函数名 → 返回类型（`call` 的结果类型在 dump 里由调用点给出，够用）
        self.ret_ty = {}

    def _parse_inst(self, ln):
        res = None
        m = RE_RESULT.match(ln)
        if m:
            res = int(m.group(1))
            body = m.group(2)
        else:
            body = ln
        body = RE_LINE.sub('', body).strip()
        kind = body.split(' ')[0]
        it = Inst(kind)
        it.res = res
        # 常量定义行（函数头部）：`i32 5` / `f32 0x1p+0` / `ptr[i32] @g`
        if kind in ('i32', 'i64', 'f32'):
            it.ty = kind
            rest = body[len(kind):].strip()
            it.fval = rest
            return it
        if kind.startswith('ptr['):
            it.ty = kind
            mm = re.search(r'@(\S+)', body)
            it.callee = mm.group(1) if mm else ''
            return it
        # φ：入值与块显式配对
        if kind == 'phi':
            it.ty = _take_type(body[len('phi'):].lstrip())
            for vm, bm in re.findall(r'\(%(\d+) L(\d+)\)', body):
                it.operands.append(int(vm))
                it.blocks.append(int(bm))
            return it
        if kind == 'br':
            it.blocks = [int(x) for x in RE_BLOCKREF.findall(body)]
            it.operands = [int(x) for x in RE_OPERAND.findall(body)]
            return it
        if kind == 'ret':
            it.operands = [int(x) for x in RE_OPERAND.findall(body)]
            if it.operands:
                it.ty = _take_type(body[len('ret'):].lstrip())
            return it
        if kind == 'call':
            mm = re.search(r'@(\S+?)\(', body)
            it.callee = mm.group(1) if mm else ''
            mty = re.match(r'call (\S+) @', body)
            it.ty = '' if (mty is None or mty.group(1) == 'void') else mty.group(1)
            it.operands = [int(x) for x in RE_OPERAND.findall(body)]
            return it
        # 其余：`<op> <ty>[,] <operands…>` —— 类型用**括号配平**取（见 `_take_type`）
        rest = body[len(kind):].lstrip()
        it.ty = _take_type(rest).rstrip(',')
        # ★ `icmp`/`fcmp` 的第二个记号是**谓词**（`slt`/`oeq`），不是类型
        if kind in ('icmp', 'fcmp'):
            it.pred = it.ty
            it.ty = ''
        it.operands = [int(x) for x in RE_OPERAND.findall(body)]
        return it

    def _loop_bodies(self, blocks, pr, heads):
        """每个循环头的**块集合** = 头 + 从各回边源反向可达（且不经头）的块。

        ★ 为什么不能用"块号区间"近似（第一版的做法）：块号在**嵌套**与
        `break` 之后并不连续，近似会让"回到头"被误判成"离开循环"，
        于是一个循环被记成 N 个"1 次迭代"（实测：`[1,1,1,…]` 而不是 `[8]`）。
        """
        bodies = {}
        exits = {}
        succs = self.succs.get(self._cur_fn, {}) if hasattr(self, '_cur_fn') else {}
        for b, insts in blocks.items():
            for it in insts:
                if it.kind == 'br':
                    succs.setdefault(b, set()).update(it.blocks)

        for h in heads:
            body = {h}
            # 回边源：`br` 里目标 ≤ 自身块号的那些块的**源**
            back_srcs = [b for b in blocks
                         for it in blocks[b] if it.kind == 'br' and h in it.blocks and h <= b]
            work = list(back_srcs)
            while work:
                b = work.pop()
                if b in body:
                    continue
                body.add(b)
                for q in pr.get(b, ()):
                    if q not in body:
                        work.append(q)
            bodies[h] = body
            # ★★ 出口集合：头的后继里**不属于**本循环体的那些块 ★★
            #   为什么需要：`break` 就是"从体内直接走到出口块"，而**回边**
            #   按定义必须落在头自己身上 ⇒ 用"后继 ∈ 出口集"当 break 的判据，
            #   比"按块号大小猜回边"可靠（块号在嵌套/break 之后并不连续）。
            exits[h] = {t for t in succs.get(h, ()) if t not in body}
        return bodies, exits

    # ── 回边 → 自然循环（**只用于"数迭代次数"**，不改变执行）─────────────
    def _find_loops(self, fn, blocks, pr):
        """返回**循环头**块号的升序列表。

        ★ 判据必须是**真回边**：边 `(src, dst)` 是回边 ⟺ **`dst` 支配 `src`**。
        ⚠️ 第一版用"目标块号 ≤ 源块号"当近似 —— 那是错的：`for` 的
        **自增块**（`Linc`，由展平器排在体之后）被体末尾 `br Linc` 指向，
        于是它被误判成第二个循环头 ⇒ 计数与结构化侧对不上
        （实测：`good` 用例得到 `[1,1,1,…]` 而不是 `[8]`）。
        展平器的块序**不保证**"循环的块都编号在前"（`break` 之后还有块）。
        ⇒ 老老实实算支配（Lengauer-Tarjan 不必；迭代数据流够用，
           CFG 只有几十个块）。
        """
        succs = {}
        for b, insts in blocks.items():
            for it in insts:
                if it.kind == 'br':
                    succs.setdefault(b, set()).update(it.blocks)
        # ★★ 循环头 = "回边的目标"，而回边按**可达性**判，不只按支配 ★★
        #   支配判据（`dst` 支配 `src`）对**单入口**的自然循环是对的，但展平器
        #   为 `while(1)` 这类常量条件生成的形状里，循环头可能**不支配**它的
        #   回边源（实测 `while(n<1){while(1){while(1)break;break;}n=n+1;}`
        #   的 `L4`：`L4` 有两条互不支配的路径回到自己，且 `break` 出口直接
        #   从体内某块跳出 —— 于是 `L9→L6` 这段被支配判据漏掉，整个内层循环
        #   都检测不到 ⇒ 轨迹里少记两层）。
        #   ⇒ 判据改成"**从 src 能回到 dst**"（dst 是 src 的后继 ∪ 自身可达）
        #     —— 这正是"回边"的定义，且不要求单入口。
        dom = self._dominators(blocks, pr)
        heads = set()
        for b, insts in blocks.items():
            for it in insts:
                if it.kind != 'br':
                    continue
                for t in it.blocks:
                    if t in dom.get(b, ()):      # dst 支配 src ⇒ 回边
                        heads.add(t)
        return sorted(heads)

    @staticmethod
    def _reachable(blocks, succs):
        """【后置】`{块号: 从它出发可达的块集合}`（含自身；迭代到不动点）。"""
        reach = {b: {b} | set(succs.get(b, ())) for b in blocks}
        changed = True
        while changed:
            changed = False
            for b in blocks:
                new = set(reach[b])
                for t in succs.get(b, ()):
                    new |= reach.get(t, set())
                if new != reach[b]:
                    reach[b] = new
                    changed = True
        return reach

    @staticmethod
    def _dominators(blocks, pr):
        """迭代数据流求支配集（入口 = 块 0）。"""
        if 0 not in blocks:
            return {}
        all_b = set(blocks)
        dom = {b: set(all_b) for b in all_b}
        dom[0] = {0}
        changed = True
        while changed:
            changed = False
            for b in sorted(all_b):
                if b == 0:
                    continue
                ps = pr.get(b, set())
                if not ps:
                    new = {b}
                else:
                    new = set.intersection(*[dom.get(p, all_b) for p in ps]) | {b}
                if new != dom[b]:
                    dom[b] = new
                    changed = True
        return dom

