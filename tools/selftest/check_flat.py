#!/usr/bin/env python3
# ============================================================================
# check_flat.py —— S06 验收轨 A/B/C 的检查器
#
#   轨 A：往返与冻结契约
#     `--emit=flat-ir` → `--from-flat` → 再 dump，**逐字节相同**；
#     与 `compiler/tests/flat/example/` 样例对逐字节相同。
#   轨 B：不变式（平面 IR 的 6 条，见 prompt §六 轨 B）
#     1. use-def 双向一致          2. SSA 支配（**真支配树**，不是近似）
#     3. φ 合法（块首 / 个数 == 前驱数 / **前驱集合 == 入值块集合**）
#     4. 终结符恰好一个且在最后    5. `alloca` 全在入口块
#     6. 指令集封闭（opcode 全在手写清单里）
#     ★ 并且要**反证**：手工构造 ≥6 份坏 IR，逐条报红（`--bad`）。
#     ⚠️ 轨 B 的"独立"含义：本文件**自己**从 dump 文本重建 CFG 与支配树，
#        不调用编译器里的 `FlatVerifier` —— 否则就是用被测物验证被测物。
#        它同时给出**独立手写**的 opcode 清单（与 `iset.txt` 核对）。
#   轨 C：覆盖性与规模
#     全部文件产出平面 IR；块数/指令数/φ 数的分布（min/median/max）；
#     与结构化 IR 的 Op 数之比（比值 ≈0 或异常小 ⇒ 静默丢内容）。
#
#   用法：
#     python3 compiler/tools/selftest/check_flat.py                  # 全量（轨 A/B/C）
#     python3 compiler/tools/selftest/check_flat.py --jobs 12
#     python3 compiler/tools/selftest/check_flat.py --bad            # 只跑坏 IR 反证
#     python3 compiler/tools/selftest/check_flat.py --only 52_scope  # 只看匹配的文件
#
#   预算是**步数**、不是墙钟（S05b 的教训）；临时文件按**相对路径**做键
#   （S05b 的教训：两条赛道有 240 个同名文件，用 basename 并发时会凭空造差异）。
# ============================================================================
import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

# ── 独立手写的指令集清单（**照 `docs/handoff/iset.txt` 逐条抄，不是从
#    `Instruction.h` 生成的**）。生成式的清单只能证明"实现与实现一致"。
OPCODES = {
    # 终结符（4）
    'br', 'ret', 'unreachable', 'switch',
    # 算术 / 逻辑（i32 / i64 / f32，按 iset 的助记符）
    'add', 'sub', 'mul', 'sdiv', 'srem', 'fadd', 'fsub', 'fmul', 'fdiv', 'fneg',
    'and', 'or', 'xor', 'shl', 'lshr', 'ashr', 'icmp', 'fcmp',
    # 内存
    'alloca', 'load', 'store', 'getelementptr', 'bitcast',
    # 类型转换
    'zext', 'sext', 'trunc', 'sitofp', 'fptosi', 'fpext', 'fptrunc',
    'ptrtoint', 'inttoptr',
    # 其它
    'call', 'phi', 'select',
}
TERMINATORS = {'br', 'ret', 'unreachable', 'switch'}
# 常量形态（dump 里的"无名"行，不是指令）
CONST_OPS = {'i32', 'i64', 'f32', 'ptr'}

# ★ "冗余 φ"与"违反不变量"分开统计：前者不改变执行结果（只是多放了一个 φ），
#   把它混进"违规"会让"490/490 合法"这个结论变得不可读。
STRICT = [False]
REDUNDANT = []

LABEL_RE = re.compile(r'^L(\d+):$')
# 指令行：  %12 = add i32 %3, %4 @line 7
INST_RE = re.compile(r'^\s*%(\d+)\s*=\s*([a-z][a-z0-9.]*)\s+(.*?)\s*(?:@line\s+(\d+))?$')
# 常量行：  %3 = i32 0 @line 4 / %9 = ptr[i32] @a @line 1
#   ⇒ 形态是"类型 + 字面量/地址"，**没有 opcode**。判据必须**确定性**地
#     区分它与指令行，否则常量会被当成"发明的 opcode"（实测：`i32` 被报
#     成 B6 违规 —— 345 次/60 文件，把整条轨 B 淹掉）。
#   ⚠️ 类型可能是嵌套的 `ptr[ptr[[20 x i32]]]` ⇒ 必须做**括号配平**，
#     不能用 `\S+`（实测 `ptr[[8` 这种半截 token）。
def split_rhs(rhs):
    """【后置】(类型文本 or None, 余下文本)。类型 = 开头到括号配平的 `]`。"""
    i = 0
    depth = 0
    while i < len(rhs):
        ch = rhs[i]
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        elif ch == ' ' and depth == 0:
            return rhs[:i], rhs[i:].strip()
        i += 1
    return rhs, ''


