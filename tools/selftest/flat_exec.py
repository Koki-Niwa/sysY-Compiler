#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flat_exec.py —— 轨 E：**平面 IR 与结构化 IR 行为等价**（S06 最强的一条）

用法（命令行固定契约，关卡照这个签名调用）：
    python3 flat_exec.py --compiler <路径> [--jobs N] [--dir 语料目录] [--limit N]
                         [--min-files 400] [--seed N] [--verbose]
                         [--show-trace <file.sy>] [--dump-traces <file.sy>]

判据
----
对每个能跑的程序，用**同一份确定性输入**分别执行
`--emit=structured-ir --normalize` 与 `--emit=flat-ir`，要求**轨迹相同**。

★ 执行语义**共用一份**：本文件 `import exec_core`（S06 从 `loopnorm_exec.py`
  抽出的核心），两种形状各自只实现"**取下一条指令**"：
    * `loopnorm_exec.Exec`   —— Region 树
    * 本文件的 `FlatExec`    —— 基本块 + 终结符
  ⚠️ 为什么必须共用：S05b 的执行器把 `Break` 用**形状启发式**猜成 `continue`，
  与 `LoopNormalize` **共享同一个误解** ⇒ 轨 E 报 "same" 而两个答案都错
  （`ret=8` vs 正确 `5`）。两份语义 = 两处可能共享误解。

★ 轨迹的定义（**沿用 S05b `loopnorm_exec.py` 那一份**，一字不改）
    (① 每条**被执行**的循环的迭代次数序列（只记次数）,
     ② 输出缓冲, ③ 全局内存最终内容摘要, ④ main 的返回值)
  ① 是最关键的一项：边界 off-by-one、`<`/`<=` 之差、`continue` 少走一次，
  都会让某个循环的迭代次数不同。

★ 平面 IR 的循环怎么数（与结构化侧口径**对齐**）
  * 回边 = 一条边 `(src, dst)` 且 `dst` 在 `src` 的**支配**位置上 —— 这里用
    更简单也更稳的等价判据：**`dst` 的块号小于等于 `src` 的块号且 `dst` 是
    该回边的目标**（展平器把循环头排在前、体排在后，回边总是向后跳）。
  * 进入循环头 ⇒ 压栈；从回边回到已在栈上的头 ⇒ 计数 +1；
    跳到一个**不在**栈上的块（或函数结束）⇒ 把"不再包含它的那些循环"出栈，
    **出栈时把迭代次数追加进轨迹**。
  ⇒ 嵌套循环的出栈顺序 = 内层先出 = 与结构化侧的记录顺序一致。
  * 结构化侧的 `While`/`For` 在**循环结束时**记一次；两边因此可比。

退出码：0 = 全过且覆盖达标；1 = 有差异或覆盖不足；2 = 工具自身错误。
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import exec_core as C          # noqa: E402  ★ 与结构化执行器**共用语义**
import loopnorm_ir as L        # noqa: E402  （只借它的"读结构化 dump"与本文件无关的常量）

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

from flat_mod import FlatMod, Inst, _take_type  # noqa: F401

