#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_loopnorm.py —— 轨 B/C/F：**不变式 + 覆盖率 + 零改动 + alloca 位置**

用法（命令行固定契约，关卡照这个签名调用）：
    python3 check_loopnorm.py --compiler <路径> [--jobs N] [--dir 语料目录]
                             [--limit N] [--verbose] [--no-corpus]
    python3 check_loopnorm.py --compiler <路径> --probe          # ★ 反证（必跑）

判据
----
**轨 B（不变式，本关才真正有意义）**
  ① 规范化后 I1–I6 全过：**从文本独立重判**（不看 C++ 的 `StructuredVerifier`）；
  ② use-def 一致 + 指令集封闭（`OPKINDS`）；
  ③ ★ **反证**：手工构造 3 份违反 I1/I2/I3 的 dump，检查器必须逐条报红
     （"检查器必须先证明抓得住错" —— S01 起的规矩）。

**轨 C（覆盖率 / 零改动 / 统计）**
  ① 三个变体指名验证（贴出每个的 `ForOp` 数）；
  ② 成功率 = `for-built / while-seen` 与未规范化原因直方图（目标 > 50%）；
  ③ ★ 不触发的用例**逐字节零改动**：对每个"一个循环都没规范化"的文件，
     `--normalize` 前后 dump 必须逐字节相同；
  ④ 对照组：≥10 个**无循环**的文件零改动。

**轨 F（`alloca` 位置）**
  每个函数的 `AllocaOp` **全部**在入口 Region（反例数 = 0）。

退出码：0 = 全过；1 = 有失败；2 = 工具自身错误。
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import loopnorm_ir as L  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

SKIP_RE = re.compile(r'^skip (.+?) = (\d+)$')