CONST_TYPES = {'i32', 'i64', 'f32', 'void'}
# ⚠️ 浮点常量是 **C99 十六进制浮点**（`0x1p+1`、`-0x1.8p+0`、`0x0p+0`），
#   不是十进制 —— `FlatDump` 用 `%a` 打印（见它的 `appendFloat`）。
#   只认十进制会让**每一个带浮点常量的文件**被假报 B6"发明了 opcode `f32`"。
_value_re = re.compile(
    r'^(-?\d+(?:\.\d+)?'
    r'|-?0[xX][0-9a-fA-F]*(?:\.[0-9a-fA-F]*)?[pP][+-]?\d+'
    r'|@[A-Za-z_][A-Za-z0-9_.]*|%\.\d+)$')
# 形参行：  （dump 里形参写在 define 行内，这里留个口子备将来用）
# ⚠️ dump 里**每条指令都带 `@line N` 后缀**（`br L3 @line 6`）⇒ 终结符的
#   正则必须用 `\s+label` 这类**紧贴形态**，不能用 `$` 收尾（实测踩到：
#   `^br\s+label\s+L(\d+)$` 一条都匹配不上 ⇒ 所有后继丢失 ⇒ 前驱全空 ⇒
#   398 个文件假报 B3）。
BR_RE = re.compile(r'^br\s+(?:i1\s+)?(%\d+|true|false)\s*,\s*label\s+L(\d+)\s*,\s*label\s+L(\d+)')
# ★ 契约（`docs/handoff/03-设计/平面IR与dump格式.md` §2 的语法）写成
#   `br LABEL` —— **没有 `label` 关键字**。本检查器第一版按 LLVM 的习惯写成
#   `br label L3` ⇒ 一条单向分支都匹配不上 ⇒ 后继全丢 ⇒ 假报 B3（398 个文件）。
BR1_RE = re.compile(r'^br\s+(?:label\s+)?L(\d+)')
SWITCH_RE = re.compile(r'^switch\b')
RET_RE = re.compile(r'^ret\b')
UNREACH_RE = re.compile(r'^unreachable\b')


# ============================================================================
# ① 解析 dump
# ============================================================================
class BB:
    __slots__ = ('idx', 'insts', 'succs', 'preds', 'phis', 'labels')

    def __init__(self, idx):
        self.idx = idx
        self.insts = []       # (resultId or None, opcode, text, line)
        self.succs = []
        self.preds = []
        self.phis = []        # (resultId, [(valName, predBlockIdx), ...])
        self.labels = []


class Fn:
    def __init__(self, name):
        self.name = name
        self.blocks = {}      # idx -> BB
        self.order = []       # 按出现顺序
        self.instCount = 0
        self.params = []      # 形参的值名（定义点在入口块）

    def bb(self, idx):
        if idx not in self.blocks:
            self.blocks[idx] = BB(idx)
            self.order.append(idx)
        return self.blocks[idx]


