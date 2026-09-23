#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
loop_backedge_census.py —— **跨层结构对应**普查：结构化循环的体若能到达
末尾或 `Continue`，平面层必须保留对应的**回边**。

为什么需要它（不变量 ㊳）
------------------------
`while (1) { break; }` 的体没有返回循环头的路径，因此没有平面回边是正确的。
本工具只要求那些体内存在可达的正常结束或 `Continue` 路径的循环有回边。
这是跨层数量检查：同一函数中多余的回边可能掩盖另一条缺失回边，因此通过
只表示没有发现这种数量缺口，不证明每个结构化循环都已逐一匹配。

判据（与 `check_flat.py` 同一套语义）
------------------------------------
* 结构化层：按 Region 的顺序遍历；`Break`/`Continue`/`Return` 终止当前
  路径，`If` 合并两支。只数体内能到达 `Yield`/末尾或 `Continue` 的循环。
  条件值不做常量折叠，因为平面 CFG 的条件分支仍保留两条边。
* 平面层：`br` 的目标块 `t` 若**支配**该 `br` 所在块 `b` ⇒ 这是一条回边
  （用真支配树，不用启发式；入口块自身不计）。
* 输出：逐个文件的三元组 + 不一致清单（**全量**，不截断）。

用法
----
    python3 compiler/tools/selftest/loop_backedge_census.py \
        --compiler compiler/build/compiler --jobs 12 [--limit N] [--tsv FILE]