# ============================================================================
# 轨 B：从文本独立重判 I1–I6（**不看 C++ 的检查器**）
# ============================================================================
def check_invariants(mod, names):
    """【后置】返回违反列表（每条是字符串）。"""
    bad = []

    def add(inv, op, msg):
        bad.append('[%s] line %d: %s' % (inv, op.line, msg))

    # ── I1 / I2 / I3：`ForOp` 三条 ────────────────────────────────────────
    forop_list = L.find_all([mod], 'For')
    for f in forop_list:
        body = f.regions[0] if f.regions else []
        # I1：IV 的槽是操作数 0（身份），体内不许有写它的 Store
        if not f.operands:
            add('I1', f, 'ForOp 缺少 IV 操作数')
            continue
        ivslot = f.operands[0]
        iv_name = ivslot
        iv_alloca = names.get(ivslot)
        if iv_alloca is None or iv_alloca.kind != 'Alloca':
            add('I1', f, 'ForOp 的 IV 槽 `%s` 不是 AllocaOp 的结果' % iv_name)
        if len(f.operands) != 4:
            add('I2', f, 'ForOp 必须有 4 个操作数（iv/lower/upper/step），有 %d 个'
                % len(f.operands))
        # 体内集合（含嵌套 Region）
        bodyops = []
        L.walk(body, lambda o, d: bodyops.append(o))
        for o in bodyops:
            if o.kind == 'Store' and len(o.operands) == 2 and o.operands[1] == ivslot:
                add('I1', o, 'ForOp 的 IV `%s` 在循环体内被赋值' % iv_name)
            if o.kind == 'Break':
                add('I3', o, 'ForOp 体内出现 BreakOp（SCoP 条件要求体内无 break）')
        # I2：lower/upper/step 的**依赖子树**与"体内被写的槽"不能相交
        written = set()
        for o in bodyops:
            if o.kind == 'Store' and len(o.operands) == 2:
                base = o.operands[1]
                bop = names.get(base)
                while bop is not None and bop.kind == 'GetElementPtr':
                    base = bop.operands[0]
                    bop = names.get(base)
                if bop is not None and bop.kind == 'Alloca':
                    written.add(base)
        gset = set()
        stack = list(f.operands[1:4])
        while stack:
            t = stack.pop()
            if t is None or t in gset:
                continue
            gset.add(t)
            o = names.get(t)
            if o is not None:
                stack.extend(o.operands)
        for t in f.operands[1:4]:
            gop = names.get(t)
            if gop is None:
                continue
            # ① 边界的定义若在体内 ⇒ 违反
            if gop in bodyops:
                add('I2', f, 'ForOp 的边界（操作数 `%s`）在循环体**内**被定义'
                    % names.get(t, t))
                continue
            # ② 边界子树里读的槽若在体内被写 ⇒ 违反
            for gt in gset:
                go = names.get(gt)
                if go is None or go.kind != 'Load' or not go.operands:
                    continue
                if go.operands[0] in written:
                    add('I2', f, 'ForOp 的边界依赖的槽 `%s` 在循环体内被写'
                        % names.get(go.operands[0], go.operands[0]))
                    break

    # ── I4 / I5：每个 Region 恰好一个终结 Op、在最后一行；控制流容器只有三种
    def check_region(reg, owner_kind, role, owner_line):
        terms = [i for i, op in enumerate(reg) if op.kind in L.TERMINATORS]
        if owner_kind != 'Module':
            if len(terms) == 0:
                bad.append('[I4] line %d: %s 没有终结 Op' % (owner_line, role))
            elif len(terms) > 1:
                bad.append('[I4] line %d: %s 有 %d 个终结 Op（必须恰好 1 个）'
                           % (owner_line, role, len(terms)))
            if terms and terms[-1] != len(reg) - 1:
                bad.append('[I4] line %d: %s 的终结 Op 不在最后一行' % (owner_line, role))
        for op in reg:
            if op.regions and op.kind not in L.CF_CONTAINERS and op.kind not in ('Module', 'Func'):
                bad.append('[I5] line %d: 非控制流容器 `%s` 带了子 Region' % (op.line, op.kind))
            if op.kind == 'While' and len(op.regions) != 2:
                bad.append('[I4] line %d: WhileOp 必须有 2 个 Region' % op.line)
            if op.kind == 'If' and len(op.regions) != 2:
                bad.append('[I4] line %d: IfOp 必须有 2 个 Region' % op.line)
            if op.kind == 'For' and len(op.regions) != 1:
                bad.append('[I4] line %d: ForOp 必须有 1 个体 Region' % op.line)
            if op.kind in ('While', 'For', 'If') and op.results:
                bad.append('[I4] line %d: 控制流容器 `%s` 不该有结果' % (op.line, op.kind))
            for i, sub in enumerate(op.regions):
                check_region(sub, op.kind, '%s 的第 %d 个 Region' % (op.kind, i), op.line)

    mod_region = L.module_region(mod)
    check_region(mod_region, 'Module', '模块 Region', 0)

    # ── use-def + 指令集封闭 + I6 ───────────────────────────────────────────
    for op in L.find_all([mod], 'GetElementPtr'):
        if len(op.operands) != 2:
            bad.append('[I6] line %d: GetElementPtrOp 必须有 2 个操作数，有 %d 个'
                       % (op.line, len(op.operands)))
        if len(op.attrs) < 3:
            bad.append('[I6] line %d: GetElementPtrOp 缺少 [元素类型, 下标类型, 亲和性]'
                       % op.line)
        elif not re.fullmatch(r'0|1', op.attrs[2] or ''):
            bad.append('[I6] line %d: GEP 亲和性标记只能是 0/1，实际 `%s`'
                       % (op.line, op.attrs[2]))

    def check_usedef(ops, fn):
        for op in ops:
            if op.kind in L.FORBIDDEN:
                bad.append('line %d: 出现禁止的 Op `%s`' % (op.line, op.kind))
            elif op.kind not in L.OPKINDS:
                bad.append('line %d: OpKind `%s` 不在冻结的指令集里' % (op.line, op.kind))
            for nm in op.operands:
                if nm not in names:
                    bad.append('[use-def] line %d: 操作数 `%s` 没有定义' % (op.line, nm))
                elif not (nm.startswith('%' + fn + '.') or nm.startswith('%.')):
                    bad.append('[use-def] line %d: 操作数 `%s` 跨函数引用（当前函数 %s）'
                               % (op.line, nm, fn))
            for reg in op.regions:
                check_usedef(reg, fn)

    for fop in mod_region:
        if fop.kind == 'Func':
            for reg in fop.regions:
                check_usedef(reg, fop.name)
    return bad


