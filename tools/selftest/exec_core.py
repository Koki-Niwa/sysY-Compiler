#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
exec_core.py —— **两种 IR 共用的执行语义**（S06 抽出；轨 E 的地基）

为什么必须有这个文件
--------------------
S05b 的 `loopnorm_exec.py` 自带一份执行语义。S06 的轨 E 要执行**平面 IR**，
如果照抄第二份，就又多了一处"两边共享同一个误解"的地方 —— 而 S05b 正好
栽在这上面：它的 `Break` 用形状启发式猜 `continue`，与 `LoopNormalize`
**共享同一个误解** ⇒ 变换前后轨迹相同、两个答案都错（`ret=8` vs 正确 `5`）。

⇒ 本文件放**全部与形状无关的语义**：
    * 数值：`s32`/`s64`/`f32`/位模式互转（溢出回绕、浮点按 f32 收窄）
    * 内存：**稀疏** `Mem`（`cells[字节偏移] = 64 位字`）+ `Ptr`
    * 算术：整数/浮点/比较（含 `sdiv`/`srem` 的除零与 `INT_MIN/-1` 归一化）
    * 内存原语：`_load`/`_store`/`load_word`/`store_word`/指针槽
    * 控制流信号：`Ret`/`BreakLoop`/`ContinueLoop`/`Budget`/`Unsupported`
    * 运行时库：`getint`/`putint`/`llvm.memcpy`/`llvm.memset` …（**唯一实现**）
    * 确定性输入：固定种子 PRNG（整数**故意取小值**，见 `next_int`）
    * 预算：**只按步数**（墙钟会让跳过集合随负载漂移，S05b 的教训）

两个执行器各自只负责"**取下一条指令**"：
    * `loopnorm_exec.Exec`  —— 结构化 IR（Region 树）
    * `flat_exec.FlatExec`  —— 平面 IR（基本块 + 终结符）

