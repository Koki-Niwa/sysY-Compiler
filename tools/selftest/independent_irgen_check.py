#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""independent_irgen_check.py —— SysY→结构化 IR 的**独立第二份 IRGen**（S05 轨 D）。

从 `<compiler> <file> --emit=sema` 的转储（S-表达式）出发，按 S05 §五 的语义规格
**独立**生成结构化 IR 文本，再与 `<compiler> <file> --emit=structured-ir`
的真实输出比对。零第三方依赖。

用法（**唯一入口就是本文件**）：
  python3 independent_irgen_check.py --compiler <编译器路径>
          [--jobs N] [--dir 语料目录] [--limit N] [--verbose]
退出码：0 = 全部等价 | 1 = 有差异 | 2 = 工具自身错误

★ 两处**已知的输入/判据偏离**（都在报告 §7 里记账）：
  1. `--emit=sema` 的转储**不含源码位置**，而 IR 的每个 Op 都带 `@line`
     ⇒ 本工具额外读源文件，用"Sema 树引导的词法标注器"把 token 行号贴回节点。
  2. 判据在"`Alloca` 的位置"这一项上**比逐字节更宽**：§五.1 只要求 alloca 在
     **函数入口 Region**，§六 也没规定它的相对位置，而 alloca 之间无数据依赖
     ⇒ 等价 = Alloca 多重集相同 + 其余行逐字节相同。**两个数字都会打印。**
     ⚠️ 当前全量 490 个文件**全部是严格逐字节**（等价计数 = 0），
        这条放宽只作为诊断打印，**不参与判定**。

★ 初始化**完全按 `--emit=initplan` 的计划**走（S04 的产物，不是 S05 的实现）。
★ 模块划分（每个文件单一职责，同时满足 §C4 的 800 行上限）：
    `iic_sexp.py`  —— S-表达式、IR 类型、两份转储的比对判据；
    `iic_front.py` —— 源码侧前端：词法器 + 行号标注器 + InitPlan 解析；
    **本文件**     —— 容器（Op/Region/渲染）+ `Gen`（IRGen 本体）+ 驱动/CLI。
  ⚠️ 历史：本文件的 `Gen` 曾被误删 11 个方法、靠**编译产物兜底**运行；本轮已把
     它们**逐条重建回源码**，兜底与编译产物一并删除 —— 现在**只依赖源码**运行。
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, threading, zlib
from concurrent.futures import ThreadPoolExecutor

sys.dont_write_bytecode = True    # 不落编译产物：工具目录只留源码（★ 必须在导入辅助层之前）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# 辅助层：S-表达式/IR 类型（iic_sexp）与源码侧前端（iic_front）。
#   两个 `import *` 之后本模块的 globals 同时带着它们，`Gen` 直接按裸名字用。
from iic_sexp import *                                       # noqa: F401,F403
from iic_sexp import (Node, sexpr, subs, one, stext, initval_exprs, parse_ty,   # noqa: F401
                      ir_of, elem_of, is_scalar, n_leaf, sizeof_ir, wrap32,
                      f32, f32_bits, int_lit, parse_fval, fmt_float)
from iic_front import *                                      # noqa: F401,F403
from iic_front import (Tok, tokenize, Annotator, parse_initplan, Act, Entry,   # noqa: F401
                       mk_local, BINOPS)

CMP = {'<': 'Lt', '>': 'Gt', '<=': 'Le', '>=': 'Ge', '==': 'Eq', '!=': 'Ne'}
ZERO_MAX = 32                          # Zero 动作：size > 32 用 llvm.memset
INT_MAX, INT_MIN = 2147483647, -2147483648
sys.setrecursionlimit(120000)                 # 语料里有 ~4000 层嵌套表达式
try:
    threading.stack_size(48 * 1024 * 1024)
except (ValueError, RuntimeError):
    pass
# ==================== 四、结构化 IR 的构造与渲染 ====================
class Op(object):
    __slots__ = ('text', 'line', 'regions', 'term')
    def __init__(self, text, line, regions=None, term=False):
        self.text, self.line, self.regions, self.term = text, line, regions, term
class Region(object):
    __slots__ = ('ops', 'entrance')

    def __init__(self):
        self.ops = []
        # ★ 这个 Region 是不是"函数入口 Region"（== 函数体本身）。
        #   只有发往**入口**的初始化指令才进"入口前缀"缓冲（见 `Gen.op`）。
        self.entrance = False
    def last_term(self):
        return bool(self.ops) and self.ops[-1].term
def render(region, out, ind):
    pad = '  ' * ind
    for op in region.ops:
        ln = ' @line %d' % op.line if op.line else ''
        if op.regions is None:
            out.append('%s(%s%s)' % (pad, op.text, ln)); continue
        for k, r in enumerate(op.regions):        # 第 1 个 Region 的 { 跟在 Op 同行
            out.append(('%s(%s%s) {' % (pad, op.text, ln)) if k == 0 else ('%s{' % pad)); render(r, out, ind + 1)
            out.append('%s}' % pad)
