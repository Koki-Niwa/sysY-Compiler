#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_crlf.py —— CRLF 与 LF 的词法结果必须完全一致（S01 §7.8）

做法：对每个含 CR 的 `.sy`，把它的 LF 版本写到工作目录，分别对【原件】与
      【LF 版】跑 `--emit=tokens`，要求两份 dump **逐字节相同**。

为什么这是对的判据：`SourceFile` 在入口就把 `\\r\\n` / 孤立 `\\r` 规范化成 `\\n`，
所以行号列号都按规范化后的文本算 —— 两种行尾的 dump 理应一模一样。
若不一致，说明有 `\\r` 漏进了 token 或列号（实测这是很容易犯的错）。

用法: check_crlf.py --compiler <path> [--verbose]
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--root", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    root = args.root
    if root is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    compiler = os.path.abspath(args.compiler)

    files = sorted(glob.glob(os.path.join(root, "tests", "**", "*.sy"), recursive=True))
    crlf_files = []
    for f in files:
        with open(f, "rb") as fh:
            if b"\r" in fh.read():
                crlf_files.append(f)

    print("=" * 76)
    print(" CRLF vs LF 词法结果一致性（S01 §7.8）")
    print(" 编译器: %s" % compiler)
    print("=" * 76)
    if not crlf_files:
        print(" 语料里没有含 CR 的 .sy —— 用构造用例覆盖（见 unit/test_lexer.cpp 第 11 组）")
        return 0

    tmp = tempfile.mkdtemp(prefix=".s01_crlf_", dir=root)
    ok = 0
    bad = []
    try:
        for src in crlf_files:
            rel = os.path.relpath(src, root)
            with open(src, "rb") as fh:
                raw = fh.read()
            lf = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            lf_path = os.path.join(tmp, rel.replace("/", "__") + ".lf.sy")
            with open(lf_path, "wb") as fh:
                fh.write(lf)

            a = os.path.join(tmp, "a.tok")
            b = os.path.join(tmp, "b.tok")
            ra = subprocess.run([compiler, src, "--emit=tokens", "-o", a],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            rb = subprocess.run([compiler, lf_path, "--emit=tokens", "-o", b],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if ra.returncode != rb.returncode:
                bad.append((rel, "退出码不同: CRLF=%d LF=%d" % (ra.returncode, rb.returncode)))
                continue
            with open(a, "rb") as fh:
                da = fh.read()
            with open(b, "rb") as fh:
                db = fh.read()
            if da != db:
                # 找第一处差异
                n = min(len(da), len(db))
                at = next((i for i in range(n) if da[i] != db[i]), n)
                bad.append((rel, "dump 不同（长度 %d vs %d），首处差异 @%d: %r vs %r"
                            % (len(da), len(db), at, da[at:at + 40], db[at:at + 40])))
                continue
            ok += 1
            if args.verbose:
                print("  ✔ %-58s %d 行 dump 完全一致" % (rel, da.count(b"\n")))

        print()
        print(" 含 CR 的 .sy 文件数 : %d" % len(crlf_files))
        print(" 逐字节一致          : %d" % ok)
        print(" 不一致              : %d" % len(bad))
        for rel, why in bad[:20]:
            print("   ✘ %s\n       %s" % (rel, why))
        print(" 判定: %s" % ("✔ CRLF 与 LF 的行列号/词法结果完全一致"
                               if not bad else "✘ 有文件不一致"))
        return 0 if not bad else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
