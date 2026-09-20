#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_lexer.py —— ★★ 词法自校验（S01 验收的核心）

不变式（phases/S01-lexer.md §5.1）：

    把 `--emit=tokens` 输出的【第 4 列】（EOF 行除外）按顺序拼接，
    应该【逐字节等于】源文本去掉所有空白与注释之后的内容。

为什么这是最强的验收：它不是"看起来对"，而是数学恒等式 —— 可逐字节判定，
且对全部范围内用例都成立，不需要人工构造期望值。
它能抓住一整类 bug：最长匹配错误、注释边界错、数字字面量被截断、漏掉某个 token。

⚠️ 本脚本的"去空白去注释"是【独立实现】（下面 strip_ws_and_comments），
   **不复用 Lexer 的任何代码** —— 否则同一个 bug 会在两边同时出现，自校验失效。

用法:
    check_lexer.py --compiler <path> [--filter <glob>] [--jobs N] [--verbose]
                   [--root DIR] [--json FILE] [--keep-going]
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
# 一、独立实现的"规范化行尾 + 去空白去注释"
#
# 与 C++ 侧（support/SourceFile.cpp）保持【语义一致】但【代码独立】：
#   * 规范化：'\r\n' → '\n'；孤立 '\r' → '\n'；末尾若无 '\n' 则补一个
#   * 空白：空格 / \t / \n / \v / \f（与 Lexer 的 isSpace 一致；'\r' 已被规范化掉）
#   * 行注释：'//' 到行尾（'\n' 本身保留给空白处理）
#   * 块注释：'/*' 到【第一个】'*/'（不嵌套；注释里的引号一律无视）
#   * 非 ASCII 字节只可能出现在注释里 —— 由注释分支整段跳过
# ============================================================================

SLASH = 0x2F   # '/'
STAR = 0x2A    # '*'
LF = 0x0A      # '\n'
CR = 0x0D      # '\r'
WHITESPACE_BYTES = set(b" \t\n\v\f")


def normalize_newlines(raw_bytes: bytes) -> bytes:
    """与 SourceFile::build() 的 normalizeNewlines + 末尾补 '\\n' 完全一致。"""
    out = bytearray()
    i = 0
    n = len(raw_bytes)
    while i < n:
        c = raw_bytes[i]
        if c == CR:
            out.append(LF)
            if i + 1 < n and raw_bytes[i + 1] == LF:
                i += 1                    # 吃掉 "\r\n" 里的 '\n'
        else:
            out.append(c)
        i += 1
    if not out or out[-1] != LF:
        out.append(LF)                    # text() 保证以 '\n' 结尾
    return bytes(out)


def _skip_comment(text: bytes, i: int, n: int) -> int:
    """i 指向 '/'。若是注释则返回其后的位置，否则原样返回 i（不消费）。"""
    if i + 1 >= n:
        return i
    nxt = text[i + 1]
    if nxt == SLASH:                      # '//' 到行尾
        i += 2
        while i < n and text[i] != LF:
            i += 1
        return i
    if nxt == STAR:                       # '/*' 到第一个 '*/'
        i += 2
        while i + 1 < n and not (text[i] == STAR and text[i + 1] == SLASH):
            i += 1
        return min(i + 2, n)
    return -1                             # 不是注释（就是个除号）


def strip_ws_and_comments(text: bytes) -> bytes:
    """独立实现：去掉所有空白与注释，返回剩余字节。"""
    out = bytearray()
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in WHITESPACE_BYTES:
            i += 1
            continue
        if c == SLASH:
            j = _skip_comment(text, i, n)
            if j >= 0:
                i = j
                continue
        out.append(c)
        i += 1
    return bytes(out)


def comment_stripped_text(text: bytes) -> bytes:
    """只去注释、【保留空白】—— 用于"跳过判定"。

    必须在去空白之前做：否则 `tensor int a` 会被拼成 `tensorinta`，
    词边界（\\b）失效，判定就会漏掉全部张量用例（设计阶段踩过这个坑）。
    """
    out = bytearray()
    i = 0
    n = len(text)
    while i < n:
        if text[i] == SLASH:
            j = _skip_comment(text, i, n)
            if j >= 0:
                i = j
                continue
        out.append(text[i])
        i += 1
    return bytes(out)


# ============================================================================
# 二、跳过判定（张量用例，D3：`tensor` / `@` 不在范围内）
#
# 判据（源码级，独立于 Lexer）：去注释后的文本里出现 `@` 或独立的 `tensor` 词。
# ============================================================================

RE_TENSOR_WORD = re.compile(rb"(?<![A-Za-z0-9_])tensor(?![A-Za-z0-9_])")