def parse_dump(text):
    """【前置】`--emit=flat-ir` 的文本。【后置】(函数名 -> Fn, 错误清单)。

    本函数**只认格式，不认语义**：语义检查全在下面的 invariants 里。
    """
    fns = {}
    errs = []
    cur = None
    blk = None
    for lineno, raw in enumerate(text.split('\n'), 1):
        line = raw.rstrip()
        if not line.strip():
            continue
        if line.startswith('define '):
            m = re.match(r'^define\s+\S+\s+@([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*\{?', line)
            if not m:
                errs.append('第 %d 行：无法解析的 define：%s' % (lineno, line))
                continue
            cur = Fn(m.group(1))
            # ★ 形参也是**定义点**（契约：形参写在 define 行内，如
            #   `define i32 @func(i32 %0)`）。不登记的话，函数体里第一次用
            #   形参就会被假报 "用了未定义的值 %0"（实测 177 个文件）。
            for pm in re.finditer(r'%([0-9]+)', m.group(2) or ''):
                cur.params.append(int(pm.group(1)))
            fns[cur.name] = cur
            blk = None
            continue
        if line == '}':
            cur = None
            blk = None
            continue
        if cur is None:
            continue      # 全局行：不参与函数内的判据
        m = LABEL_RE.match(line)
        if m:
            blk = cur.bb(int(m.group(1)))
            continue
        b = blk if blk is not None else cur.bb(0)   # define 之后、首个标签之前
        b.insts.append((lineno, line))
        cur.instCount += 1
    # ② 解析每条指令：填 succs / phis
    for fn in fns.values():
        for idx in fn.order:
            b = fn.blocks[idx]
            for _, line in b.insts:
                body = line.strip()
                m = BR_RE.match(body)
                if m:
                    b.succs = [int(m.group(2)), int(m.group(3))]
                    continue
                m = BR1_RE.match(body)
                if m:
                    b.succs = [int(m.group(1))]
                    continue
                if SWITCH_RE.match(body):
                    b.succs = [int(x) for x in re.findall(r'L(\d+)', body)]
                    continue
                if body.startswith('%') and '= phi ' in body:
                    vals = re.findall(r'\((%\d+)\s+L(\d+)\)', body)
                    # ⚠️ 存的必须是**去缩进后**的文本：`invariants` 要拿它跟
                    #   `b.insts[0][1].strip()` 比"φ 是否在块首"。存原始带缩进的
                    #   行会让比较永远不等 ⇒ 假报 B3（实测 362 个文件）。
                    b.phis.append((body.strip(), [(v, int(p)) for v, p in vals]))
        for idx in fn.order:
            for s in fn.blocks[idx].succs:
                fn.bb(s).preds.append(idx)
    return fns, errs


# ============================================================================
# ② 支配树（迭代数据流 —— 不许用"看起来对"的近似）
# ============================================================================
def dominators(fn, entry=0):
    """【后置】dom[b] = 支配 b 的块集合（含 b 自己）。

    用经典的迭代算法（Cooper 的"逆后序 + 交集"）。**必须**是真支配集，
    因为轨 B 的 SSA 判据（使用点被定义点支配）依赖它，而"看起来对"的
    近似（比如"编号更小"）会在回边处错。
    """
    if entry not in fn.blocks:
        return {}
    order = fn.order
    # 逆后序（**迭代** DFS：性能用例的块数上万，递归会爆 Python 栈 ——
    #   实测 `RecursionError`；S02 的 `destroyTree` 也是同一条教训）
    seen = set()
    post = []
    stack = [(entry, 0)]
    seen.add(entry)
    while stack:
        u, k = stack.pop()
        succs = [v for v in fn.blocks[u].succs if v in fn.blocks]
        if k < len(succs):
            stack.append((u, k + 1))
            v = succs[k]
            if v not in seen:
                seen.add(v)
                stack.append((v, 0))
        else:
            post.append(u)
    rpo = list(reversed(post))
    index = {b: i for i, b in enumerate(rpo)}
    allb = set(rpo)
    dom = {b: (set(allb) if b != entry else {entry}) for b in rpo}
    changed = True
    while changed:
        changed = False
        for b in rpo[1:]:
            preds = [p for p in fn.blocks[b].preds if p in index]
            if not preds:
                new = {b}          # 不可达块：只支配自己
            else:
                new = set.intersection(*[dom[p] for p in preds]) | {b}
            if new != dom[b]:
                dom[b] = new
                changed = True
    return dom


def dominates(dom, a, b):
    return a in dom.get(b, set())


# ============================================================================
# ③ 轨 B：六条不变式
# ============================================================================
def defs_of(fn):
    """【后置】值名 -> (块号, 行号)。值名用 `%N` 里的 N（函数内唯一）。"""
    d = {}
    dup = []
    for n in fn.params:
        d[n] = (0, '形参')
    for idx in fn.order:
        for _, line in fn.blocks[idx].insts:
            body = line.strip()
            m = re.match(r'^%(\d+)\s*=', body)
            if not m:
                continue
            n = int(m.group(1))
            if n in d:
                dup.append((n, d[n][0], idx))
            else:
                d[n] = (idx, line)
    return d, dup


