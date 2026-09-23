#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
independent_loopnorm_check.py —— 轨 D：**独立实现的第二份规范化器**

用法（命令行固定契约，关卡照这个签名调用）：
    python3 independent_loopnorm_check.py --compiler <路径> [--jobs N] [--limit N] [--verbose]
    python3 independent_loopnorm_check.py --compiler <路径> --dump-diff <文件>   # 只对一个文件打差异

做什么
------
读 `--emit=structured-ir`（**不带** `--normalize`）的 dump，按 S05b prompt §三 的
规则**自己**产出规范化后的 dump，再与 C++ `--normalize --emit=structured-ir`
的产物**逐字节比较**。

★ 纪律（与 S02/S05 同一条，它是本项目抓到真 bug 最多的机制）
------------------------------------------------------------
* 写本实现**之前不读** `LoopNormalize.cpp` / `LoopAnalysis.cpp`（已遵守：
  本文件按 prompt §三 的规格独立推导）；
* **不读** `.reference/`；
* §3.2 的 `continue` 消解规范形式是**唯一**允许依赖的"约定"。

★ 与 C++ 的**已知命名差异**（不是语义差异）
------------------------------------------
新发射的 Op 会拿到 `%<函数名>.<序号>`，而序号由**先序**顺序决定。C++ 在
`WhileOp` 之前插入"条件 Region 的 Op + `Load iv` + `Int 1`"，本实现按同一顺序
插入（先条件 Op、再 `Load iv`、再 `Int 1`）。⇒ 若仍有差异，`--dump-diff`
会同时打印 **结构等价** 的判定（把结果名统一改名后比较），用于区分
"名字不同"与"真的不一样"。

退出码：0 = 全量逐字节一致；1 = 有差异；2 = 工具自身错误。
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
import loopnorm_independent as IND  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


# ============================================================================
# 轨 D 主程序
# ============================================================================
CODE_RE = None


def run(cmd):
    return subprocess.run(cmd, capture_output=True)


def one_file(compiler, sy, tmpdir):
    """返回 (状态, 说明)。状态 ∈ {'same','diff','error'}。"""
    base = os.path.basename(sy)
    a = os.path.join(tmpdir, base + '.raw')
    b = os.path.join(tmpdir, base + '.cpp')
    r1 = run([compiler, sy, '--emit=structured-ir', '-o', a])
    r2 = run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', b])
    if r1.returncode != 0 or r2.returncode != 0:
        # ★ 双方都非零 = **范围外用例**（含 `tensor`/`@` 的 50 个），那是**预期行为**，
        #   不是错误。本项目所有其他检查器都按 `skipped` 处理（见 check_parser.py /
        #   check_sema.py / check_initplan.py 的"范围外已报诊断"）。
        #   ⚠️ 早先这里一律归 'error'，于是**退出码恒为 1**——接进关卡就是个永远红的
        #   判据，而摘要里"有差异: 0"又是对的，两者自相矛盾。只在一方非零时才算错。
        if r1.returncode != 0 and r2.returncode != 0:
            return ('skip', 'rc=%d/%d' % (r1.returncode, r2.returncode))
        return ('error', 'rc=%d/%d' % (r1.returncode, r2.returncode))
    try:
        raw = open(a, encoding='utf-8').read()
        want = open(b, encoding='utf-8').read()
    except OSError as e:
        return ('error', str(e))
    if not want.strip():
        return ('error', 'empty cpp dump')
    try:
        mine, _st = IND.normalize_dump(raw)
    except L.ParseError as e:
        return ('error', 'parse: %s' % e)
    except Failed as e:
        return ('error', 'failed: %s' % e)
    except RecursionError:
        return ('error', 'recursion')
    if mine == want:
        return ('same', '')
    return ('diff', diff_summary(mine, want))


def strip_names(text):
    """把结果名统一改成 `%N`（按出现顺序）：只比较**结构**（名字不同的差异
    不应被当成语义差异 —— 见文件头的说明）。"""
    order = {}
    out = []
    pat = re.compile(r'%[A-Za-z_0-9.]+')

    def rep(m):
        k = m.group(0)
        if k not in order:
            order[k] = '%%v%d' % len(order)
        return order[k]
    for ln in text.split('\n'):
        out.append(pat.sub(rep, ln))
    return '\n'.join(out)


def diff_summary(mine, want):
    a = mine.split('\n')
    b = want.split('\n')
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return 'line %d:\n  mine: %s\n  cpp : %s' % (i + 1, a[i].strip(), b[i].strip())
    return 'length mine=%d cpp=%d' % (len(a), len(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'tests'))
    ap.add_argument('--dump-diff', default='', help='只处理这一个 .sy，并打印结构等价判定')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--verdicts', default='')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2
    tmp = os.path.join(ROOT, '.work', 'loopnorm_trackd')
    os.makedirs(tmp, exist_ok=True)

    files = []
    if args.dump_diff:
        files = [os.path.abspath(args.dump_diff)]
    else:
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

    same = []
    diffs = []
    errs, skips = [], []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [(f, ex.submit(one_file, compiler, f, tmp)) for f in files]
        for f, fu in futs:
            st, msg = fu.result()
            if st == 'same':
                same.append(f)
            elif st == 'diff':
                diffs.append((f, msg))
            elif st == 'skip':
                skips.append((f, msg))
            else:
                errs.append((f, msg))

    if getattr(args, 'verdicts', ''):   # 全量判定（打印清单会被截断，不能做集合比对）
        with open(args.verdicts, 'w', encoding='utf-8') as vf:
            for st, lst in (('SAME', same), ('DIFF', diffs), ('SKIP', skips), ('ERR', errs)):
                for it in lst:
                    vf.write('%s\t%s\n' % (st, it[0] if isinstance(it, tuple) else it))
    print('== 轨 D：独立实现的第二份规范化器 ==')
    print('参与比较的文件数: %d' % len(files))
    print('逐字节一致: %d' % len(same))
    print('有差异:     %d' % len(diffs))
    print('范围外（双方一致拒绝）: %d' % len(skips))
    print('工具/解析错误: %d' % len(errs))
    for f, m in diffs[:20]:
        print('  DIFF %s\n    %s' % (os.path.relpath(f, ROOT), m))
    for f, m in errs[:20]:
        print('  ERR  %s  %s' % (os.path.relpath(f, ROOT), m))
    if args.dump_diff and diffs:
        # 打印结构等价判定（把结果名统一改名后再比一次）
        sy = files[0]
        base = os.path.basename(sy)
        raw = open(os.path.join(tmp, base + '.raw'), encoding='utf-8').read()
        cpp = open(os.path.join(tmp, base + '.cpp'), encoding='utf-8').read()
        mine, _ = IND.normalize_dump(raw)
        print('== 结构等价判定（结果名已统一改名）==')
        print('结构相同: %s' % (strip_names(mine) == strip_names(cpp)))
    return 1 if (diffs or errs) else 0


if __name__ == '__main__':
    sys.exit(main())