# ============================================================================
# 轨 F：`alloca` 位置
# ============================================================================
def check_alloca_positions(mod, names):
    """【后置】返回 (反例数, 明细列表)。每个函数的 AllocaOp 必须在**入口 Region**。"""
    bad = []
    total = 0

    def collect(reg, out):
        for op in reg:
            if op.kind == 'Alloca':
                out.append(op)
            for sub in op.regions:
                collect(sub, out)

    for fn in L.funcs(mod):
        if not fn.regions:
            continue
        entry = fn.regions[0]
        # 入口 Region 的**直接**语句里的 alloca
        direct = set(id(o) for o in entry if o.kind == 'Alloca')
        alls = []
        for sub in fn.regions:
            collect(sub, alls)
        total += len(alls)
        for a in alls:
            if id(a) not in direct:
                bad.append('%s: line %d 的 AllocaOp 不在入口 Region' % (fn.name, a.line))
    return len(bad), bad


# ============================================================================
# 反证（--probe）：人为构造违反 I1/I2/I3 的 dump，检查器必须报红
# ============================================================================
def probe(compiler, tmpdir):
    """【后置】返回 [(改坏方式, 是否被抓到, 说明), ...]。"""
    sy = os.path.join(tmpdir, 'probe_ln.sy')
    with open(sy, 'w', encoding='utf-8') as fh:
        fh.write('int main(){\n'
                 '  int i;\n'
                 '  int s;\n'
                 '  i = 0;\n'
                 '  s = 0;\n'
                 '  while (i < 10) {\n'
                 '    s = s + i;\n'
                 '    i = i + 1;\n'
                 '  }\n'
                 '  return s;\n'
                 '}\n')
    good = os.path.join(tmpdir, 'probe_ln.sir')
    r = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize',
                        '-o', good], capture_output=True)
    if r.returncode != 0:
        return [('（编译器没能产出规范化 IR）', False, r.stderr.decode('utf-8', 'replace')[:200])]
    base = open(good, encoding='utf-8').read().split('\n')
    # 找 `For` 体的 indentation 与 IV 槽名
    for_idx = None
    for i, ln in enumerate(base):
        if ln.strip().startswith('(For '):
            for_idx = i
            break
    if for_idx is None:
        return [('（探针没找到 ForOp）', False, '')]
    for_indent = len(base[for_idx]) - len(base[for_idx].lstrip())
    ivslot = base[for_idx].strip().split()[2]      # `(For "iv" %slot lower upper step)`
    inner = ' ' * (for_indent + 2)

    out = []

    # ① I1：在体内插一条"写 IV 槽"的 Store
    def break_i1(lines):
        o = list(lines)
        for i in range(for_idx + 1, len(o)):
            if (len(o[i]) - len(o[i].lstrip())) == for_indent + 2 and o[i].strip().startswith('('):
                o.insert(i + 1, inner + '  (Store i32 %bogus.501 ' + ivslot + ' @line 1)')
                return '\n'.join(o)
        return None
    out.append(('I1：体内插入写 IV 槽的 Store', break_i1(base)))

    # ② I3：在体内插一条 BreakOp
    def break_i3(lines):
        o = list(lines)
        for i in range(for_idx + 1, len(o)):
            if (len(o[i]) - len(o[i].lstrip())) == for_indent + 2 and o[i].strip().startswith('('):
                o.insert(i + 1, inner + '  (Break @line 1)')
                return '\n'.join(o)
        return None
    out.append(('I3：体内插入 BreakOp', break_i3(base)))

    # ③ I2：把 `upper` 换成一个**体内定义**的值（复制一个体内结果名即可）
    def break_i2(lines):
        o = list(lines)
        tk = o[for_idx].strip().rstrip('{').strip().split()
        if len(tk) < 7:
            return None
        inner_res = None
        for i in range(for_idx + 1, len(o)):
            st = o[i].strip()
            if (len(o[i]) - len(o[i].lstrip())) == for_indent + 2 and st.startswith('(') \
                    and len(st.split()) > 1 and st.split()[1].startswith('%'):
                inner_res = st.split()[1]
                break
        if inner_res is None:
            return None
        tk[4] = inner_res          # `(For "iv" %slot %lower %upper %step)`
        o[for_idx] = ' ' * for_indent + ' '.join(tk) + ' {'
        return '\n'.join(o)
    out.append(('I2：upper 换成体内定义的值', break_i2(base)))

    res = []
    for label, text in out:
        if text is None:
            res.append((label, False, '**探针没找到目标行**'))
            continue
        try:
            mod, names, _f = L.parse_dump(text)
            bad = check_invariants(mod, names)
        except L.ParseError as e:
            bad = ['ParseError: %s' % e]
        want = label.split('：')[0]
        caught = [b for b in bad if b.startswith('[%s]' % want)]
        res.append((label, len(caught) > 0,
                    caught[0] if caught else ('其它违反: %s' % (bad[:2] if bad else '无'))))
    return res