退出码：0 = 全部对应；1 = 有文件缺回边；2 = 工具自身错误。
"""

import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import loopnorm_ir as L          # noqa: E402
from flat_mod import FlatMod     # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


NORMAL = 'normal'
BREAK = 'break'
CONTINUE = 'continue'
STOP = 'stop'


def region_exits(ops, required):
    """有限的结构化路径分析；返回本 Region 可达的出口种类。

    每个 Op/Region 最多访问一次。循环本身可经条件的 false 边退出；
    它体内的 Break 在此层被消费，Continue 只返回该循环头。
    """
    exits = {NORMAL}
    for op in ops:
        if NORMAL not in exits:
            break
        exits.remove(NORMAL)
        if op.kind == 'Yield':
            exits.add(NORMAL)
            break
        if op.kind == 'Break':
            exits.add(BREAK)
            break
        if op.kind == 'Continue':
            exits.add(CONTINUE)
            break
        if op.kind in ('Return', 'Goto', 'Unreachable'):
            exits.add(STOP)
            break
        if op.kind == 'If':
            for branch in op.regions:
                exits.update(region_exits(branch, required))
            if len(op.regions) < 2:
                exits.add(NORMAL)
        elif op.kind in ('While', 'For'):
            body = op.regions[-1] if op.regions else []
            body_exits = region_exits(body, required)
            if NORMAL in body_exits or CONTINUE in body_exits:
                required[0] += 1
            exits.add(NORMAL)
        else:
            exits.add(NORMAL)
    return exits


def required_loops_by_function(mod):
    """各函数中体内存在可达回头路径的结构化循环数。"""
    counts = {}
    for fn in L.funcs(mod):
        required = [0]
        region_exits(fn.regions[0] if fn.regions else [], required)
        counts[fn.name] = required[0]
    return counts


def head_pred_report(fn_blocks, heads):
    """权威判据（用 `flat_mod._find_loops` 的循环头）：

    * 每个头块的**前驱集合**（由 `br` 边算）；头块没有任何前驱 ⇒ **畸形**
      （循环头必然有一条进入边，否则那个循环不可达）。
    * 报告：`头块:前驱数` 列表。

    ⚠️ 曾经想自己算支配集来判"自支配但无回边入边"，**判据本身写错了**
      （`dom` 初值把"全部块"给了非入口块 ⇒ 每个块都被判成自支配 ⇒
       `41_unary_op2` 这种零循环的文件报出 4 条回边）。⇒ 按 §11.5，
      改用**工具链里已有的权威实现**，不自己重写支配。
    """
    preds = {b: set() for b in fn_blocks}
    for b, insts in fn_blocks.items():
        for it in insts:
            if it.kind == 'br':
                for t in it.blocks:
                    if t in fn_blocks:
                        preds[t].add(b)
    out = []
    for h in sorted(heads):
        ps = preds.get(h, set())
        out.append('%d:%d' % (h, len(ps)))
    return out


def inspect_ir(sir_text, flat_text):
    """返回 (所需回边数, 平面头数, 头详情, 数量缺口/畸形头详情)。"""
    m, _names, _ = L.parse_dump(sir_text)
    required = required_loops_by_function(m)
    fm = FlatMod(flat_text)
    total = 0
    detail = []
    bad = []
    for fname in sorted(set(required) | set(fm.funcs)):
        blocks = fm.funcs.get(fname, ({}, [], []))[0]
        heads = fm.loops.get(fname, [])
        total += len(heads)
        if len(heads) < required.get(fname, 0):
            bad.append('%s:所需%d/平面%d' % (fname, required[fname], len(heads)))
        for item in head_pred_report(blocks, heads):
            label = '%s:L%s' % (fname, item)
            detail.append(label)
            if int(item.rsplit(':', 1)[1]) < 2:
                bad.append(label)
    return sum(required.values()), total, ','.join(detail), ','.join(bad)


def one_file(compiler, sy, tmpdir):
    key = os.path.relpath(sy, ROOT).replace('/', '__')
    sir = os.path.join(tmpdir, key + '.sir')
    flt = os.path.join(tmpdir, key + '.flat')
    r1 = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', sir],
                        capture_output=True)
    r2 = subprocess.run([compiler, sy, '--emit=flat-ir', '-o', flt], capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        return ('err', 0, 0, '编译失败 rc=%d/%d' %
                (r1.returncode, r2.returncode), '')
    try:
        with open(sir, encoding='utf-8') as fh:
            sir_text = fh.read()
        with open(flt, encoding='utf-8') as fh:
            flat_text = fh.read()
        loops, total, detail, bad = inspect_ir(sir_text, flat_text)
        return ('ok', loops, total, detail, bad)
    except Exception as e:                       # noqa: BLE001  —— 普查工具，报出来即可
        return ('err', 0, 0, '%s: %s' % (type(e).__name__, e), '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', default=os.path.join(ROOT, 'compiler', 'build', 'compiler'))
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--start', type=int, default=0, help='跳过前 N 个（分批跑用）')
    ap.add_argument('--expect-files', type=int, default=0,
                    help='关卡预期的参与文件数；不一致视为工具错误')
    ap.add_argument('--tsv', default='')
    args = ap.parse_args()

    files = []
    for dirpath, dirnames, filenames in os.walk(args.dir):
        dirnames[:] = [d for d in dirnames if d not in ('.git', '__pycache__')]
        for fn in filenames:
            if fn.endswith('.sy'):
                p = os.path.join(dirpath, fn)
                try:
                    with open(p, 'rb') as fh:
                        if b'tensor' not in fh.read():
                            files.append(p)
                except OSError:
                    pass
    files.sort()
    if args.start:
        files = files[args.start:]
    if args.limit:
        files = files[:args.limit]
    tmpdir = os.path.join(ROOT, '.work', 'census')
    os.makedirs(tmpdir, exist_ok=True)

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        res = list(ex.map(lambda f: (f,) + one_file(args.compiler, f, tmpdir), files))

    short, skipped, errs = [], [], []
    for f, st, loops, back, msg, bad in res:
        rel = os.path.relpath(f, ROOT)
        if st == 'skip':
            skipped.append(rel)
        elif st == 'err':
            errs.append((rel, msg))
        elif bad:
            short.append((rel, loops, back, bad))
    if args.tsv:
        with open(args.tsv, 'w', encoding='utf-8') as tf:
            for f, st, loops, back, msg, _bad in res:
                rel = os.path.relpath(f, ROOT)
                tf.write('%s\t%s\t%d\t%d\t%s\n' % (st, rel, loops, back, msg))

    print('== 跨层结构对应普查（㊳：结构化循环 vs 平面层循环头）==')
    print('参与文件数: %d' % len(files))
    print('两侧都产出: %d ; 跳过: %d ; 工具错误: %d'
          % (len(files) - len(skipped) - len(errs), len(skipped), len(errs)))
    print('**所需回边缺失或头的前驱数 < 2 的文件: %d**' % len(short))
    for rel, loops, back, msg in short:
        print('  · %s  需回边循环 %d / 平面头 %d   详情: %s' % (rel, loops, back, msg))
    for rel, msg in errs[:10]:
        print('  ! %s  %s' % (rel, msg))
    if args.expect_files and len(files) != args.expect_files:
        print('  ! 参与文件数 %d 与预期 %d 不符' % (len(files), args.expect_files))
        return 2
    return 2 if errs else (1 if short else 0)


if __name__ == '__main__':
    sys.exit(main())