⚠️ 本文件是**纯库**（不打印任何东西、不读环境变量）；跳过/报错由调用方决定。
"""

import hashlib
import struct

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

# 运行时库的**唯一**签名表：名字 → (返回类型, 参数个数)
#   ⚠️ 与 C++ 侧的签名表**不是**同一张（那边管类型检查与 ABI 发射）；
#      这里是执行器要用的最小集合。两边都必须与官方 sylib.c 一致。
RUNTIME_RET = {
    'getint': ('i32', 0), 'getch': ('i32', 0), 'getfloat': ('f32', 0),
    'getarray': ('i32', 1), 'getfarray': ('i32', 1),
}
RUNTIME_VOID = {'putint', 'putch', 'putfloat', 'putarray', 'putfarray'}
# 计时函数（`_sysy<name>` 前缀规则，见 AGENT-CONTEXT §3.2）
TIMING = {'_sysystarttime', '_sysystoptime', '_sysy_starttime', '_sysy_stoptime'}
BUILTIN_VOID = {'llvm.memcpy', 'llvm.memset'}


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


class Mem(object):
    """**稀疏**内存：`cells[字节偏移] = 64 位字`（未写过的字恒为 0）。

    为什么必须稀疏：语料里有 1.65 GB 的静态数组（`sl1-3.sy`）与 2.16 亿元素的
    零初始化全局 —— 真的分配会立刻 OOM。
    """
    __slots__ = ('size', 'cells', 'name')

    def __init__(self, size, name=''):
        self.size = size
        self.cells = {}
        self.name = name

    def read_word(self, off, width):
        off &= MASK64
        v = self.cells.get(off)
        if v is None:
            return 0
        if width == 1:
            return v & 0xFF
        if width == 2:
            return v & 0xFFFF
        return v & MASK32 if width == 4 else v & MASK64

    def write_word(self, off, width, val):
        off &= MASK64
        if width == 1:
            old = self.cells.get(off, 0)
            self.cells[off] = (old & ~0xFF) | (val & 0xFF)
        elif width == 2:
            old = self.cells.get(off, 0)
            self.cells[off] = (old & ~0xFFFF) | (val & 0xFFFF)
        elif width == 4:
            self.cells[off] = val & MASK32
        else:
            self.cells[off] = val & MASK64

    def digest(self):
        """只含**被写过**的字 ⇒ 稀疏也稳定（排序后哈希）。

        ⚠️ **格式必须与 S05b 的 `loopnorm_exec.Mem.digest` 逐字节一致**
        （`<QQ` 打包 + 取前 16 个 hex 字符）：轨迹里的 `mem` 字段是判据的
        一部分，改格式会让"重构前后轨迹相同"这条锚定失效 ——
        而锚定正是证明"抽取执行核没有改变语义"的唯一手段。
        """
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
        Exception.__init__(self, value)
        self.value = value


class BreakLoop(Exception):
    """跳出当前循环（**只由 `Break` 抛**，不再有任何形状启发式）。"""


class ContinueLoop(Exception):
    """跳到当前循环的下一次迭代（**只由 `Continue` 抛**）。"""


class Budget(Exception):
    """步数预算耗尽（调用方**跳过并报告原因**，不许静默）。"""


class Unsupported(Exception):
    """执行器不支持的构造（调用方跳过并报告原因）。"""


class Machine(object):
    """两种形状共用的执行状态与语义。

    【约定】子类实现 `step(op, env)`（"取下一条指令"），并调用这里的方法。
    本类的 `_int_bin`/`_load`/`_store`/`runtime` 等是**唯一实现**。
    """
    # 预算只有**步数**（超限即跳过并报告）。
    #   ⚠️ 墙钟预算会让跳过集合随负载漂移 ⇒ 覆盖率不可复现（S05b 的账）。
    MAX_STEPS = 5000000

    def __init__(self, seed=12345):
        self.globals = {}          # 全局名 → Ptr
        self.out = []              # 输出缓冲（putint/putch/…）
        self.loop_trace = []       # 循环迭代次数序列（**只记次数**）
        self.steps = 0
        self.rng = seed
        self.depth = 0
        self.ptr_slots = {}        # (id(mem), 偏移) → Ptr（指针槽：数组形参）

    # ── 确定性"输入" ─────────────────────────────────────────────────────
    def next_int(self):
        """确定性的"输入整数"（固定种子 ⇒ 可复现）。

        ★ 为什么**故意取小值**（`[-5, 10]`）：语料里的性能用例是
          `n = getint(); while (i < n) { … }`。大随机数会让循环跑十亿次
          ⇒ 只能靠时间预算跳过，而**这些恰好最需要行为判据**。
          小值下同一个循环仍被真实执行（迭代次数、边界、`continue` 全覆盖）。
        """
        x = self.rng & MASK32
        x ^= (x << 13) & MASK32
        x ^= (x >> 17)
        x ^= (x << 5) & MASK32
        self.rng = x & MASK32
        return (self.rng % 16) - 5

    def next_float(self):
        return f32(self.next_int() % 1000) / 4.0

    # ── 内存原语 ─────────────────────────────────────────────────────────
    def load_word(self, p, off, width):
        return p.mem.read_word(p.off + off, width)

    def store_word(self, p, off, width, val):
        p.mem.write_word((p.off + off) & MASK64, width, val)

    def ptr_slot_get(self, p):
        return self.ptr_slots.get((id(p.mem), p.off & MASK64))

    def ptr_slot_set(self, p, v):
        self.ptr_slots[(id(p.mem), p.off & MASK64)] = v

    # 类型文本 → 字节大小（`i32`/`f32`=4、`i64`/`ptr`=8、`[N x T]` 递归）
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
                # ⚠️ S05 的 IRGen 在**嵌套数组初始化**里会把标量写进"子数组类型"
                #   的槽（`int a[3][2] = {…}` ⇒ `(Store ptr[[2 x i32]] %int %gep)`）。
                #   那是既有的（冻结）降级形态；两边完全相同 ⇒ 按 i32 写。
                self.store_word(p, 0, 4, int(v) & MASK32)
                return
            self.ptr_slot_set(p, v)
            return
        self.store_word(p, 0, 4, int(v) & MASK32)

    # ── 算术（i32 二元；溢出回绕、除零按铁律 6 归一化）─────────────────
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

    # ── 形状无关的单条指令语义（**两边共用**）───────────────────────────
    # 【后置】执行一条"语义与形状无关"的指令；`env` 是该处的"名字 → 值"表。
    #         返回 True 表示"已处理"；False 表示"这条要由调用方自己处理"
    #         （控制流、终结符、以及两侧不同的取名方式）。
    def try_default(self, k, op, env):
        if k == 'Int':
            env[op.results[0]] = s32(int(op.attrs[0]))
            return True
        if k == 'Float':
            env[op.results[0]] = self.parse_float(op.attrs[0])
            return True
        if k == 'GetGlobal':
            nm = op.name
            if nm not in self.globals:
                raise Unsupported('unknown global `%s`' % nm)
            env[op.results[0]] = self.globals[nm]
            return True
        if k == 'Bitcast':
            env[op.results[0]] = env[op.operands[0]]
            return True
        if k == 'Load':
            ty = op.attrs[0] if op.attrs else 'i32'
            env[op.results[0]] = self._load(env[op.operands[0]], ty)
            return True
        if k == 'Store':
            ty = op.attrs[0] if op.attrs else 'i32'
            self._store(env[op.operands[1]], ty, env[op.operands[0]])
            return True
        if k == 'GetElementPtr':
            # 结构化层：`attrs = [元素类型, 下标类型, 亲和性]`，下标按元素个数
            ety = op.attrs[0] if op.attrs else 'i32'
            p = env[op.operands[0]]
            i = env[op.operands[1]]
            env[op.results[0]] = Ptr(p.mem, (p.off + i * size_of(ety)) & MASK64)
            return True
        if k in ('AddI', 'SubI', 'MulI', 'DivI', 'ModI'):
            env[op.results[0]] = self._int_bin(k, env[op.operands[0]], env[op.operands[1]])
            return True
        if k == 'MinusI':
            env[op.results[0]] = s32(-env[op.operands[0]])
            return True
        if k in ('AddF', 'SubF', 'MulF', 'DivF'):
            a = env[op.operands[0]]
            b = env[op.operands[1]]
            try:
                v = {'AddF': a + b, 'SubF': a - b, 'MulF': a * b,
                     'DivF': a / b if b != 0 else float('nan')}[k]
            except ZeroDivisionError:
                v = float('nan')
            env[op.results[0]] = f32(v)
            return True
        if k == 'MinusF':
            env[op.results[0]] = f32(-env[op.operands[0]])
            return True
        if k in ('Eq', 'Ne', 'Lt', 'Le', 'Gt', 'Ge'):
            a = env[op.operands[0]]
            b = env[op.operands[1]]
            # 浮点比较：任一操作数是 f32 ⇒ 按浮点比（NaN 全假，除 une）
            if isinstance(a, float) or isinstance(b, float):
                a = float(a)
                b = float(b)
            r = {'Eq': a == b, 'Ne': a != b, 'Lt': a < b, 'Le': a <= b,
                 'Gt': a > b, 'Ge': a >= b}[k]
            env[op.results[0]] = 1 if r else 0
            return True
        if k == 'I2F':
            env[op.results[0]] = f32(env[op.operands[0]])
            return True
        if k == 'F2I':
            v = env[op.operands[0]]
            # 饱和：与铁律 6 的归一化一致（NaN→0、越界→饱和）
            if v != v:
                env[op.results[0]] = 0
            elif v >= 2147483648.0:
                env[op.results[0]] = 2147483647
            elif v < -2147483648.0:
                env[op.results[0]] = -2147483648
            else:
                env[op.results[0]] = int(v)
            return True
        if k == 'Sext':
            env[op.results[0]] = s64(env[op.operands[0]])
            return True
        if k == 'Select':
            c = env[op.operands[0]]
            env[op.results[0]] = env[op.operands[1]] if c else env[op.operands[2]]
            return True
        return False

    # ── 运行时库（**唯一实现**）──────────────────────────────────────────
    def runtime(self, name, args, call_func):
        """【后置】执行一次调用，返回返回值（void → None）。
        `call_func(name, args)` 由子类提供（用户函数的调用入口）。
        """
        if name in TIMING:
            return None
        if name in RUNTIME_VOID:
            if name == 'putint':
                self.out.append(str(int(args[0])))
                return None
            if name == 'putch':
                self.out.append('c%d' % (int(args[0]) & MASK32))
                return None
            if name == 'putfloat':
                self.out.append('f%08x' % bits_of_f32(args[0]))
                return None
            if name == 'putarray':
                n = int(args[0])
                p = args[1]
                self.out.append('a%d' % n)
                for i in range(max(0, min(n, 1000))):
                    self.out.append(str(s32(self.load_word(p, i * 4, 4))))
                return None
            if name == 'putfarray':
                n = int(args[0])
                p = args[1]
                self.out.append('fa%d' % n)
                for i in range(max(0, min(n, 1000))):
                    self.out.append('f%08x' % (self.load_word(p, i * 4, 4) & MASK32))
                return None
        if name in BUILTIN_VOID:
            # ★★ 两个内建的**字节数 = n × 元素宽度** ★★
            #   LLVM 的签名是 `memcpy(dst, src, n, align)` / `memset(dst, val, n, align)`
            #   且 `n` 是**字节数**；它的"批量宽度"在 LLVM 里是 `align` 之外的
            #   指令属性，而我们这一层的发射器按 `iset.txt` 的口径把**元素宽度**
            #   放在第 4 个参数上（对齐/宽度这一个槽）。
            #   ⚠️ 第一版把 `args[3]` **整个忽略**、并把 `n` 当元素个数用：
            #     发射器给的是 `n = 24（字节）`、宽度 4 ⇒ 只该复制 24 字节，
            #     却被当成"24 个元素" = 96 字节 ⇒ 越界复制、把后面的数据冲掉。
            #     实测 `int c[2][3] = {{1,2,3},{4,5,6}}; return c[1][2];`
            #     gcc 是 6、我们算 0（元素在错误的偏移上）。
            #   判据与 `Mem.write_word(off, width, v)` 的 `width` 同一口径。
            esz = int(args[3]) if len(args) > 3 else 1
            if esz <= 0:
                esz = 1
            _ = esz   # 见下：`n` 已经是字节数，宽度只决定**怎么走**
            if name == 'llvm.memset':
                p, val, n = args[0], int(args[1]) & MASK32, int(args[2])
                if val != 0:
                    raise Unsupported('memset with non-zero value')
                if n > 10 ** 7:
                    raise Unsupported('memset too large (%d)' % n)
                for off in range(0, n, esz):
                    p.mem.cells.pop((p.off + off) & MASK64, None)
                return None
            # llvm.memcpy
            dst, src, n = args[0], args[1], int(args[2])
            if n > 10 ** 7:
                raise Unsupported('memcpy too large (%d)' % n)
            step = esz if esz in (1, 2, 4, 8) else 4
            words = {}
            for off in range(0, n, step):
                v = src.mem.cells.get((src.off + off) & MASK64)
                if v:
                    words[off] = v
            for off, v in words.items():
                dst.mem.cells[(dst.off + off) & MASK64] = v
            return None
        if name in RUNTIME_RET:
            if name == 'getint':
                return self.next_int()
            if name == 'getch':
                return 65 + (self.next_int() & 15)
            if name == 'getfloat':
                return self.next_float()
            if name == 'getarray':
                p = args[0]
                n = 8
                for i in range(n):
                    self.store_word(p, i * 4, 4, self.next_int() & MASK32)
                return n
            if name == 'getfarray':
                p = args[0]
                n = 8
                for i in range(n):
                    self.store_word(p, i * 4, 4, bits_of_f32(self.next_float()))
                return n
        return call_func(name, args)

    # ── 公共收尾：全局内存摘要 ───────────────────────────────────────────
    def finish_trace(self, ret):
        h = hashlib.sha256()
        for name in sorted(self.globals):
            g = self.globals[name]
            h.update(name.encode())
            h.update(g.mem.digest().encode())
        return {'loops': list(self.loop_trace), 'out': list(self.out),
                'mem': h.hexdigest()[:16], 'ret': ret}

    # ── 确定性（`%a` 十六进制浮点，与 dump 的格式一致）───────────────────
    @staticmethod
    def parse_float(tok):
        if tok == 'nan':
            return float('nan')
        return f32(float.fromhex(tok) if tok.startswith('0x') or 'p' in tok else float(tok))


# 类型文本 → 字节大小（执行器用；与 C++ 的 `typeByteSize` 同口径）
_SIZE_CACHE = {}


def size_of(ty):
    """`i32`=4 · `f32`=4 · `i64`=8 · `ptr...`=8 · `[N x T]`=N×T。"""
    v = _SIZE_CACHE.get(ty)
    if v is not None:
        return v
    t = ty.strip()
    if t in ('i32', 'f32'):
        v = 4
    elif t in ('i1', 'i8'):
        v = 1
    elif t == 'i64' or t.startswith('ptr'):
        v = 8
    elif t.startswith('[') and t.endswith(']'):
        n_str, _, elem = t[1:-1].partition(' x ')
        try:
            v = int(n_str.strip()) * size_of(elem)
        except ValueError:
            v = 0
    else:
        v = 0
    _SIZE_CACHE[ty] = v
    return v