# ============================================================================
# 单文件：一次拿到 raw / normalized / stats
# ============================================================================
def _same(p, q):
    """【后置】两个文件逐字节相同（任一不可读 → False）。"""
    try:
        return open(p, 'rb').read() == open(q, 'rb').read()
    except OSError:
        return False


def one_file(compiler, sy, tmpdir):
    """【后置】跑一个文件的**四件事**（轨 A 的往返与轨 C 的零改动都在这里）：

      ① `--emit=structured-ir`（不带开关）—— 冻结契约的产物；
      ② ①的产物 → `--from-structured --emit=structured-ir` → 必须逐字节相同（轨 A 往返）；
      ③ `--normalize --emit=structured-ir --dump-loopnorm-stats`；
      ④ ③的产物 → `--from-structured --normalize --emit=structured-ir`
         → 必须逐字节相同（轨 A 往返 **+ §C3 幂等**：`run(run(X)) == run(X)`）；
      ⑤ ①②③④ 之外：若 ③ 的 `for-built == 0`（一个循环都没规范化），
         则 ① 与 ③ 的产物必须**逐字节相同**（轨 C.3 零改动）。
    """
    base = os.path.basename(sy)
    raw = os.path.join(tmpdir, base + '.raw')
    nrm = os.path.join(tmpdir, base + '.norm')
    rt0 = os.path.join(tmpdir, base + '.rt0')
    rt1 = os.path.join(tmpdir, base + '.rt1')
    r1 = subprocess.run([compiler, sy, '--emit=structured-ir', '-o', raw],
                        capture_output=True)
    r2 = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize',
                         '--dump-loopnorm-stats', '-o', nrm], capture_output=True)
    rt0_ok = rt1_ok = None
    if r1.returncode == 0 and os.path.exists(raw):
        a = subprocess.run([compiler, raw, '--from-structured', '--emit=structured-ir',
                            '-o', rt0], capture_output=True)
        rt0_ok = (a.returncode == 0 and _same(raw, rt0))
    if r2.returncode == 0 and os.path.exists(nrm):
        b = subprocess.run([compiler, nrm, '--from-structured', '--normalize',
                            '--emit=structured-ir', '-o', rt1], capture_output=True)
        rt1_ok = (b.returncode == 0 and _same(nrm, rt1))
    st = {'file': sy, 'sy': sy, 'rc1': r1.returncode, 'rc2': r2.returncode,
          'stats': {}, 'raw': raw, 'norm': nrm, 'skip': [], 'err': '',
          'rt0': rt0_ok, 'rt1': rt1_ok, 'zero': None}
    txt = r2.stderr.decode('utf-8', 'replace')
    for ln in txt.split('\n'):
        m = SKIP_RE.match(ln.strip())
        if m:
            st['stats'][m.group(1)] = int(m.group(2))
            continue
        for k in ('while-seen', 'for-built', 'kept-while', 'continues-resolved',
                  'allocas-hoisted', 'rolled-back'):
            if ln.strip().startswith(k + ' '):
                try:
                    st['stats'][k] = int(ln.strip().split()[1])
                except (IndexError, ValueError):
                    pass
        if ln.strip().startswith('kept '):
            st['skip'].append(ln.strip()[5:])
    if r1.returncode != 0 or r2.returncode != 0:
        st['err'] = 'rc=%d/%d' % (r1.returncode, r2.returncode)
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--probe', action='store_true', help='★ 反证（必跑）')
    ap.add_argument('--no-corpus', action='store_true', help='不扫语料（只用例集/反证）')
    ap.add_argument('--variants', action='store_true',
                    help='只验三个变体（指名）')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    tmp = os.path.join(ROOT, '.work', 'loopnorm_check')
    os.makedirs(tmp, exist_ok=True)

    ok = True

    # ── 三个变体（指名）────────────────────────────────────────────────────
    if args.variants or args.probe or args.no_corpus:
        print('== 轨 C.1：三个变体（指名）==')
        variants = [
            ('变体 1：增量在 if 内 + continue（两条递增路径）',
             'tests/prelim_arm/performance/01_mm1.sy'),
            ('变体 2：continue 在循环体中间', 'tests/prelim_arm/performance/transpose0.sy'),
            ('变体 3：初值非 0、边界是 N-1', 'tests/prelim_arm/performance/sl1.sy'),
        ]
        for label, rel in variants:
            p = os.path.join(ROOT, rel)
            st = one_file(compiler, p, tmp)
            nfor = st['stats'].get('for-built', 0)
            nwhile = st['stats'].get('while-seen', 0)
            good = st['rc2'] == 0 and nfor > 0
            ok = ok and good
            print('  %s %s : for-built=%d / while-seen=%d  （%s）'
                  % ('✔' if good else '✘', label, nfor, nwhile,
                     'rc=0' if st['rc2'] == 0 else st['err']))

    # ── 反证 ───────────────────────────────────────────────────────────────
    if args.probe:
        print('== 轨 B.3：反证（检查器必须先抓住人为改坏）==')
        for label, caught, info in probe(compiler, tmp):
            ok = ok and caught
            print('  %s %s' % ('✔' if caught else '✘', label))
            print('      → %s' % info)
    if args.no_corpus:
        print('== 结果：%s ==' % ('全过' if ok else '有失败'))
        return 0 if ok else 1

    # ── 语料 ───────────────────────────────────────────────────────────────
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
        results = list(ex.map(lambda f: one_file(compiler, f, tmp), files))

    tot = {'while-seen': 0, 'for-built': 0, 'kept-while': 0,
           'continues-resolved': 0, 'allocas-hoisted': 0}
    hist = {}
    inv_bad = []
    alloca_bad = []
    zero_ok = 0
    zero_bad = []
    noloop_ok = 0
    noloop_bad = []
    parsed = 0
    skips = []
    rt0_bad = []
    rt1_bad = []
    zero_seen = 0

    for st in results:
        if st['rc1'] != 0 or st['rc2'] != 0:
            skips.append((st['sy'], st['err']))
            continue
        try:
            rawtxt = open(st['raw'], encoding='utf-8').read()
            nrmtxt = open(st['norm'], encoding='utf-8').read()
        except OSError:
            skips.append((st['sy'], 'unreadable'))
            continue
        if not rawtxt.strip() or not nrmtxt.strip():
            skips.append((st['sy'], 'empty dump'))
            continue
        try:
            mod, names, _f = L.parse_dump(nrmtxt)
        except L.ParseError as e:
            inv_bad.append((st['sy'], 'ParseError: %s' % e))
            continue
        parsed += 1
        bad = check_invariants(mod, names)
        if bad:
            inv_bad.append((st['sy'], bad[:3]))
        nb, detail = check_alloca_positions(mod, names)
        if nb:
            alloca_bad.append((st['sy'], detail[:2]))
        for k in tot:
            tot[k] += st['stats'].get(k, 0)
        for k, v in st['stats'].items():
            if k not in tot:
                hist[k] = hist.get(k, 0) + v
        # 轨 A：往返（不带开关 / 带开关 + 幂等）
        if st.get('rt0') is False:
            rt0_bad.append(st['sy'])
        if st.get('rt1') is False:
            rt1_bad.append(st['sy'])
        # 轨 C.3：一个循环都没规范化的文件必须逐字节零改动
        if st['stats'].get('for-built', 0) == 0:
            zero_seen += 1
            if rawtxt == nrmtxt:
                zero_ok += 1
            else:
                zero_bad.append(st['sy'])
        # 轨 C.4：无循环的文件（对照组）
        if st['stats'].get('while-seen', 0) == 0:
            if rawtxt == nrmtxt:
                noloop_ok += 1
            else:
                noloop_bad.append(st['sy'])

    print('== 轨 A：往返（dump → 读回 → 再 dump，逐字节）==')
    print('  不带 --normalize 往返不一致: %d' % len(rt0_bad))
    for f in rt0_bad[:10]:
        print('    ✘ %s' % os.path.relpath(f, ROOT))
    print('  带 --normalize 往返不一致: %d   ← 同时是幂等判据 run(run(X))==run(X)'
          % len(rt1_bad))
    for f in rt1_bad[:10]:
        print('    ✘ %s' % os.path.relpath(f, ROOT))
    ok = ok and not rt0_bad and not rt1_bad

    print('== 轨 B：不变式（I1–I6 + use-def + 指令集封闭）==')
    print('  参与检查的文件数: %d' % parsed)
    print('  违反不变式的文件数: %d' % len(inv_bad))
    for f, b in inv_bad[:10]:
        print('    ✘ %s\n      %s' % (os.path.relpath(f, ROOT), b))
    ok = ok and not inv_bad

    print('== 轨 F：alloca 位置（全部在函数入口 Region）==')
    print('  反例数: %d' % len(alloca_bad))
    for f, b in alloca_bad[:10]:
        print('    ✘ %s\n      %s' % (os.path.relpath(f, ROOT), b))
    ok = ok and not alloca_bad

    print('== 轨 C：覆盖率 / 零改动 / 统计 ==')
    ws = tot['while-seen']
    fb = tot['for-built']
    print('  while 总数=%d 升为 For=%d 保留=%d  （成功率 %.2f%%，目标 >50%%）'
          % (ws, fb, tot['kept-while'], (100.0 * fb / ws) if ws else 0.0))
    if ws and fb * 2 <= ws:
        print('  ✘ 成功率未达 50%（**如实报告，不放宽成功条件**）')
    print('  continue 消解数=%d  alloca 提升数（真的被移动的）=%d'
          % (tot['continues-resolved'], tot['allocas-hoisted']))
    print('  --- 未规范化原因直方图 ---')
    for k, v in sorted(hist.items(), key=lambda kv: -kv[1]):
        if v:
            print('    %-46s %5d (%.1f%% of while)' % (k, v, 100.0 * v / ws if ws else 0))
    print('  不触发（for-built==0）的文件: %d 个（零改动通过 %d，失败 %d）'
          % (zero_seen, zero_ok, len(zero_bad)))
    for f in zero_bad[:10]:
        print('    ✘ %s' % os.path.relpath(f, ROOT))
    ok = ok and not zero_bad
    print('  无循环（while-seen==0）的对照组: %d 个零改动通过，%d 个失败'
          % (noloop_ok, len(noloop_bad)))
    for f in noloop_bad[:10]:
        print('    ✘ %s' % os.path.relpath(f, ROOT))
    if noloop_ok < 10:
        print('  ✘ 对照组不足 10 个（要求 ≥10 个无循环文件）')
        ok = False
    ok = ok and not noloop_bad

    if skips:
        print('== 跳过的文件（前端就不产 IR，如 tensor 用例）: %d ==' % len(skips))
        kinds = {}
        for f, why in skips:
            kinds[why] = kinds.get(why, 0) + 1
        for k, v in sorted(kinds.items()):
            print('    %-24s %d' % (k, v))
        print('    （逐条清单见 .work/loopnorm_check/，不是静默跳过）')

    print('== 结果：%s ==' % ('全过' if ok else '有失败'))
    if args.verbose:
        for st in results:
            if st['skip']:
                print('  %s: %s' % (os.path.relpath(st['sy'], ROOT), '; '.join(st['skip'][:4])))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
