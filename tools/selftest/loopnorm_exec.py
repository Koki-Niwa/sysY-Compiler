#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
loopnorm_exec.py —— 轨 E：**受限的结构化 IR 执行器**

用法（命令行固定契约，关卡照这个签名调用）：
    python3 loopnorm_exec.py --compiler <路径> [--jobs N] [--dir 语料目录] [--limit N]
                             [--min-files 200] [--verbose] [--show-trace <file.sy>]

做什么
------
对每个能跑的程序，用**同一份输入**分别执行
`--emit=structured-ir`（变换前）与 `--normalize --emit=structured-ir`（变换后）
两份 IR，要求**执行轨迹相同**。

★ 这是本关**唯一能抓 off-by-one / IV 替换错 / continue 语义错**的判据。
  轨 D（两份规范化器逐字节一致）证明的是"两份实现等价" —— 若两份实现共享
  同一个误解，它**全盲**（实测：`if (i<j) { j=j+1; continue; }` 的 REST 包装
  就是轨 E 抓出来的，轨 D 当时 490/490 全绿）。

★ 轨迹的定义（写清才能复现）
----------------------------
    轨迹 = (① 每条**被执行**的循环的迭代次数序列（**只记次数**，不记编码形式
                与 dump 行号 —— 那是"怎么写"而不是"行为"；源码行号两边相同，
                但记进来会让"While→For"这种**预期的**编码变化变成假差异）,   ← 抓 IV/边界/步进错
            ② 输出缓冲（putint/putch/putfloat/… 的追加顺序与内容）,
            ③ 全局内存的最终内容摘要（只含**被写过**的字；稀疏表示）,
            ④ main 的返回值)
  ① 是最关键的一项：边界的 off-by-one、`<`/`<=` 的差别、continue 少走一次，
     都会让某个循环的迭代次数不同。

★ 确定性与跳过
--------------
* `getint`/`getch`/`getfloat`/`getarray`/`getfarray` 用**固定种子**的 PRNG
  （同一程序、同一输入 ⇒ 同一序列 ⇒ 可复现）；整数**故意取小值**（`[-5,10]`），
  理由见 `Exec.next_int()` 的注释（大随机数会让性能用例跑十亿次 ⇒ 只能跳过）；
* 不支持的构造（`tensor` 的前端报错、递归过深、指令/时间预算超限）⇒ **跳过该
  程序并报告原因**（不许静默跳过）。