def scan_uses(body):
    """【后置】一行里用到的值名列表（`%N`）。φ 的入值也算使用。"""
    return [int(x) for x in re.findall(r'%(\d+)', body)]


def invariants(fn):
    """【后置】违反的不变式清单（人类可读，带不变式编号）。"""
    errs = []
    dom = dominators(fn)
    defs, dup = defs_of(fn)
    for n, first, second in dup:
        errs.append('[B1] 值 %%s 被定义了两次（L%s 与 L%s）' % (first, second) % n)
    # ── B1：use-def 双向一致 ────────────────────────────────────────────
    for idx in fn.order:
        b = fn.blocks[idx]
        for _, line in b.insts:
            body = line.strip()
            if re.match(r'^%(\d+)\s*=', body):
                # 定义点右侧的操作数
                rhs = body.split('=', 1)[1]
                for u in scan_uses(rhs):
                    if u not in defs:
                        errs.append('[B1] L%d 用了未定义的值 %%%d：%s' % (idx, u, body))
    # ── B4：终结符恰好一个且在最后 ──────────────────────────────────────
    for idx in fn.order:
        b = fn.blocks[idx]
        terms = []
        for pos, (_, line) in enumerate(b.insts):
            body = line.strip()
            if BR_RE.match(body) or BR1_RE.match(body) or SWITCH_RE.match(body) \
               or RET_RE.match(body) or UNREACH_RE.match(body):
                terms.append(pos)
        if len(terms) > 1:
            errs.append('[B4] 块 L%d 有 %d 个终结符' % (idx, len(terms)))
        elif len(terms) == 1 and terms[0] != len(b.insts) - 1:
            errs.append('[B4] 块 L%d 的终结符不是最后一条（后面还有 %d 条）'
                        % (idx, len(b.insts) - 1 - terms[0]))
    # ── B5：alloca 全在入口块 ──────────────────────────────────────────
    for idx in fn.order:
        b = fn.blocks[idx]
        for _, line in b.insts:
            body = line.strip()
            if '= alloca ' in body and idx != 0:
                errs.append('[B5] 入口块之外有 alloca（L%d）：%s' % (idx, body))
    # ── B6：指令集封闭 ─────────────────────────────────────────────────
    for idx in fn.order:
        for _, line in fn.blocks[idx].insts:
            body = line.strip()
            m = INST_RE.match(body)
            if not m:
                continue
            op = m.group(2)
            if op in OPCODES:
                continue
            # 常量行不是指令 ⇒ 用**确定性**判据排除（见 split_rhs 的说明）
            ty, rest = split_rhs(op + ' ' + m.group(3))
            if (ty in CONST_TYPES or (ty.startswith('ptr[') and ty.endswith(']'))) \
               and _value_re.match(rest.split(' ')[0] if rest else ''):
                continue
            errs.append('[B6] 发明了 opcode `%s`（L%d）：%s' % (op, idx, body))
    # ── B3：φ 的合法性 ────────────────────────────────────────────────
    for idx in fn.order:
        b = fn.blocks[idx]
        # ★ "φ 全在块首" 是**整块**的性质：前 len(phis) 条必须**依次**是这些 φ。
        #   ⚠️ 不能对每个 φ 都跟"第一条"比 —— 那样第二个 φ 起必然报"不在块首"
        #   （实测：203 个文件、6853 条假报）。
        for k, (phiText, vals) in enumerate(b.phis):
            head = b.insts[k][1].strip() if k < len(b.insts) else None
            if head != phiText:
                errs.append('[B3] 第 %d 个 φ 不在块首区（L%d）：%s' % (k + 1, idx, phiText))
            got = sorted(set(p for _, p in vals))
            want = sorted(set(b.preds))
            if got != want:
                errs.append('[B3] L%d 的 φ 入值块集合 %s != 真实前驱集合 %s'
                            % (idx, got, want))
            if len(vals) != len(b.preds):
                errs.append('[B3] L%d 的 φ 入值个数 %d != 前驱个数 %d'
                            % (idx, len(vals), len(b.preds)))
            if len(set(v for v, _ in vals)) < 2:
                # ⚠️ 这一条**不比** B1–B6 的不变量：它不改变任何执行结果，
                #   只是"多放了一个 φ"。`--strict` 时才算违规。
                if STRICT[0]:
                    errs.append('[B3] L%d 的 φ **冗余**：所有入值都是同一个值 %s（判据 2'
                                ' 要求"不多放"，这种 φ 应被省略）'
                                % (idx, vals[0][0] if vals else '?'))
                else:
                    REDUNDANT.append(idx)
    # ── B2：SSA 支配 ──────────────────────────────────────────────────
    for idx in fn.order:
        b = fn.blocks[idx]
        for pos, (_, line) in enumerate(b.insts):
            body = line.strip()
            isPhi = '= phi ' in body
            for u in scan_uses(body):
                if u not in defs:
                    continue      # B1 已报
                db = defs[u][0]
                if isPhi:
                    # φ 的入值 (v Lp) 只需在**对应的前驱块末尾**可用 ⇒
                    # 在 Lp 里查；v 必须**支配 Lp 的终结符**，等价于 v 的定义点
                    # 支配 Lp 自己或 v 是常量（常量不在 CFG 里，跳过）。
                    continue
                if not dominates(dom, db, idx) and db != idx:
                    errs.append('[B2] L%d 用了 %%%d，但它的定义点 L%d **不支配** L%d'
                                % (idx, u, db, idx))
    # φ 的入值支配（单独一轮：需要"入值块"信息）
    for idx in fn.order:
        b = fn.blocks[idx]
        for _, vals in b.phis:
            for v, p in vals:
                if v not in defs:
                    continue
                db = defs[v][0]
                if not dominates(dom, db, p) and db != p:
                    errs.append('[B2] φ(L%d) 的入值 %%%d 的定义点 L%d 不支配它的'
                                '前驱块 L%d' % (idx, v, db, p))
    return errs


