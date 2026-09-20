#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_init_cases.py —— 轨 C：跑 `compiler/tests/init/` 下的两组用例

    compiler/tests/init/spec/<编号>-<短名>/case.sy      规范里的每个例子
                                        /expect.flat   期望的扁平值表
                                        （**没有** expect.flat ⇒ 该用例必须报错）

    compiler/tests/init/min/<编号>-<短名>/bad.sy        必须报错
                                       /good.sy         与 bad 只差一处，必须通过
                                       /expect          一行：`<编号> <行号>`

判据：
  * `spec` 合法用例：`--emit=initplan` 退出码 0；**独立检查器**模拟计划后与
    `expect.flat` 逐元素相同。这里刻意复用 `check_initplan.py`（而不是自己再
    实现一遍比对）—— 它才是"独立第二实现"，用例集只负责提供数据。
  * `spec` 非法用例：退出码 1，且 stderr 里至少有一个 `error:`。
  * `min`：与 S03 的 `run_sema_cases.py` 同款判据 —— bad 的退出码必须是 1、
    诊断编号**恰好**是期望的那一个、行号一致；good 必须退出码 0 且无 `error:`；
    并且 bad/good **只差一处**（统一 diff 只有一个 hunk）。
  * `spec` 用例的局部动作数上界放宽到"元素数"（金样例里有刻意的大数组用例）。

命令行（**关卡会用，签名不许改**）:
    run_init_cases.py --compiler <可执行文件路径> [--spec-dir DIR] [--min-dir DIR]
                      [--filter GLOB] [--verbose] [--jobs N] [--timeout SEC]
退出码：0 = 全过；1 = 有用例失败；2 = 工具自身错误。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from check_initplan import check_plan_text          # noqa: E402

TESTS = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'init'))
DEFAULT_SPEC = os.path.join(TESTS, 'spec')
DEFAULT_MIN = os.path.join(TESTS, 'min')

# `file:line:col: error: [E-XXXX] message`
DIAG_RE = re.compile(r':(\d+):(\d+): error: \[(E-[A-Z0-9-]+)\]')
ERROR_RE = re.compile(r'error:')


def run_compiler(compiler, path, out, timeout):
    return subprocess.run([compiler, '--emit=initplan', path, '-o', out],
                          capture_output=True, timeout=timeout)


def read_expect(path):
    txt = open(path, encoding='utf-8').read().strip()
    parts = txt.split()
    if len(parts) != 2:
        raise ValueError('expect 文件应为 `<编号> <行号>`，实际是 %r' % txt)
    return parts[0], int(parts[1])


def minimal_contrast(a, b):
    """bad.sy 与 good.sy 是否只差一处（统一 diff 只有一个 hunk）。"""
    import difflib
    ops = [o for o in difflib.SequenceMatcher(None, a.splitlines(True),
                                              b.splitlines(True)).get_opcodes()
           if o[0] != 'equal']
    if not ops:
        return False, 'bad.sy 与 good.sy 完全相同'
    if len(ops) > 1:
        return False, '相差 %d 处（应为 1 处）' % len(ops)
    tag, i1, i2, j1, j2 = ops[0]
    if tag == 'replace' and (i2 - i1) <= 2 and (j2 - j1) <= 2:
        return True, ''
    if tag in ('insert', 'delete') and (i2 - i1) <= 2 and (j2 - j1) <= 2:
        return True, ''
    return False, 'diff hunk 太大：%s %d 行 -> %d 行' % (tag, i2 - i1, j2 - j1)


class Result(object):
    def __init__(self, name):
        self.name = name
        self.failures = []
        self.notes = []

    @property
    def ok(self):
        return not self.failures


