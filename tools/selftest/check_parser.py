#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_parser.py —— ★★ 语法往返验证（S02 验收的轨 A）

不变式（phases/S02-parser.md §6.1）：

    对每个源文件：
        source --emit=ast-->        A.ast
        A.ast  --from-ast --emit=ast--> A2.ast
    要求 **A.ast 与 A2.ast 逐字节相同**。

为什么这条不是同义反复（prompt §6.2）：
  * 它验证 printer 与 parser **相互一致**（打印出的结构能被自己重新读成同一棵树）
  * 它验证 **printer 不丢信息**：少任何一个字段，重读后就会不一致
  * 它覆盖 490 个真实文件的全部形态，而不是人工构造的样例

⚠️ 它**不能**验证什么（prompt §6.3，**必须写进报告**）：
  它发现不了"优先级理解错了"。若 parser 把 `a+b*c` 错解析成 `(a+b)*c`，
  printer 会忠实打印 `(* (+ a b) c)`，重读又得到同一棵树 ⇒ 往返照样通过。
  抓这类错误只能靠 test_parser.cpp 的树形断言（轨 B，34 条）。

范围与期望（prompt §八，实测 540 个用例）：
  * 490 个应往返成功（两条路径都退出 0、两份文本逐字节相同）
  * 50 个含 `@` / `tensor` 的应**报诊断、退出码 1、不崩**（D3：不实现）
    —— 判据与 TESTING-GUIDE §7 一致：**先去掉注释**再找关键词
       （否则 `23_json.sy` 这类在注释里提到关键词的文件会被误判）

用法:
    check_parser.py --compiler <path> [--filter <glob>] [--jobs N] [--verbose]
                    [--root DIR] [--json FILE] [--timeout SEC]
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