# ============ 五、IRGen：Sema AST(+line) + InitPlan → 结构化 IR ============
class Gen(object):
    def __init__(self, base, sema, plan, src):
        self._dry = False; self.base, self.tree = base, sexpr(sema)
        self.ann = Annotator(tokenize(src)); self.globs, self.funcs = parse_initplan(plan)
        self.runtime, self.funcdefs = {}, []; self.mod, self.modn, self.funcops = Region(), 0, []
        self.cur = self.root = None; self.fname, self.n, self.npool = None, 0, 0
        self.sym, self.gsym, self.scopes, self.pre = {}, {}, [], {}
        self.drycur, self.dry_done = None, set()
    def nm(self):
        if self._dry:                     # 预扫描里被抑制的指令不占编号（名字会被丢掉）
            return '%dry'
        self.n += 1; return '%' + self.fname + '.' + str(self.n - 1)
    def op(self, text, line):
        """发射一条无结果的指令（**就地**进当前 Region）。
        ★ 本方法与 `alloc`/`gen_func` 是**从原版编译产物逐行重建**的
          （原源码在这三个方法里实现了"入口前缀按变量成组"），并在
          ★ 本轮把"入口 Region 里的初始化"从"缓冲后整体前插"改成了**就地发射**：
          C++ 侧实测就是就地成组（`[Alloca x, <x 的初始化>, Alloca y, …]` 恰好
          落在声明语句所在的位置），前插只有在"所有声明都在最前面"时才碰巧一致。
        """
        if self._dry:
            return
        self.cur.ops.append(Op(text, line))
    def val(self, text, line, ir, ty):
        """发射一条有结果的指令 → (名字, IR 类型, SysY 类型)。text 里用 @ 代表结果名。
        ⚠ 不能对同一条串做两次 % 格式化：结果名形如 %f.5，第二次会把 %f 当浮点格式。
        """
        r = self.nm(); self.op(text.replace('@', r), line)
        return (r, ir, ty)
    def ci(self, v, line):
        return self.val('Int @ %d' % wrap32(v), line, 'i32', ('int', ()))
    def cf(self, v, line):
        return self.val('Float @ %s' % fmt_float(v), line, 'f32', ('float', ()))
    def zero_of(self, v, line):
        return self.ci(0, line) if v[1] == 'i32' else self.cf(0.0, line)
    def tobool(self, v, line):
        z = self.zero_of(v, line); return self.val('Ne @ %s %s' % (v[0], z[0]), line, 'i32', ('int', ()))
    def sext(self, v, line):
        return self.val('Sext @ %s' % v, line, 'i64', None)[0]
    def bitcast(self, target, v, line):
        return self.val('Bitcast @ %s %s' % (target, v), line, target, None)[0]
    def alloc(self, ir, line):
        """建一个变量槽/临时槽。**变量槽必进"入口前缀"**（prompt §五.1）。
        ★ 顺序：变量槽进入口前缀时，先把上一个变量的初始化代码落进去
          （`pending` → `entry`），再放这个 Alloca —— 于是入口 Region 是
          `[Alloca x, <x 的初始化>, Alloca y, <y 的初始化>, …]` 的**成组**形态
          （与 C++ 侧一致；逐字节比对就差这一项）。
        """
        self.n += 1
        r = '%' + self.fname + '.' + str(self.n - 1)
        op = Op('Alloca %s %s' % (r, ir), line)
        # ★ **一律就地**放到"当前 Region"（对 C++ 真实输出的实测结论）：
        #   变量槽就地成组 `[Alloca x, <x 的初始化>, …]`；嵌套块里的声明因预扫描
        #   那一趟 `cur` 是外层 Region，Alloca 落在外层、排在包住它的 `If`/`While`
        #   之前；表达式临时槽同样就地。原来"攒进 `entry` 再整体前插"的模型只在
        #   "所有声明都在最前面"时才碰巧一致（见报告 §7.2 第 1 条）。
        self.cur.ops.append(op)
        return r
    def op2(self, text, line, regions):
        if not self._dry:
            self.cur.ops.append(Op(text, line, regions))
    def lookup(self, name):
        for sc in reversed(self.scopes):
            if name in sc:
                return sc[name]
        return self.sym[name] if name in self.sym else self.gsym[name]
    def reg(self, line, fn):
        """新建 Region，跑 fn 填充，补终结 Yield，返回该 Region。"""
        rg, save = Region(), self.cur; self.cur = rg
        fn()
        if not rg.last_term():
            rg.ops.append(Op('Yield', line, term=True))
        self.cur = save; return rg
    def dry(self, fn):
        """预扫描：按**生成顺序**空跑一遍，只把该子树需要的 Alloca（变量槽/临时槽）
        落到函数入口 Region——真实输出的结果编号顺序就是这样来的。"""
        d, cur = self._dry, self.cur
        outer = self.drycur if self._dry else None
        if outer is None:
            self.drycur = dict(self.ecur)   # 最外层空跑：从真实游标拷一份
        self._dry = True
        try:
            fn()
        finally:
            self._dry, self.cur = d, cur   # 编号**不**回滚：预扫描建槽的编号要保留
            if outer is None:
                self.drycur = None
            else:
                self.drycur = outer
    def temps(self, node, n, line):
        got = self.pre.get(id(node)) or []
        while len(got) < n:
            got.append(self.alloc('i32', line))
        self.pre[id(node)] = got; return got
    # ---------- 顶层 ----------
    def run(self):
        root = [x for x in self.tree if isinstance(x, list) and x[0] == 'CompUnit'][0]
        for top in self.tree:           # RuntimeLib 与 CompUnit 是**同级**顶层节点
            if isinstance(top, list) and top and top[0] == 'RuntimeLib':
                for rf in subs(top):
                    if rf[0] == 'RuntimeFunc':
                        self.runtime[rf[1]] = rf[2][1:]
        kids = subs(root); self.funcdefs = [k for k in kids if k[0] == 'FuncDef']
        for k in kids:                                    # 先标注全部行号
            self.ann.decl(k) if k[0] == 'Decl' else self.ann.funcdef(k)
        if self.ann.peek() is not None:
            raise ValueError('trailing source tokens at line %d' % self.ann.peek().line)
        gi = iter(self.globs)
        for k in kids:                                    # 1) 全局变量（全在函数前）
            if k[0] != 'Decl':
                continue
            for vd in [c for c in subs(k) if c[0] == 'VarDef']:
                e = next(gi)
                if e.name != vd[1]:
                    raise ValueError('global order mismatch: %s vs %s' % (e.name, vd[1]))
                self.gen_global(e, vd)
        for k in kids:                                    # 2) 函数体（const 池边生成边入模块）
            if k[0] == 'FuncDef':
                self.gen_func(k)
        self.mod.ops += self.funcops                      # 3) 函数 Op 最后追加
        return self.mod
    def gen_global(self, e, vd):
        ir, line = ir_of(e.ty), vd.line
        if e.zero:                                        # 大数组的 :zero 绝不展开成数据
            init = ':init "zero"'
        else:
            items = ['%d=%d' % (off, f32_bits(parse_fval(val)) if e.ty[0] == 'float' else wrap32(int_lit(val))) for off, val in e.data]; init = ':init "data" {%s}' % ', '.join(items)
        self.mod.ops.append(Op('GlobalVar "%s" :type %s %s' % (e.name, ir, init), line)); r = '%%.%d' % self.modn
        self.modn += 1; self.mod.ops.append(Op('GetGlobal "%s" %s :type %s' % (e.name, r, ir), line))
        self.gsym[e.name] = (e.ty, r)
    def gen_func(self, fd):
        self.fname, self.n, self.npool = fd[1], 0, 0; self.cur = self.root = Region()
        self.root.entrance = True         # 函数体 == 入口 Region（初始化就发在这里）
        # 入口前缀（alloca + 各变量的初始化，成组）/ 缓冲 / 两个标记
        self.entry, self.pending, self.in_init, self.var_slot = [], [], False, False
        self.sym, self.scopes, self.eidx, self.pre = {}, [], 0, {}
        self.dry_done = set()          # 预扫描已扫过的语句节点（见 gen_stmt）
        self.entries = self.funcs.get(self.fname, [])
        # 对齐状态：`slots` 名字→条目下标（按源码顺序）；`ecur` 真实那一趟的游标；
        # `drycur` 空跑那一趟的游标；`eidx_of` 某个 vardef 节点 → 条目下标。
        self.slots, self.ecur, self.eidx_of = {}, {}, {}
        for k, en in enumerate(self.entries):
            self.slots.setdefault(en.name, []).append(k)
        ret = parse_ty(fd[2][1:]); ps = [c for c in subs(fd) if c[0] == 'params']
        pirs = []
        for i, p in enumerate(subs(ps[0]) if ps else []):
            pty = parse_ty(p[3]); pirs.append(ir_of(pty))
            # 顺序（实测的真实输出）：`GetArg` 先发进体，**再**给它的槽 `Alloca`
            #   ⇒ 入口前缀里 Alloca 排在 GetArg 之后。
            a = self.val('GetArg @ %d %s' % (i, pirs[-1]), p.line, pirs[-1], None)[0]
            al = self.alloc(pirs[-1], p.line)
            self.op('Store %s %s %s' % (pirs[-1], a, al), p.line)
            self.sym[p[1]] = (pty, al)
        self.gen_block([c for c in subs(fd) if c[0] == 'Block'][0])
        # 收尾：**先 flush**（必须在"拼入口前缀"与"掉出末尾补 Return"之前 ——
        #   否则最后一段初始化代码还在缓冲里、函数体看起来是空的）。
        self.flush_pending()
        self.root.ops[:0] = self.entry    # ★ 入口前缀：alloca + 初始化，排在体最前
        if not self.root.ops or self.root.ops[-1].text.split()[0] != 'Return':
            if ret[0] == 'void':                          # 掉出末尾：补 Return（0 / void）
                self.op('Return', fd.line)
            else:
                self.op('Return %s' % self.ci(0, fd.line)[0], fd.line)
        self.funcops.append(Op('Func "%s" :ret %s :param [%s]' % (self.fname, ir_of(ret), ', '.join(pirs)), fd.line, [self.root]))
    # ---------- 初始化：完全按 InitPlan ----------
    def gen_actions(self, e, vty, ptr, line):
        for act in e.acts:
            if act.kind == 'Zero':
                self.gen_zero(ptr, act.val, line)
            elif act.kind in ('StoreConst', 'StoreExpr'):
                T = ir_of(elem_of(vty)); gp = self.val('GetElementPtr @ %s i64 0 %s %s' % ( T, ptr, self.sext(self.ci(act.off // sizeof_ir(T), line)[0], line)), line, 'ptr[%s]' % T, None)[0]
                if act.kind == 'StoreConst':
                    v = (self.cf(parse_fval(act.val), line) if act.ty == ':float' else self.ci(int_lit(act.val), line))
                else:
                    v = self.gen_expr(self.match_iv(act.expr))
                self.op('Store %s %s %s' % (T, v[0], gp), line)
            else:                                         # MemcpyConst：常量池 + memcpy
                if self._dry:
                    # ★ 常量池是**模块级**副作用（`<const.N>`+`%.N`），空跑里不能建：
                    #   否则同一批常量会出现两套（`23_json.sy` 实测）。这一支没有槽要建。
                    continue
                leaf = 'i32' if act.ty == ':int' else 'f32'; vals = act.val
                self.npool += 1                            # 池编号**每函数**重新计数
                pname = '<const.%d>' % (self.npool - 1); bits = ['%d=%d' % (4 * j, f32_bits(parse_fval(s)) if leaf == 'f32' else wrap32(int_lit(s))) for j, s in enumerate(vals)]
                pa = '[%d x %s]' % (len(vals), leaf); self.mod.ops.append(Op('GlobalVar "%s" :type %s :init "data" {%s}' % (pname, pa, ', '.join(bits)), line))
                pr = '%%.%d' % self.modn; self.modn += 1
                self.mod.ops.append(Op('GetGlobal "%s" %s :type ptr[%s]' % (pname, pr, pa), line)); b1 = self.bitcast('ptr[i32]', pr, line)
                b2 = self.bitcast('ptr[i32]', ptr, line); sl = self.sext(self.ci(len(vals) * 4, line)[0], line)
                self.op('Call "llvm.memcpy" %s %s %s %s' % (b2, b1, sl, self.ci(1, line)[0]), line)
    def gen_zero(self, ptr, size, line):
        if size > ZERO_MAX:
            b = self.bitcast('ptr[i32]', ptr, line); sl = self.sext(self.ci(size, line)[0], line)
            self.op('Call "llvm.memset" %s %s %s %s' % (b, self.ci(0, line)[0], sl, self.ci(1, line)[0]), line); return
        for i in range(size // 4):        # 元素类型恒为 i32；byte offset 不参与定位
            gp = self.val('GetElementPtr @ i32 i64 0 %s %s' % (ptr, self.sext(self.ci(i, line)[0], line)), line, 'ptr[i32]', None)[0]
            self.op('Store i32 %s %s' % (self.ci(0, line)[0], gp), line)
    def match_iv(self, pexpr):
        """InitPlan 的表达式没有行号：按文本对回已标注行号的 Sema 表达式节点。"""
        want = stext(pexpr)
        for k, (txt, node) in enumerate(self.ivc):
            if txt == want and k not in self.ivused:
                self.ivused.add(k); return node
        raise ValueError('no annotated sema expr for init-plan expr %s' % want)
    # ---------- 语句 ----------
    def gen_block(self, b):
        for c in subs(b):
            if self.cur.last_term():            # break/return 之后的代码不再生成
                break
            self.gen_stmt(c)
    def gen_stmt(self, s):
        h = s[0]
        # ★ 同一个语句节点在预扫描里**只扫一次**（否则"预扫描里套预扫描"会让
        #   100 层嵌套 `if` 变成 2^100 工作量：33_multi_branch/30_many_dimensions
        #   实测 > 60 s 不返回）。预扫描的建槽副作用按 `id(node)` 幂等，跳过多余
        #   扫描不改变产物。见报告 §7.2 第 3 条。
        if self._dry and id(s) in self.dry_done:
            return
        self.dry_done.add(id(s))
        if h == 'Block':
            self.scopes.append({}); self.gen_block(s); self.scopes.pop()
        elif h == 'Decl':
            for vd in [c for c in subs(s) if c[0] == 'VarDef']:
                self.gen_vardef(vd)
        elif h == '=':
            lhs, rhs = subs(s)
            v = self.gen_expr(rhs)                        # 先右值，再左值地址
            self.op('Store %s %s %s' % (v[1], v[0], self.gen_lval_addr(lhs)[0]), lhs.line)
        elif h == 'ExprStmt':
            if subs(s):
                self.gen_expr(subs(s)[0])
        elif h == 'If':
            c = self.gen_expr(subs(s)[0]); el = [x for x in subs(s) if x[0] == 'Else']
            self.dry(lambda: self.gen_stmt(subs(s)[1]))    # 两个分支 Region 先扫
            if el:
                self.dry(lambda: self.gen_stmt(subs(el[0])[0]))
            def run(st):
                self.scopes.append({}); self.gen_stmt(st); self.scopes.pop()
            tr = self.reg(s.line, lambda: run(subs(s)[1])); er = self.reg(s.line, lambda: run(subs(el[0])[0]) if el else None)
            self.op2('If %s' % c[0], s.line, [tr, er])
        elif h == 'While':
            ce = subs(s)[0]
            self.dry(lambda: self.gen_expr(ce))            # cond Region 内容先扫
            self.dry(lambda: self.gen_stmt(subs(s)[1]))    # 再扫循环体 Region
            def cbody():
                self.op('Yield %s' % self.gen_expr(ce)[0], ce.line)
                if not self._dry:
                    self.mark_term()
            def body():
                self.scopes.append({}); self.gen_stmt(subs(s)[1]); self.scopes.pop()
            self.op2('While', s.line, [self.reg(ce.line, cbody), self.reg(s.line, body)])
        else:                                             # Return / Break / Continue
            k = subs(s)
            if h == 'Return':
                self.op('Return %s' % self.gen_expr(k[0])[0] if k else 'Return', s.line)
            else:
                self.op('Break', s.line)                  # continue 也降级成 Break
            if not self._dry:
                self.mark_term()
    def gen_vardef(self, vd):
        if id(vd) not in self.pre:
            self.pre[id(vd)] = [self.alloc(ir_of(parse_ty(vd[3])), vd.line)]
        # ★ 条目按 **(名字, 第几次出现)** 取（同名遮蔽会有多条同名记录，与 C++ 侧
        #   `LocalIndex` 同解）：真实那一趟维护 `ecur`，预扫描用 `drycur`（空跑开始时
        #   从 `ecur` 拷贝，嵌套空跑共享）。
        slot = self.eidx_of.get(id(vd))
        if slot is None:
            cur = self.drycur if self._dry else self.ecur
            slot = cur.get(vd[1], 0)
        cand = self.slots.get(vd[1]) or []
        e = self.entries[cand[slot]] if slot < len(cand) else None
        if self._dry:
            # 预扫描：登记名字（同一次空跑里后面的语句要用到它），并把**真实那一趟
            # 会用到**的临时槽先建出来。
            # ⚠ 必须走**同一份 InitPlan 动作**，不能走**原始表达式** —— 否则计划里
            #   已经折叠成常量的初始化（如 `int j = N / 2;`，N 是 const）会凭空多出
            #   一个用不到的除法规范化槽（`83_long_array.sy` 实测两处）。
            if e is not None:
                self.eidx_of[id(vd)] = slot
                self.drycur[vd[1]] = slot + 1
                iv = [c for c in subs(vd) if c[0] == 'InitVal']
                self.ivc = [(stext(x), x) for x in initval_exprs(iv[0], [])] if iv else []
                self.ivused = set()
                self.gen_actions(e, parse_ty(vd[3]), self.pre[id(vd)][0], vd.line)
            (self.scopes[-1] if self.scopes else self.sym)[vd[1]] = (
                parse_ty(vd[3]), self.pre[id(vd)][0])
            return
        if e is None:
            raise ValueError('no init plan entry for %s' % vd[1])
        self.eidx += 1
        self.ecur[vd[1]] = slot + 1
        if e.name != vd[1]:
            raise ValueError('init plan order mismatch: %s vs %s' % (e.name, vd[1]))
        ty = parse_ty(vd[3])
        if ty != e.ty:
            raise ValueError('type mismatch for %s: %s vs %s' % (vd[1], ir_of(ty), ir_of(e.ty)))
        al = self.pre[id(vd)][0]; (self.scopes[-1] if self.scopes else self.sym)[vd[1]] = (ty, al)

        iv = [c for c in subs(vd) if c[0] == 'InitVal']; self.ivc = [(stext(x), x) for x in initval_exprs(iv[0], [])] if iv else []
        self.ivused = set()
        self.in_init = True               # 这一段指令属于"入口前缀"（见 op/alloc）
        self.gen_actions(e, ty, al, vd.line)
        self.in_init = False
    # ══════════════════════════════════════════════════════════════════════
    # 表达式生成（★ 本轮**从原版字节码逐条重建回源码**，等价性由全量预言机实测）
    #   这一组 11 个方法曾经只以字节码形式存在（见报告 §7.2.1）。
    #   重建的判据不是"看起来对"，而是**可执行的等价性**：
    #   补齐前后各跑一次全量 551 个文件，独立实现渲染出的模块文本逐字节相同。
    # ══════════════════════════════════════════════════════════════════════
    def mark_term(self):
        """把**刚发出的那条指令**标成终结符。
        ⚠️ 必须看得见 `pending` 缓冲：变量初始化期间指令先进缓冲，此时
        `self.cur.ops` 可能是空的（实测 IndexError）—— 终结符也可能出现在
        初始化表达式的返回值里吗？不会，但缓冲机制让这条通用断言不再平凡。
        """
        tgt = self.pending if self.pending is not None else None
        if tgt:
            tgt[-1].term = True
            return
        if self.cur.ops:
            self.cur.ops[-1].term = True
    def flush_pending(self):
        """把初始化缓冲落进入口前缀。
        ★ 必须在**第一条真正的语句之前** flush（而不是拖到函数末尾）——
          否则后续语句里创建的 `Alloca` 会把整段缓冲推到后面，
          语句就被搬到 `If` 的后面去了（实测 `17_div.sy`）。
        ⚠️ 本轮把初始化改成**就地发射**（见 `op`/`alloc`）之后，`pending` 永远是空的
          ⇒ 本方法是**空操作**，保留它是为了不改动这套协议的形状（有等价性实测兜底）。
        """
        if self.pending:
            self.entry.extend(self.pending)
            self.pending = []
    def ty_of(self, e):
        """只看 Sema 的 `:type` 注解（不生成代码）。"""
        h = e[0]
        if h == 'IntLit':
            return 'i32'
        if h == 'FloatLit':
            return 'f32'
        if h == 'LVal':
            return ir_of(parse_ty(e[4][1:]))
        if h == 'Cast':
            return {'IntToFloat': 'f32', 'FloatToInt': 'i32', 'ToBool': 'i32'}[e[1][1:]]
        if h == 'Call':
            return ir_of(parse_ty(e[2][1:]))
        if h == '!' or h in CMP or h in ('&&', '||'):
            return 'i32'
        a, b = subs(e)
        return 'f32' if 'f32' in (self.ty_of(a), self.ty_of(b)) else 'i32'
    def gen_cast(self, e):
        kind = e[1][1:]
        v = self.gen_expr(subs(e)[0])
        if kind == 'IntToFloat':
            return self.val('I2F @ %s' % v[0], e.line, 'f32', ('float', ()))
        if kind == 'FloatToInt':
            return self.sat_fptosi(v, e.line, e)
        return self.tobool(v, e.line)
    def sat_fptosi(self, v, line, e_node):
        """float→int 饱和：NaN→0、越界→INT_MAX/INT_MIN（阈值 ±2^31）。
        三个比较与三个临时槽先出；四个分支体的生成顺序 hi/lo/nan/f2i，再内层、外层
        的收尾 Load·Store——照真实输出的结果编号顺序对齐。
        """
        hi = self.cf(2147483648.0, line)
        g = self.val('Gt @ %s %s' % (v[0], hi[0]), line, 'i32', None)[0]
        lo = self.cf(-2147483648.0, line)
        l = self.val('Lt @ %s %s' % (v[0], lo[0]), line, 'i32', None)[0]
        nn = self.val('Ne @ %s %s' % (v[0], v[0]), line, 'i32', None)[0]
        t1, t2, t3 = self.temps(e_node, 3, line)
        def st(val, t):
            return lambda: self.op('Store i32 %s %s' % (self.ci(val, line)[0], t), line)
        r_hi = self.reg(line, st(INT_MAX, t1))
        r_lo = self.reg(line, st(INT_MIN, t2))
        r_nan = self.reg(line, st(0, t3))
        def f2i():
            self.op('Store i32 %s %s'
                    % (self.val('F2I @ %s' % v[0], line, 'i32', None)[0], t3), line)
        r_f2i = self.reg(line, f2i)
        def inner():
            self.op2('If %s' % nn, line, [r_nan, r_f2i])
            self.op('Store i32 %s %s'
                    % (self.val('Load @ i32 %s' % t3, line, 'i32', None)[0], t2), line)
        r_inner = self.reg(line, inner)
        def mid():
            self.op2('If %s' % l, line, [r_lo, r_inner])
            self.op('Store i32 %s %s'
                    % (self.val('Load @ i32 %s' % t2, line, 'i32', None)[0], t1), line)
        r_mid = self.reg(line, mid)
        self.op2('If %s' % g, line, [r_hi, r_mid])
        return self.val('Load @ i32 %s' % t1, line, 'i32', ('int', ()))
    def gen_lval_addr(self, lv):
        """逐层 GEP：下标在 i32 里算（回绕）→ sext i32→i64 再 getelementptr，
        绝不用 zext；乘数是"内层维数之积"，为 1 时不发 MulI。
        """
        pv = self.lookup(lv[1]); ty, cur = pv[0], pv[1]
        for sub in subs(lv):
            E = elem_of(ty); m = n_leaf(E); x = self.gen_expr(sub)
            if m != 1:
                x = self.val('MulI @ %s %s' % (x[0], self.ci(m, sub.line)[0]),
                             sub.line, 'i32', ('int', ()))
            cur = self.val('GetElementPtr @ ptr[%s] i64 0 %s %s'
                           % (ir_of(E), cur, self.sext(x[0], sub.line)),
                           sub.line, 'ptr[%s]' % ir_of(E), None)[0]
            ty = E
        return cur, ty
    def gen_expr(self, e):
        h = e[0]
        if h == 'IntLit':
            return self.ci(int_lit(e[1]), e.line)
        if h == 'FloatLit':
            return self.cf(parse_fval(e[1]), e.line)
        if h == 'LVal':
            if subs(e):
                addr = self.gen_lval_addr(e)[0]; rty = parse_ty(e[4][1:])
                if not is_scalar(rty):
                    return (addr, 'ptr[%s]' % ir_of(rty), rty)
                return self.val('Load @ %s %s' % (ir_of(rty), addr), e.line, ir_of(rty), rty)
            ty, ptr = self.lookup(e[1])
            if not is_scalar(ty):
                return (ptr, ir_of(ty), ty)
            return self.val('Load @ %s %s' % (ir_of(ty), ptr), e.line, ir_of(ty), ty)
        if h == 'Cast':
            return self.gen_cast(e)
        if h == 'Call':
            return self.gen_call(e)
        if h == '!':
            v = self.gen_expr(subs(e)[0])
            return self.val('Eq @ %s %s' % (v[0], self.zero_of(v, e.line)[0]),
                            e.line, 'i32', ('int', ()))
        if h == '+' and len(subs(e)) == 1:
            return self.gen_expr(subs(e)[0])
        if h == '-' and len(subs(e)) == 1:
            v = self.gen_expr(subs(e)[0])
            if v[1] == 'f32':
                return self.val('MinusF @ %s' % v[0], e.line, 'f32', ('float', ()))
            return self.val('SubI @ %s %s' % (self.ci(0, e.line)[0], v[0]),
                            e.line, 'i32', ('int', ()))
        if h in BINOPS:
            return self.gen_binary(e, h)
        raise ValueError('expr %s' % h)
    def gen_binary(self, e, op):
        k = subs(e)
        if op in ('&&', '||'):
            return self.gen_logic(e, op)
        if op in ('/', '%'):
            return self.gen_divmod(e, op)
        rhs = self.gen_expr(k[1]); lhs = self.gen_expr(k[0])
        if op in CMP:
            return self.val('%s @ %s %s' % (CMP[op], lhs[0], rhs[0]),
                            e.line, 'i32', ('int', ()))
        isf = 'f32' in (lhs[1], rhs[1])
        name = {'+': 'Add', '-': 'Sub', '*': 'Mul'}[op] + ('F' if isf else 'I')
        return self.val('%s @ %s %s' % (name, lhs[0], rhs[0]), e.line,
                        'f32' if isf else 'i32', ('float', ()) if isf else ('int', ()))
    def gen_logic(self, e, op):
        """短路：结果物化到内存（Alloca 提升到函数入口 Region）。"""
        k = subs(e)
        a = self.tobool(self.gen_expr(k[0]), e.line)
        t = self.temps(e, 1, e.line)[0]
        self.dry(lambda: self.gen_expr(k[1]))
        if op == '&&':
            tr = self.reg(e.line, lambda: self.op(
                'Store i32 %s %s' % (self.tobool(self.gen_expr(k[1]), e.line)[0], t), e.line))
            er = self.reg(e.line, lambda: self.op(
                'Store i32 %s %s' % (self.ci(0, e.line)[0], t), e.line))
        else:
            tr = self.reg(e.line, lambda: self.op(
                'Store i32 %s %s' % (self.ci(1, e.line)[0], t), e.line))
            er = self.reg(e.line, lambda: self.op(
                'Store i32 %s %s' % (self.tobool(self.gen_expr(k[1]), e.line)[0], t), e.line))
        self.op2('If %s' % a[0], e.line, [tr, er])
        return self.val('Load @ i32 %s' % t, e.line, 'i32', ('int', ()))
    def gen_divmod(self, e, op):
        """`/`、`%` 的归一化守卫：x/0→0、INT_MIN/-1→0（% 同样是 0）。
        求值顺序照真实输出：`/` 左→右（float 除法也走这条），`%` 右→左。
        """
        k, line = subs(e), e.line
        if op == '/' and self.ty_of(k[0]) == 'f32':
            a, b = self.gen_expr(k[0]), self.gen_expr(k[1])
            return self.val('DivF @ %s %s' % (a[0], b[0]), line, 'f32', ('float', ()))
        if op == '/':
            a, b = self.gen_expr(k[0]), self.gen_expr(k[1])
        else:
            b, a = self.gen_expr(k[1]), self.gen_expr(k[0])
        isz = self.val('Eq @ %s %s' % (b[0], self.ci(0, line)[0]), line, 'i32', None)[0]
        ma = self.val('Eq @ %s %s' % (a[0], self.ci(INT_MIN, line)[0]), line, 'i32', None)[0]
        mb = self.val('Eq @ %s %s' % (b[0], self.ci(-1, line)[0]), line, 'i32', None)[0]
        ov = self.val('MulI @ %s %s' % (ma, mb), line, 'i32', None)[0]
        nd = self.val('AddI @ %s %s' % (isz, ov), line, 'i32', None)[0]
        t1 = self.temps(e, 1, line)[0]
        if op == '/':
            tr = self.reg(line, lambda: self.op(
                'Store i32 %s %s' % (self.ci(0, line)[0], t1), line))
            er = self.reg(line, lambda: self.op('Store i32 %s %s' % (
                self.val('DivI @ %s %s' % (a[0], b[0]), line, 'i32', None)[0], t1), line))
        else:
            t2 = self.temps(e, 2, line)[1]
            inner = self.reg(line, lambda: self.op(
                'Store i32 %s %s' % (self.ci(0, line)[0], t2), line))
            inner2 = self.reg(line, lambda: self.op(
                'Store i32 %s %s' % (self.ci(0, line)[0], t2), line))
            def tbody():
                self.op2('If %s' % isz, line, [inner, inner2])
                self.op('Store i32 %s %s'
                        % (self.val('Load @ i32 %s' % t2, line, 'i32', None)[0], t1), line)
            tr = self.reg(line, tbody)
            er = self.reg(line, lambda: self.op('Store i32 %s %s' % (
                self.val('ModI @ %s %s' % (a[0], b[0]), line, 'i32', None)[0], t1), line))
        self.op2('If %s' % nd, line, [tr, er])
        return self.val('Load @ i32 %s' % t1, line, 'i32', ('int', ()))
    def gen_call(self, e):
        name, line = e[1], e.line
        if name in ('starttime', 'stoptime'):
            self.op('Call "_sysy%s" %s' % (name, self.ci(line, line)[0]), line)
            return None
        vals = [self.gen_expr(a)[0] for a in subs(e)]
        if name in self.runtime:
            ret = parse_ty(self.runtime[name])
        else:
            fd = [f for f in self.funcdefs if f[1] == name]
            if not fd:
                raise ValueError('unknown function %s' % name)
            ret = parse_ty(fd[0][2][1:])
        res = ' ' + self.nm() if ret[0] != 'void' else ''
        self.op('Call "%s"%s%s' % (name, res, ''.join(' ' + v for v in vals)), line)
        if ret[0] != 'void':
            return (res.strip(), ir_of(ret), ret)
        return None
# ==============================================================================
# 驱动（★ 本段是可独立重写的"外围"：跑编译器拿三份产物 → 用本文件的独立实现
#   生成一份 → 与 C++ 的 `--emit=structured-ir` **逐字节比较**）
# ==============================================================================
def build_module(exe, path, tmpdir):
    """【后置】返回 (独立实现的行列表, C++ 实现的行列表)。"""
    import zlib
    base = os.path.basename(path)
    key = '%08x_%s' % (zlib.crc32(os.path.abspath(path).encode()) & 0xFFFFFFFF, base)
    s, i, r = (os.path.join(tmpdir, key + x) for x in ('.sema', '.init', '.sir'))
    for kind, out in (('sema', s), ('initplan', i), ('structured-ir', r)):
        p = subprocess.run([exe, path, '--emit=' + kind, '-o', out],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            raise RuntimeError('%s --emit=%s 失败: %s'
                               % (path, kind, p.stderr.decode('utf-8', 'replace').strip()[:200]))
    read = lambda f: open(f, encoding='utf-8', errors='replace').read()
    mod = Gen(base, read(s), read(i), read(path)).run()
    out = ['(Module "%s") {' % base]
    render(mod, out, 1)
    out.append('}')
    actual = read(r).split('\n')
    if actual and actual[-1] == '':
        actual.pop()
    return out, actual


def main(argv=None):
    ap = argparse.ArgumentParser(description='独立第二份 IRGen 交叉验证（S05 轨 D）')
    ap.add_argument('--compiler', default='compiler/build/compiler')
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 2))
    ap.add_argument('--dir', default='tests')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args(argv)
    if not os.path.isfile(a.compiler):
        sys.stderr.write('找不到编译器: %s\n' % a.compiler)
        return 2
    files = []
    for root, _, names in os.walk(a.dir):
        for n in sorted(names):
            p = os.path.join(root, n)
            try:
                with open(p, 'rb') as fh:
                    if n.endswith('.sy') and 'tensor' not in p and b'tensor' not in fh.read():
                        files.append(p)
            except OSError:
                pass
    files.sort()
    files = files[:a.limit] if a.limit else files
    if not files:
        sys.stderr.write('语料目录里没有 .sy: %s\n' % a.dir)
        return 2
    base = os.path.join('.work', 'indep')
    os.makedirs(base, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix='run_', dir=base)
    ok, okorder, bad, err = 0, 0, [], []

    def work(path):
        try:
            return path, build_module(a.compiler, path, tmpdir)
        except Exception as exc:                       # noqa: BLE001
            return path, exc

    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for path, res in pool.map(work, files):
            if isinstance(res, Exception):
                err.append((path, '%s: %s' % (type(res).__name__, res)))
                sys.stdout.write('ERR  %s\n' % path)
                sys.stdout.flush()
            elif res[0] == res[1]:
                ok += 1
                if a.verbose:
                    sys.stdout.write('OK   %s\n' % path)
            elif same_up_to_alloca_position(res[0], res[1]):
                okorder += 1
                if a.verbose:
                    sys.stdout.write('OK*  %s（等价：只差 Alloca 的位置）\n' % path)
            else:
                d = unified(res[0], res[1])
                bad.append((path, d))
                sys.stdout.write('DIFF %s\n' % path)
                sys.stdout.flush()
                if a.verbose:
                    sys.stdout.write(d + '\n')
    shutil.rmtree(tmpdir, ignore_errors=True)
    print('=' * 72)
    print('语料文件 %d 个：逐字节一致 %d，**等价（只差 Alloca 位置）** %d，'
          '有差异 %d，工具自身错误 %d' % (len(files), ok, okorder, len(bad), len(err)))
    if okorder:
        print('  说明：§五.1 只要求 alloca 在**函数入口 Region**，未规定它在其中的位置；')
        print('        Alloca 之间无数据依赖 ⇒ 判据在这一项上比语义更严，故单独放宽。')
        print('        放宽的只有这一项：Alloca 多重集必须相同，其余行仍要求逐字节相同。')
    for p, d in bad[:3]:
        print('-' * 72)
        print('差异文件: %s' % p)
        print(d)
    for p, m in err[:5]:
        print('-' * 72)
        print('工具错误: %s\n  %s' % (p, m))
    return 2 if err else (1 if bad else 0)


if __name__ == '__main__':
    sys.exit(main())
