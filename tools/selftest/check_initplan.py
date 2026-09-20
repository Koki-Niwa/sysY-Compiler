#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_initplan.py —— S04 轨 A / 轨 B：初始化计划的**独立检查器**

它把编译器当**黑盒**：只跑 `--emit=initplan` 取转储，不 import、不读它的任何
代码。三件事各写一遍（分别在本目录的三个文件里）：

    initplan_sem.py   —— 期望值 + 常量表达式求值 + C11 §6.7.9 的填充模型
    initplan_dump.py  —— 转储的解析 + "模拟成字节数组"
    本文件            —— 逐元素比对 + 规模上界 + 反证 + 报告

语法树复用 S02 的**独立 parser**（`independent_parser_check.py`：它自己实现
词法与语法，已在 490 个文件上与 C++ 逐字节一致）；**语义部分全部自己写**。

判据（每一条都在报告里给了证据）：
  ① 每个对象的**每一个元素**：期望值与模拟值必须相同；
  ② `StoreExpr` 覆盖的位置：允许（局部初始化器可以引用变量）；
  ③ 初始化器"未写到"的元素：模拟值必须是 0（规范 §3 ConstDef 6.3）；
  ④ **覆盖性**：带初始化器的对象，每个元素都必须被某个动作写过 ——
     否则"零填充"只是碰巧的（`int a[4096] = {1}` 的 4095 个零必须来自动作）；
  ⑤ 局部未初始化 ⇒ **零动作**；全局未初始化 ⇒ `:zero`；
  ⑥ 全局 `:data` 非零元素数 ≤ 1000；局部动作数 ≤ min(元素数, 100)
     （`--action-limit` 可放宽，供金样例目录用）。

用法:
    check_initplan.py --compiler <可执行文件> [--jobs N] [--root DIR]
                      [--dir SUBDIR] [--filter GLOB] [--timeout SEC] [--verbose]
                      [--probe] [--json FILE] [--metrics FILE] [--action-limit N]
