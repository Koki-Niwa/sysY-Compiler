#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
arena.py —— **三方对照**：gcc / 结构化 IR / 平面 IR，喂**同一串确定性输入**。

为什么必须有它
--------------
`flat_exec.py` 只能回答"两种形状**是否一致**"，对"**两条赛道一致地错**"
完全免疫 —— S06 已经抓到 5 处这种 bug（stride 双重相乘、字节偏移当平面下标、
memcpy 元素宽度、全局元素类型、数组形参索引）。而"结构化侧解释器"与
"平面侧解释器"还有可能**共享同一个误解**（S05b 的 `Break`→`continue`
启发式就是：轨 E 报 same 而两个答案都错）。

⇒ 唯一能穿透的手段是**外部锚定**（TESTING-GUIDE §11）：拿 gcc 当独立真值，
并且给三方喂**逐字节相同**的输入。本工具就是这件事的可执行版本。

★★ 三方的输入必须同源 ★★
    `exec_core.Machine.next_int()` 是确定性 PRNG（`[-5,10]`，故意取小值，
    见那边的注释）。gcc 侧用 `arena_runtime.c` **复刻**同一串整数：
      * `getint()`  → 第 k 个 PRNG 值
      * `getch()`   → `65 + (PRNG & 15)`
      * `getarray()` → **恒 8 个** PRNG 值（与 `exec_core` 一致）
      * `getfloat()` / `getfarray()` → 消耗同一串整数，Python 式 % 1000 / 4
    这些语义**必须逐条对齐**，否则 gcc 不是真值。

用法
----
    python3 compiler/tools/selftest/arena.py <file.sy> [--n 64] [--seed 12345]

输出（每方一行 + 差异字段）：

    gcc   : rc=0 out='100\\n'
    结构化: ret=0 out='100\\n' mem=... loops(条数)=15
    平面  : ret=0 out='0\\n' mem=... loops(条数)=15
    >>> gcc/平面 差异 输出缓冲

退出码：0 = 三方可观察行为一致且两种 IR 轨迹一致；1 = 有差异；
        2 = 工具自身错误（gcc 编译失败、编译器报错等）。
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    '..', '..', '..'))

import exec_core as C          # noqa: E402
import flat_exec as FE         # noqa: E402
import flat_mod as FM          # noqa: E402
import loopnorm_exec as LE     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME_C = os.path.join(HERE, 'arena_runtime.c')


def gen_inputs(seed, k):
    """**唯一**的输入生成器：与 `exec_core.Machine.next_int` 逐位相同。

    ⚠️ 不要"顺手"改成 `random` 或别的范围：语料里的性能用例是
    `n = getint(); while (i < n)` —— 大随机数会让循环跑十亿次（只能靠预算
    跳过），而**这些恰好最需要行为判据**。
    """
    r = seed & C.MASK32
    out = []
    for _ in range(k):
        x = r & C.MASK32
        x ^= (x << 13) & C.MASK32
        x ^= (x >> 17)
        x ^= (x << 5) & C.MASK32
        r = x & C.MASK32
        out.append((r % 16) - 5)
    return out


def gcc_truth(sy, n_in, seed, tmpdir):
    """用 gcc 编译 + 跑，返回 (进程退出码, 原样 stdout) 或抛 `RuntimeError`。"""
    if not os.path.exists(RUNTIME_C):
        raise RuntimeError('缺少运行库：%s' % RUNTIME_C)
    exe = os.path.join(tmpdir, 'bin')
    # SysY 源码通常不含运行库声明。C 的隐式函数声明会让 getfloat
    # 被当作返回 int，也会把 putfloat 的 float 实参当 double 传递。
    # 为所有已实现的运行库函数提供原型，确保 GCC 按正确 ABI 调用。
    decls = os.path.join(tmpdir, 'arena_runtime.h')
    with open(decls, 'w', encoding='ascii') as f:
        f.write('int getint(void), getch(void), getarray(int *);\n'
                'float getfloat(void);\n'
                'int getfarray(float *);\n'
                'void putint(int), putch(int), putarray(int, int *);\n'
                'void putfloat(float), putfarray(int, float *);\n'
                'void starttime(void), stoptime(void);\n')
    # `-x c` 强制把 .sy 当 C 编译（扩展名不认识）；`-x none` 让运行库回到按扩展名判。
    cmd = ['gcc', '-w', '-include', decls, '-x', 'c', '-I', HERE, sy,
           '-x', 'none', RUNTIME_C, '-o', exe]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('gcc 编译失败：\n' + r.stderr.decode(errors='replace')[:900])
    data = (' '.join(str(v) for v in gen_inputs(seed, n_in)) + '\n').encode()
    g = subprocess.run([exe], input=data, capture_output=True, timeout=600)
    # latin-1 是字节到字符的一一映射；putch 可以写出非 UTF-8 字节。
    return g.returncode, g.stdout.decode('latin-1')