def out_of_scope_reasons(text: bytes):
    """返回范围外原因列表（空 = 在范围内）。输入应为【规范化后】的文本。"""
    reasons = []
    stripped = comment_stripped_text(text)
    if b"@" in stripped:
        reasons.append("'@'")
    if RE_TENSOR_WORD.search(stripped):
        reasons.append("'tensor'")
    return reasons


# ============================================================================
# 三、跑编译器 + 解析 dump
# ============================================================================

class DumpError(Exception):
    pass


def parse_dump(dump_bytes: bytes):
    """解析 --emit=tokens 的输出。

    返回 (pieces, eof_count, n_tokens, kinds)：
      pieces = 第 4 列按顺序拼接（EOF 行除外）
      kinds  = 第 3 列按顺序（EOF 行除外）

    kinds 用于【边界检查】：拼接不变式对"把一个 token 拆成两半"是全盲的
    （拆分不改变拼接结果，也不破坏每一半各自的 loc+length），只有比对
    kind 序列才能发现。

    格式（§4）：<行>\t<列>\t<种类>\t<原文>
    """
    pieces = []
    kinds = []
    eof_count = 0
    n_tokens = 0
    for lineno, raw in enumerate(dump_bytes.split(b"\n")):
        if raw == b"":
            continue                      # 末尾换行的空尾巴
        parts = raw.split(b"\t")
        if len(parts) != 4:
            raise DumpError(
                "第 %d 行不是 4 列制表符分隔: %r" % (lineno + 1, raw[:120]))
        line_s, col_s, kind, text = parts
        if not line_s.isdigit() or not col_s.isdigit():
            raise DumpError("第 %d 行的行/列不是数字: %r" % (lineno + 1, raw[:120]))
        if int(line_s) == 0 or int(col_s) == 0:
            raise DumpError("第 %d 行出现 0 行号/列号（必须 1-based）: %r"
                            % (lineno + 1, raw[:120]))
        n_tokens += 1
        if kind == b"EOF":
            eof_count += 1
            if text != b"-":
                raise DumpError("EOF 行第 4 列必须是 '-': %r" % (raw[:120],))
            continue
        pieces.append(text)
        kinds.append(kind.decode("utf-8", "replace"))
    if eof_count != 1:
        raise DumpError("EOF 行应恰好出现 1 次，实际 %d 次" % eof_count)
    return b"".join(pieces), eof_count, n_tokens, kinds


def first_diff(got: bytes, want: bytes, ctx: int = 40):
    """返回第一处差异的可读描述（各取前后 ctx 字节）。"""
    n = min(len(got), len(want))
    at = n
    for i in range(n):
        if got[i] != want[i]:
            at = i
            break
    lo = max(0, at - ctx)
    g = got[lo:at + ctx]
    w = want[lo:at + ctx]
    if at >= n and len(got) != len(want):
        detail = "长度不同: got=%d want=%d（前 %d 字节相同）" % (len(got), len(want), n)
    else:
        detail = ("首处差异 @ 偏移 %d: got=%r want=%r" % (at, g, w))
    return detail, at, g, w


def is_texty(data: bytes) -> bool:
    if not data:
        return True
    printable = sum(1 for b in data if 32 <= b < 127 or b in (9, 10))
    return printable >= len(data) - 1


# ============================================================================
# 四、单个文件的检查
# ============================================================================