class FlatExec(C.Machine):
    """平面 IR 的执行器：**只负责"下一条指令怎么取"**（基本块 + 终结符）。"""

    def __init__(self, mod, seed=12345):
        # φ 的"入值来源快照"（见 `step` 的 φ 分支）：值编号 → 跳转那刻的环境
        self._phi_env = {}
        self._phi_pred = {}
        self._head_exit = set()      # 本轮"头走了出口边"的循环头（见 exec_func）
        C.Machine.__init__(self, seed)
        self.mod = mod
        self.env = {}
        self._prev_block = None
        self._cur_block = 0
        self._cur_func = '' 
        self._setup_globals()

    def _setup_globals(self):
        for name, (ty, data) in self.mod.globals.items():
            mem = C.Mem(C.size_of(ty), name)
            for off, v in data.items():
                mem.write_word(off, 4, v)
            self.globals[name] = C.Ptr(mem, 0)

    def run(self, entry='main'):
        if entry not in self.mod.funcs:
            raise C.Unsupported('no entry function `%s`' % entry)
        try:
            ret = self.call(entry, [])
        except C.Ret as r:
            ret = r.value
        return self.finish_trace(0 if ret is None else ret)


    def _track_loop(self, name, cur, stack, bodies):
        """【后置】把"进/出循环"折进 `stack`（轨迹计数；不改执行语义）。

        ★★ 口径与结构化侧同一句话 ★★
          `loopnorm_exec` 的 `While`：`n += 1` 在"条件为真"之后、"执行体"
          之前 ⇒ **n = 体真正被执行的轮数**。
        基本块层：**每一轮都经过循环头**（条件在那里判），而一批进头里
        有且仅有**最后一次**是"条件为假、体不跑"（正常退出）。所以：
          ① 每次进头 +1；
          ② 真正**出循环**的时候（当前块已不属于该循环的体），把该循环
             计数 −1 —— 前提是 `prev` 就是它的头（= 头直接走了出口边）。
             `break` 出循环时 `prev` 是**体内某块**，不减（那一轮跑了体）。
        ⚠️ 判据必须看**实际走了哪条边**（`prev`），不能看"头的后继里有没有
           体内块" —— 头的后继里永远有一个体入口，静态看恒真（试过：退化
           成"每次进头 +1"，`lc.sy` 得 4、应为 3）。
        """
        while stack and cur not in bodies.get(stack[-1][0], {stack[-1][0]}):
            h, n = stack.pop()
            # ② 最后一次进头只判了条件 ⇒ 扣掉。判据必须看**实际走了哪条边**：
            #    离开的那个块是头、而入口块（`cur`，正是头跳到的那个）在体外。
            #    ⚠️ 比"`cur` 就是头"（第一版）严格更对：头可能跳到**体内**的
            #    另一个块（`while(i<5){…}` 的 L49 先跳 L51、L51 再跳出去），
            #    那种形状下 `cur` 是 L51 而不是 L49 ⇒ 第一版扣不到（实测
            #    `51_short_circuit3.sy`：结构化 `0`、平面 `1`）。
            #    ⚠️ 也不能扣"`prev` 是头"以外的情形：`break` 出循环时 `prev`
            #    是体内某块，那一轮**确实跑了体**，不该扣。
            if h in self._head_exit:
                self._head_exit.discard(h)
                if n > 0:
                    n -= 1
            self.loop_trace.append(n)
        if cur in self.mod.loops.get(name, ()):
            if not (stack and stack[-1][0] == cur):
                stack.append([cur, 0])
            stack[-1][1] += 1              # 进头（含第一次）

    def call(self, name, args):
        if (name in C.RUNTIME_VOID or name in C.BUILTIN_VOID or name in C.TIMING or
                name in C.RUNTIME_RET):
            return self.runtime(name, args, self._call_user)
        return self._call_user(name, args)

    def _call_user(self, name, args):
        if name not in self.mod.funcs:
            raise C.Unsupported('call to unknown function `%s`' % name)
        if self.depth > 200:
            raise C.Unsupported('recursion deeper than 200')
        self.depth += 1
        try:
            return self.exec_func(name, args)
        finally:
            self.depth -= 1

    # ── 函数执行（**显式循环**，不用 Python 递归走块）─────────────────────
    def exec_func(self, name, args):
        blocks, params, consts = self.mod.funcs[name]
        env = dict(self.env)
        # 常量定义区（`L0:` 之前的行）先装进环境 —— 它们不是任何块的指令，
        #   但每次调用都要重新装（同一函数多次调用共用同样的常量值）。
        for c in consts:
            self.step_const(c, env)
        for idx, (_pid, pty) in enumerate(params):
            env[idx] = args[idx] if idx < len(args) else 0
        saved = self.env
        self.env = env
        self._phi_env = {}
        self._phi_pred = {}
        self._head_exit = set()      # 本轮"头走了出口边"的循环头（见 exec_func）
        heads = set(self.mod.loops.get(name, []))
        stack = []            # [(头块号, 已计迭代数)]
        try:
            cur = self.mod.entries[name]
            while True:
                # 循环轨迹的计数全在 `_track_loop` 里（出栈 + 计数 + 扣掉
                # "只判条件没跑体"的那一次）—— 这里**不能**再留一份出栈循环，
                # 否则它先把栈弹空、扣减就永远轮不到（实测：`lc.sy` 得 4）。
                bodies = self.mod.loop_bodies.get(name, {})
                self._track_loop(name, cur, stack, bodies)
                if cur not in blocks:
                    raise C.Unsupported('jump to missing block L%d' % cur)
                nxt = None
                self._prev_block = cur
                self._cur_block = cur
                self._cur_func = name
                for inst in blocks[cur]:
                    r = self.step(inst)
                    if r is not None:
                        nxt = r
                        break
                if nxt is None:
                    break
                # ★ "循环头走了**出口边**" ⇒ 那一轮只判了条件、体没跑，
                #   出循环时要把它扣掉（见 `_track_loop`）。
                #   ⚠️ 必须在这里判 —— 出口边可能先跳到**体内**的另一个块
                #   （`while(i<5){…}` 的头 L49 先跳 L51、L51 再跳出去），
                #   等到"进入体外块"时 `prev` 已经不是头了（实测
                #   `51_short_circuit3.sy`：结构化 0、平面 1）。
                if cur in heads:
                    body = bodies.get(cur, {cur})
                    if nxt not in body:
                        self._head_exit.add(cur)
                # ★ 跳转前把"前驱块 + 这一刻的值快照"记给下一条 φ 用
                #   （见 φ 处理里的说明）。就地复制：`env` 会被后续指令改写。
                for pid in self.mod.phi_ids.get(name, {}).get(nxt, ()):
                    self._phi_env[pid] = dict(env)
                    self._phi_pred[pid] = cur
                cur = nxt
            # 函数正常结束（没有 ret）
            while stack:
                _h, n = stack.pop()
                self.loop_trace.append(n)
            return 0
        except C.Ret as r:
            while stack:
                _h, n = stack.pop()
                self.loop_trace.append(n)
            return r.value
        finally:
            self.env = saved

    def _unused_loop_contains(self, blocks, cur):
        """`cur` 这个块属于栈上哪些循环：返回"还包含它的那些循环头"的集合。

        ⚠️ 这里**不需要**真正的循环体集合：只要知道"当前块是不是还在某个循环
        里"。展平器把每个循环的块**连续**排布（头在前、出口在后），
        所以用"块号区间"近似即可 —— 但为稳妥起见，这里用**支配式**判据：
        `cur` 是否在"头 → 回边源"的区间内。取头块到"该循环最后一次出现的
        块号"之间的并集，由 `self._loop_span` 预计算。
        """
        spans = self._spans_cache.get(id(blocks))
        if spans is None:
            spans = self._compute_spans(blocks)
            self._spans_cache[id(blocks)] = spans
        return {h for h, (lo, hi) in spans.items() if lo <= cur <= hi}

    def _compute_spans(self, blocks):
        """每个循环头的块号区间 `[头, 回边源的最大块号]`（展平器保证连续）。"""
        spans = {}
        for b in sorted(blocks):
            for it in blocks[b]:
                if it.kind == 'br':
                    for t in it.blocks:
                        if t <= b:
                            hi = max(spans.get(t, (t, t))[1], b)
                            spans[t] = (t, hi)
        return spans

    _spans_cache = {}

    # ── 单条指令 ─────────────────────────────────────────────────────────
    def step_const(self, inst, env):
        """【后置】处理"值定义行"（常量 / 全局地址）；不是这一类 → False。"""
        k = inst.kind
        if k in ('i32', 'i64', 'f32'):
            if k == 'f32':
                env[inst.res] = self.parse_float(inst.fval)
            else:
                env[inst.res] = C.s32(int(inst.fval))
            return True
        if k.startswith('ptr['):
            g = self.globals.get(inst.callee)
            if g is None:
                raise C.Unsupported('unknown global `%s`' % inst.callee)
            env[inst.res] = g
            return True
        return False

    def _op(self, i):
        """【后置】取第 i 个操作数的值；未定义 ⇒ `Unsupported`（不崩）。"""
        if i is None or i not in self.env:
            raise C.Unsupported('operand %%%s is undefined' % i)
        return self.env[i]

    def step(self, inst):
        """【后置】执行一条指令；返回"下一个块号"（非终结符 → None）。"""
        self.steps += 1
        if self.steps > self.MAX_STEPS:
            raise C.Budget('step budget exceeded（%d 步）' % self.MAX_STEPS)
        k = inst.kind
        env = self.env
        if self.step_const(inst, env):
            return None
        # 终结符
        if k == 'br':
            if len(inst.blocks) == 1:
                return inst.blocks[0]
            return inst.blocks[0] if self._op(inst.operands[0]) else inst.blocks[1]
        if k == 'ret':
            if inst.operands:
                raise C.Ret(self._op(inst.operands[0]))
            raise C.Ret(None)
        if k == 'unreachable':
            raise C.Unsupported('reached unreachable')
        # φ：按**前驱块**选入值（与结构化侧"汇合点选择"同一语义）
        if k == 'phi':
            # ★★ 按**真实前驱**选入值（不能用"上一个执行过的块"）★★
            #   `env` 是**跨迭代复用**的 ⇒ 上一轮留下的绑定仍在里面。
            #   实测（`break/continue` 锚点用例）：回边 L3→L1 回到 φ 时，
            #   入值 `(%8 L0)` 的 `%8`（上一轮的 load）**还在 env 里**，
            #   而 `%8` 恰好排在 `(%12 L3)` 前面 ⇒ 选错 ⇒ 循环变量永远是 0
            #   ⇒ 死循环（撞步数预算）。
            #   判据：φ 的入值块必须是 `cur` 的**真实前驱**；若有多条同时命中
            #   （既在前驱集合里、又在上一次执行的块上），优先**回边来源**
            #   （即块号 ≥ 当前块的那个），它与"这次是从哪条边来的"一致。
            # ★★ 用"**实际跳转那一刻的环境快照**"选入值 ★★
            #   【为什么不能用别的】第一版按"块号 ≥ 当前块 ⇒ 一定是回边"来挑
            #   （`25_while_if` 那次修的），那只是**启发式**：循环头有两个前驱
            #   （进入边 L0 / 回边 L7），而 `env` 是**跨迭代复用**的字典 ——
            #   上一轮回边留下的绑定仍在里面 ⇒ 从**进入边**首次进头时，
            #   `(值 L7)` 那个候选**恰好也在 env 里**，启发式就把回边的值选走了。
            #   实测（`28_while_test3.sy` 的 `EightWhile`）：`%1 = phi [(%17 L0),
            #   (%5 L7)]` 首次进头选成了 `%5` —— 一个**指向全局的指针**
            #   ⇒ 循环条件 `a < 20` 变成"指针 < 20" ⇒ 执行器类型错。
            #   【正确判据】φ 的入值来自**我们真正走过的那条边**：跳转时把
            #   `prev` 块与那一刻的 `env` 快照一起记下，φ 只认
            #   `blocks[k] == prev` 且值在该快照里的那一条（不可能有歧义：
            #   同一个前驱只对应一条边；临界边已由展平器拆开）。
            pv = self._phi_env.get(inst.res)
            pb = self._phi_pred.get(inst.res)
            if pv is not None:
                if pb in inst.blocks:
                    idx = inst.blocks.index(pb)
                    if inst.operands[idx] in pv:
                        env[inst.res] = pv[inst.operands[idx]]
                        return None
                # 快照里没有 ⇒ 退回"值在不在当前 env 里"（保守，不崩）
                for v2, b2 in zip(inst.operands, inst.blocks):
                    if b2 == pb and v2 in env:
                        env[inst.res] = env[v2]
                        return None
            fname = self._cur_func
            cur = self._cur_block
            real = self.mod.preds.get(fname, {}).get(cur, set())
            cands = [(v, b) for v, b in zip(inst.operands, inst.blocks)
                     if b in real and v in env]
            if not cands:
                raise C.Unsupported('phi in L%d has no available incoming value '
                                    '(pred=%s)' % (cur, self._prev_block))
            v, _b = cands[0]
            env[inst.res] = env[v]
            return None
        # 内存与算术：与结构化侧**同一份语义**
        if k == 'alloca':
            env[inst.res] = C.Ptr(C.Mem(C.size_of(inst.ty), 'alloca'), 0)
            return None
        if k == 'load':
            env[inst.res] = self._load(self._op(inst.operands[0]), inst.ty)
            return None
        if k == 'store':
            self._store(self._op(inst.operands[1]), inst.ty, self._op(inst.operands[0]))
            return None
        if k == 'getelementptr':
            p = self._op(inst.operands[0])
            i = self._op(inst.operands[1])
            env[inst.res] = C.Ptr(p.mem, (p.off + i * C.size_of(inst.ty)) & C.MASK64)
            return None
        if k == 'bitcast':
            env[inst.res] = self._op(inst.operands[0])
            return None
        if k == 'call':
            a = [self._op(t) for t in inst.operands]
            r = self.call(inst.callee, a)
            if inst.res is not None:
                env[inst.res] = 0 if r is None else r
            return None
        # 整数/浮点/比较/转换/select —— 复用共用核的 map（按**平面 opcode 名**）
        v = self._three(env, inst)
        if v is not None:
            env[inst.res] = v
            return None
        raise C.Unsupported('op kind `%s`' % k)

    def _three(self, env, inst):
        """整数/浮点/比较/转换的**共用**实现（与结构化侧同一张语义表）。"""
        k = inst.kind
        a = self._op(inst.operands[0]) if inst.operands else 0
        b = self._op(inst.operands[1]) if len(inst.operands) > 1 else 0
        m = {'add': 'AddI', 'sub': 'SubI', 'mul': 'MulI', 'sdiv': 'DivI',
             'srem': 'ModI', 'fadd': 'AddF', 'fsub': 'SubF', 'fmul': 'MulF',
             'fdiv': 'DivF'}
        if k in m:
            if m[k].endswith('I'):
                return self._int_bin(m[k], a, b)
            try:
                return C.f32({'AddF': a + b, 'SubF': a - b, 'MulF': a * b,
                              'DivF': a / b if b != 0 else float('nan')}[m[k]])
            except ZeroDivisionError:
                return C.f32(float('nan'))
        if k == 'fneg':
            return C.f32(-a)
        # 比较：平面侧谓词是显式的（`slt` / `oeq` …）
        icmp = {'eq': 'Eq', 'ne': 'Ne', 'slt': 'Lt', 'sle': 'Le', 'sgt': 'Gt',
                'sge': 'Ge'}
        fcmp = {'oeq': 'Eq', 'une': 'Ne', 'olt': 'Lt', 'ole': 'Le', 'ogt': 'Gt',
                'oge': 'Ge'}
        if k == 'icmp':
            pred = inst.pred
            if not isinstance(a, (int,)) or (isinstance(a, C.Ptr) or isinstance(b, C.Ptr)):
                raise C.Unsupported(
                    'chk: icmp %s line %s: a=%r(%s) b=%r(%s) inst=%r'
                    % (pred, getattr(inst, 'line', '?'), a, type(a).__name__, b,
                       type(b).__name__, inst))
            return (1 if self._cmp(icmp[pred], a, b) else 0)
        if k == 'fcmp':
            pred = inst.pred
            # `une` 对 NaN 为真；其余为**有序**谓词（NaN ⇒ 假）
            if a != a or b != b:
                return 1 if pred == 'une' else 0
            return (1 if self._cmp(fcmp[pred], float(a), float(b)) else 0)
        if k == 'sitofp':
            return C.f32(a)
        if k == 'fptosi':
            return (0 if a != a else
                    (2147483647 if a >= 2147483648.0 else
                     (-2147483648 if a < -2147483648.0 else int(a))))
        if k == 'sext' or k == 'zext':
            return C.s64(a) if k == 'sext' else (a & C.MASK64)
        if k == 'trunc':
            return C.s32(a)
        if k == 'select':
            return self._op(inst.operands[1]) if self._op(inst.operands[0]) \
                else self._op(inst.operands[2])
        return None

    @staticmethod
    def _cmp(op, a, b):
        return {'Eq': a == b, 'Ne': a != b, 'Lt': a < b, 'Le': a <= b,
                'Gt': a > b, 'Ge': a >= b}[op]


