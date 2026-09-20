#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
perf_tokens.py —— 词法器性能实测（S01 验证标准 §6 / §7.5）

两个硬指标：
  1. **540 个文件全部 token 化 < 1 秒 —— 含进程启动**（所以【串行】跑）
  2. **86 KB 单行 < 100 ms**（`tests/*/h_functional/29_long_line.sy`）

⚠️ 口径说明（很重要）：这个脚本测的是"比赛调用口径"——每次编译一个新进程。
   动态链接 + main 启动的固定成本约 1.6 ms/次，在 540 个文件的总额里占大头
   （见下面 ① 的 --emit=nothing 基线拆分）。**算法本身的吞吐**要用
   `unit/bench_lexer`（进程内循环、无启动开销）来看，两者不要混为一谈。

用法: perf_tokens.py --compiler <path> [--root DIR] [--repeat N]
"""

import argparse
import glob
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import time


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--root", default=None)
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args(argv)

    root = args.root
    if root is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    compiler = os.path.abspath(args.compiler)

    def run_all(files, extra_args, outf):
        """串行跑一遍；返回 (wall, 子进程 CPU, 退出码非 0 的个数)"""
        ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        t0 = time.perf_counter()
        nonzero = 0
        for f in files:
            rc = subprocess.run([compiler, f] + extra_args + ["-o", outf],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL).returncode
            if rc != 0:
                nonzero += 1
        wall = time.perf_counter() - t0
        ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu = (ru1.ru_utime - ru0.ru_utime) + (ru1.ru_stime - ru0.ru_stime)
        return wall, cpu, nonzero

    tmp = tempfile.mkdtemp(prefix=".s01_perf_", dir=root)
    try:
        files = sorted(glob.glob(os.path.join(root, "tests", "**", "*.sy"),
                                 recursive=True))
        print("=" * 76)
        print(" 词法器性能实测")
        print(" 编译器: %s" % compiler)
        print(" 口径  : 串行 subprocess（含进程启动），与比赛调用方式一致")
        print("=" * 76)
        outf = os.path.join(tmp, "o.tok")

        best = None
        nonzero = 0
        for _ in range(args.repeat):
            wall, cpu, nz = run_all(files, ["--emit=tokens"], outf)
            if best is None or wall < best[0]:
                best = (wall, cpu)
            nonzero = nz
        wall, cpu = best
        base_wall, base_cpu, _ = run_all(files, ["--emit=nothing"], outf)

        print()
        print(" ① 全部 .sy 文件串行处理（%d 个文件，repeat=%d）" % (len(files), args.repeat))
        print("    --emit=tokens  : wall %.3f s   CPU %.3f s" % (wall, cpu))
        print("    --emit=nothing : wall %.3f s   CPU %.3f s   ← 基线（启动+读文件+CRLF）"
              % (base_wall, base_cpu))
        print("    ⇒ 词法本身增量: wall %.3f s   CPU %.3f s"
              % (wall - base_wall, cpu - base_cpu))
        print("    平均每文件     : %.2f ms（竞争口径，主要是启动固定成本）"
              % (wall / len(files) * 1000))
        print("    退出码非 0 数  : %d ← 应为 14（含 '@' 的 tensor 用例；D3 报错是预期行为）"
              % nonzero)
        print("    判定           : %s（要求 < 1.000 s）"
              % ("✔ 达标" if wall < 1.0 else "△ 未达标 —— 见上面的基线拆分"))

        longs = sorted(glob.glob(os.path.join(root, "tests", "**",
                                              "29_long_line.sy"), recursive=True))
        print()
        print(" ② 86 KB 单行（%d 个副本）" % len(longs))
        for f in longs:
            size = os.path.getsize(f)
            runs = []
            rc = 0
            for _ in range(max(5, args.repeat)):
                t0 = time.perf_counter()
                rc = subprocess.run([compiler, f, "--emit=tokens", "-o", outf],
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL).returncode
                runs.append(time.perf_counter() - t0)
            runs.sort()
            med = runs[len(runs) // 2]
            print("    %-56s %6d B" % (os.path.relpath(f, root), size))
            print("        最好 %.1f ms / 中位 %.1f ms / 最差 %.1f ms  （5+ 次，退出码 %d）"
                  % (runs[0] * 1000, med * 1000, runs[-1] * 1000, rc))
            print("        判定: %s（要求 < 100 ms）"
                  % ("✔ 达标" if med < 0.1 else "✘ 超标"))
        print("=" * 76)
        print(" ⚠️ 上面的 wall 含进程启动（静态构建约 0.9 ms/次，占绝大部分）。")
        print("    算法吞吐请看：unit/bench_lexer <file.sy> <轮数>（进程内循环，无启动开销）")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
