#!/usr/bin/env python3
"""gen_manifest.py —— 扫描 tests/ 生成 tests/manifest.tsv（P00 prompt §4.6）

输出格式（每行 7 列，TAB 分隔）：

    <suite>\t<category>\t<name>\t<sy路径>\t<in路径或->\t<out路径>\t<skip原因或->

已实测确认的语料事实（处理规则）：
  * 有些用例【没有 .in】→ 用空 stdin 跑，<in路径> 写 '-'
  * 有些 .in 比 .sy 多（隐藏测试点）→ 跳过，不为孤立 .in 生成行
  * 含 `tensor` 的用例 → 标 SKIP（不是 FAIL）；判断前【先去注释与字符串】
  * 同名文件在不同 suite 内容可能不同 → manifest 按 suite/category/name 唯一标识，
    【不要按 name 去重】（两条赛道的同名用例是两个不同的程序，都要过）

用法:
    python3 compiler/tools/selftest/gen_manifest.py [--root <工作区根>]
                                                    [--out tests/manifest.tsv]
                                                    [--stdout] [--quiet]
"""

import argparse
import os
import re
import sys

SUITES = ["final_arm", "final_riscv", "prelim_arm", "prelim_riscv"]
CATEGORIES = ["functional", "h_functional", "performance"]
# 按 prompt 的输出示例顺序
CATEGORY_ORDER = {c: i for i, c in enumerate(CATEGORIES)}

TENSOR_RE = re.compile(r"\btensor\b")


def strip_comments_and_strings(src: str) -> str:
    """去掉 // 行注释、/* */ 块注释、"..." 与 '...' 字符串（转义感知）。

    为什么必须这样判断：23_json.sy 之类的用例在【注释里】出现关键词，
    直接 grep 会误判成 tensor 用例（TESTING-GUIDE §7）。
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
        elif c in ("\"", "'"):
            quote = c
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")   # 占位，保持 token 边界
        else:
            out.append(c)
            i += 1
    return "".join(out)


def is_tensor_case(path: str) -> bool:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return bool(TENSOR_RE.search(strip_comments_and_strings(f.read())))


def discover_suites(tests_dir):
    """优先用固定清单；额外目录也一并扫描（目录结构变了不至于静默漏掉）。"""
    found = [s for s in SUITES if os.path.isdir(os.path.join(tests_dir, s))]
    for entry in sorted(os.listdir(tests_dir)):
        p = os.path.join(tests_dir, entry)
        if os.path.isdir(p) and entry not in found and not entry.startswith("."):
            found.append(entry)
    return found


def scan(root):
    tests_dir = os.path.join(root, "tests")
    rows = []
    stats = {"sy": 0, "tensor": 0, "no_in": 0, "orphan_in": 0, "orphan_out": 0}
    orphans = []
    for suite in discover_suites(tests_dir):
        sdir = os.path.join(tests_dir, suite)
        cats = [c for c in CATEGORIES if os.path.isdir(os.path.join(sdir, c))]
        for entry in sorted(os.listdir(sdir)):
            if os.path.isdir(os.path.join(sdir, entry)) and entry not in cats:
                cats.append(entry)
        for cat in cats:
            cdir = os.path.join(sdir, cat)
            names = sorted(f[:-3] for f in os.listdir(cdir) if f.endswith(".sy"))
            ins = {f[:-3] for f in os.listdir(cdir) if f.endswith(".in")}
            for name in names:
                sy = os.path.relpath(os.path.join(cdir, name + ".sy"), root)
                in_path = os.path.relpath(os.path.join(cdir, name + ".in"), root)
                out_path = os.path.relpath(os.path.join(cdir, name + ".out"), root)
                stats["sy"] += 1
                if not os.path.isfile(os.path.join(root, in_path)):
                    in_path = "-"
                    stats["no_in"] += 1
                if not os.path.isfile(os.path.join(root, out_path)):
                    out_path = "-"
                skip = "-"
                if is_tensor_case(os.path.join(root, sy)):
                    skip = "tensor"
                    stats["tensor"] += 1
                rows.append((suite, cat, name, sy, in_path, out_path, skip))
            for extra in sorted(ins - set(names)):
                stats["orphan_in"] += 1
                orphans.append(os.path.join(suite, cat, extra + ".in"))
            # 孤立 .out 也要报告：它同样说明"有测试点没有对应源码"
            # （实测 final_riscv/prelim_* 各 3 个 prime_search*.out）
            outs = {f[:-4] for f in os.listdir(cdir)
                    if f.endswith(".out")}
            for extra in sorted(outs - set(names)):
                stats["orphan_out"] += 1
                orphans.append("OUT:" + os.path.join(suite, cat, extra + ".out"))
    # 排序：suite 按固定顺序（未知 suite 排后面），category 按 functional/h_functional/performance
    suite_order = {s: i for i, s in enumerate(SUITES)}
    rows.sort(key=lambda r: (suite_order.get(r[0], 99), CATEGORY_ORDER.get(r[1], 99), r[2]))
    return rows, stats, orphans


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")))
    ap.add_argument("--out", default=None)
    ap.add_argument("--stdout", action="store_true", help="打到标准输出而不写文件")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    out_path = args.out or os.path.join(root, "tests", "manifest.tsv")
    rows, stats, orphans = scan(root)

    lines = ["# SysY 测试清单（由 compiler/tools/selftest/gen_manifest.py 生成，勿手改）",
             "# 列: suite\tcategory\tname\tsy\tin\tout\tskip",
             "# suite/category/name 是唯一标识；同名用例在不同赛道是不同程序，不要按 name 去重"]
    for r in rows:
        lines.append("\t".join(r))
    text = "\n".join(lines) + "\n"

    if args.stdout:
        sys.stdout.write(text)
    else:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)

    if not args.quiet:
        n_skip = sum(1 for r in rows if r[6] != "-")
        msg = ("[gen_manifest] %d 个用例（其中 %d 个 tensor 标 SKIP）→ %s\n"
               "  无 .in 的用例 %d 个（空 stdin）；忽略的孤立 .in %d 个\n"
               % (len(rows), n_skip,
                  "stdout" if args.stdout else out_path,
                  stats["no_in"], stats["orphan_in"]))
        for o in orphans:
            msg += "  孤立文件（无对应 .sy，隐藏测试点）: %s\n" % o
        sys.stderr.write(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