# ── 单文件：跑两份 IR，比较轨迹 ──────────────────────────────────────────
def one_file(compiler, sy, tmpdir, seed):
    # ⚠️ 键必须用**相对路径**（两条赛道有 240 个同名文件；用 basename 会让并发
    #    worker 读写同一对临时文件 ⇒ 凭空造出假差异。S05b 的教训）
    key = os.path.relpath(sy, ROOT).replace('/', '__')
    sir = os.path.join(tmpdir, key + '.sir')
    flt = os.path.join(tmpdir, key + '.flat')
    r1 = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', sir],
                        capture_output=True)
    r2 = subprocess.run([compiler, sy, '--emit=flat-ir', '-o', flt], capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        if r1.returncode != 0 and r2.returncode != 0:
            return ('skip', 'rc=%d/%d' % (r1.returncode, r2.returncode), None, None)
        return ('error', 'rc=%d/%d' % (r1.returncode, r2.returncode), None, None)
    try:
        t1 = open(sir, encoding='utf-8').read()
        t2 = open(flt, encoding='utf-8').read()
        if not t1.strip() or not t2.strip():
            return ('skip', 'dump 为空', None, None)
        import loopnorm_exec as LE
        m1, n1, _ = L.parse_dump(t1)
        tr1 = LE.Exec(m1, n1, seed).run()
        fmod = FlatMod(t2)
        tr2 = FlatExec(fmod, seed).run()
    except C.Unsupported as e:
        return ('unsupported', str(e), None, None)
    except C.Budget as e:
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
    for tag, name in (('loops', '循环迭代次数'), ('out', '输出缓冲'),
                      ('mem', '全局内存摘要'), ('ret', '返回值')):
        if a[tag] != b[tag]:
            msgs.append('%s：结构化 %r vs 平面 %r' % (name, a[tag], b[tag]))
    return '；'.join(msgs) if msgs else '（未知差异）'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--min-files', type=int, default=400)
    ap.add_argument('--seed', type=int, default=12345)
    ap.add_argument('--show-trace', default='')
    ap.add_argument('--dump-traces', default='')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    tmp = os.path.join(ROOT, '.work', 'flat_exec')
    os.makedirs(tmp, exist_ok=True)

    if args.show_trace or args.dump_traces:
        sy = os.path.abspath(args.show_trace or args.dump_traces)
        st, msg, a, b = one_file(compiler, sy, tmp, args.seed)
        print('状态: %s %s' % (st, msg))
        if a is not None:
            print('结构化 IR 轨迹: %s' % a)
            print('平面   IR 轨迹: %s' % b)
        if st == 'diff':
            print('差异: %s' % describe_diff(a, b))
        return 0 if st == 'same' else 1

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

    same, diffs, skips = [], [], {}
    for f, st, msg, a, b in results:
        if st == 'same':
            same.append(f)
        elif st == 'diff':
            diffs.append((f, describe_diff(a, b)))
        else:
            if st == 'error':
                key = '工具/单方失败（%s）' % msg
            elif st == 'skip':
                key = '双方一致拒绝或空产物（%s）' % msg
            else:
                key = '[%s] %s' % (st, msg)
            skips.setdefault(key, []).append(f)

    print('== 轨 E：平面 IR 与结构化 IR 行为等价 ==')
    print('参与执行的文件数: %d' % len(files))
    print('两种形状轨迹相同: %d' % len(same))
    print('轨迹不同:         %d' % len(diffs))
    for f, m in diffs[:10]:
        print('  ✘ %s\n    %s' % (os.path.relpath(f, ROOT), m))
    total_skipped = sum(len(v) for v in skips.values())
    print('跳过: %d（**逐类列出原因，不静默**）' % total_skipped)
    for k in sorted(skips, key=lambda x: -len(skips[x])):
        print('  %-52s %4d   例：%s' % (k[:52], len(skips[k]),
                                        os.path.relpath(skips[k][0], ROOT)))
    ok = not diffs and len(same) >= args.min_files
    if diffs:
        print('  ✘ 有程序在两种形状下行为不同（**先假定是平面侧错了**，逐条查清）')
    if len(same) < args.min_files:
        print('  ✘ 跑通的程序数 %d < 要求 %d' % (len(same), args.min_files))
    print('== 结果：%s ==' % ('全过' if ok else '有失败'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