def check_one(compiler: str, rel_path: str, root: str, workdir: str, timeout: float):
    """返回 dict：{path, status, detail...}；status ∈ ok/skipped/fail/error"""
    sy = os.path.join(root, rel_path)
    res = {"path": rel_path, "status": "fail", "detail": ""}

    try:
        with open(sy, "rb") as f:
            raw = f.read()
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "无法读取源码: %s" % e
        return res

    text = normalize_newlines(raw)
    reasons = out_of_scope_reasons(text)
    if reasons:
        res["status"] = "skipped"
        res["detail"] = "范围外（%s）" % " 且 ".join(reasons)
        return res

    want = strip_ws_and_comments(text)

    outf = os.path.join(workdir, rel_path.replace("/", "__") + ".tok")
    try:
        proc = subprocess.run(
            [compiler, sy, "--emit=tokens", "-o", outf],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        res["status"] = "error"
        res["detail"] = "编译器超时（> %.1fs）" % timeout
        return res
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "无法执行编译器: %s" % e
        return res

    rc = proc.returncode
    err = proc.stderr.decode("utf-8", "replace").strip()

    if rc != 0:
        res["status"] = "error"
        res["detail"] = ("范围内文件却退出码 %d（不该有词法错误）\n      stderr: %s"
                         % (rc, err.replace("\n", "\n      ")[:600]))
        return res

    try:
        with open(outf, "rb") as f:
            dump = f.read()
    except OSError as e:
        res["status"] = "error"
        res["detail"] = "无 dump 产物: %s" % e
        return res
    finally:
        try:
            os.remove(outf)
        except OSError:
            pass

    try:
        got, _eof, n_tokens, kinds = parse_dump(dump)
    except DumpError as e:
        res["status"] = "fail"
        res["detail"] = "dump 格式错误: %s" % e
        return res

    res["n_tokens"] = n_tokens

    # ① 拼接不变式（抓"漏/多字符"类错误）
    if got != want:
        detail, at, g, w = first_diff(got, want)
        res["status"] = "fail"
        res["detail"] = ("%s\n      源码偏移 %d 附近 want=%r\n      got 拼接=%r"
                         % (detail, at,
                            w if is_texty(w) else w.hex(),
                            g if is_texty(g) else g.hex()))
        return res

    # ② 【边界检查】kind 序列（抓"token 边界错"类错误 —— 拼接检查对此全盲）
    exp_kinds = independent_kinds(text)
    if kinds != exp_kinds:
        k = next((i for i in range(max(len(kinds), len(exp_kinds)))
                  if (kinds[i] if i < len(kinds) else None)
                  != (exp_kinds[i] if i < len(exp_kinds) else None)), 0)
        res["status"] = "fail"
        res["detail"] = ("token 边界错误：第 %d 个 token 种类不符\n"
                         "      dump=%r  独立实现=%r\n"
                         "      （拼接不变式对此类错误全盲，只有本检查能发现）"
                         % (k,
                            kinds[k] if k < len(kinds) else "「缺失」",
                            exp_kinds[k] if k < len(exp_kinds) else "「缺失」"))
        return res

    res["status"] = "ok"
    res["detail"] = ("%d 个 token，拼接 %d 字节逐字节相同，kind 序列一致"
                     % (n_tokens, len(want)))
    return res


# ============================================================================
# 五、主流程
# ============================================================================

def find_test_root(root: str) -> str:
    """测试语料根目录：优先 <root>/tests/final_arm，退回 <root>/tests。"""
    cand = os.path.join(root, "tests", "final_arm")
    if os.path.isdir(cand):
        return os.path.join(root, "tests")
    cand = os.path.join(root, "tests")
    if os.path.isdir(cand):
        return cand
    raise SystemExit("[check_lexer] 找不到测试语料目录: %s" % cand)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="SysY 词法自校验：token 拼接 == 原文去空白去注释")
    ap.add_argument("--compiler", required=True, help="编译器可执行文件路径")
    ap.add_argument("--filter", default="**/*.sy",
                    help="相对于 tests/ 的 glob（默认 **/*.sy）")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4)),
                    help="并行度（默认 = CPU 核数）")
    ap.add_argument("--verbose", action="store_true", help="逐文件打印结果")
    ap.add_argument("--root", default=None, help="仓库根目录（默认自动探测）")
    ap.add_argument("--json", default=None, help="把结果写到 JSON 文件")
    ap.add_argument("--timeout", type=float, default=30.0, help="单文件超时（秒）")
    ap.add_argument("--show-failures", type=int, default=20,
                    help="最多打印多少个失败明细（默认 20）")
    args = ap.parse_args(argv)

    compiler = os.path.abspath(args.compiler)
    if not os.path.isfile(compiler) or not os.access(compiler, os.X_OK):
        raise SystemExit("[check_lexer] 编译器不可执行: %s" % compiler)

    root = args.root
    if root is None:
        # 脚本在 compiler/tools/selftest/ 下 → 上溯 3 级 = 仓库根
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    tests_root = find_test_root(root)

    pattern = os.path.join(tests_root, args.filter)
    files = sorted(glob.glob(pattern, recursive=True))
    rels = [os.path.relpath(f, root) for f in files if os.path.isfile(f)]
    if not rels:
        raise SystemExit("[check_lexer] 没有匹配到文件: %s" % pattern)

    print("=" * 78)
    print(" 词法自校验 check_lexer.py")
    print(" 编译器   : %s" % compiler)
    print(" 语料     : %s   （filter=%s）" % (tests_root, args.filter))
    print(" 不变式   : 第 4 列按顺序拼接 == 原文去空白去注释（逐字节）")
    print(" 并行度   : %d" % args.jobs)
    print("=" * 78)

    t0 = time.time()
    results = []
    # 工作目录放在【仓库内】（DSH 沙箱只保证 session workspace 可写；
    # 不要假设 /tmp 可用），跑完整体删除。名字固定便于失败时人工查看。
    workdir = tempfile.mkdtemp(prefix=".s01_check_lexer_", dir=root)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(check_one, compiler, r, root, workdir, args.timeout): r
                    for r in rels}
            for fut in concurrent.futures.as_completed(futs):
                try:
                    results.append(fut.result())
                except Exception as e:                  # noqa: BLE001 - 报告而不是崩
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
    n_tokens = sum(r.get("n_tokens", 0) for r in results if r["status"] == "ok")

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

    print()
    print("=" * 78)
    print(" 总计            : %d 个 .sy 文件" % n_total)
    print(" 范围外跳过      : %d 个（含 '@' 或 'tensor'，D3 不实现）" % n_skip)
    print(" 范围内验证      : %d 个" % (n_ok + n_fail + n_err))
    print("   ✔ 不变式成立  : %d" % n_ok)
    print("   ✘ 失败        : %d" % n_fail)
    print("   ‼ 执行异常    : %d" % n_err)
    print(" 范围内 token 总数: %d" % n_tokens)
    print(" 耗时            : %.2f s（%.1f 文件/秒）"
          % (elapsed, n_total / elapsed if elapsed > 0 else 0.0))
    print("=" * 78)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"compiler": compiler, "filter": args.filter,
                       "total": n_total, "ok": n_ok, "skipped": n_skip,
                       "failed": n_fail, "error": n_err,
                       "tokens": n_tokens, "seconds": elapsed,
                       "results": results}, f, ensure_ascii=False, indent=1)

    # 判定：范围内文件必须【全部】成立，且不能有执行异常
    in_scope = n_total - n_skip
    if n_ok == in_scope and n_fail == 0 and n_err == 0:
        print(" 判定：✔ %d/%d 个范围内文件自校验通过（%d 个范围外已跳过）"
              % (n_ok, in_scope, n_skip))
        return 0
    print(" 判定：✘ 自校验未通过 —— 这就是词法器有 bug 的地方，不要跳过")
    return 1