退出码：0 = 全过；1 = 有违反；2 = 工具自身错误。
"""

import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ★ 独立 parser 是**递归下降**的，而语料 `86_long_code2.sy` 有一个几千项的
#   左结合 `+` 链（C++ 侧是迭代的，Python 侧不是）。默认递归上限 1000 会直接
#   RecursionError。这里把上限抬高，并让真正的解析在**大栈线程**里跑
#   （只抬上限而不换栈，深链会真的把 C 栈打穿 ⇒ 段错误）。
sys.setrecursionlimit(300000)
_STACK_SZ = 256 * 1024 * 1024


def run_with_big_stack(fn, *args, **kwargs):
    """在一个 256 MB 栈的线程里跑 fn，把结果/异常带回来。"""
    box = {}

    def target():
        try:
            box['r'] = fn(*args, **kwargs)
        except BaseException as e:            # noqa: BLE001
            box['e'] = e

    old = threading.stack_size(_STACK_SZ)
    try:
        t = threading.Thread(target=target)
        t.start()
        t.join()
    finally:
        threading.stack_size(old)
    if 'e' in box:
        raise box['e']
    return box.get('r')

from independent_parser_check import Parser, tokenize, read_source       # noqa: E402
import struct                                                          # noqa: E402

from initplan_sem import Extract, bits_of, count_of                     # noqa: E402
from initplan_dump import Sim, count_of_type, parse_plan, parse_type    # noqa: E402


def float_bits(v):
    """把 Python float 按 IEEE-754 单精度转成 32 位无符号位模式。"""
    return struct.unpack('<I', struct.pack('<f', v))[0]


# ============================================================================
# 1. 单个对象的比对
# ============================================================================
def check_object(obj, sim, viol, require_cover=True):
    """【前置】sim 已经把计划模拟进它的**稀疏**状态（见 initplan_dump.Sim）。
    【后置】把每一处不符追加到 viol。

    ── 为什么是稀疏的（S04 实测）──────────────────────────────────────────
      语料里有 `int a[30000010]`、`int buffer[50000000]`。按元素逐个扫
      （哪怕只循环 `range(n)`）在 50M 元素上要几十秒 —— 检查器会超时。
      而这个模块的判据本来就不需要扫全表：**没被写过的位置语义上就是 0**。

    判据（逐条对应 prompt §七 / §四.3）：
      ① 模拟出的每个**非零**格子，必须与期望值逐位相同；
      ② 期望值**未知**（引用了变量等）的位置：只要求"没被写过 ⇒ 模拟值必须是 0"；
      ③ 期望值**已知且非零**的位置：必须被写过（覆盖性，`require_cover`）；
         没写过时模拟值必为 0 ⇒ 与"非零期望"矛盾，抓得住；
      ④ 被 `StoreExpr` 覆盖 ⇒ 允许（运行期值编译期不知道），除非期望是常量 0。
    """
    isFloat = (obj.elem == 'float')

    def expect_at(i):
        e = obj.flat[i] if i < len(obj.flat) else None
        if isFloat and e is not None:
            return ('f', float(e[1]))       # 浮点数组一律按 IEEE-754 比
        return e

    # ① 模拟出来的每个非零格子
    for off, got in sim.cells.items():
        i = off // 4
        exp = expect_at(i)
        if exp is None:
            viol.append('%s[%d]（偏移 %d）：计划写了 0x%08x，但期望值编译期不知道'
                        % (obj.label, i, off, got))
            continue
        want = bits_of(exp)
        if want != got:
            viol.append('%s[%d]（偏移 %d）：期望 0x%08x，计划的模拟值是 0x%08x'
                        % (obj.label, i, off, want, got))

    # ② + ④ StoreExpr 覆盖的位置
    for off in sim.unknown:
        i = off // 4
        exp = expect_at(i)
        if exp is not None and bits_of(exp) == 0:
            viol.append('%s[%d]：计划写了运行期表达式，但期望值是常量 0' % (obj.label, i))

    # ③ 覆盖性（**用区间减法，不扫全表** —— 语料里有 50M 元素的数组）
    #   * 局部且写了初始化器：整个对象都必须被写过（"未写到的隐式初始化为 0"
    #     要求那些 0 **有来源**；`int a[4096] = {1}` 的 4095 个零要来自动作）。
    #   * 其余：只有"期望非零"的位置必须被写过。
    #   * 局部且写了初始化器：**整个对象**都必须被写过（那些隐式 0 要有来源）。
    #   * 其余：只有"被显式写到的位置"里期望非零的那些必须被写过。
    #   ⚠️ 只遍历 `obj.assigned`（稀疏）与 `unwritten_ranges`（区间），
    #      **不**遍历 `obj.flat`：语料里有 3×10^7 个元素的数组，扫一遍要十几秒。
    gaps = sim.unwritten_ranges(sim.nbytes)
    if require_cover:
        for lo, hi in gaps:
            for off in range(lo - (lo % 4), hi, 4):
                if off < lo:
                    continue
                i = off // 4
                exp = expect_at(i) if i < len(obj.flat) else None
                if exp is None or bits_of(exp) == 0:
                    viol.append('%s[%d]（偏移 %d）：期望是常量 0x%08x，但没有任何动作写它'
                                % (obj.label, i, off, 0 if exp is None else bits_of(exp)))
    else:
        for i in obj.assigned:
            e = obj.flat[i]
            exp = expect_at(i)
            if e is None or bits_of(exp) == 0:
                continue
            if not sim.is_written(4 * i):
                viol.append('%s[%d]（偏移 %d）：期望是常量 0x%08x，但没有任何动作写它'
                            % (obj.label, i, 4 * i, bits_of(exp)))


def check_global_pairs(obj, entry, viol):
    """全局 `:data` 的**精确**检查（§4.2/§4.3 的强制项）。

    期望集合 = 源码里那些"求值后非零"的元素。
    计划必须与它逐项相同：不多（不许列零值）、不少（漏了元素就是丢初值）、
    不重、偏移升序、值逐位相同。

    ⚠️ 两处实现要点（S04 实测）：
      * 不能只靠"模拟后比字节"：`:data` 的语义是"其余元素为 0"，所以把元素
        从表里删掉、或者改成已出现过的偏移，模拟出来的字节可能仍然一致 ——
        只有**集合层面**的比对才抓得住。
      * **不许**扫全表（`for i in enumerate(obj.flat)`）：语料里有
        `int buffer[50000000]`，扫一遍要几十秒。期望集合从 `obj.flat` 的
        **非零项**直接取（稀疏），配一个 `dict`。
    """
    isFloat = (obj.elem == 'float')
    want = {}
    # ★ 只遍历**被显式写到**的位置（稀疏）：`int buffer[50000000] = {}` 的
    #   `flat` 有 5×10^7 项，逐项扫会超时。
    for i in sorted(obj.assigned):
        v = obj.flat[i]
        if v is None:
            viol.append('%s：全局初始化器的元素 #%d 不是常量（规范 §3 Initial Values 1）'
                        % (obj.label, i))
            continue
        if isFloat:
            v = ('f', float(v[1]))
        b = bits_of(v)
        if b:
            want[4 * i] = b
    if len(want) > 1000:
        viol.append('%s：期望的非零元素有 %d 个（契约上界 1000）' % (obj.label, len(want)))
    offs = [p[0] for p in entry['pairs']]
    if offs != sorted(offs):
        viol.append('%s：:data 的偏移不是升序（契约 §4.3）' % obj.label)
    if len(set(offs)) != len(offs):
        viol.append('%s：:data 里有重复偏移' % obj.label)
    n = count_of(obj.shape)
    got = {}
    for off, tok in entry['pairs']:
        if off % 4 != 0 or off >= 4 * n:
            viol.append('%s：:data 的偏移 %d 不是对象的合法元素偏移' % (obj.label, off))
            continue
        try:
            if ('x' in tok or 'p' in tok):
                ev = float_bits(float.fromhex(tok))
            elif isFloat:
                ev = float_bits(float(tok))
            else:
                ev = int(tok) & 0xFFFFFFFF
        except (ValueError, OverflowError):
            viol.append('%s：:data 的值 %r 无法解析' % (obj.label, tok))
            continue
        got[off] = ev
        if ev == 0:
            viol.append('%s：:data 里出现了零值（契约：:data 只列非零）' % obj.label)
    for off in sorted(set(want) | set(got)):
        if off not in got:
            viol.append('%s：:data 缺少偏移 %d（期望值 0x%08x）' % (obj.label, off, want[off]))
        elif off not in want:
            viol.append('%s：:data 多出偏移 %d（那个位置期望是 0）' % (obj.label, off))
        elif want[off] != got[off]:
            viol.append('%s：:data 在偏移 %d 的值是 0x%08x，期望 0x%08x'
                        % (obj.label, off, got[off], want[off]))


# ============================================================================
# 2. 单文件检查
# ============================================================================
def in_scope(text):
    code = re.sub(r'//[^\n]*', '', text)
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.S)
    return not ('@' in code or re.search(r'\btensor\b', code))


class FileResult(object):
    def __init__(self, path, rel):
        self.path = path
        self.rel = rel
        self.status = 'ok'
        self.violations = []
        self.dump_bytes = 0
        self.max_actions = 0
        self.action_top = []


def build_objs(sy_path):
    """独立解析 + 独立语义 ⇒ 期望对象表。"""
    def go():
        tree = Parser(tokenize(read_source(sy_path))).parse_compunit()
        return Extract(tree).run()
    return run_with_big_stack(go)


def check_plan_text(sy_path, text, action_limit=None):
    """对一份**给定**的转储文本跑一遍检查（正式检查与反证共用）。

    【后置】返回 (违反列表, 转储字节数, [(动作数, 对象名)])。
    """
    objs = build_objs(sy_path)
    try:
        plan = parse_plan(text)
    except Exception as e:                       # noqa: BLE001
        return ['转储无法解析：%s' % e], len(text.encode('utf-8')), []
    viol = []
    top = []
    # ★ 同名遮蔽：一个名字可能对应多条记录（块内/块外各一条）。
    #   转储与源码都是**按源文件顺序**产出 ⇒ 按顺序配对（不是"取最后一个"）。
    taken = {}
    for obj in objs:
        bucket = plan['objs'].get(obj.label)
        k = taken.get(obj.label, 0)
        if not bucket or k >= len(bucket):
            viol.append('转储里没有对象 %s（第 %d 次出现）' % (obj.label, k + 1))
            continue
        taken[obj.label] = k + 1
        entry = bucket[k]
        t = parse_type(entry['type'])
        if t is None:
            viol.append('%s 的类型记号 %r 非法' % (obj.label, entry['type']))
            continue
        n = count_of_type(t)
        if n != count_of(obj.shape):
            viol.append('%s：转储的类型 %s 与源码形状 %s 不符'
                        % (obj.label, entry['type'], obj.shape))
            continue
        elem = 'float' if t[0] == 'float' else 'int'
        sim = Sim(elem, 4 * n)
        if entry['kind'] == 'zero':
            if not obj.global_:
                viol.append('%s：局部对象不允许 :zero' % obj.label)
            # ★ 只查**被显式写到**的位置（稀疏）：:zero 要求它们全是已知的 0。
            #   扫 `obj.flat` 会在 `int a[30000010]` 上花十几秒（S04 实测）。
            bad = None
            for i in sorted(obj.assigned):
                v = obj.flat[i]
                if v is None or bits_of(v) != 0:
                    bad = i
                    break
            if obj.has_init and bad is not None:
                viol.append('%s：转储是 :zero，但源码初始化器的元素 #%d 不是常量 0'
                            % (obj.label, bad))
                continue
            sim.run([('Zero', 0, 4 * n)], viol)
            # 覆盖性只在"**写了初始化器**的局部对象"上要求：局部未初始化 ⇒
            # 值不确定 ⇒ 计划必须零动作（下面那条判据管它），谈不上覆盖。
            check_object(obj, sim, viol,
                         require_cover=(not obj.global_) and obj.has_init)
            if obj.global_:
                check_global_pairs(obj, entry, viol)
        else:
            if not obj.global_ and not obj.has_init and entry['acts']:
                viol.append('%s：局部未初始化（值不确定）却产出了 %d 条动作'
                            % (obj.label, len(entry['acts'])))
            # 全局 `:data` 的动作是"每个 (偏移 值) 一次写"；局部有显式动作序列。
            acts = entry['acts']
            if obj.global_:
                acts = []
                for off, tok in entry['pairs']:
                    try:
                        ev = (float.fromhex(tok) if ('x' in tok or 'p' in tok)
                              else int(tok))
                    except ValueError:
                        continue          # 解析失败已由 check_global_pairs 报出
                    acts.append(('StoreConst', off,
                                 ('f', ev) if ('x' in tok or 'p' in tok) else ('i', ev)))
            sim.run(acts, viol)
            # ★ 覆盖性只在**局部**对象上要求：全局 `:data` 的语义就是"表里没有的
            #   元素为 0"，它的"不漏不重"由 check_global_pairs 的集合比对保证。
            # 覆盖性只在"**写了初始化器**的局部对象"上要求：局部未初始化 ⇒
            # 值不确定 ⇒ 计划必须零动作（下面那条判据管它），谈不上覆盖。
            check_object(obj, sim, viol,
                         require_cover=(not obj.global_) and obj.has_init)
            if obj.global_:
                check_global_pairs(obj, entry, viol)
        limit = action_limit if action_limit is not None else min(count_of(obj.shape), 100)
        if obj.has_init and sim.actions > limit:
            viol.append('%s：动作数 %d 超过上界 %d' % (obj.label, sim.actions, limit))
        top.append((sim.actions, obj.label))
    return viol, len(text.encode('utf-8')), top


def check_file(compiler, path, rel, tmpdir, timeout, action_limit=None):
    r = FileResult(path, rel)
    if not in_scope(read_source(path)):
        r.status = 'skip'
        return r
    out = os.path.join(tmpdir, re.sub(r'[^A-Za-z0-9_.-]', '_', rel) + '.plan')
    try:
        p = subprocess.run([compiler, '--emit=initplan', path, '-o', out],
                           capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        r.status = 'error'
        r.violations.append('超时（%.0fs）' % timeout)
        return r
    err = p.stderr.decode('utf-8', 'replace')
    if p.returncode != 0 or 'error:' in err:
        r.status = 'error'
        first = err.strip().split('\n')[0] if err.strip() else '(空)'
        r.violations.append('--emit=initplan 退出码 %d / stderr 有 error：%s'
                            % (p.returncode, first))
        return r
    if not os.path.exists(out):
        r.status = 'error'
        r.violations.append('没有产出转储')
        return r
    text = open(out, encoding='utf-8', errors='replace').read()
    try:
        viol, nbytes, top = check_plan_text(path, text, action_limit)
    except Exception as e:                       # noqa: BLE001
        r.status = 'error'
        r.violations.append('检查器异常：%r' % (e,))
        return r
    r.violations = viol
    r.dump_bytes = nbytes
    r.action_top = top
    r.max_actions = max([a for a, _ in top] or [0])
    if viol:
        r.status = 'fail'
    return r


# ============================================================================
# 3. 反证：人为改坏的转储必须逐条报红（S01 起的规矩）
# ============================================================================
def _first_pair(lines):
    for i, ln in enumerate(lines):
        m = re.match(r'^(\s*)\((\d+)\s+(\S+?)\)\s*$', ln)
        if m:
            return i, m
    return None, None


def mutate_plan(text, which):
    """对一份转储做**定向破坏**。返回 (坏文本, 说明) 或 (None, 原因)。"""
    lines = text.split('\n')
    if which in ('offset', 'value', 'drop'):
        i, m = _first_pair(lines)
        if m is None:
            return None, '这份转储里没有 :data 对'
        if which == 'offset':
            lines[i] = '%s(%d %s)' % (m.group(1), int(m.group(2)) + 4, m.group(3))
            return '\n'.join(lines), '把 :data 里一个非零元素的偏移 +4'
        if which == 'value':
            lines[i] = '%s(%s 999999)' % (m.group(1), m.group(2))
            return '\n'.join(lines), '把 :data 里一个非零元素的值改掉'
        del lines[i]
        return '\n'.join(lines), '从 :data 里删掉一个非零元素'
    if which == 'memcpy':
        for i, ln in enumerate(lines):
            m = re.match(r'^(\s*)\(MemcpyConst\s+(\d+)\s+:(\w+)\s*$', ln)
            if m:
                lines[i] = '%s(MemcpyConst %d :%s' % (m.group(1), int(m.group(2)) + 4,
                                                      m.group(3))
                return '\n'.join(lines), '把 MemcpyConst 的起始偏移 +4（整段右移一格）'
            m = re.match(r'^(\s*)\(MemcpyConst\s+(\d+)\s+:(\w+)\s+(.+?)\)\s*$', ln)
            if m:
                vals = m.group(4).split()
                vals[0] = '424242'
                lines[i] = '%s(MemcpyConst %s :%s %s)' % (m.group(1), m.group(2),
                                                          m.group(3), ' '.join(vals))
                return '\n'.join(lines), '把 MemcpyConst 的一个值改掉'
        return None, '这份转储里没有 MemcpyConst'
    if which == 'zero':
        for i, ln in enumerate(lines):
            m = re.match(r'^(\s*)\(Zero\s+(\d+)\s+(\d+)\)\s*$', ln)
            if m and int(m.group(3)) >= 8:
                off, n = int(m.group(2)), int(m.group(3))
                lines[i] = '%s(Zero %d %d)' % (m.group(1), off, max(4, n // 2))
                return '\n'.join(lines), '把一条 Zero 的字节数改小一半（覆盖性因此破了）'
        return None, '这份转储里没有够长的 Zero'
    return None, '未知的破坏方式 %s' % which


def run_probe(compiler, root, tmpdir, timeout):
    case = os.path.join(root, 'compiler/tests/init/probe/probe.sy')
    if not os.path.exists(case):
        print('✘ 找不到反证用例 %s' % case, file=sys.stderr)
        return 2
    out = os.path.join(tmpdir, 'probe.plan')
    p = subprocess.run([compiler, '--emit=initplan', case, '-o', out],
                       capture_output=True, timeout=timeout)
    if p.returncode != 0:
        print('✘ 反证用例编译失败：%s' % p.stderr.decode('utf-8', 'replace')[:200],
              file=sys.stderr)
        return 2
    text = open(out, encoding='utf-8').read()

    viol, _, _ = check_plan_text(case, text)
    ok = not viol
    print('%-12s 未破坏            ⇒ %d 条违反 %s'
          % ('probe.sy', len(viol), '✔' if ok else '✘ ' + viol[0]))

    names = {
        'offset': '把 :data 里一个非零元素的偏移 +4',
        'value': '把 :data 里一个非零元素的值改掉',
        'memcpy': '把 MemcpyConst 的偏移/值改掉',
        'zero': '把一条 Zero 的字节数改小一半',
        'drop': '从 :data 里删掉一个非零元素',
    }
    for which in ('offset', 'value', 'memcpy', 'zero', 'drop'):
        bad, why = mutate_plan(text, which)
        print('-' * 74)
        if bad is None:
            print('%-8s ✘ 造不出这种破坏：%s' % (which, why))
            ok = False
            continue
        viol, _, _ = check_plan_text(case, bad)
        print('%s  期望违反 —— %s' % (which, names[which]))
        print('  %s %d 条，实际报错原文：' % ('✔ 抓到了' if viol else '✘ 没抓到', len(viol)))
        if viol:
            print('      %s' % viol[0])
        else:
            ok = False
    print('=' * 74)
    print('判定：%s' % ('✔ 检查器不是"永远说 OK"：未破坏零违反、五处破坏逐条报红' if ok
                      else '✘ 检查器抓不住人为破坏 —— 它的"全过"不可信'))
    return 0 if ok else 1


# ============================================================================
# 4. 驱动器
# ============================================================================
def discover(root, pattern_filter, subdir='tests'):
    base = os.path.join(root, subdir)
    out = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith('.sy'):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            if pattern_filter and not (fnmatch.fnmatch(rel, pattern_filter)
                                       or fnmatch.fnmatch(name, pattern_filter)):
                continue
            out.append((full, rel))
    out.sort(key=lambda x: x[1])
    return out


def main():
    ap = argparse.ArgumentParser(description='S04 轨 A/B：初始化计划的独立检查器')
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..', '..')))
    ap.add_argument('--dir', default='tests', help='扫描的子目录（默认 tests/）')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--filter', default=None)
    ap.add_argument('--timeout', type=float, default=60.0)
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--json', default=None)
    ap.add_argument('--metrics', default=None)
    ap.add_argument('--probe', action='store_true',
                    help='只跑反证：人为改坏的转储必须逐条报红')
    ap.add_argument('--action-limit', type=int, default=None,
                    help='覆盖"局部动作数上界"（金样例目录用元素数）')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmpdir:
        if args.probe:
            return run_probe(compiler, args.root, tmpdir, args.timeout)

        files = discover(args.root, args.filter, args.dir)
        if not files:
            print('找不到用例', file=sys.stderr)
            return 2

        def one(item):
            full, rel = item
            try:
                return check_file(compiler, full, rel, tmpdir, args.timeout,
                                  args.action_limit)
            except Exception as e:               # noqa: BLE001
                r = FileResult(full, rel)
                r.status = 'error'
                r.violations.append('检查器异常：%r' % (e,))
                return r

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(one, files))
        results.sort(key=lambda r: r.rel)

        stats = {}
        total_bytes = 0
        biggest, top = [], []
        for r in results:
            stats[r.status] = stats.get(r.status, 0) + 1
            total_bytes += r.dump_bytes
            biggest.append((r.dump_bytes, r.rel))
            for acts, name in r.action_top:
                top.append((acts, '%s::%s' % (r.rel, name)))
            if r.status in ('fail', 'error'):
                print('✘ %s' % r.rel)
                for v in r.violations[:6]:
                    print('    %s' % v)
            elif args.verbose:
                print('✔ %-56s dump=%7d B max_actions=%d'
                      % (r.rel, r.dump_bytes, r.max_actions))
        biggest.sort(reverse=True)
        top.sort(reverse=True)

        print('=' * 78)
        print('  范围内通过 (ok)        : %d' % stats.get('ok', 0))
        print('  范围外已跳过 (skip)    : %d' % stats.get('skip', 0))
        print('  失败         (fail)    : %d' % stats.get('fail', 0))
        print('  异常         (error)   : %d' % stats.get('error', 0))
        print('  合计                   : %d' % len(results))
        print('  转储总量               : %.2f MB' % (total_bytes / 1048576.0))
        if biggest:
            print('  单文件最大             : %s (%.3f MB)'
                  % (biggest[0][1], biggest[0][0] / 1048576.0))
        print('  动作数 Top-10          :')
        for acts, name in top[:10]:
            print('      %6d  %s' % (acts, name))
        print('=' * 78)

        if args.metrics:
            with open(args.metrics, 'w', encoding='utf-8') as f:
                json.dump({'total_bytes': total_bytes, 'biggest': biggest[:10],
                           'top_actions': top[:10]}, f, ensure_ascii=False, indent=1)
        if args.json:
            with open(args.json, 'w', encoding='utf-8') as f:
                json.dump([{'file': r.rel, 'status': r.status,
                            'violations': r.violations} for r in results],
                          f, ensure_ascii=False, indent=1)

        bad = stats.get('fail', 0) + stats.get('error', 0)
        if bad:
            print('判定：✘ 轨 A/B 未通过（%d 个文件有问题）' % bad)
            return 1
        print('判定：✔ 轨 A（模拟计划 → 与源码语义逐元素一致）+ 轨 B（规模上界）全部通过')
        return 0


if __name__ == '__main__':
    sys.exit(main())