def emit_traces(compiler, sy, seed, tmpdir):
    """产出两份 IR 并各自执行一次，返回 (结构化轨迹, 平面轨迹)。"""
    sir = os.path.join(tmpdir, 'a.sir')
    flt = os.path.join(tmpdir, 'a.flat')
    r1 = subprocess.run([compiler, sy, '--emit=structured-ir', '--normalize', '-o', sir],
                        capture_output=True)
    r2 = subprocess.run([compiler, sy, '--emit=flat-ir', '-o', flt], capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        raise RuntimeError('编译器 rc=%d/%d\n%s%s' % (
            r1.returncode, r2.returncode,
            r1.stderr.decode(errors='replace')[:400],
            r2.stderr.decode(errors='replace')[:400]))
    m1, n1, _ = FE.L.parse_dump(open(sir, encoding='utf-8').read())
    t1 = LE.Exec(m1, n1, seed).run()
    t2 = FE.FlatExec(FM.FlatMod(open(flt, encoding='utf-8').read()), seed).run()
    return t1, t2


def _float_stdout(part):
    """把解释器的 f32 位模式还原为 C printf("%a") 的文本。"""
    if not (part.startswith('f') and len(part) == 9):
        raise ValueError('无效的浮点输出记号：%r' % part)
    val = C.f32_of_bits(int(part[1:], 16))
    val_hex = val.hex()
    if 'p' in val_hex:
        mantissa, exponent = val_hex.split('p', 1)
        val_hex = mantissa.rstrip('0').rstrip('.') + 'p' + exponent
    return val_hex


def stdout_of(t):
    """把解释器的调用记号还原为 sylib.c 写出的 stdout。"""
    out = []
    parts = t['out']
    i = 0
    while i < len(parts):
        part = parts[i]
        i += 1
        if part.startswith(('fa', 'a')):
            floating = part.startswith('fa')
            prefix = 'fa' if floating else 'a'
            n = int(part[len(prefix):])
            # exec_core 有 1000 项上限；再多就没有足够信息还原真实 stdout。
            if n > 1000:
                raise ValueError('数组输出长度 %d 超出解释器的 1000 项上限' % n)
            count = max(0, n)
            if len(parts) - i < count:
                raise ValueError('数组输出缺少元素：%r' % part)
            values = parts[i:i + count]
            i += count
            if floating:
                values = [_float_stdout(value) for value in values]
            out.append('%d:' % n)
            for value in values:
                out.append(' ' + value)
            out.append('\n')
        elif part.startswith('c'):
            out.append(chr(int(part[1:]) & 0xff))  # putchar 只写低 8 位
        elif part.startswith('f'):
            out.append(_float_stdout(part))
        else:
            out.append(part)  # putint
    return ''.join(out)


def brief(t, out):
    return "out=%r mem=%s loops(条数)=%d" % (out, t['mem'], len(t['loops']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sy', help='要对照的 .sy 文件')
    ap.add_argument('--compiler', default=os.path.join(ROOT, 'compiler', 'build', 'compiler'))
    ap.add_argument('--n', type=int, default=64, help='喂给 gcc 的 PRNG 整数个数')
    ap.add_argument('--seed', type=int, default=12345, help='必须与两侧执行器同一个种子')
    args = ap.parse_args()

    sy = os.path.abspath(args.sy)
    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(sy):
        print('找不到 %s' % sy, file=sys.stderr)
        return 2
    if not os.path.exists(compiler):
        print('找不到编译器 %s' % compiler, file=sys.stderr)
        return 2

    tmpdir = tempfile.mkdtemp(prefix='arena_')
    try:
        rc, out = gcc_truth(sy, args.n, args.seed, tmpdir)
        print('gcc   : rc=%d out=%r' % (rc, out))
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print('gcc   : 失败：%s' % e)
        return 2
    if rc < 0:
        print('gcc   : 被信号 %d 终止，无法比较 main 返回值' % -rc)
        return 2

    try:
        t1, t2 = emit_traces(compiler, sy, args.seed, tmpdir)
        out1, out2 = stdout_of(t1), stdout_of(t2)
        print('结构化: ret=%r %s' % (t1['ret'], brief(t1, out1)))
        print('平面  : ret=%r %s' % (t2['ret'], brief(t2, out2)))
    except Exception as e:                      # noqa: BLE001  —— 排障工具，报出来即可
        print('执行    : 失败：%s: %s' % (type(e).__name__, e))
        return 2

    # POSIX 进程状态只有 main 返回值的低 8 位；不能直接比解释器的完整 i32。
    different = False
    for lane, trace, lane_out in (('结构化', t1, out1), ('平面', t2, out2)):
        if rc != (int(trace['ret']) & 0xff):
            print('>>> gcc/%s 差异 main 返回值（退出码 %d vs 低 8 位 %d）' %
                  (lane, rc, int(trace['ret']) & 0xff))
            different = True
        if out != lane_out:
            print('>>> gcc/%s 差异 输出缓冲' % lane)
            different = True
    for tag, name in (('loops', '循环迭代次数'), ('out', '输出缓冲'),
                      ('mem', '全局内存摘要'), ('ret', '返回值')):
        if t1[tag] != t2[tag]:
            print('>>> 结构化/平面 差异 %s' % name)
            different = True
    if not different:
        print('>>> 三方可观察行为一致；两种形状轨迹一致')
    return 1 if different else 0


if __name__ == '__main__':
    sys.exit(main())