_KW = {'const': 'kw_const', 'int': 'kw_int', 'float': 'kw_float',
       'void': 'kw_void', 'if': 'kw_if', 'else': 'kw_else',
       'while': 'kw_while', 'break': 'kw_break', 'continue': 'kw_continue',
       'return': 'kw_return'}

_SIMPLE = {'+': 'plus', '-': 'minus', '*': 'star', '/': 'slash', '%': 'percent',
           '<': 'less', '>': 'greater', '=': 'assign', '!': 'not',
           '(': 'lparen', ')': 'rparen', '{': 'lbrace', '}': 'rbrace',
           '[': 'lbracket', ']': 'rbracket', ',': 'comma', ';': 'semicolon'}

_DOUBLE = {'<=': 'lesseq', '>=': 'greatereq', '==': 'eqeq', '!=': 'noteq',
           '&&': 'ampamp', '||': 'pipepipe'}

# 注意分支顺序：十六进制在前、指数形式不强制要求小数点
_KIND_RE = re.compile(r"""
      [A-Za-z_][A-Za-z_0-9]*
    | 0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?[0-9]+)?
    | (?:[0-9]+\.[0-9]*|\.[0-9]+|[0-9]+)(?:[eE][+-]?[0-9]+)?
    | <=|>=|==|!=|&&|\|\|
    | [-+*/%<>=!(){}\[\],;]
""", re.X)


def independent_kinds(text):
    """按 SysY 词法规则独立算出 kind 序列（不含 EOF）。text 可为 str 或 bytes。"""
    if isinstance(text, (bytes, bytearray)):
        text = text.decode("utf-8", "replace")
    out = []
    i, n = 0, len(text)
    WS = " \t\r\n\v\f"
    while i < n:
        c = text[i]
        if c in WS:
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
            continue
        m = _KIND_RE.match(text, i)
        if not m:
            out.append("invalid")
            i += 1
            continue
        tok = m.group(0)
        if tok in _DOUBLE:
            out.append(_DOUBLE[tok])
        elif tok in _SIMPLE:
            out.append(_SIMPLE[tok])
        elif tok[:2].lower() == "0x":
            # 十六进制里的 e 不是指数：只有小数点或 p 指数才是浮点
            out.append("floatlit" if ("." in tok or "p" in tok or "P" in tok)
                       else "intlit")
        elif tok[0].isdigit() or tok[0] == ".":
            out.append("floatlit" if ("." in tok or "e" in tok or "E" in tok)
                       else "intlit")
        elif tok in _KW:
            out.append(_KW[tok])
        else:
            out.append("ident")
        i = m.end()
    return out


if __name__ == "__main__":
    sys.exit(main())


# ══════════════════════════════════════════════════════════════════════════════
# 独立实现的 kind 序列 —— 用于堵上"最长匹配"盲点
#
# 为什么必须单独做这件事：
#   拼接不变式对【token 边界错误】是全盲的。把 `<=` 拆成 `<` `=`：
#     · 第 4 列拼接结果不变          → 拼接检查 PASS
#     · 每一半的 loc+length 仍自洽    → 元数据检查 PASS
#   只有比对【kind 序列】才能发现。
#
# 本函数【不读也不调用】C++ 实现，是按 SysY 词法规则另写一遍。
# ══════════════════════════════════════════════════════════════════════════════
