#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_tokens_meta.py —— 逐 token 校验 `loc` + `length` 能反查原文（S01 §7.6）

    src.text()[offsetOf(loc) : offsetOf(loc)+length] == 该 token 的原文

这是 --emit=tokens 之外的一条独立检查：dump 只给出行/列/原文，
**不验证列号真的指对位置**。本脚本用行/列独立复算偏移量，
再要求它切出来的字节与第 4 列逐字节相同。

⚠️ 偏移量按【规范化后】文本独立重算（LF，末尾补 '\\n'），不复用编译器代码。

用法: check_tokens_meta.py --compiler <path> [--jobs N] [--verbose]
"""

import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys
import tempfile


def normalize_newlines(raw: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == 0x0D:
            out.append(0x0A)
            if i + 1 < n and raw[i + 1] == 0x0A:
                i += 1
        else:
            out.append(c)
        i += 1
    if not out or out[-1] != 0x0A:
        out.append(0x0A)
    return bytes(out)


def line_starts(text: bytes):
    """返回 line_start[k] = 第 k+1 行（1-based）的起始偏移；末尾一个哨兵。"""
    starts = [0]
    for i, c in enumerate(text):
        if c == 0x0A and i + 1 < len(text):
            starts.append(i + 1)
    if starts[-1] != len(text):
        starts.append(len(text))
    return starts


def offset_of(text: bytes, starts, line: int, col: int):
    """按 SourceFile::offsetOf 的约定独立复算（列越界夹到行尾）。"""
    if line == 0 or line > len(starts) - 1:
        return len(text)
    if col == 0:
        return starts[line - 1]
    start = starts[line - 1]
    end = starts[line]
    line_len = end - start
    while line_len > 0 and text[start + line_len - 1] == 0x0A:
        line_len -= 1
    off = start + (col - 1)
    return start + line_len if off > start + line_len else off


def check_one(compiler, rel, root, workdir):
    sy = os.path.join(root, rel)
    outf = os.path.join(workdir, rel.replace("/", "__") + ".tok")
    res = {"path": rel, "status": "ok", "detail": "", "n": 0}
    try:
        with open(sy, "rb") as f:
            text = normalize_newlines(f.read())
        # 退出码 1（词法错误，如含 '@' 的 tensor 用例）时 dump 仍会被写出 ——
        # 这里不为退出码设门槛（那是 check_lexer.py 的事），只要求产物存在且可解析。
        subprocess.run([compiler, sy, "--emit=tokens", "-o", outf],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=60)
        if not os.path.exists(outf):
            res["status"] = "error"
            res["detail"] = "没有产出 dump 文件"
            return res
        with open(outf, "rb") as f:
            dump = f.read()
    except Exception as e:                                   # noqa: BLE001
        res["status"] = "error"
        res["detail"] = "执行失败: %r" % (e,)
        return res

    starts = line_starts(text)
    n_eof = 0
    n_tok = 0
    for lineno, raw in enumerate(dump.split(b"\n")):
        if raw == b"":
            continue
        parts = raw.split(b"\t")
        if len(parts) != 4:
            res["status"] = "fail"
            res["detail"] = "第 %d 行不是 4 列: %r" % (lineno + 1, raw[:100])
            return res
        line, col, kind, body = int(parts[0]), int(parts[1]), parts[2], parts[3]
        if kind == b"EOF":
            n_eof += 1
            continue
        n_tok += 1
        off = offset_of(text, starts, line, col)
        got = text[off:off + len(body)]
        if got != body:
            res["status"] = "fail"
            res["detail"] = ("%s:%d:%d (%s) loc+length 反查得到 %r，第 4 列是 %r"
                             % (rel, line, col, kind.decode(), got[:60], body[:60]))
            return res
    if n_eof != 1:
        res["status"] = "fail"
        res["detail"] = "EOF 行出现 %d 次" % n_eof
        return res
    res["n"] = n_tok
    return res


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 4))
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--root", default=None)
    args = ap.parse_args(argv)

    root = args.root
    if root is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    compiler = os.path.abspath(args.compiler)
    files = sorted(glob.glob(os.path.join(root, "tests", "**", "*.sy"), recursive=True))
    rels = [os.path.relpath(f, root) for f in files]

    print("=" * 76)
    print(" 逐 token loc+length 反查原文（S01 §7.6）")
    print(" 编译器: %s" % compiler)
    print("=" * 76)

    workdir = tempfile.mkdtemp(prefix=".s01_meta_", dir=root)
    try:
        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = [ex.submit(check_one, compiler, r, root, workdir) for r in rels]
            for fut in concurrent.futures.as_completed(futs):
                results.append(fut.result())
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    results.sort(key=lambda r: r["path"])
    bad = [r for r in results if r["status"] != "ok"]
    n_tok = sum(r["n"] for r in results if r["status"] == "ok")
    if args.verbose:
        for r in results:
            print("  %s %-58s %s" % ("✔" if r["status"] == "ok" else "✘",
                                     r["path"], r["detail"]))
    if bad:
        for r in bad[:20]:
            print("  ✘ %s\n      %s" % (r["path"], r["detail"]))
    print()
    print(" 文件数        : %d" % len(results))
    print(" 通过          : %d" % (len(results) - len(bad)))
    print(" 失败          : %d" % len(bad))
    print(" 覆盖 token 数 : %d" % n_tok)
    print(" 判定          : %s" % ("✔ 全部文件的每个 token 都能用 loc+length 反查原文"
                                   if not bad else "✘ 有失败"))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