# ============================================================================
# 一、独立实现的"去注释"（**不复用编译器的任何代码**）
#
# 与 C++ 侧的语义保持一致但代码独立：注释里的关键词不算数。
#   * 行注释：'//' 到行尾
#   * 块注释：'/*' 到第一个 '*/'（不嵌套）
#   * 注释里的引号一律无视（SysY 没有字符串字面量）
# ============================================================================
def strip_comments(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i = min(i + 2, n)
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def out_of_scope_reason(text: str):
    """返回 None（在范围内）或一个说明字符串（范围外）。"""
    code = strip_comments(text)
    reasons = []
    if '@' in code:
        reasons.append('含 `@`')
    if re.search(r'\btensor\b', code):
        reasons.append('含 `tensor`')
    return ' 且 '.join(reasons) if reasons else None


# ============================================================================
# 二、单文件验证
# ============================================================================
def first_diff(a: bytes, b: bytes):
    """返回 (说明, 偏移, 上下文)；用于失败时给人看。"""
    i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
    lo = max(0, i - 120)
    ctx_a = a[lo:i + 120].decode('utf-8', 'replace')
    ctx_b = b[lo:i + 120].decode('utf-8', 'replace')
    if i >= min(len(a), len(b)):
        return ('长度不同（%d vs %d），前 %d 字节相同' % (len(a), len(b), i), i, '')
    return ('第 %d 字节不同' % i, i,
            'A.ast:  %r\n      A2.ast: %r' % (ctx_a, ctx_b))


def check_one(compiler, rel_path, root, workdir, timeout):
    """返回 dict：{path, status, detail, ...}；status ∈ ok/skipped/fail/error"""
    sy = os.path.join(root, rel_path)
    res = {"path": rel_path, "status": "fail", "detail": ""}

    try:
        with open(sy, 'rb') as f:
            raw = f.read()
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "无法读取源码: %s" % e
        return res

    text = raw.decode('utf-8', 'replace')
    reason = out_of_scope_reason(text)

    tag = rel_path.replace('/', '__')
    a1 = os.path.join(workdir, tag + '.1.ast')
    a2 = os.path.join(workdir, tag + '.2.ast')

    def run(args):
        return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout)

    # ── ① 源 → AST ──────────────────────────────────────────────────────
    try:
        p1 = run([compiler, sy, '--emit=ast', '-o', a1])
    except subprocess.TimeoutExpired:
        res["status"] = "error"
        res["detail"] = "【源→AST】超时（> %.1fs）—— 语法器可能死循环" % timeout
        return res
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "无法执行编译器: %s" % e
        return res

    if reason is not None:
        # ── 范围外（@ / tensor）：**必须报诊断、退出码 1、不崩** ──
        res["status"] = "skipped"
        if p1.returncode != 1:
            res["status"] = "fail"
            res["detail"] = ("范围外文件（%s）应退出码 1，实际 %d"
                             % (reason, p1.returncode))
            if p1.returncode < 0:
                res["detail"] = ("范围外文件（%s）被信号杀死（signal %d）—— 【崩了】"
                                 % (reason, -p1.returncode))
            return res
        if not p1.stderr.strip():
            res["status"] = "fail"
            res["detail"] = "范围外文件（%s）退出码 1 但没有诊断输出" % reason
            return res
        err = p1.stderr.decode('utf-8', 'replace')
        if 'error:' not in err:
            res["status"] = "fail"
            res["detail"] = "范围外文件（%s）的诊断里没有 'error:'" % reason
            return res
        res["detail"] = "%s → 退出码 1、有诊断（符合预期）" % reason
        res["first_diag"] = err.strip().split('\n')[0]
        return res

    if p1.returncode != 0:
        res["status"] = "error"
        res["detail"] = ("【源→AST】范围内文件却退出码 %d\n      stderr: %s"
                         % (p1.returncode,
                            p1.stderr.decode('utf-8', 'replace').strip()
                            .replace('\n', '\n      ')[:800]))
        return res
    if p1.stderr.strip():
        # 范围内文件不应该有任何诊断（warning 也不行 —— S02 只做语法）
        res["status"] = "fail"
        res["detail"] = ("【源→AST】范围内文件报出了诊断（应当零诊断）:\n      %s"
                         % p1.stderr.decode('utf-8', 'replace').strip()
                         .replace('\n', '\n      ')[:600])
        return res

    # ── ② AST 文本 → AST → AST 文本 ─────────────────────────────────────
    try:
        p2 = run([compiler, a1, '--from-ast', '--emit=ast', '-o', a2])
    except subprocess.TimeoutExpired:
        res["status"] = "error"
        res["detail"] = "【AST→AST】超时（> %.1fs）—— S-表达式读取器可能死循环" % timeout
        return res

    if p2.returncode != 0:
        res["status"] = "error"
        res["detail"] = ("【AST→AST】退出码 %d（--from-ast 读不回自己打印的文本！）\n"
                         "      stderr: %s"
                         % (p2.returncode,
                            p2.stderr.decode('utf-8', 'replace').strip()
                            .replace('\n', '\n      ')[:800]))
        return res

    try:
        with open(a1, 'rb') as f:
            b1 = f.read()
        with open(a2, 'rb') as f:
            b2 = f.read()
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "读不到产物: %s" % e
        return res

    if not b1:
        res["status"] = "fail"
        res["detail"] = "A.ast 是空文件（范围内文件应当产出 AST）"
        return res

    # ── ③ 逐字节比较 ────────────────────────────────────────────────────
    if b1 != b2:
        why, at, ctx = first_diff(b1, b2)
        res["status"] = "fail"
        res["detail"] = "往返不一致：%s\n      %s" % (why, ctx)
        return res

    res["status"] = "ok"
    res["detail"] = "%d 字节逐字节相同" % len(b1)
    res["bytes"] = len(b1)
    return res