# ============================================================================
# ④ 轨 C：规模统计
# ============================================================================
def sizes(fn):
    nphi = sum(len(b.phis) for b in fn.blocks.values())
    return len(fn.order), fn.instCount, nphi


# ============================================================================
# ⑤ 子进程驱动
# ============================================================================
def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')


def one_file(args):
    """【后置】(相对路径, 状态, 详情, 规模, 结构化 Op 数)。

    状态：ok / frontend / flatgen / verify / roundtrip / mismatch / crash
    """
    root, comp, rel = args
    tmp = tempfile.mkdtemp(prefix='chkflat_')
    try:
        a = os.path.join(tmp, 'a.flat')
        rc, out, err = run([comp, rel, '--emit=flat-ir', '-o', a], root)
        if rc != 0:
            if '[E-FLAT-VERIFY]' in err or '[VN]' in err:
                return rel, 'verify', err.strip().split('\n')[0], None, 0
            if '[E-FLATGEN]' in err:
                return rel, 'flatgen', err.strip().split('\n')[0], None, 0
            if rc >= 128:
                return rel, 'crash', err.strip().split('\n')[-1:][0] if err else '', None, 0
            return rel, 'frontend', err.strip().split('\n')[0], None, 0
        text = open(a, encoding='utf-8').read()
        fns, perrs = parse_dump(text)
        if perrs:
            return rel, 'parse', perrs[0], None, 0
        total = [0, 0, 0]
        viol = []
        for fn in fns.values():
            n, ni, np_ = sizes(fn)
            total[0] += n
            total[1] += ni
            total[2] += np_
            for e in invariants(fn):
                viol.append('%s: %s' % (fn.name, e))
        if viol:
            return rel, 'invariant', ' | '.join(viol[:2]), tuple(total), 0
        # 轨 A：往返
        b = os.path.join(tmp, 'b.flat')
        rc2, _, err2 = run([comp, rel, '--emit=flat-ir', '-o', a], root)
        rc3, _, err3 = run([comp, '--from-flat', a, '--emit=flat-ir', '-o', b], root)
        if rc3 != 0:
            return rel, 'roundtrip', err3.strip().split('\n')[0], tuple(total), 0
        if open(b, encoding='utf-8').read() != text:
            # 找出第一处差异，便于归因
            la = text.split('\n')
            lb = open(b, encoding='utf-8').read().split('\n')
            d = next((i for i in range(min(len(la), len(lb))) if la[i] != lb[i]),
                     min(len(la), len(lb)))
            return rel, 'mismatch', '首个差异在第 %d 行：%r vs %r' % (
                d + 1, la[d] if d < len(la) else '<EOF>', lb[d] if d < len(lb) else '<EOF>'), tuple(total), 0
        # 轨 C 的分母：结构化 IR 的 Op 数
        s = os.path.join(tmp, 'a.sir')
        rc4, _, _ = run([comp, rel, '--emit=structured-ir', '--normalize', '-o', s], root)
        nops = 0
        if rc4 == 0:
            nops = len(re.findall(r'^\s*\(', open(s, encoding='utf-8').read(), re.M)) - \
                   len(re.findall(r'^\s*\(Func\b', open(s, encoding='utf-8').read(), re.M))
        return rel, 'ok', '', tuple(total), nops
    finally:
        for f in os.listdir(tmp):
            try:
                os.unlink(os.path.join(tmp, f))
            except OSError:
                pass
        os.rmdir(tmp)