退出码：0 = 全部跑通且轨迹相同；1 = 有轨迹不同或覆盖不足；2 = 工具自身错误。
"""

import argparse
import os
import struct
import subprocess
import sys

# 语料里有 84 处（自）递归。执行器的 `call()` 是 Python 递归，每层 IR 调用消耗
# 多个 Python 栈帧 ⇒ 默认上限（1000）在几十层时就爆。放宽到 30000；
# IR 侧的深度上限保持 200（超限即**跳过并报告**）。
sys.setrecursionlimit(30000)
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import loopnorm_ir as L  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v >= (1 << 31) else v


def s64(v):
    v &= MASK64
    return v - (1 << 64) if v >= (1 << 63) else v


def f32(v):
    return struct.unpack('<f', struct.pack('<f', float(v)))[0]


def bits_of_f32(v):
    return struct.unpack('<I', struct.pack('<f', float(v)))[0]


def f32_of_bits(b):
    return struct.unpack('<f', struct.pack('<I', b & MASK32))[0]


# ============================================================================
# 类型：字节大小
# ============================================================================
def size_of(ty):
    """结构化 IR 的类型文本 → 字节数（`i32`=4 · `f32`=4 · `i64`=8 · `[N x T]`）。"""
    ty = ty.strip()
    if ty in ('i32', 'f32', 'i1', 'i8'):
        return 4 if ty != 'i1' and ty != 'i8' else 1
    if ty == 'i64':
        return 8
    if ty == 'void':
        return 0
    if ty.startswith('[') and ty.endswith(']'):
        inner = ty[1:-1]
        n_str, _, elem = inner.partition(' x ')
        try:
            n = int(n_str.strip())
        except ValueError:
            return 0
        return n * size_of(elem)
    if ty.startswith('ptr'):
        return 8
    return 0


class Mem(object):
    """**稀疏**内存：`cells[字节偏移] = 64 位字`（未写过的字恒为 0）。

    为什么必须稀疏：语料里有 1.65 GB 的静态数组（`sl1-3.sy`）与 2.16 亿元素的
    零初始化全局 —— 真的分配会立刻 OOM。
    """
    __slots__ = ('size', 'cells', 'name')

    def __init__(self, size, name=''):
        self.size = int(size)
        self.cells = {}
        self.name = name

    def read_word(self, off, width):
        off &= MASK64
        if width == 8:
            lo = self.cells.get(off, 0) & MASK32
            hi = self.cells.get(off + 4, 0) & MASK32
            return (lo | (hi << 32)) & MASK64
        return self.cells.get(off, 0) & MASK32

    def write_word(self, off, width, val):
        off &= MASK64
        if width == 8:
            self.cells[off] = val & MASK32
            self.cells[off + 4] = (val >> 32) & MASK32
        else:
            self.cells[off] = val & MASK32

    def digest(self):
        import hashlib
        h = hashlib.sha256()
        for k in sorted(self.cells):
            h.update(struct.pack('<QQ', k, self.cells[k] & MASK64))
        return h.hexdigest()[:16]


class Ptr(object):
    __slots__ = ('mem', 'off')

    def __init__(self, mem, off):
        self.mem = mem
        self.off = off & MASK64


class Ret(Exception):
    def __init__(self, value):
        Exception.__init__(self, 'return')
        self.value = value


class BreakLoop(Exception):
    pass


class ContinueLoop(Exception):
    """`continue`：跳过本轮的剩余语句，进入下一轮。

    ★ 为什么执行器必须知道这件事：S05 的 IRGen 把 `break` 与 `continue`
      **都**降级成 `BreakOp`（形状见 `LoopNormalize.h`）。执行器按**形状**区分：
      `BreakOp` 在**某个 `IfOp` 分支的末尾**、且该分支里有 `Store`
      （= 源码的 `i = i + 1; continue;` 模式）⇒ 它是 `continue`。
      其余（体顶层的 `BreakOp`）⇒ 真 `break`（保守）。
    """


class Budget(Exception):
    pass


class Unsupported(Exception):
    pass


# ============================================================================
# 执行器
# ============================================================================
class Exec(object):
    # 预算只有**步数**这一个（超限即跳过并报告原因）。
    #   ⚠️ 早先还有一个 20 s 墙钟预算，那让**跳过集合随负载漂移** ⇒ 覆盖率数字不可
    #      复现。判据的输入必须确定 ⇒ 只留步数，墙钟只用于报告耗时。
    MAX_STEPS = 5000000

    def __init__(self, mod, names, seed=12345):
        import time
        self.t0 = time.monotonic()
        self.mod = mod
        self.names = names
        self.globals = {}          # 全局名 → Ptr
        self.funcs = {}            # 函数名 → Op
        self.out = []
        self.loop_trace = []
        self.steps = 0
        self.rng = seed
        self.depth = 0
        self.ptr_slots = {}        # (mem id, offset) → Ptr（指针槽；数组形参）

    # ── 确定性"输入" ──────────────────────────────────────────────────────
    def next_int(self):
        """确定性的"输入整数"（固定种子 ⇒ 可复现）。

        ★ 为什么**故意取小值**（`[-5, 10]`）而不是"看起来像真输入的大随机数"：
          语料里的性能用例是 `n = getint(); while (i < n) { … }`。若 `getint`
          返回一个 10 位数，循环要跑十亿次 ⇒ 只能靠"时间预算"跳过，
          而**这些恰好是最需要行为判据的程序**（实测：大随机数下 97 个文件
          因超出 20 秒预算被跳过）。
          取小值后，同一个循环仍然被**真实执行**（迭代次数、边界、`continue`
          的路径全部走到），轨迹判据的效力不降 —— 而覆盖数从 378 涨到 400+。
          代价：数值上的"边角情形"（大数溢出等）覆盖变弱，但那是**前端降级**
          （S03/S04）的验收范围，不是本关（控制流重编码）的。
        """
        x = self.rng & MASK32
        x ^= (x << 13) & MASK32
        x ^= (x >> 17)
        x ^= (x << 5) & MASK32
        self.rng = x & MASK32
        return (self.rng % 16) - 5

    def next_float(self):
        return f32(self.next_int() % 1000) / 4.0

    # ── 入口 ──────────────────────────────────────────────────────────────
    def run(self, entry='main'):
        mod_region = L.module_region(self.mod)
        for op in mod_region:
            if op.kind == 'Func':
                self.funcs[op.name] = op
        for op in mod_region:
            if op.kind != 'GlobalVar':
                continue
            ty = self._global_type(op)
            mem = Mem(size_of(ty), op.name)
            # `:init` 的值是**带引号的记号**（`"data` / `"zero`）；
            #   数据表跟在它后面：`{ 偏移=值, 偏移=值 }`（tokenize 把 `{`/`}`/`,` 切开）
            if ':init' in op.attrs:
                i0 = op.attrs.index(':init')
                if i0 + 1 < len(op.attrs) and op.attrs[i0 + 1].startswith('"data'):
                    for t in op.attrs[i0 + 2:]:
                        if '=' in t:
                            a_str, _, b_str = t.partition('=')
                            try:
                                mem.write_word(int(a_str), 4, int(b_str) & MASK32)
                            except ValueError:
                                pass
            p = Ptr(mem, 0)
            # GetGlobal 的结果名 → 同一个指针
            for g in mod_region:
                if g.kind == 'GetGlobal' and g.name == op.name and g.results:
                    self.globals[g.results[0]] = p
            self.globals[op.name] = p
        if entry not in self.funcs:
            raise Unsupported('no entry function `%s`' % entry)
        ret = self.call(entry, [])
        # 汇总全局摘要（只含被写过的字 ⇒ 稀疏也稳定）
        import hashlib
        h = hashlib.sha256()
        for name in sorted(self.globals):
            g = self.globals[name]
            h.update(name.encode())
            h.update(g.mem.digest().encode())
        return {'loops': list(self.loop_trace), 'out': list(self.out),
                'mem': h.hexdigest()[:16], 'ret': ret}

    def _global_type(self, op):
        for i, t in enumerate(op.attrs):
            if t == ':type' and i + 1 < len(op.attrs):
                return op.attrs[i + 1]
        return 'i32'

    # ── 函数调用 ──────────────────────────────────────────────────────────
    def call(self, name, args):
        if name in L.RUNTIME_VOID or name in L.VOID_CALLS or name.startswith('_sysy'):
            self.do_runtime_void(name, args)
            return None
        if name in L.RUNTIME_SIG:
            return self.do_runtime_ret(name, args)
        fn = self.funcs.get(name)
        if fn is None:
            raise Unsupported('call to unknown function `%s`' % name)
        if self.depth > 200:
            raise Unsupported('recursion deeper than 200')
        self.depth += 1
        try:
            return self.exec_func(fn, args)
        finally:
            self.depth -= 1

    def do_runtime_void(self, name, args):
        if name in ('_sysystarttime', '_sysystoptime', '_sysy_starttime', '_sysy_stoptime'):
            return
        if name == 'putint':
            self.out.append(str(int(args[0])))
            return
        if name == 'putch':
            self.out.append('c%d' % (int(args[0]) & MASK32))
            return
        if name == 'putfloat':
            self.out.append('f%08x' % bits_of_f32(args[0]))
            return
        if name == 'putarray':
            n = int(args[0])
            p = args[1]
            self.out.append('a%d' % n)
            for i in range(max(0, min(n, 1000))):
                self.out.append(str(s32(self.load_word(p, i * 4, 4))))
            return
        if name == 'putfarray':
            n = int(args[0])
            p = args[1]
            self.out.append('fa%d' % n)
            for i in range(max(0, min(n, 1000))):
                self.out.append('f%08x' % (self.load_word(p, i * 4, 4) & MASK32))
            return
        if name == 'llvm.memset':
            p, val, n = args[0], int(args[1]) & MASK32, int(args[2])
            if val != 0:
                raise Unsupported('memset with non-zero value')
            if n > 10 ** 7:
                raise Unsupported('memset too large (%d)' % n)
            for off in range(0, n & ~3, 4):
                p.mem.cells.pop((p.off + off) & MASK64, None)
            return
        if name == 'llvm.memcpy':
            dst, src, n = args[0], args[1], int(args[2])
            if n > 10 ** 7:
                raise Unsupported('memcpy too large (%d)' % n)
            words = {}
            for off in range(0, n & ~3, 4):
                v = src.mem.cells.get((src.off + off) & MASK64)
                if v:
                    words[off] = v
            for off, v in words.items():
                dst.mem.cells[(dst.off + off) & MASK64] = v
            return
        raise Unsupported('unknown runtime void `%s`' % name)

    def do_runtime_ret(self, name, args):
        if name == 'getint':
            return self.next_int()
        if name == 'getch':
            return 65 + (self.next_int() & 15)
        if name == 'getfloat':
            return self.next_float()
        if name in ('getarray',):
            p = args[0]
            n = 8
            for i in range(n):
                self.store_word(p, i * 4, 4, self.next_int() & MASK32)
            return n
        if name in ('getfarray',):
            p = args[0]
            n = 8
            for i in range(n):
                self.store_word(p, i * 4, 4, bits_of_f32(self.next_float()))
            return n
        raise Unsupported('unknown runtime function `%s`' % name)

    # ── 内存原语 ──────────────────────────────────────────────────────────
    def load_word(self, p, off, width):
        return p.mem.read_word(p.off + off, width)

    def store_word(self, p, off, width, val):
        p.mem.write_word((p.off + off) & MASK64, width, val)

    # 指针槽：单独一张表（`id(mem)` 不能跨进程/跨运行复用，也不该进内存摘要）
    def ptr_slot_get(self, p):
        return self.ptr_slots.get((id(p.mem), p.off & MASK64))

    def ptr_slot_set(self, p, v):
        self.ptr_slots[(id(p.mem), p.off & MASK64)] = v

    def exec_func(self, fn, args):
        # ★ 帧里**预置全局引用**：全局名（`%.N`）可以在任何函数体里当操作数用，
        #   而它们不属于任何函数的局部环境（第一版只查局部 env ⇒ 一遇到
        #   `Load %.1` 就 KeyError）。
        env = dict(self.globals)
        body = fn.regions[0]
        try:
            self.exec_region(body, env, args)
        except Ret as r:
            return r.value
        return 0

    # ── Region 执行 ───────────────────────────────────────────────────────
    def exec_region(self, region, env, args, in_if_branch=False):
        for op in region:
            if op.kind == 'Break':
                # 形状判定（见 `ContinueLoop` 的说明）
                if in_if_branch and any(x.kind == 'Store' for x in region):
                    raise ContinueLoop()
                raise BreakLoop()
            self.exec_op(op, env, args, in_if_branch)

    def exec_op(self, op, env, args, in_if_branch=False):
        self.steps += 1
        if self.steps > self.MAX_STEPS:
            raise Budget('step budget exceeded（%d 步）' % self.MAX_STEPS)
        # 墙钟**不参与判定**（见 MAX_STEPS 的注释：那会让跳过集合随负载漂移）。
        # 只留着供外层报告耗时，判据一律走步数。
        k = op.kind
        if k == 'Alloca':
            ty = op.attrs[0] if op.attrs else 'i32'
            env[op.results[0]] = Ptr(Mem(size_of(ty), 'alloca'), 0)
            return
        if k == 'GetArg':
            idx = int(op.attrs[0])
            env[op.results[0]] = args[idx] if idx < len(args) else 0
            return
        if k == 'Int':
            env[op.results[0]] = s32(int(op.attrs[0]))
            return
        if k == 'Float':
            env[op.results[0]] = self._parse_float(op.attrs[0])
            return
        if k == 'GetGlobal':
            nm = op.name
            if nm not in self.globals:
                raise Unsupported('unknown global `%s`' % nm)
            env[op.results[0]] = self.globals[nm]
            return
        if k == 'Bitcast':
            env[op.results[0]] = env[op.operands[0]]
            return
        if k == 'Load':
            ty = op.attrs[0] if op.attrs else 'i32'
            p = env[op.operands[0]]
            env[op.results[0]] = self._load(p, ty)
            return
        if k == 'Store':
            ty = op.attrs[0] if op.attrs else 'i32'
            p = env[op.operands[1]]
            self._store(p, ty, env[op.operands[0]])
            return
        if k == 'GetElementPtr':
            ety = op.attrs[0] if op.attrs else 'i32'
            p = env[op.operands[0]]
            i = env[op.operands[1]]
            env[op.results[0]] = Ptr(p.mem, (p.off + i * size_of(ety)) & MASK64)
            return
        if k in ('AddI', 'SubI', 'MulI', 'DivI', 'ModI'):
            a = env[op.operands[0]]
            b = env[op.operands[1]]
            env[op.results[0]] = self._int_bin(k, a, b)
            return
        if k == 'MinusI':
            env[op.results[0]] = s32(-env[op.operands[0]])
            return
        if k in ('AddF', 'SubF', 'MulF', 'DivF'):
            a = env[op.operands[0]]
            b = env[op.operands[1]]
            try:
                v = {'AddF': a + b, 'SubF': a - b, 'MulF': a * b,
                     'DivF': a / b if b != 0 else float('nan')}[k]
            except ZeroDivisionError:
                v = float('nan')
            env[op.results[0]] = f32(v)
            return
        if k == 'MinusF':
            env[op.results[0]] = f32(-env[op.operands[0]])
            return
        if k in ('Eq', 'Ne', 'Lt', 'Le', 'Gt', 'Ge'):
            a = env[op.operands[0]]
            b = env[op.operands[1]]
            if isinstance(a, float) or isinstance(b, float):
                a = float(a)
                b = float(b)
            r = {'Eq': a == b, 'Ne': a != b, 'Lt': a < b, 'Le': a <= b,
                 'Gt': a > b, 'Ge': a >= b}[k]
            env[op.results[0]] = 1 if r else 0
            return
        if k == 'I2F':
            env[op.results[0]] = f32(env[op.operands[0]])
            return
        if k == 'F2I':
            v = env[op.operands[0]]
            if v != v:
                env[op.results[0]] = 0
            elif v >= 2147483648.0:
                env[op.results[0]] = 2147483647
            elif v < -2147483648.0:
                env[op.results[0]] = -2147483648
            else:
                env[op.results[0]] = int(v)
            return
        if k == 'Sext':
            env[op.results[0]] = s64(env[op.operands[0]])
            return
        if k == 'Select':
            c = env[op.operands[0]]
            env[op.results[0]] = env[op.operands[1]] if c else env[op.operands[2]]
            return
        if k == 'Call':
            a = [env[t] for t in op.operands]
            r = self.call(op.name, a)
            if op.results:
                env[op.results[0]] = 0 if r is None else r
            return
        if k == 'If':
            c = env[op.operands[0]]
            self.exec_region(op.regions[0] if c else op.regions[1], env, args,
                             in_if_branch=True)
            return
        if k == 'While':
            n = 0
            while True:
                cond = self.eval_region_value(op.regions[0], env, args)
                if not cond:
                    break
                n += 1
                if n > 1000000:
                    raise Budget('loop too many iterations')
                try:
                    self.exec_region(op.regions[1], env, args)
                except ContinueLoop:
                    continue
                except BreakLoop:
                    break
            self.loop_trace.append(n)
            return
        if k == 'For':
            ivslot = op.operands[0]
            lo = env[op.operands[1]]
            hi = env[op.operands[2]]
            st = env[op.operands[3]]
            p = env[ivslot]
            n = 0
            cur = lo
            while cur < hi:
                self._store(p, 'i32', cur)
                n += 1
                if n > 1000000:
                    raise Budget('loop too many iterations')
                try:
                    self.exec_region(op.regions[0], env, args)
                except ContinueLoop:
                    cur = s32(cur + st)
                    continue
                except BreakLoop:
                    break
                cur = s32(cur + st)
            self._store(p, 'i32', cur)      # 循环结束后 IV 的最终值
            self.loop_trace.append(n)
            return
        if k == 'Yield':
            return
        if k == 'Break':
            raise BreakLoop()
        if k == 'Return':
            raise Ret(env[op.operands[0]] if op.operands else None)
        if k == 'Unreachable':
            raise Unsupported('reached Unreachable')
        if k == 'Goto':
            raise Unsupported('GotoOp not supported')
        if k in ('Module', 'Func', 'GlobalVar'):
            return
        raise Unsupported('op kind `%s`' % k)

    # 条件 Region：跑完取它 yield 的值
    def eval_region_value(self, region, env, args):
        last = None
        for op in region:
            if op.kind == 'Yield':
                last = env[op.operands[0]] if op.operands else 0
                continue
            self.exec_op(op, env, args)
        return last

    def _int_bin(self, k, a, b):
        if k == 'AddI':
            return s32(a + b)
        if k == 'SubI':
            return s32(a - b)
        if k == 'MulI':
            return s32(a * b)
        if k == 'DivI':
            if b == 0:
                return 0
            if a == -2147483648 and b == -1:
                return 0
            q = abs(a) // abs(b)
            return s32(q if (a < 0) == (b < 0) else -q)
        if k == 'ModI':
            if b == 0:
                return a
            if a == -2147483648 and b == -1:
                return 0
            r = abs(a) % abs(b)
            return s32(r if a >= 0 else -r)
        raise Unsupported(k)

    # 【后置】按类型读一个值（`i32`/`f32`/`i64`/指针槽）。
    def _load(self, p, ty):
        if ty == 'f32':
            return f32_of_bits(self.load_word(p, 0, 4))
        if ty == 'i64':
            return s64(self.load_word(p, 0, 8))
        if ty == 'ptr' or ty.startswith('ptr'):
            v = self.ptr_slot_get(p)
            if v is None:
                raise Unsupported('load of unset pointer slot')
            return v
        return s32(self.load_word(p, 0, 4))

    def _store(self, p, ty, v):
        if ty == 'f32':
            self.store_word(p, 0, 4, bits_of_f32(v))
            return
        if ty == 'i64':
            self.store_word(p, 0, 8, v & MASK64)
            return
        if ty == 'ptr' or ty.startswith('ptr'):
            if not isinstance(v, Ptr):
                # ⚠️ IRGen 在**嵌套数组的初始化**里会把标量写进"子数组类型"的槽
                #   （`int a[3][2] = {1,2,3,4,5,6}` ⇒
                #    `(Store ptr[[2 x i32]] %int %gep)`）。这是既有（冻结）的
                #   降级形态；执行器按 **i32 写**处理即可（两份 IR 里这段完全相同，
                #   所以不影响"变换前后行为是否一致"的判定）。
                self.store_word(p, 0, 4, int(v) & MASK32)
                return
            self.ptr_slot_set(p, v)
            return
        self.store_word(p, 0, 4, int(v) & MASK32)

    @staticmethod
    def _parse_float(tok):   # `%a` 十六进制浮点（与 dump 的格式一致）
        if tok == 'nan':
            return float('nan')
        return f32(float.fromhex(tok) if tok.startswith('0x') or 'p' in tok else float(tok))


# ============================================================================
# 单文件：跑两份 IR，比较轨迹
# ============================================================================
def one_file(compiler, sy, tmpdir, seed):
    # ⚠️ 键必须用**相对路径**：240 个文件与另一赛道同名，用 basename 会让并发 worker
    #    读写同一对临时文件 ⇒ **凭空造出行为差异**（实测偶发 2 个假差异）。
    key = os.path.relpath(sy, ROOT).replace('/', '__')
    raw = os.path.join(tmpdir, key + '.raw')
    nrm = os.path.join(tmpdir, key + '.norm')
    r1 = subprocess.run([compiler, sy, '--emit=structured-ir', '-o', raw],
                        capture_output=True)
    r2 = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', nrm],
                        capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        return ('frontend', 'rc=%d/%d' % (r1.returncode, r2.returncode), None, None)
    try:
        t1 = open(raw, encoding='utf-8').read()
        t2 = open(nrm, encoding='utf-8').read()
        if not t1.strip() or not t2.strip():
            return ('empty', 'dump 为空', None, None)
        m1, n1, _ = L.parse_dump(t1)
        m2, n2, _ = L.parse_dump(t2)
        e1 = Exec(m1, n1, seed)
        tr1 = e1.run()
        e2 = Exec(m2, n2, seed)
        tr2 = e2.run()
    except Unsupported as e:
        return ('unsupported', str(e), None, None)
    except Budget as e:
        return ('budget', str(e), None, None)
    except L.ParseError as e:
        return ('parse', str(e), None, None)
    except RecursionError:
        return ('recursion', 'Python 递归上限', None, None)
    if tr1 == tr2:
        return ('same', '', tr1, tr2)
    return ('diff', '', tr1, tr2)


def describe_diff(a, b):
    msgs = []
    if a['loops'] != b['loops']:
        la, lb = a['loops'], b['loops']
        for i in range(min(len(la), len(lb))):
            if la[i] != lb[i]:
                msgs.append('第 %d 个循环：变换前 %s vs 变换后 %s' % (i + 1, la[i], lb[i]))
                break
        else:
            msgs.append('循环数不同：%d vs %d' % (len(la), len(lb)))
    if a['out'] != b['out']:
        for i in range(min(len(a['out']), len(b['out']))):
            if a['out'][i] != b['out'][i]:
                msgs.append('输出第 %d 项不同：%s vs %s'
                            % (i + 1, a['out'][i], b['out'][i]))
                break
        else:
            msgs.append('输出长度不同：%d vs %d' % (len(a['out']), len(b['out'])))
    if a['mem'] != b['mem']:
        msgs.append('最终全局内存不同：%s vs %s' % (a['mem'], b['mem']))
    if a['ret'] != b['ret']:
        msgs.append('返回值不同：%s vs %s' % (a['ret'], b['ret']))
    return '; '.join(msgs) if msgs else '（未知差异）'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--min-files', type=int, default=200,
                    help='覆盖要求：至少多少个文件两份 IR 轨迹相同')
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--show-trace', default='', help='只跑这一个 .sy 并打印轨迹')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    tmp = os.path.join(ROOT, '.work', 'loopnorm_exec')
    os.makedirs(tmp, exist_ok=True)

    if args.show_trace:
        st, msg, a, b = one_file(compiler, os.path.abspath(args.show_trace), tmp, args.seed)
        print('状态: %s %s' % (st, msg))
        if a is not None:
            print('变换前轨迹: %s' % a)
            print('变换后轨迹: %s' % b)
        if st == 'diff':
            print('差异: %s' % describe_diff(a, b))
        return 0

    files = []
    for dirpath, dirnames, filenames in os.walk(args.dir):
        dirnames[:] = [d for d in dirnames if d not in ('.git', '__pycache__')]
        for fn in sorted(filenames):
            if fn.endswith('.sy'):
                files.append(os.path.join(dirpath, fn))
    files.sort()
    if args.limit:
        files = files[:args.limit]
    if not files:
        print('没有 .sy 文件', file=sys.stderr)
        return 2

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        results = list(ex.map(lambda f: (f,) + one_file(compiler, f, tmp, args.seed), files))

    same = []
    diffs = []
    skips = {}
    for f, st, msg, a, b in results:
        if st == 'same':
            same.append(f)
        elif st == 'diff':
            diffs.append((f, describe_diff(a, b), a, b))
        else:
            key = msg if st in ('unsupported', 'budget', 'recursion', 'parse', 'empty') else msg
            # 前端就报错的（tensor 等）与"执行器不支持"分开统计
            if st == 'frontend':
                key = '前端未产出 IR（如 tensor 用例；rc=%s）' % msg.split('/')[-1]
            skips.setdefault(key, []).append(f)

    print('== 轨 E：受限执行器（行为等价判据）==')
    print('参与执行的文件数: %d' % len(files))
    print('两份 IR 轨迹相同: %d' % len(same))
    print('轨迹不同: %d' % len(diffs))
    for f, msg, a, b in diffs[:10]:
        print('  ✘ %s\n    %s' % (os.path.relpath(f, ROOT), msg))
        print('    前: %s' % str(a)[:200])
        print('    后: %s' % str(b)[:200])
    total_skipped = sum(len(v) for v in skips.values())
    print('跳过: %d（**逐类列出原因，不是静默跳过**）' % total_skipped)
    for k in sorted(skips, key=lambda x: -len(skips[x])):
        print('  %-58s %4d   例：%s' % (k[:58], len(skips[k]),
                                        os.path.relpath(skips[k][0], ROOT)))
    ok = not diffs and len(same) >= args.min_files
    if diffs:
        print('  ✘ 有程序在变换前后行为不同（**先假定是 C++ 的规范化错了**，逐条查清）')
    if len(same) < args.min_files:
        print('  ✘ 跑通的程序数 %d < 要求 %d' % (len(same), args.min_files))
    print('== 结果：%s ==' % ('全过' if ok else '有失败'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