# ============================================================================
# 三、主流程
# ============================================================================
def find_test_root(root):
    """测试语料根目录：优先 <root>/tests/final_arm，退回 <root>/tests。"""
    cand = os.path.join(root, "tests", "final_arm")
    if os.path.isdir(cand):
        return os.path.join(root, "tests")
    cand = os.path.join(root, "tests")
    if os.path.isdir(cand):
        return cand
    raise SystemExit("[check_parser] 找不到测试语料目录: %s" % cand)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="SysY 语法往返自校验：source → AST → 重解析 → AST，两份文本逐字节相同")
    ap.add_argument("--compiler", required=True, help="编译器可执行文件路径")
    ap.add_argument("--filter", default="**/*.sy",
                    help="相对于 tests/ 的 glob（默认 **/*.sy）")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4)),
                    help="并行度（默认 = CPU 核数）")
    ap.add_argument("--verbose", action="store_true", help="逐文件打印结果")
    ap.add_argument("--root", default=None, help="仓库根目录（默认自动探测）")
    ap.add_argument("--json", default=None, help="把结果写到 JSON 文件")
    ap.add_argument("--timeout", type=float, default=60.0, help="单文件超时（秒）")
    ap.add_argument("--show-failures", type=int, default=20,
                    help="最多打印多少个失败明细（默认 20）")
    args = ap.parse_args(argv)

    compiler = os.path.abspath(args.compiler)
    if not os.path.isfile(compiler) or not os.access(compiler, os.X_OK):
        raise SystemExit("[check_parser] 编译器不可执行: %s" % compiler)

    root = args.root
    if root is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    tests_root = find_test_root(root)

    pattern = os.path.join(tests_root, args.filter)
    files = sorted(glob.glob(pattern, recursive=True))
    rels = [os.path.relpath(f, root) for f in files if os.path.isfile(f)]
    if not rels:
        raise SystemExit("[check_parser] 没有匹配到文件: %s" % pattern)

    print("=" * 78)
    print(" 语法往返自校验 check_parser.py")
    print(" 编译器   : %s" % compiler)
    print(" 语料     : %s   （filter=%s）" % (tests_root, args.filter))
    print(" 不变式   : source --emit=ast→A.ast；A.ast --from-ast --emit=ast→A2.ast；A==A2")
    print(" 期望     : 490 个往返成功；50 个含 @/tensor 的报诊断 + 退出码 1")
    print(" 并行度   : %d" % args.jobs)
    print("=" * 78)

    t0 = time.time()
    results = []
    # 工作目录放在【仓库内】（DSH 沙箱只保证 session workspace 可写；
    # 不要假设 /tmp 可用），跑完整体删除。名字固定便于失败时人工查看。
    workdir = tempfile.mkdtemp(prefix=".s02_check_parser_", dir=root)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(check_one, compiler, r, root, workdir, args.timeout): r
                    for r in rels}
            for fut in concurrent.futures.as_completed(futs):
                try:
                    results.append(fut.result())
                except Exception as e:      # noqa: BLE001 - 报告而不是崩
                    results.append({"path": futs[fut], "status": "error",
                                    "detail": "内部异常: %r" % (e,)})
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    elapsed = time.time() - t0

    results.sort(key=lambda r: r["path"])
    n_ok = sum(1 for r in results if r["status"] == "ok")
    n_skip = sum(1 for r in results if r["status"] == "skipped")
    n_fail = sum(1 for r in results if r["status"] == "fail")
    n_err = sum(1 for r in results if r["status"] == "error")
    n_total = len(results)
    n_bytes = sum(r.get("bytes", 0) for r in results if r["status"] == "ok")

    if args.verbose:
        print()
        for r in results:
            mark = {"ok": "✔", "skipped": "−", "fail": "✘", "error": "‼"}[r["status"]]
            print("  %s %-58s %s" % (mark, r["path"], r["detail"].split("\n")[0]))

    bad = [r for r in results if r["status"] in ("fail", "error")]
    if bad:
        print()
        print("── 失败明细（最多 %d 个）──" % args.show_failures)
        for r in bad[:args.show_failures]:
            print("  ✘ %s" % r["path"])
            print("      %s" % r["detail"])
        if len(bad) > args.show_failures:
            print("  … 还有 %d 个" % (len(bad) - args.show_failures))

    # 范围外文件的诊断样例（证明"报诊断而不是静默通过"）
    skips = [r for r in results if r["status"] == "skipped"]
    if skips and args.verbose:
        print()
        print("── 范围外（@ / tensor）样例 ──")
        for r in skips[:5]:
            print("  − %s\n      %s\n      %s"
                  % (r["path"], r["detail"], r.get("first_diag", "")))

    print()
    print("=" * 78)
    print(" 统计")
    print("   范围内往返成功 (ok)     : %d" % n_ok)
    print("   范围外已报诊断 (skipped): %d" % n_skip)
    print("   失败           (fail)   : %d" % n_fail)
    print("   异常           (error)  : %d" % n_err)
    print("   合计                    : %d" % n_total)
    print("   往返产出的 AST 总字节数 : %d" % n_bytes)
    print("   耗时                    : %.2f s（并行度 %d）" % (elapsed, args.jobs))
    print("=" * 78)

    if args.json:
        with open(args.json, 'w') as f:
            json.dump({"compiler": compiler, "total": n_total, "ok": n_ok,
                       "skipped": n_skip, "fail": n_fail, "error": n_err,
                       "elapsed": elapsed, "results": results}, f,
                      ensure_ascii=False, indent=2)
        print(" 结果已写入: %s" % args.json)

    if n_fail == 0 and n_err == 0:
        print(" ✔ 轨 A（往返）全部通过：%d 个范围内文件逐字节相同，"
              "%d 个范围外文件报诊断且不崩" % (n_ok, n_skip))
        return 0
    print(" ✘ 轨 A（往返）有失败 —— 【不要进入下一个会话】")
    return 1


if __name__ == "__main__":
    sys.exit(main())