# ============================================================================
# ⑥ 轨 B 的反证：手工构造的坏 IR
# ============================================================================
BAD_HEAD = 'define i32 @f() {\n'
BAD_CASES = [
    ('未支配的使用', BAD_HEAD +
     '  %0 = i32 1 @line 1\n'
     '  br i1 %3, label L0, label L1 @line 1\n'
     'L0:\n  %3 = i32 0 @line 1\n  ret i32 %0 @line 1\n'
     'L1:\n  ret i32 %0 @line 1\n}\n'),
    ('φ 入值少一个', BAD_HEAD +
     '  %0 = i32 1 @line 1\n  %1 = i32 2 @line 1\n'
     '  br i1 %0, label L0, label L1 @line 1\n'
     'L0:\n  br label L2 @line 1\n'
     'L1:\n  br label L2 @line 1\n'
     'L2:\n  %2 = phi i32 [(%0 L0)] @line 1\n  ret i32 %2 @line 1\n}\n'),
    ('两个终结符', BAD_HEAD +
     '  br label L0 @line 1\n'
     'L0:\n  br label L1 @line 1\n  ret i32 0 @line 1\n'
     'L1:\n  ret i32 0 @line 1\n}\n'),
    ('alloca 在循环里', BAD_HEAD +
     '  br label L0 @line 1\n'
     'L0:\n  %0 = alloca i32 @line 1\n  br label L0 @line 1\n}\n'),
    ('发明的 opcode', BAD_HEAD +
     '  %0 = i32 1 @line 1\n  %1 = neg i32 %0 @line 1\n  ret i32 %1 @line 1\n}\n'),
    ('引用了不存在的标签', BAD_HEAD +
     '  %0 = i32 1 @line 1\n  br label L7 @line 1\n}\n'),
    ('φ 不在块首', BAD_HEAD +
     '  %0 = i32 1 @line 1\n'
     '  br i1 %0, label L0, label L1 @line 1\n'
     'L0:\n  br label L2 @line 1\n'
     'L1:\n  br label L2 @line 1\n'
     'L2:\n  %1 = i32 7 @line 1\n  %2 = phi i32 [(%0 L0), (%1 L1)] @line 1\n'
     '  ret i32 %2 @line 1\n}\n'),
    ('φ 前驱集合不匹配', BAD_HEAD +
     '  %0 = i32 1 @line 1\n  %1 = i32 2 @line 1\n'
     '  br i1 %0, label L0, label L1 @line 1\n'
     'L0:\n  br label L2 @line 1\n'
     'L1:\n  br label L2 @line 1\n'
     'L2:\n  %2 = phi i32 [(%0 L0), (%1 L1), (%0 L1)] @line 1\n  ret i32 %2 @line 1\n}\n'),
]


