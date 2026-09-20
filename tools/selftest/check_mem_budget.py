#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_mem_budget.py —— 把"编译器的内存开销不得随源码里的一个数字爆炸"变成门禁。

为什么需要它
------------
S03 收尾时发现：语义分析对**带初始化器的非 const 全局数组**按元素物化常量值
（`SemaDecl.cpp` 里 `isGlobal || d->isConst` 把"检查常量性"和"把值留下来"混成了一个条件）。
于是 `tests/final_arm/h_functional/23_json.sy` 里的 `int buffer[50000000] = {};`
吃掉了 **574 MB**，而那份数据**一个字节都不可能被读到**（非 const 对象不是符号常量，
`int a[g[0]]` 会报 E-ARRAY-DIM）。

这类缺陷有三个特点，所以必须由关卡兜住：
  * **语料的正确性判据完全看不见它**（490 个用例全绿、诊断全对）；
  * 开销随源码里的**一个数字**线性无界 ⟹ 现场赛一个 `int a[200000000] = {};` 就是 OOM；
  * 报告里的"内存峰值"很容易取样取错——S03 报告取的是**转储文本最大**的文件
    （137 MB），而真正的峰值在另一个 7.5 KB 的小文件上，差 4.2 倍。

判据
----
对每个"压力文件"，实测 `--emit=<kind>` 的子进程峰值 RSS，要求不超过**该文件自己的预算**。
预算写成绝对上限而不是"增量"：转储本身的大小是格式决定的（`86_long_code2.sy` 的
`--emit=ast` 输出就有 128 MB，进程峰值 137 MB 是合理的），而"小而内存爆炸"的
文件恰恰是异常信号。

用法
----
    python3 check_mem_budget.py --compiler <路径> --root /home/koki1/try
退出码：0 = 全部在预算内；1 = 有超预算；2 = 工具自身错误（例如取不到峰值）。
"""

import argparse
import os
import resource
import subprocess
import sys
import tempfile

# (相对 tests/ 的路径, --emit 取值, 峰值 RSS 上限 MB, 为什么是这个数)
CASES = [
    ('final_arm/h_functional/23_json.sy', 'sema', 32,
     '源 7.5 KB / 转储 40 KB，却含 int buffer[50000000] = {}；'
     '修好物化门之前实测 574 MB，修好后 2.2 MB'),
    ('final_arm/h_functional/23_json.sy', 'ast', 32,
     '同上，语法转储侧本来就该是常数级'),
    ('final_arm/functional/86_long_code2.sy', 'ast', 256,
     '转储文本本身就有 128 MB，进程峰值 137 MB 属合理；'
     '给它 256 MB 是为了抓住"又叠了一份"的回归'),
    ('final_arm/performance/sl1.sy', 'sema', 32,
     '216000000 个元素的非 const 全局（各 864 MB），无初始化器；'
     '一旦有人在 Sema 侧按元素展开就会爆'),
]


def peak_rss_mb(compiler: str, path: str, kind: str, workdir: str):
    """用子进程的 rusage 取峰值（比 /usr/bin/time 更可移植，且不受 shell 包装影响）。"""
    out = os.path.join(workdir, 'out.txt')
    with open(out, 'wb') as f:
        p = subprocess.run([compiler, '--emit=' + kind, path, '-o', out],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if p.returncode != 0:
        return None, 'exit=%d %s' % (p.returncode, p.stderr.decode('utf-8', 'replace')[:120])
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    return ru.ru_maxrss / 1024.0, None   # Linux: KB → MB


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--compiler', required=True)
    ap.add_argument('--root', default='/home/koki1/try')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    tests = os.path.join(args.root, 'tests')
    bad = 0
    print('%-42s %-5s %9s %9s' % ('压力文件', 'emit', '峰值MB', '预算MB'))
    with tempfile.TemporaryDirectory() as wd:
        for rel, kind, budget, why in CASES:
            path = os.path.join(tests, rel)
            if not os.path.exists(path):
                print('  ! 找不到 %s（跳过）' % rel)
                continue
            # ⚠️ RUSAGE_CHILDREN 是"所有已回收子进程的最大值"，只增不减。
            #    所以每个用例都用一个**全新的**解释器来量，否则后面的用例会读到前面的峰值。
            code = (
                'import resource,subprocess,sys;'
                'p=subprocess.run(sys.argv[1:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);'
                'print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)'
            )
            p = subprocess.run([sys.executable, '-c', code, args.compiler,
                                '--emit=' + kind, path, '-o', os.path.join(wd, 'o.txt')],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if p.returncode != 0:
                print('  ✘ %-40s %-5s  无法测量：%s' % (rel, kind,
                      p.stderr.decode('utf-8', 'replace').strip()[:80]))
                bad += 1
                continue
            mb = int(p.stdout.strip()) / 1024.0
            flag = '✔' if mb <= budget else '✘'
            if mb > budget:
                bad += 1
            print('  %s %-40s %-5s %9.1f %9d' % (flag, rel, kind, mb, budget))
            if args.verbose or mb > budget:
                print('       依据：%s' % why)
    if bad:
        print('判定：✘ %d 项超出内存预算（见上面每一行的依据）' % bad)
        return 1
    print('判定：✔ 全部在内存预算内')
    return 0


if __name__ == '__main__':
    sys.exit(main())