# ============================================================================
# spec 组
# ============================================================================
def run_spec_case(compiler, base, name, tmpdir, timeout):
    r = Result(name)
    d = os.path.join(base, name)
    sy = os.path.join(d, 'case.sy')
    exp = os.path.join(d, 'expect.flat')
    if not os.path.exists(sy):
        r.failures.append('缺少 case.sy')
        return r
    out = os.path.join(tmpdir, name + '.plan')
    p = run_compiler(compiler, sy, out, timeout)
    err = p.stderr.decode('utf-8', 'replace')
    legal = os.path.exists(exp)

    if not legal:
        # 非法用例：必须报错
        if p.returncode != 1:
            r.failures.append('非法用例的退出码是 %d，应为 1' % p.returncode)
        if not ERROR_RE.search(err):
            r.failures.append('非法用例的 stderr 里没有 error:')
        r.notes.append(err.strip().split('\n')[0] if err.strip() else '(无诊断)')
        return r

    if p.returncode != 0 or ERROR_RE.search(err):
        r.failures.append('合法用例退出码 %d / stderr 有 error：%s'
                          % (p.returncode, err.strip().split('\n')[0] if err.strip() else ''))
        return r
    want = {}
    for ln in open(exp, encoding='utf-8'):
        ln = ln.strip()
        if not ln:
            continue
        k, _, v = ln.partition(':')
        want[k.strip()] = v.split()
    text = open(out, encoding='utf-8', errors='replace').read() if os.path.exists(out) else ''
    if not text:
        r.failures.append('没有产出转储')
        return r
    # 用独立检查器做"模拟计划 + 逐元素比对"（局部动作数上界放宽到元素数）
    try:
        viol, _, _ = check_plan_text(sy, text, action_limit=10 ** 9)
    except Exception as e:                        # noqa: BLE001
        r.failures.append('独立检查器异常：%r' % (e,))
        return r
    for v in viol:
        r.failures.append(v)
    # 再把转储里的实际值摊平成 expect.flat 的形式比对一次（两份判据互为佐证）
    #   ⚠️ 必须带上**元素类型**：浮点常量的位模式（`0x3f800000`）与十进制值（`1`）
    #      在文本上完全不同，不带类型就没法比（S04 实测踩过）。
    got = flatten_from_dump(text)
    for k, vals in want.items():
        if k not in got:
            r.failures.append('转储里没有对象 %s' % k)
            continue
        g = got[k]
        if [x for x in g] != [x for x in vals]:
            r.failures.append('%s 的值是 %s，期望 %s' % (k, ' '.join(g[:12]),
                                                          ' '.join(vals[:12])))
    return r