def run_bad(comp, root):
    """【后置】(通过数, 总数, 失败清单)。每条坏 IR 都必须被报红。"""
    ok = 0
    fails = []
    tmp = tempfile.mkdtemp(prefix='chkbad_')
    try:
        for name, text in BAD_CASES:
            p = os.path.join(tmp, 'bad.flat')
            with open(p, 'w', encoding='utf-8') as fh:
                fh.write(text)
            rc, out, err = run([comp, '--from-flat', p, '--emit=flat-ir', '-o',
                                os.path.join(tmp, 'o.flat')], root)
            # 编译器**读回时的验证器**是第一道关；本文件的独立检查是第二道。
            try:
                fns, perrs = parse_dump(text)
            except Exception as exc:                      # noqa: BLE001
                perrs = ['解析异常：%s' % exc]
            viol = list(perrs)
            for fn in fns.values():
                viol.extend(invariants(fn))
            if (rc != 0 and ('E-FLAT' in err or 'error' in err)) or viol:
                ok += 1
            else:
                fails.append('%s（编译器 rc=%d，独立检查也没报）' % (name, rc))
    finally:
        for f in os.listdir(tmp):
            try:
                os.unlink(os.path.join(tmp, f))
            except OSError:
                pass
        os.rmdir(tmp)
    return ok, len(BAD_CASES), fails


# ============================================================================
# ⑦ 主流程
# ============================================================================
def corpus(root):
    out = []
    for dirpath, _, files in os.walk(os.path.join(root, 'tests')):
        for f in files:
            if f.endswith('.sy'):
                out.append(os.path.relpath(os.path.join(dirpath, f), root))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', default='compiler/build/compiler')
    ap.add_argument('--root', default='.')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--only', default=None, help='只跑相对路径里含该子串的文件')
    ap.add_argument('--bad', action='store_true', help='只跑轨 B 的坏 IR 反证')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--strict', action='store_true',
                    help='把"冗余 φ"（入值全同）也当成违反不变式')
    a = ap.parse_args()
    STRICT[0] = a.strict
    root = os.path.abspath(a.root)
    comp = os.path.abspath(os.path.join(root, a.compiler))

    if not a.bad:
        rels = corpus(root)
        if a.only:
            rels = [r for r in rels if a.only in r]
        total = len(rels)
        print('=== 轨 A/B/C：%d 个文件（jobs=%d）' % (total, a.jobs))
        buckets = {}
        okd = []
        with ProcessPoolExecutor(max_workers=a.jobs) as ex:
            for rel, st, detail, sz, nops in ex.map(one_file, [(root, comp, r) for r in rels]):
                buckets.setdefault(st, []).append((rel, detail))
                if st == 'ok':
                    okd.append((rel, sz, nops))
        for st in sorted(buckets, key=lambda s: -len(buckets[s])):
            v = buckets[st]
            print('  %-11s %4d' % (st, len(v)))
            if st != 'ok' and not a.quiet:
                for rel, detail in v[:8]:
                    print('      %s: %s' % (rel, detail[:150]))
                if len(v) > 8:
                    print('      …（另有 %d 个同类）' % (len(v) - 8))
        # 轨 C：规模分布
        if okd:
            def col(i):
                xs = sorted(x[1][i] for x in okd)
                return (xs[0], statistics.median(xs), xs[-1])
            print('=== 轨 C：%d 个文件全部产出平面 IR' % len(okd))
            print('    基本块 min/median/max = %s' % (col(0),))
            print('    指令数 min/median/max = %s' % (col(1),))
            print('    φ 数   min/median/max = %s' % (col(2),))
            ratios = [x[1][1] / x[2] for x in okd if x[2] > 0]
            if ratios:
                print('    指令数/结构化 Op 数：min=%.2f median=%.2f max=%.2f（%d 个有分母）'
                      % (min(ratios), statistics.median(ratios), max(ratios), len(ratios)))
                tiny = [(r, s[1], n) for r, s, n in okd if n > 0 and s[1] / n < 0.2]
                if tiny:
                    print('    ⚠️ 比值异常小（可能静默丢内容）：%d 个' % len(tiny))
                    for r, ni, n in tiny[:8]:
                        print('        %s: 平面 %d 条 vs 结构化 %d 条' % (r, ni, n))
            else:
                print('    ⚠️ 没有任何文件有结构化 Op 数分母 —— 轨 C 的比值不可判')

    okb, totb, failb = run_bad(comp, root)
    print('=== 轨 B 反证（手工坏 IR）：%d/%d 报红' % (okb, totb))
    for f in failb:
        print('    ✗ %s' % f)
    return 0 if (okb == totb and (a.bad or not [k for k in buckets if k != 'ok'])) else 1


if __name__ == '__main__':
    sys.exit(main())
