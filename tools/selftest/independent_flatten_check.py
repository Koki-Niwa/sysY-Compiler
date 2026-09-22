#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
independent_flatten_check.py —— 轨 D：**独立实现的第二份 FlattenCFG**
用法（关卡照这个签名调用）：
    python3 independent_flatten_check.py --compiler <路径> [--jobs N] [--limit N]
                                         [--dir <语料目录>] [--verbose]
    python3 independent_flatten_check.py --compiler <路径> --dump-diff <文件.sy>
    python3 independent_flatten_check.py --compiler <路径> --selfcheck [--jobs N]
做什么：读 `--emit=structured-ir --normalize` 的 dump（结构化 IR：变量在内存里、控制流是
树），按 `docs/handoff/03-设计/平面IR与dump格式.md` **§6** 自己展平成平面 CFG，再与 C++
`--emit=flat-ir` 的产物**逐字节比较**（`--selfcheck` 不比对，只做自洽检查）。退出码：
0 = 全部逐字节一致、1 = 至少一处差异或错误、2 = 工具自身错误。
★ 纪律：写本文件**之前不读** `structured/FlattenCFG.{h,cpp}`、`ir/FlatDump.cpp`、
`.reference/`（已遵守：§6.1–§6.4 全部按文档正文独立推导）；只把 §2–§5 当格式/命名约定；
差异逐条归因，不照抄 C++。
★ 判据（§6）：① 函数一个入口块 `L0`，容器入口/汇合点各成一块，`(Yield)` = 跳到本 Region
续点，`(Return)`/`(Unreachable)` 结束当前块；② `While`/`For` 按 §6.2 降级；③ φ 只在**前驱
数 ≥ 2** 的块、只对"在分支里被 store 过、又被 load 过"的**槽**放，值全同不放、缺值补零；
④ 临界边（源块多后继 **且** 目标块多前驱）插一个只有 br 的空块拆开。格式取舍见
`Flattener` 的注释与报告：φ 入值**空格**分隔（§5"拼法"列；§2 概览写逗号）、`br` 目标块
不带 `%`（§4.2）、常量在**第一次引用处**打印定义行、全局地址当常量（没有 getglobal）。
零第三方依赖（Python 3 标准库），可直接从源码运行（不生成 .pyc/marshal）。
"""
import argparse
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loopnorm_ir as L  # noqa: E402  （**纯解析器**，不含任何展平语义）
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
ICMP_PRED = {'Eq': 'eq', 'Ne': 'ne', 'Lt': 'slt', 'Le': 'sle', 'Gt': 'sgt', 'Ge': 'sge'}
FCMP_PRED = {'Eq': 'oeq', 'Ne': 'une', 'Lt': 'olt', 'Le': 'ole', 'Gt': 'ogt', 'Ge': 'oge'}
INT_BIN = {'AddI': 'add', 'SubI': 'sub', 'MulI': 'mul', 'DivI': 'sdiv', 'ModI': 'srem'}
FLT_BIN = {'AddF': 'fadd', 'SubF': 'fsub', 'MulF': 'fmul', 'DivF': 'fdiv'}
CASTS = {'I2F': ('f32', 'sitofp i32 ', ' to f32'), 'F2I': ('i32', 'fptosi f32 ', ' to i32'),
         'Sext': ('i64', 'sext i32 ', ' to i64')}
ZERO_TEXT = {'i32': '0', 'i64': '0', 'f32': '0x0p+0'}
SCALAR_TY = ('i32', 'f32', 'i64')
CONST_DEF = re.compile(r'^  %\d+ = (i32|i64|f32|ptr\[)')     # 常量/全局地址的定义行
BR1 = re.compile(r'^  br (L\d+)(?: @line \d+)?$')
BR2 = re.compile(r'^  br i1 %(\d+), label (L\d+), label (L\d+)(?: @line \d+)?$')
PHI = re.compile(r'^  %(\d+) = phi (\S+) \[(.*?)\](?: @line \d+)?$')
DEF = re.compile(r'^  %(\d+) = (.*)$')
TERM = re.compile(r'^  (br |ret |unreachable(\s|$))')

class FlattenError(Exception):
    """本实现拒绝展平的输入（畸形 / 超出结构化层规格）。"""

def norm_ty(t):
    """类型拼法规范化：共用的结构化解析器在**嵌套** `[` 后会留下空格
    （`ptr [[2 x i32]]`），而 C++ 两侧的类型打印都是 `ptr[[2 x i32]]`（§3：两层同一个
    拼法）⇒ 这里统一去掉 `[` 后、`]` 前的空白。"""
    t = re.sub(r'\[\s+', '[', t)              # `[ 3 x i32]` → `[3 x i32]`
    t = re.sub(r'\s+\]', ']', t)              # `[3 x i32 ]` → `[3 x i32]`
    return re.sub(r'\bptr\s+\[', 'ptr[', t)   # `ptr [i32]` → `ptr[i32]`


def ptr_to(t):
    return 'ptr[%s]' % t

def pointee(t):
    return t[4:-1] if (t.startswith('ptr[') and t.endswith(']')) else t

def fbits(text):
    """f32 常量的**位模式**（去重用：`-0.0`/`+0.0` 位模式不同、`nan != nan`）。"""
    try: return struct.pack('>f', float.fromhex(text))
    except (ValueError, OverflowError): return text.encode('utf-8')

def srcline(op):
    """源码行号：取 attrs 里的 `@line N`（**不是** `op.line` —— 那是 dump 自身的行号）。"""
    a = op.attrs
    for i in range(len(a) - 2, -1, -1):
        if a[i] == '@line':
            try: return int(a[i + 1])
            except (ValueError, IndexError): return 0
    return 0

def func_sig(op):
    """`(Func "f" :ret i32 :param [ptr[i32], i32] @line 4)` → ('i32', ['ptr[i32]', 'i32'])"""
    ret, params, a = 'void', [], op.attrs
    for i, t in enumerate(a):
        if t == ':ret' and i + 1 < len(a): ret = norm_ty(a[i + 1])
        elif t == ':param' and i + 1 < len(a):
            s = a[i + 1].strip()
            s = s[1:-1] if (s.startswith('[') and s.endswith(']')) else s
            params = [norm_ty(x.strip()) for x in s.split(',') if x.strip()]
    return ret, params

# ── 平面层的值模型（先建结构，**最后**统一编号：编号只依赖最终结构）───────────

class V(object):
    """一个平面值。`k`：'p' 形参 / 'c' 常量 / 'g' 全局地址 / 'i' 指令 / 'f' φ / 'e' Cell。
    `ty` 类型、`num` 编号（编号遍填）、`resolved`（只有 Cell 用）、`parts/text/name/line/
    incoming/block` 按种类使用。"""
    __slots__ = ('k', 'ty', 'num', 'resolved', 'parts', 'text', 'name', 'line',
                 'incoming', 'block')
    def __init__(self, k, ty):
        self.k, self.ty, self.num, self.resolved = k, ty, None, None
        self.parts, self.text, self.name, self.line = None, None, None, 0
        self.incoming, self.block = [], None

class Block(object):
    __slots__ = ('phis', 'insts', 'term', 'idx')
    def __init__(self):
        self.phis, self.insts, self.term, self.idx = [], [], None, -1

class Typed(object):
    """`<类型> %N` 形式的操作数（只用于 call 的实参：§5 第 33 行的形式）。"""
    __slots__ = ('val',)
    def __init__(self, val):
        self.val = val

class Func(object):
    __slots__ = ('name', 'ret', 'params', 'blocks')
    def __init__(self, name, ret, params):
        self.name, self.ret, self.params, self.blocks = name, ret, params, []

def mk(ty, parts, line):
    v = V('i', ty)
    v.parts, v.line = parts, line
    return v

def const(ty, text, line):
    v = V('c', ty)
    v.text, v.line = text, line
    return v

def gref(ty, name, line):
    v = V('g', ty)
    v.name, v.line = name, line
    return v

def deref(v):
    """跟着 Cell 的 resolve 链找到最终值（`%N` 只认最终值）。"""
    for _ in range(64):
        if v is not None and v.k == 'e' and v.resolved is not None: v = v.resolved
        else: return v
    raise FlattenError('Cell 解析链过深（疑似自环）')

def dsty(v):
    return deref(v).ty

def items_of(b):
    return b.phis + b.insts + ([b.term] if b.term else [])

def operands_of(item):
    """一条指令/终结符/φ 的操作数（按出现顺序；φ 的入值也算操作数）。"""
    if item.k == 'f': return [v for v, _pb in item.incoming]
    out = []
    for p in (item.parts or []):
        out.append(p.val if isinstance(p, Typed) else p if isinstance(p, V) else None)
    return [v for v in out if v is not None]

class Flattener(object):
    """一份结构化 dump → 一份平面 IR。
    ★ φ 的放置判据（§6.3，先写成注释再写代码）：
      ① 只在**前驱数 ≥ 2** 的块放：If 的 join、While/For 的循环头、循环出口；
      ② 只对"在分支里被 store 过、又在别处被 load 过"的**槽**（= 一个 alloca 结果，
         允许经 `getelementptr` 常量 0 下标折回）放；只写不读的槽**一个 φ 都不放**；
      ③ 每个 φ 至少有一个非自身入值（循环头 φ 的入值来自循环前与循环体）。
      实现走"变量版本"的 env（槽 → 当前值，不建支配树）：store 到一个"被读过"的槽 ⇒ 更新
      env（指令照发：不做 mem2reg，铁律 1）；load 一个"被读过"的槽 ⇒ 直接取 env 的值
      （**不发 load**）；汇合点各入边值全同就不放 φ，缺值的入边补该类型的零常量。
    """
    def __init__(self, names, fnret, fop):
        self.names, self.fnret, self.fop = names, fnret, fop
        self.ret, self.pty = func_sig(fop)
        self.fn = Func(fop.name, self.ret, [V('p', t) for t in self.pty])
        self.vmap, self.consts, self.grefs, self.loops = {}, {}, {}, []
        self.tracked_flat = {}   # 平面槽值 → True（按 alloca 出现顺序，deterministic）
        self._prescan()
        self.cur = self.new_block()                  # L0 = 入口块
    def _prescan(self):
        """哪些槽是"被读过"的（判据 2 的过滤器）+ For 的 IV 槽。"""
        self.read, self.brwrite, self.forslot, self.slot_order = set(), set(), set(), []
        def visit(ops, depth):
            for op in ops:
                k = op.kind
                if k == 'Alloca' and op.attrs and op.attrs[0] in SCALAR_TY:
                    self.slot_order.append(op)
                elif k == 'For' and op.operands:
                    s = self.root_slot(op.operands[0])
                    self.forslot.add(id(s)) if s is not None else None
                elif k in ('Load', 'Store') and op.operands:
                    s = self.root_slot(op.operands[-1])
                    if s is not None:
                        if k == 'Load': self.read.add(id(s))
                        elif depth > 0: self.brwrite.add(id(s))
                for reg in op.regions:
                    visit(reg, depth + (1 if k in ('If', 'While', 'For') else 0))
        visit(self.fop.regions[0] if self.fop.regions else [], 0)
        # ★ 判据 2：只写不读的槽不跟踪（"只写不读 ⇒ 一个 φ 都不放"）；被读过的槽才进
        # env（§6.3"实现方式"）；For 的 IV 槽**强制**跟踪（§6.2 明文要 φ）。
        self.tracked = set(op for op in self.slot_order
                           if id(op) in self.forslot or id(op) in self.read)
    def root_slot(self, pname):
        """`%p` 的**根槽**：沿 `getelementptr`（常量 0 下标）回到 `alloca`；不是本地标量
        槽 ⇒ None（数组/聚合槽不折：元素访问不是"槽的值"）。"""
        d = self.names.get(pname)
        if d is None: return None
        if d.kind == 'Alloca': return d if (d.attrs and d.attrs[0] in SCALAR_TY) else None
        if d.kind == 'GetElementPtr' and len(d.operands) == 2:
            b = self.names.get(d.operands[0])
            if b is not None and b.kind == 'Alloca' and b.attrs and b.attrs[0] in SCALAR_TY \
                    and self._is_zero(d.operands[1]):
                return b
        return None
    def _is_zero(self, name):
        d = self.names.get(name)
        if d is None: return False
        if d.kind == 'Int': return bool(d.attrs) and d.attrs[0] == '0'
        return self._is_zero(d.operands[0]) if (d.kind == 'Sext' and d.operands) else False
    def call_ret(self, callee):
        if callee in self.fnret: return self.fnret[callee]
        if callee in L.VOID_CALLS or callee in L.RUNTIME_VOID or callee.startswith('_sysy'):
            return 'void'
        return L.RUNTIME_SIG[callee][0] if callee in L.RUNTIME_SIG else 'i32'
    def val(self, name):
        """结构化结果名 → 平面值；全局地址在这里**按需物化**（每个函数一份）。"""
        if name in self.vmap: return self.vmap[name]
        d = self.names.get(name)
        if d is not None and d.kind == 'GetGlobal' and len(d.attrs) > 1:
            key = (ptr_to(norm_ty(d.attrs[1])), d.name)
            if key not in self.grefs: self.grefs[key] = gref(key[0], d.name, srcline(d))
            self.vmap[name] = self.grefs[key]
            return self.grefs[key]
        raise FlattenError('未定义的值 %s' % name)
    def const(self, ty, text, line):
        key = (ty, fbits(text) if ty == 'f32' else text)
        if key not in self.consts: self.consts[key] = const(ty, text, line)
        return self.consts[key]
    def zero(self, ty, line):
        if ty not in ZERO_TEXT: raise FlattenError('类型 %s 没有零常量（本实现不跟踪这类槽）' % ty)
        return self.const(ty, ZERO_TEXT[ty], line)
    def new_block(self):
        b = Block()
        self.fn.blocks.append(b)
        return b
    def emit(self, inst):
        if self.cur is None:          # 终结符之后还有 Op（不该发生）→ 补一个不可达块
            self.cur = self.new_block()
        self.cur.insts.append(inst)
        return inst
    def term(self, parts, line, env):
        if self.cur is not None:
            self.cur.term = mk(None, parts, line)
            self.cur = None
    def run_region(self, ops, env):
        """【后置】展平到 self.cur；返回 ('yield', 值, 行) / ('break'|'return'|
        'unreachable'|'fell', None, 行)。"""
        for op in ops:
            k = op.kind
            if k == 'Yield':
                return ('yield', self.val(op.operands[0]) if op.operands else None,
                        srcline(op))
            if k == 'Break':
                if not self.loops: raise FlattenError('Break 不在循环里')
                self.loops[-1]['breaks'].append((self.cur, dict(env)))
                self.term(['br ', self.loops[-1]['exit']], srcline(op), env)
                return ('break', None, srcline(op))
            if k == 'Return':
                v = self.val(op.operands[0]) if op.operands else None
                self.term(['ret void'] if v is None else ['ret %s ' % dsty(v), v],
                          srcline(op), env)
                return ('return', None, srcline(op))
            if k == 'Unreachable':
                self.term(['unreachable'], srcline(op), env)
                return ('unreachable', None, srcline(op))
            if k == 'If': self.do_if(op, env)
            elif k == 'While': self.do_while(op, env)
            elif k == 'For': self.do_for(op, env)
            else: self.do_simple(op, env)
        return ('fell', None, srcline(ops[-1]) if ops else 0)
    def do_simple(self, op, env):
        k, a, line = op.kind, [norm_ty(x) for x in op.attrs], srcline(op)
        res = op.results[0] if op.results else None
        v0 = self.val(op.operands[0]) if op.operands else None
        v1 = self.val(op.operands[1]) if len(op.operands) > 1 else None
        if k == 'GetArg':
            idx = int(a[0]) if a else 0
            if idx >= len(self.fn.params): raise FlattenError('GetArg 下标越界：%d' % idx)
            self.vmap[res] = self.fn.params[idx]
        elif k in ('Int', 'Float'):
            self.vmap[res] = self.const('i32' if k == 'Int' else 'f32',
                                        a[0] if a else '0', line)
        elif k == 'Alloca':
            obj = a[0] if a else 'i32'
            self.vmap[res] = self.emit(mk(ptr_to(obj), ['alloca %s' % obj], line))
            if op in self.tracked: self.tracked_flat[self.vmap[res]] = True
        elif k == 'GetGlobal':                           # 模块级地址：按需物化
            if res is not None: self.vmap[res] = self.val(res)
        elif k in ('Load', 'Store'):
            ty = a[0] if a else 'i32'
            slot = self.root_slot(op.operands[-1])
            sv = self.vmap.get(slot.results[0]) if slot is not None else None
            if k == 'Load' and sv is not None and sv in env:
                self.vmap[res] = env[sv]                 # ★ 判据 2 的 load 替换
            elif k == 'Load':
                self.vmap[res] = self.emit(
                    mk(ty, ['load %s, ptr[%s] ' % (ty, ty), v0], line))
            else:
                self.emit(mk(None, ['store %s ' % ty, v0, ', ptr[%s] ' % ty, v1], line))
                if sv is not None and sv in self.tracked_flat: env[sv] = v0
        elif k == 'GetElementPtr':
            ety = a[0] if a else 'i32'
            self.vmap[res] = self.emit(mk(ptr_to(ety), [
                'getelementptr %s, ptr[%s] ' % (ety, ety), v0, ', i64 ', v1], line))
        elif k == 'Bitcast':
            dst = a[0] if a else ptr_to('i32')
            src = dsty(v0)
            self.vmap[res] = self.emit(mk(dst, [
                'bitcast %s ' % (src if src.startswith('ptr[') else ptr_to(src)),
                v0, ' to %s' % dst], line))
        elif k == 'Call':
            ret = self.call_ret(op.name)
            parts = ['call %s @%s(' % (ret, op.name)]
            for i, an in enumerate(op.operands):
                parts.append(', ' if i else '')
                parts.append(Typed(self.val(an)))
            parts.append(')')
            inst = self.emit(mk(None if res is None else ret, parts, line))
            if res is not None: self.vmap[res] = inst
        elif k in INT_BIN or k in FLT_BIN:
            ty = 'f32' if k in FLT_BIN else 'i32'
            self.vmap[res] = self.emit(mk(ty, [
                '%s %s ' % (FLT_BIN.get(k, INT_BIN.get(k)), ty), v0, ', ', v1], line))
        elif k == 'MinusF':
            self.vmap[res] = self.emit(mk('f32', ['fneg f32 ', v0], line))
        elif k == 'MinusI':                              # §5：没有 neg，取负用 sub 0, x
            self.vmap[res] = self.emit(mk('i32', [
                'sub i32 ', self.zero('i32', line), ', ', v0], line))
        elif k in ICMP_PRED:
            ty = dsty(v0)
            self.vmap[res] = self.emit(mk('i32', [
                ('fcmp %s f32 ' % FCMP_PRED[k] if ty == 'f32'
                 else 'icmp %s %s ' % (ICMP_PRED[k], ty)), v0, ', ', v1], line))
        elif k in CASTS:
            spec = CASTS[k]
            self.vmap[res] = self.emit(mk(spec[0], [spec[1], v0, spec[2]], line))
        else: raise FlattenError('不支持的 Op：%s' % k)
    def do_if(self, op, env):
        """If：cond 在当前块求值 → then/else/join 三块；两个 Region 的续点 = join。"""
        c = self.val(op.operands[0])
        lt, le, lj = self.new_block(), self.new_block(), self.new_block()
        self.term(['br i1 ', c, ', label ', lt, ', label ', le], srcline(op), env)
        inc = []
        for blk, reg in ((lt, op.regions[0]),
                         (le, op.regions[1] if len(op.regions) > 1 else [])):
            self.cur = blk
            e = dict(env)
            r = self.run_region(reg, e)
            if r[0] in ('yield', 'fell'):
                pred = self.cur            # 真正发出 br 的块（可能是嵌套容器的汇合块）
                self.term(['br ', lj], r[2] or srcline(op), e)
                inc.append((pred, e))
        self.cur = lj
        env.clear()
        env.update(self.merge(lj, inc, srcline(op)))
    def merge(self, block, incoming, line):
        """汇合：前驱 ≥ 2 才可能有 φ；值全同不放；缺值补零（判据 1/2/3）。"""
        if not incoming: return {}
        if len(incoming) == 1: return dict(incoming[0][1])
        out, slots = {}, []
        for _pb, e in incoming:
            for s in e:
                if s not in slots: slots.append(s)
        for slot in slots:
            vals = []
            for _pb, e in incoming:
                v = deref(e.get(slot))
                if v is None:
                    if slot not in self.tracked_flat:
                        vals = None
                        break
                    v = self.zero(pointee(dsty(slot)), line)
                vals.append(v)
            if vals is None: continue   # 未跟踪的槽且各边值不一致 ⇒ 不发 φ（照发真 load）
            if all(v is vals[0] for v in vals): out[slot] = vals[0]
            else:
                ph = V('f', pointee(dsty(slot)))
                ph.incoming = [(v, pb) for v, (pb, _e) in zip(vals, incoming)]
                block.phis.append(ph)
                out[slot] = ph
        return out
    def resolve(self, cells, incoming):
        """循环头入边（**入口边在前**）：值全同 ⇒ 退化；缺值/自身入值 ⇒ 用入口边的值。"""
        for slot, c in cells.items():
            entry = deref(incoming[0][1].get(slot))
            if entry is None: entry = self.zero(c.ty, 0)
            vals = []
            for _pb, e in incoming:
                v = deref(e.get(slot))
                vals.append(entry if (v is None or v is c) else v)
            c.incoming = [(v, pb) for v, (pb, _e) in zip(vals, incoming)]
            if all(v is vals[0] for v in vals): c.resolved = vals[0]
            else:
                c.resolved = V('f', c.ty)
                c.resolved.incoming = list(c.incoming)
                c.block.phis.append(c.resolved)
    def head_cells(self, lh, env_lp):
        """循环头：所有被跟踪的槽先放 Cell 占位（回边的值展平体之后才知道）。"""
        henv, cells = dict(env_lp), {}
        for slot in self.tracked_flat:
            cells[slot] = henv[slot] = V('e', pointee(dsty(slot)))
            cells[slot].block = lh
        return henv, cells
    def do_while(self, op, env):
        """While：Lh（条件 Region）→ br i1 → Lb（体，尾回跳 Lh）/ Lx（出口）。"""
        lp, env_lp = self.cur, dict(env)
        lh = self.new_block()
        self.term(['br ', lh], srcline(op), env_lp)
        henv, cells = self.head_cells(lh, env_lp)
        lb, lx = self.new_block(), self.new_block()
        self.loops.append({'exit': lx, 'breaks': []})
        self.cur = lb
        benv = dict(henv)
        r = self.run_region(op.regions[1] if len(op.regions) > 1 else [], benv)
        back = r[0] in ('yield', 'fell')
        pred = self.cur                         # 回边的真正前驱块
        if back: self.term(['br ', lh], r[2] or srcline(op), benv)
        lp_ = self.loops.pop()
        self.resolve(cells, [(lp, env_lp)] + ([(pred, benv)] if back else []))
        self.cur = lh                           # 循环头 = 条件 Region
        cenv = dict((s, deref(v)) for s, v in henv.items())
        r2 = self.run_region(op.regions[0] if op.regions else [], cenv)
        if r2[0] != 'yield' or r2[1] is None: raise FlattenError('While 条件 Region 没有 Yield 值')
        pred = self.cur
        self.term(['br i1 ', r2[1], ', label ', lb, ', label ', lx],
                  r2[2] or srcline(op), cenv)
        self.cur = lx
        env.clear()
        env.update(self.merge(lx, [(pred, cenv)] + lp_['breaks'], srcline(op)))
    def do_for(self, op, env):
        """For（§6.2）：Lp 求边界 → Lh 放 IV 的 φ + icmp → Lb 体 → Linc 步进写回槽 → Lx。"""
        if len(op.operands) < 4: raise FlattenError('For 缺操作数（要 [槽, lower, upper, step]）')
        slot = self.val(op.operands[0])
        lo, hi, st = (self.val(op.operands[i]) for i in (1, 2, 3))
        ty, line = dsty(lo), srcline(op)
        if slot not in self.tracked_flat:        # 兜底：IV 槽必须被跟踪
            self.tracked_flat[slot] = True
        lp, env_lp = self.cur, dict(env)
        env_lp[slot] = lo                       # 约定 6.2.3：%lo 是 Lp 边上的 IV 入值
        lh = self.new_block()
        self.term(['br ', lh], line, env_lp)
        henv, cells = self.head_cells(lh, env_lp)
        lb, linc, lx = self.new_block(), self.new_block(), self.new_block()
        ivnext = mk(ty, None, line)           # 先占位：Linc 里补 parts 并入列
        self.loops.append({'exit': lx, 'breaks': []})
        self.cur = lb
        benv = dict(henv)
        r = self.run_region(op.regions[0] if op.regions else [], benv)
        if r[0] in ('yield', 'fell'): self.term(['br ', linc], r[2] or line, benv)
        lp_ = self.loops.pop()
        self.cur = linc                         # Linc：步进 + IV 的最终值写回槽（6.2.1）
        ivnext.parts = ['%s %s ' % ('fadd' if ty == 'f32' else 'add', ty),
                        cells[slot], ', ', st]
        self.emit(ivnext)
        self.emit(mk(None, ['store %s ' % ty, ivnext, ', ptr[%s] ' % ty, slot], line))
        lenv = dict(benv)
        lenv[slot] = ivnext
        self.term(['br ', lh], line, lenv)
        self.cur = lh                           # 循环头：IV 的 φ + 条件判断 + 分支
        self.resolve({slot: cells[slot]}, [(lp, env_lp), (linc, lenv)])
        hres = dict((s, deref(v)) for s, v in henv.items())
        hres[slot] = cells[slot]
        cnd = self.emit(mk('i32', [('fcmp olt f32 ' if ty == 'f32'
                                      else 'icmp slt %s ' % ty),
                                     cells[slot], ', ', hi], line))
        self.term(['br i1 ', cnd, ', label ', lb, ', label ', lx], line, hres)
        self.resolve(dict((s, c) for s, c in cells.items() if s is not slot),
                     [(lp, env_lp), (linc, lenv)])
        self.cur = lx
        env.clear()
        env.update(self.merge(lx, [(lh, hres)] + lp_['breaks'], line))
    def finish(self):
        """收尾：两边都终结的 join（空块）补 `unreachable`，然后拆临界边。"""
        for b in self.fn.blocks:
            if b.term is None: b.term = mk(None, ['unreachable'], 0)
        self.split_critical()
        return self.fn
    def split_critical(self):
        """★ §6.4 临界边拆分：**临界边** = 源块有多个后继、目标块有多个前驱。等 φ 全放好
        后扫描全部有向边，为每个这样的 (src, dst) 追加一个**只有 br** 的空中转块（约定
        6.4.1：没有 @line；块号按扫描顺序追加在函数块列表末尾），并把 φ 的入值键从 src 改到
        中转块（约定 6.4.2）。"""
        blocks = list(self.fn.blocks)
        succ, pred = {}, {}
        for b in blocks:
            succ[b] = tg = []
            for t in [x for x in (b.term.parts if b.term else []) if isinstance(x, Block)]:
                if t not in tg: tg.append(t)
            for t in tg:
                pred.setdefault(t, [])
                if b not in pred[t]: pred[t].append(b)
        for src in blocks:
            if len(succ.get(src, [])) <= 1: continue
            for dst in succ.get(src, []):
                if len(pred.get(dst, [])) <= 1: continue
                mid = self.new_block()
                mid.term = mk(None, ['br ', dst], 0)
                src.term.parts = [mid if x is dst else x for x in src.term.parts]
                for ph in dst.phis:
                    ph.incoming = [(v, mid if pb is src else pb) for v, pb in ph.incoming]
                pred[dst] = [mid if x is src else x for x in pred.get(dst, [])]
                pred[mid], succ[mid] = [src], [dst]
                succ[src] = [mid if x is dst else x for x in succ[src]]

def flatten_dump(text):
    """【前置】`--emit=structured-ir --normalize` 的产物（可能畸形）。
    【后置】返回平面 IR 文本（末尾恰好一个换行）。畸形输入抛 ParseError/FlattenError。"""
    mod, names, fnret = L.parse_dump(text)
    modops = L.module_region(mod)
    lines = [global_line(op) for op in modops if op.kind == 'GlobalVar']
    for op in modops:
        if op.kind != 'Func': continue
        fl = Flattener(names, fnret, op)
        env = {}
        r = fl.run_region(op.regions[0] if op.regions else [], env)
        if r[0] == 'fell' and fl.cur is not None:      # 函数体没终结（不该发生）
            fl.term(['unreachable'], 0, env)
        lines.extend(render_function(fl.finish()))
    return '\n'.join(lines) + '\n'

def global_line(op):
    """`@g = global <类型> <初始化器> @line N`（§2/§4.4）。"""
    a, ty, init = op.attrs, 'i32', 'zeroinitializer'
    for i, t in enumerate(a):
        if t == ':type' and i + 1 < len(a): ty = norm_ty(a[i + 1])
        elif t == ':init' and i + 1 < len(a) and a[i + 1].lstrip('"') == 'data':
            it = a[i + 2:]
            if '@line' in it: it = it[:it.index('@line')]
            it = [x for x in it if x not in ('{', '}', ',', '=')]
            init = '{ ' + ', '.join('%s=%s' % (it[x], it[x + 1])
                                    for x in range(0, len(it) - 1, 2)) + ' }'
    n = srcline(op)
    return '@%s = global %s %s%s' % (op.name, ty, init, ' @line %d' % n if n else '')

def render_function(fn):
    """为什么必须**两遍**：循环头的 φ 的入值来自回边，而回边的定义在文本里排在后面
    （§6.2 的 `%iv = phi [... (%iv.next Linc)]`），打印 φ 时那些值还没有号。"""
    n, cdefs = [0], []

    def alloc(v):
        v.num = n[0]; n[0] += 1
    for p in fn.params: alloc(p)
    for i, b in enumerate(fn.blocks): b.idx = i
    for b in fn.blocks:
        for item in items_of(b):
            if item.ty is not None: alloc(item)          # ① 先给本条指令的定义号
            for v in operands_of(item):                  # ② 再给未编号的常量发号
                v = deref(v)
                if v.k in ('c', 'g') and v.num is None:
                    alloc(v); cdefs.append(v)
    out = ['define %s @%s(%s) {' % (
        fn.ret, fn.name, ', '.join('%s %%%d' % (p.ty, p.num) for p in fn.params))]
    for v in cdefs:            # 常量定义行提到函数顶部，按拿号顺序（C++ 的真实排版）
        out.append('  %%%d = %s %s%s' % (
            v.num, v.ty, v.text if v.k == 'c' else '@' + v.name,
            ' @line %d' % v.line if v.line else ''))
    for b in fn.blocks:
        out.append('L%d:' % b.idx)
        for item in items_of(b):
            if item.k == 'f':
                out.append('  %%%d = phi %s [%s]%s' % (item.num, item.ty, ', '.join(
                    '(%%%d L%d)' % (deref(v).num, pb.idx) for v, pb in
                    sorted(item.incoming, key=lambda x: x[1].idx)),
                    ' @line %d' % item.line if item.line else ''))
                continue
            s = '%%%d = ' % item.num if item.ty is not None else ''
            for p in item.parts:
                if isinstance(p, Block): s += 'L%d' % p.idx
                elif isinstance(p, Typed): s += '%s %%%d' % (p.val.ty, deref(p.val).num)
                elif isinstance(p, V): s += '%%%d' % deref(p).num
                else: s += p
            out.append('  ' + s + (' @line %d' % item.line if item.line else ''))
    out.append('}')
    return out

def parse_flat(text):
    """把自己产出的文本按函数/块切出来（检查器专用：**重新解析**，不复用构造期对象）。"""
    funcs, cur = [], None
    for ln in text.split('\n'):
        if ln.startswith('define '):
            cur = {'hdr': ln, 'blocks': [], 'pre': []}
            funcs.append(cur)
        elif cur is not None:
            if ln == '}': cur = None
            elif re.match(r'^L\d+:$', ln): cur['blocks'].append((ln[:-1], []))
            elif cur['blocks']: cur['blocks'][-1][1].append(ln)
            else: cur['pre'].append(ln)          # 函数顶部的常量定义行
    return funcs

def self_check(text):
    """【后置】返回 (问题列表, 不可达块数)。检查项：① 操作数 `%N` 都在**同一个函数**里
    有定义；② 每块恰好一个终结符且在最后；③ φ 在块首且入值块集合 == 该块前驱集合
    （无重复）；④ 除计数到的不可达块外，块都可达（可达性按函数算：块号每函数从 L0 起）。"""
    bad, un = [], 0
    for fn in parse_flat(text):
        name = fn['hdr'][:40]
        defined = set(int(m.group(1)) for m in re.finditer(r'%(\d+)', fn['hdr']))
        for ln in fn['pre'] + [x for _l, ls in fn['blocks'] for x in ls]:
            m = DEF.match(ln)                 # 先把本函数所有定义收齐（φ 允许前向引用）
            if m: defined.add(int(m.group(1)))
        preds, succ = dict((lab, set()) for lab, _l in fn['blocks']), {}
        for lab, lines in fn['blocks']:
            if len([ln for ln in lines if TERM.match(ln)]) != 1:
                bad.append('%s %s: 终结符不是恰好 1 个' % (name, lab))
                continue
            if not TERM.match(lines[-1]): bad.append('%s %s: 终结符不在最后' % (name, lab))
            for ln in lines:
                for v in re.findall(r'%(\d+)', ln):
                    if int(v) not in defined:
                        bad.append('%s %s: 用了未定义的值 %%%s' % (name, lab, v))
            m1, m2 = BR1.match(lines[-1]), BR2.match(lines[-1])
            succ[lab] = [m1.group(1)] if m1 else ([m2.group(2), m2.group(3)] if m2 else [])
            for t in succ[lab]: preds.setdefault(t, set()).add(lab)
        for lab, lines in fn['blocks']:
            seen = False
            for ln in lines:
                m = PHI.match(ln)
                if not m:
                    seen = seen or not CONST_DEF.match(ln)   # 常量定义行不算"非 φ 指令"
                    continue
                if seen: bad.append('%s %s: φ 不在块首' % (name, lab))
                got = [b for _v, b in re.findall(r'\(%(\d+) (L\d+)\)', m.group(3))]
                if len(got) != len(set(got)):
                    bad.append('%s %s: φ 入值块重复' % (name, lab))
                if set(got) != preds.get(lab, set()):
                    bad.append('%s %s: φ 入值块 %s ≠ 前驱 %s' % (
                        name, lab, sorted(set(got)), sorted(preds.get(lab, set()))))
        seen, stack = set(), (['L0'] if fn['blocks'] else [])
        while stack:
            b = stack.pop()
            if b not in seen:
                seen.add(b)
                stack.extend(succ.get(b, []))
        un += len([lab for lab, _l in fn['blocks'] if lab not in seen])
    return bad, un

def rel_key(sy):
    """临时文件名键 = **相对工作区根的路径**，`/` → `__`（不许用 basename：
    两条赛道有 240 个同名文件，basename 键会凭空造出差异）。"""
    ap = os.path.abspath(sy)
    rel = os.path.relpath(ap, ROOT) if ap.startswith(ROOT + os.sep) else ap.lstrip('/')
    return rel.replace(os.sep, '__')

def run(cmd):
    """跑编译器；**不许因为编译器不在/正在被重新链接而崩掉**（失败返回 127）。"""
    try:
        return subprocess.run(cmd, capture_output=True)
    except OSError as e:
        return subprocess.CompletedProcess(cmd, 127, b'', str(e).encode())

def build_mine(compiler, sy, tmpdir):
    """结构化 dump → 我的平面文本。返回 (文本 or None, 说明, 是否"范围外")。"""
    a = os.path.join(tmpdir, rel_key(sy) + '.sir')
    r = run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', a])
    if r.returncode != 0: return None, 'rc=%d' % r.returncode, True
    try:
        return flatten_dump(open(a, encoding='utf-8').read()), '', False
    except OSError as e:
        return None, str(e), False
    except L.ParseError as e:
        return None, '解析：%s' % e, False
    except FlattenError as e:
        return None, '展平：%s' % e, False
    except RecursionError:
        return None, '递归过深', False

def one_file(compiler, sy, tmpdir):
    """返回 (状态, 说明)：same / diff / skip / error。"""
    b = os.path.join(tmpdir, rel_key(sy) + '.flat')
    r2 = run([compiler, sy, '--emit=flat-ir', '-o', b])
    mine, err, outscope = build_mine(compiler, sy, tmpdir)
    # 双方都非零 = **范围外用例**（tensor 那 50 个）：预期行为，记 skipped 不记 error
    if outscope and r2.returncode != 0: return ('skip', 'rc=%s/%d' % (err, r2.returncode))
    if mine is None:
        return ('error', 'rc=%s/%d' % (err, r2.returncode) if outscope else err)
    if r2.returncode != 0: return ('error', 'rc=0/%d' % r2.returncode)
    try: want = open(b, encoding='utf-8').read()
    except OSError as e: return ('error', str(e))
    if not want.strip(): return ('error', 'C++ 侧产物为空')
    if mine == want: return ('same', '')
    a, c = mine.split('\n'), want.split('\n')
    for i in range(min(len(a), len(c))):
        if a[i] != c[i]:
            return ('diff', 'line %d:\n      mine: %s\n      cpp : %s'
                    % (i + 1, a[i].strip(), c[i].strip()))
    return ('diff', 'length mine=%d cpp=%d' % (len(a), len(c)))

def one_selfcheck(compiler, sy, tmpdir):
    mine, err, outscope = build_mine(compiler, sy, tmpdir)
    if mine is None: return ('skip' if outscope else 'error', err, 0)
    if not mine.strip() or not parse_flat(mine): return ('error', '产物为空', 0)
    bad, un = self_check(mine)
    return ('error' if bad else 'ok', '；'.join(bad[:3]), un)

def strip_names(text):
    """`%N` 按出现顺序统一改名（`%v0`, `%v1` …）：区分"纯命名差异"与真语义差异。"""
    order = {}
    def rep(m):
        if m.group(0) not in order: order[m.group(0)] = '%%v%d' % len(order)
        return order[m.group(0)]
    return re.sub(r'%\d+', rep, text)

def collect_files(args):
    if args.dump_diff: return [os.path.abspath(args.dump_diff)]
    files = []
    for dirpath, dirnames, filenames in os.walk(args.dir):
        dirnames[:] = [d for d in dirnames if d not in ('.git', '__pycache__', '.work')]
        files.extend(os.path.join(dirpath, fn) for fn in sorted(filenames)
                     if fn.endswith('.sy'))
    files.sort()
    return files[:args.limit] if args.limit else files

def print_dump_diff(sy, tmp):
    """`--dump-diff`：结构等价判定（两份产物的 `%N` 都按出现顺序改名后再比）。"""
    key = rel_key(sy)
    print('== 结构等价判定（结果名已统一改名）==')
    try:
        mine = flatten_dump(open(os.path.join(tmp, key + '.sir'), encoding='utf-8').read())
        cpp = open(os.path.join(tmp, key + '.flat'), encoding='utf-8').read()
        print('结构相同: %s' % (strip_names(mine) == strip_names(cpp)))
    except OSError:
        print('结构相同: N/A（C++ 侧没有 --emit=flat-ir 的产物）')
    except Exception as e:                       # noqa: BLE001 —— 工具不许崩
        print('结构相同: N/A（本实现展平失败：%s）' % e)

def do_selfcheck(args, compiler, files, tmp):
    ok, errs, skips, un = [], [], [], 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for f, fu in [(f, ex.submit(one_selfcheck, compiler, f, tmp)) for f in files]:
            st, msg, n = fu.result()
            if st == 'ok':
                ok.append(f); un += n
            elif st == 'skip': skips.append((f, msg))
            else: errs.append((f, msg))
    print('== 轨 D 自洽检查（独立实现的第二份 FlattenCFG，不比对 C++）==')
    for lab, val in (('参与检查的文件数', len(files)), ('自洽', len(ok)),
                     ('范围外（结构化侧就拒绝）', len(skips)),
                     ('自洽检查失败', len(errs)),
                     ('有意保留的不可达块（两边都终结的 join）', un)):
        print('%s: %d' % (lab, val))
    for f, m in errs[:20]: print('  ERR  %s  %s' % (os.path.relpath(f, ROOT), m))
    if args.verbose:
        for f, m in skips[:20]: print('  SKIP %s  %s' % (os.path.relpath(f, ROOT), m))
    return 1 if errs else 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--dump-diff', default='', help='只处理这一个 .sy，并打印结构等价判定')
    ap.add_argument('--selfcheck', action='store_true',
                    help='只对自己的产物做自洽检查（C++ 侧没实现 --emit=flat-ir 时用）')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr); return 2
    files = collect_files(args)
    if not files:
        print('没有 .sy 文件', file=sys.stderr); return 2
    tmp = os.path.join(ROOT, '.work', 'flatten_trackd')
    os.makedirs(tmp, exist_ok=True)
    if args.selfcheck: return do_selfcheck(args, compiler, files, tmp)
    same, diffs, errs, skips = [], [], [], []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for f, fu in [(f, ex.submit(one_file, compiler, f, tmp)) for f in files]:
            st, msg = fu.result()
            {'same': same, 'diff': diffs, 'skip': skips,
             'error': errs}[st].append(f if st == 'same' else (f, msg))
    print('== 轨 D：独立实现的第二份 FlattenCFG ==')
    for lab, val in (('参与比较的文件数', len(files)), ('逐字节一致', len(same)),
                     ('有差异', len(diffs)), ('范围外（双方一致拒绝）', len(skips)),
                     ('工具/解析错误', len(errs))):
        print('%s: %d' % (lab, val))
    for f, m in diffs[:20]: print('  DIFF %s\n    %s' % (os.path.relpath(f, ROOT), m))
    for f, m in errs[:20]: print('  ERR  %s  %s' % (os.path.relpath(f, ROOT), m))
    if errs and not same and not diffs and len(errs) + len(skips) == len(files):
        print('  提示：C++ 侧 `--emit=flat-ir` 可能还没实现（轨 D 无法比较）⇒ 改用 '
              '`--selfcheck`')
    if args.verbose:
        for f in same[:20]: print('  SAME %s' % os.path.relpath(f, ROOT))
    if args.dump_diff: print_dump_diff(files[0], tmp)
    return 1 if (diffs or errs) else 0

if __name__ == '__main__':
    sys.exit(main())