def flatten_from_dump(text):
    """从 `--emit=initplan` 的转储里摊平出 {对象名: [值字符串]}（**测试侧**用）。"""
    from initplan_dump import parse_plan, parse_type, count_of_type
    plan = parse_plan(text)
    out = {}
    # `parse_plan` 里每个名字映射到**列表**（同名遮蔽会有多条），按顺序取最后
    # 一条即可 —— 本函数只用来核对 spec/ 金样例（那里没有同名遮蔽）。
    for name, bucket in plan['objs'].items():
        e = bucket[-1]
        t = parse_type(e['type'])
        if t is None:
            continue
        elem = t[0]                      # 'int' / 'float'：决定怎么把位模式写成文本
        n = count_of_type(t)
        arr = ['0'] * n
        if e['kind'] == 'zero':
            out[name] = arr
            continue
        for a in e['acts']:
            if a[0] == 'StoreConst':
                arr[a[1] // 4] = fmt_val(a[2], elem)
            elif a[0] == 'MemcpyConst':
                for j, v in enumerate(a[2]):
                    arr[a[1] // 4 + j] = fmt_val(v, elem)
            elif a[0] == 'StoreExpr':
                arr[a[1] // 4] = '?'
        for off, tok in e['pairs']:
            arr[off // 4] = normalise(tok, elem)
        out[name] = arr
    return out


def fmt_val(v, elem):
    """把计划里的一个常量写成 expect.flat 的文本形式（按**元素类型**）：
    浮点数组的每个元素都是 float（`1` 也要写成 `1`，不是它的位模式）。"""
    k, x = v
    if elem == 'float' or k == 'f':
        return '%g' % x
    return str(x)


def normalise(tok, elem):
    """`(偏移 值)` 里的值 → expect.flat 的文本形式。"""
    try:
        if 'x' in tok or 'p' in tok:
            return '%g' % float.fromhex(tok)
        if elem == 'float':
            return '%g' % float(tok)
        return str(int(tok))
    except ValueError:
        return tok


# ============================================================================
# min 组
# ============================================================================
def run_min_case(compiler, base, name, tmpdir, timeout):
    r = Result(name)
    d = os.path.join(base, name)
    bad, good, exp = (os.path.join(d, x) for x in ('bad.sy', 'good.sy', 'expect'))
    for p in (bad, good, exp):
        if not os.path.exists(p):
            r.failures.append('缺少文件 %s' % os.path.basename(p))
            return r
    try:
        code, line = read_expect(exp)
    except ValueError as e:
        r.failures.append(str(e))
        return r
    ok_min, why = minimal_contrast(open(bad, encoding='utf-8').read(),
                                   open(good, encoding='utf-8').read())
    if not ok_min:
        r.failures.append('不是最小对照：%s' % why)

    out = os.path.join(tmpdir, name + '.bad.plan')
    pb = run_compiler(compiler, bad, out, timeout)
    errb = pb.stderr.decode('utf-8', 'replace')
    r.notes.append(errb.strip().split('\n')[0] if errb.strip() else '(无诊断)')
    if pb.returncode != 1:
        r.failures.append('bad.sy 的退出码是 %d，应为 1' % pb.returncode)
    codes = sorted({m.group(3) for m in DIAG_RE.finditer(errb)})
    if codes != [code]:
        r.failures.append('bad.sy 报的编号是 %s，期望恰好是 [%s]'
                          % (codes or '（无）', code))
    lines = sorted({int(m.group(1)) for m in DIAG_RE.finditer(errb) if m.group(3) == code})
    if lines != [line]:
        r.failures.append('bad.sy 报 %s 的行号是 %s，期望是 [%d]'
                          % (code, lines or '（无）', line))

    pg = run_compiler(compiler, good, os.path.join(tmpdir, name + '.good.plan'), timeout)
    errg = pg.stderr.decode('utf-8', 'replace')
    if pg.returncode != 0:
        r.failures.append('good.sy 的退出码是 %d，应为 0；stderr: %s'
                          % (pg.returncode, errg.strip().split('\n')[0]))
    if ERROR_RE.search(errg):
        r.failures.append('good.sy 的 stderr 里有 error:')
    return r


# ============================================================================
# 驱动器
# ============================================================================
def main():
    ap = argparse.ArgumentParser(description='S04 轨 C：初始化器金样例 + 最小对照')
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--spec-dir', default=DEFAULT_SPEC)
    ap.add_argument('--min-dir', default=DEFAULT_MIN)
    ap.add_argument('--filter', default=None)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--timeout', type=float, default=60.0)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print('找不到编译器：%s' % compiler, file=sys.stderr)
        return 2

    import fnmatch
    import concurrent.futures
    jobs = []
    for base, runner, tag in ((args.spec_dir, run_spec_case, 'spec'),
                              (args.min_dir, run_min_case, 'min')):
        if not os.path.isdir(base):
            print('找不到用例目录：%s' % base, file=sys.stderr)
            return 2
        names = sorted(n for n in os.listdir(base) if os.path.isdir(os.path.join(base, n)))
        if args.filter:
            names = [n for n in names if fnmatch.fnmatch(n, args.filter)]
        for n in names:
            jobs.append((base, runner, tag, n))
    if not jobs:
        print('用例目录里没有用例', file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmpdir:
        def one(job):
            base, runner, tag, name = job
            try:
                r = runner(compiler, base, name, tmpdir, args.timeout)
            except Exception as e:                # noqa: BLE001
                r = Result(name)
                r.failures.append('运行器异常：%r' % (e,))
            r.name = '%s/%s' % (tag, name)
            return r

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(one, jobs))
    results.sort(key=lambda r: r.name)

    for r in results:
        if r.ok:
            if args.verbose:
                print('✔ %-40s %s' % (r.name, '；'.join(r.notes)))
            else:
                print('✔ %s' % r.name)
        else:
            print('✘ %s' % r.name)
            for n in r.notes:
                print('    %s' % n)
            for f in r.failures:
                print('    ✘ %s' % f)

    bad = [r for r in results if not r.ok]
    n_spec = len([r for r in results if r.name.startswith('spec/')])
    n_min = len([r for r in results if r.name.startswith('min/')])
    print('=' * 78)
    print('  spec 金样例 : %d（通过 %d）'
          % (n_spec, n_spec - len([r for r in bad if r.name.startswith('spec/')])))
    print('  min  最小对照: %d（通过 %d）'
          % (n_min, n_min - len([r for r in bad if r.name.startswith('min/')])))
    if bad:
        print('判定：✘ 轨 C 未通过')
        return 1
    print('判定：✔ 轨 C 全部通过（%d 个规范金样例 + %d 组最小对照）' % (n_spec, n_min))
    return 0


if __name__ == '__main__':
    sys.exit(main())
