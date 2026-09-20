#!/usr/bin/env python3
"""_run_tests.py —— run_tests.sh 的实现体（P00 prompt §4.3/§4.4）

每个用例的流程（与 prompt 一字不差地对应）：
    ① 编译：  $COMPILER <case>.sy --emit=llvm-ir -o $WORK/case.ll
    ② 降级：  清 IR（去 triple/datalayout/target-features）→ llvm-as → llc -mtriple=<target>
    ③ 链接：  <target 的链接命令> case.s runtime/sylib.c -o prog
    ④ 运行：  ./prog < case.in > got.bin ; code=$?
    ⑤ 比对：  got.bin + code 按 C0–C3 多候选匹配 case.out

失败类型：
    COMPILE_FAIL  我们的编译器失败（或没产出 .ll）
    BUILD_FAIL    清 IR / llvm-as / llc / 链接失败 → 说明我们 dump 的 IR 有问题
    WRONG_OUTPUT  输出与 .out 不一致
    TIMEOUT       超时
    SKIP          主动跳过（tensor 用例等）

用法见 run_tests.sh（不要直接调用本脚本，除了 selftest 场景）。
"""

import argparse
import concurrent.futures
import fnmatch
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from expect_out import CANDIDATE_NAMES, candidate_expectations, match  # noqa: E402

# 失败类型（run_tests.sh 汇总行会打印这些字面量）
def _has_definition(path):
    """.ll 里是否有函数定义（'define ' 开头的非注释行）。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("define "):
                    return True
    except OSError:
        return False
    return False


COMPILE_FAIL = "COMPILE_FAIL"
BUILD_FAIL = "BUILD_FAIL"
WRONG_OUTPUT = "WRONG_OUTPUT"
TIMEOUT = "TIMEOUT"
SKIP = "SKIP"
PASS = "PASS"
FORMAT_WARN = "PASS(FORMAT_C1)"   # 命中 C1–C3：算通过，但要标注"格式差异"
FORMAT_NOTE = "PASS(FORMAT_C2/C3)"  # 命中 C2/C3：无末尾换行等格式差异

STATUS_ZH = {
    PASS: "通过",
    COMPILE_FAIL: "COMPILE_FAIL",
    BUILD_FAIL: "BUILD_FAIL",
    WRONG_OUTPUT: "WRONG_OUTPUT",
    TIMEOUT: "TIMEOUT",
    SKIP: "SKIP",
}

# ---- 三种 target 的降级/链接/运行命令（P00 prompt §4.4，照抄） ----
# ⚠️ 契约：我们的 .ll 是【目标无关】的（无 triple/datalayout/target-features），
#    目标由降级阶段用 llc -mtriple 决定。同一份 .ll 要跑三种 target 做验证。
TARGETS = {
    "x86": {
        "llc_triple": "x86_64-linux-gnu",
        "llc_extra": [],
        "link_cc": "clang",
        "link_extra": [],
        "runner": [],
        "static": False,
    },
    "aarch64": {
        "llc_triple": "aarch64-linux-gnu",
        "llc_extra": [],
        "link_cc": "aarch64-linux-gnu-gcc",
        "link_extra": [],
        "runner": ["qemu-aarch64"],
        "static": True,
    },
    "riscv64": {
        "llc_triple": "riscv64-linux-gnu",
        # riscv64 必须显式给扩展，否则链接报
        # "ilp32d/lp64d ABI can't be used when d extension isn't supported"
        "llc_extra": ["-mattr=+m,+a,+f,+d,+c"],
        "link_cc": "riscv64-linux-gnu-gcc",
        "link_extra": ["-march=rv64gc", "-mcmodel=medany"],
        "runner": ["qemu-riscv64"],
        "static": True,
    },
}

STOP = threading.Event()


# ─────────────────────────────────────────────────────────────────────────────
# 清单
# ─────────────────────────────────────────────────────────────────────────────
class Case:
    __slots__ = ("suite", "category", "name", "sy", "stdin", "expected", "skip", "key", "line_no")

    def __init__(self, suite, category, name, sy, stdin, expected, skip, line_no=0):
        self.suite = suite
        self.category = category
        self.name = name
        self.sy = sy
        self.stdin = stdin
        self.expected = expected
        self.skip = skip
        self.key = "%s/%s/%s" % (suite, category, name)
        self.line_no = line_no


def load_manifest(path, root):
    """读 manifest.tsv；返回 (cases, errors)。行格式见 gen_manifest.py。"""
    cases, errors = [], []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 7:
                errors.append("%s:%d: 期望 7 列，实际 %d 列" % (path, line_no, len(parts)))
                continue
            suite, category, name, sy, stdin, expected, skip = parts
            # manifest 里 "-" 表示"无/不适用"，必须规范化掉：
            # 否则 evaluate() 会把字面量 '-' 当成真值 → 全部误判成 SKIP
            if skip == "-":
                skip = ""
            sy_abs = sy if os.path.isabs(sy) else os.path.join(root, sy)
            if not os.path.isfile(sy_abs):
                errors.append("%s:%d: 源文件不存在: %s" % (path, line_no, sy))
                continue
            if expected != "-" and not os.path.isfile(
                    expected if os.path.isabs(expected) else os.path.join(root, expected)):
                errors.append("%s:%d: 期望输出不存在: %s" % (path, line_no, expected))
                continue
            cases.append(Case(suite, category, name, sy, stdin, expected, skip, line_no))
    return cases, errors


def select(cases, args):
    out = []
    wanted_cases = set()
    if args.case:
        for chunk in args.case.split(","):
            if chunk.strip():
                wanted_cases.add(chunk.strip())
    for c in cases:
        if wanted_cases and not (c.name in wanted_cases or c.key in wanted_cases):
            continue
        if args.suite and c.suite != args.suite:
            continue
        if args.category and c.category != args.category:
            continue
        if args.filter and not (fnmatch.fnmatch(c.name, args.filter)
                                or fnmatch.fnmatch(c.sy, args.filter)):
            continue
        out.append(c)
    return out


# ─────────────────────────────────────────────────────────────────────────────
# 步骤实现
# ─────────────────────────────────────────────────────────────────────────────
def run_cmd(cmd, cwd, stdin_path=None, timeout=60, env=None):
    """执行命令。返回 (returncode, stdout_bytes, stderr_text, timed_out)。
    returncode 为 None 表示启动失败。"""
    fin = None
    try:
        if stdin_path:
            fin = open(stdin_path, "rb")
        else:
            fin = open(os.devnull, "rb")
        p = subprocess.run(cmd, cwd=cwd, stdin=fin,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout, env=env)
        return (p.returncode, p.stdout,
                p.stderr.decode("utf-8", "replace")[-4000:], False)
    except subprocess.TimeoutExpired:
        return (None, b"", "", True)
    except OSError as e:
        return (None, b"", "cannot execute %r: %s" % (cmd[0], e), False)
    finally:
        if fin is not None:
            fin.close()


def clean_ir(raw_text):
    """把 clang 产的 .ll 变成【目标无关】的干净 IR（selftest 需要；S07 之后
    我们的编译器本来就不发射这些东西，此处是幂等的）。"""
    out = []
    for line in raw_text.splitlines():
        if line.startswith("target triple") or line.startswith("target datalayout"):
            continue
        for attr in (' "target-cpu"="', ' "target-features"="', ' "tune-cpu"="'):
            while attr in line:
                i = line.index(attr)
                j = line.index('"', i + len(attr))
                line = line[:i] + line[j + 1:]
        out.append(line)
    return "\n".join(out) + "\n"


def tail(text, n=6, width=200):
    lines = [l[:width] for l in (text or "").splitlines() if l.strip()]
    return "\n".join(lines[-n:])


def evaluate(case, args, workdir, llvm_bin):
    """跑完一个用例，返回结果字典。"""
    res = {
        "key": case.key, "suite": case.suite, "category": case.category,
        "name": case.name, "status": PASS, "failure": "", "detail": "",
        "seconds": 0.0, "candidate": "", "skip_reason": case.skip or "",
    }
    t0 = time.time()
    try:
        if case.skip:
            res["status"] = SKIP
            return res

        sy = case.sy if os.path.isabs(case.sy) else os.path.join(args.root, case.sy)
        exp = case.expected
        if exp != "-" and not os.path.isabs(exp):
            exp = os.path.join(args.root, exp)
        stdin_path = None
        if case.stdin != "-":
            stdin_path = case.stdin if os.path.isabs(case.stdin) \
                else os.path.join(args.root, case.stdin)
            if not os.path.isfile(stdin_path):
                # 只有 manifest 的 skip 列【显式声明】的才 SKIP；
                # 声明了却在磁盘上找不到 → 算失败，避免"490/490"静默缩水。
                res["status"] = COMPILE_FAIL
                res["detail"] = ("manifest 声明了输入文件但磁盘上不存在: %s "
                                 "（若确应跳过，请在 manifest 的 skip 列声明）" % case.stdin)
                return res

        # ① 编译 → .ll
        if not args.no_compile_step:
            cmd = [args.compiler, sy, "--emit=llvm-ir", "-o", "case.ll"]
            if args.optimize:
                cmd.append("-O1")
            if args.no_structured:
                cmd.append("--no-structured")
            if args.toy_backend:
                cmd.append("--toy-backend")
            rc, _, err, to = run_cmd(cmd, workdir, timeout=args.compile_timeout)
            if to:
                res["status"] = TIMEOUT
                res["detail"] = "编译超时: %s" % " ".join(cmd)
                return res
            if rc != 0:
                res["status"] = COMPILE_FAIL
                res["detail"] = tail(err) or ("退出码 %s" % rc)
                return res
            if not os.path.isfile(os.path.join(workdir, "case.ll")):
                res["status"] = COMPILE_FAIL
                res["detail"] = "编译器退出码 0 但没有产出 case.ll"
                return res
            # ★ 守卫：产出的必须是【真实模块】而不是空壳。
            #   没有 `define` 就说明 IRGen 还没实现（或静默产出了空模块）——
            #   否则会一路走到链接才报 "undefined reference to 'main'"，被误分类成
            #   BUILD_FAIL（"我们 dump 的 IR 有问题"），把矛头指错方向。
            #   见 S00 审查报告 F2。
            if not _has_definition(os.path.join(workdir, "case.ll")):
                res["status"] = COMPILE_FAIL
                res["detail"] = ("产出的是空模块（无 'define'）—— IRGen 尚未实现或静默失败；"
                                 "这不是 IR 缺陷，是编译器还没产出函数")
                return res

        # ② 清 IR + llvm-as 校验 + llc 降级
        #    --no-compile-step（selftest）时，manifest 的 <sy路径> 列就是现成的 .ll
        ll_in = os.path.join(workdir, "case.ll")
        if args.no_compile_step and not os.path.isfile(ll_in):
            ll_in = sy
        if not os.path.isfile(ll_in):
            res["status"] = BUILD_FAIL
            res["detail"] = "找不到待降级的 .ll: %s" % ll_in
            return res
        try:
            with open(ll_in, "r", encoding="utf-8", errors="replace") as f:
                raw = f.read()
        except OSError as e:
            res["status"] = BUILD_FAIL
            res["detail"] = "无法读取 .ll: %s" % e
            return res
        with open(os.path.join(workdir, "clean.ll"), "w", encoding="utf-8") as f:
            f.write(clean_ir(raw))
        rc, _, err, to = run_cmd([os.path.join(llvm_bin, "llvm-as"), "clean.ll",
                                  "-o", "clean.bc"], workdir, timeout=args.timeout)
        if rc != 0:
            res["status"] = BUILD_FAIL
            res["detail"] = ("llvm-as 拒绝我们 dump 的 IR（IR 非法）:\n"
                             + (tail(err) or "退出码 %s" % rc))
            return res

        tgt = TARGETS[args.target]
        llc_cmd = [os.path.join(llvm_bin, "llc"), "-mtriple=" + tgt["llc_triple"]] \
            + tgt["llc_extra"] + ["-filetype=asm", "clean.bc", "-o", "case.s"]
        rc, _, err, to = run_cmd(llc_cmd, workdir, timeout=args.timeout)
        if rc != 0:
            res["status"] = BUILD_FAIL
            res["detail"] = "llc 降级失败:\n" + (tail(err) or "退出码 %s" % rc)
            return res

        # ③ 链接
        link = [tgt["link_cc"]]
        if tgt["static"]:
            link.append("-static")
        link += ["case.s", os.path.join(args.root, "runtime", "sylib.c"),
                 "-o", "prog", "-w", "-lm"] + tgt["link_extra"]
        rc, _, err, to = run_cmd(link, workdir, timeout=args.timeout)
        if rc != 0:
            res["status"] = BUILD_FAIL
            res["detail"] = "链接失败:\n" + (tail(err) or "退出码 %s" % rc)
            return res

        # ④ 运行（超时 = 单用例预算；性能用例由调用方 --timeout 放宽）
        run = tgt["runner"] + ["./prog"]
        rc, out, err, to = run_cmd(run, workdir, stdin_path=stdin_path,
                                   timeout=args.timeout)
        if to:
            res["status"] = TIMEOUT
            res["detail"] = "运行超过 %ss" % args.timeout
            return res
        if rc is None:
            res["status"] = BUILD_FAIL
            res["detail"] = "无法运行产物: " + tail(err)
            return res
        if rc < 0:
            res["status"] = WRONG_OUTPUT
            res["detail"] = "程序被信号杀死 (signal %d)" % (-rc)
            return res

        got_path = os.path.join(workdir, "got.bin")
        with open(got_path, "wb") as f:
            f.write(out)

        # ⑤ 比对：C0 优先；C1–C3 命中算通过但标注格式差异
        if exp == "-":
            res["status"] = SKIP
            res["skip_reason"] = "用例没有 .out（无法判定）"
            return res
        want = open(exp, "rb").read()
        hit = match(want, out, rc)
        if hit < 0:
            res["status"] = WRONG_OUTPUT
            cands = candidate_expectations(out, rc)
            res["detail"] = ("期望 %d 字节 / 实际候选 C0 %d 字节（退出码 %s）\n"
                             "  首处差异: %s\n  期望前 %d 字节: %r\n  实际前 %d 字节: %r"
                             % (len(want), len(cands[0]), rc,
                                _first_diff(want, cands[0]),
                                min(80, len(want)), want[:80],
                                min(80, len(cands[0])), cands[0][:80]))
            return res
        if hit != 0:
            res["status"] = FORMAT_WARN if hit == 1 else FORMAT_NOTE
            res["candidate"] = CANDIDATE_NAMES[hit]
            res["detail"] = ("按 %s 命中（C0 不中 → .out 格式差异，非语义错误）"
                             % CANDIDATE_NAMES[hit])
        return res
    finally:
        res["seconds"] = time.time() - t0
        if args.clean_work and not res["detail"]:
            shutil.rmtree(workdir, ignore_errors=True)


def _first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return "offset %d: 期望 %r 实际 %r" % (i, a[i:i + 1], b[i:i + 1])
    if len(a) != len(b):
        return "长度不同（期望 %d，实际 %d）" % (len(a), len(b))
    return "无（内容相同）"


# ─────────────────────────────────────────────────────────────────────────────
# 主流程
# ─────────────────────────────────────────────────────────────────────────────
def build_argparser():
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--compiler", default=os.path.join(HERE, "..", "build", "compiler"))
    p.add_argument("--manifest", default=None)
    # 默认工作区根 = 本脚本上溯三层（tools/ → compiler/ → 工作区）。
    # 可用 --root 或 SYSY_ROOT 覆盖，便于在其他机器上跑。
    _default_root = os.environ.get(
        "SYSY_ROOT",
        os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")))
    p.add_argument("--root", default=_default_root)
    p.add_argument("--work", default=None, help="工作目录（默认临时目录）")
    p.add_argument("--case", default=None)
    p.add_argument("--suite", default=None)
    p.add_argument("--category", default=None)
    p.add_argument("--filter", default=None)
    p.add_argument("--target", default="x86", choices=sorted(TARGETS))
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--compile-timeout", type=float, default=60.0)
    p.add_argument("--optimize", action="store_true", help="-O1")
    p.add_argument("--no-structured", action="store_true")
    p.add_argument("--toy-backend", action="store_true")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--keep-going", action="store_true")
    p.add_argument("--json", default=None)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--clean-work", action="store_true")
    p.add_argument("--no-compile-step", action="store_true",
                   help="selftest 用：跳过 ①，直接吃现成的 case.ll")
    p.add_argument("--llvm-bin", default=None)
    return p


def find_llvm_bin(explicit):
    if explicit:
        return explicit
    for cand in ("/usr/lib/llvm-18/bin", "/usr/lib/llvm-17/bin", "/usr/lib/llvm-16/bin"):
        if os.path.isfile(os.path.join(cand, "llc")):
            return cand
    llc = shutil.which("llc")
    if llc:
        return os.path.dirname(llc)
    return ""


def main(argv=None):
    args = build_argparser().parse_args(argv)
    args.root = os.path.abspath(args.root)
    args.compiler = os.path.abspath(args.compiler)
    if args.manifest is None:
        args.manifest = os.path.join(args.root, "tests", "manifest.tsv")
    args.llvm_bin = find_llvm_bin(args.llvm_bin)

    work_root = args.work or tempfile.mkdtemp(prefix="sysy_tests_")
    os.makedirs(work_root, exist_ok=True)

    if not args.no_compile_step:
        if not os.path.isfile(args.compiler) or not os.access(args.compiler, os.X_OK):
            sys.stderr.write("[ERROR] 编译器不存在或不可执行: %s\n"
                             "        先构建: cmake -S compiler -B compiler/build -G Ninja "
                             "&& cmake --build compiler/build\n" % args.compiler)
            return 2
    if not args.llvm_bin:
        sys.stderr.write("[ERROR] 找不到 llc/llvm-as（--llvm-bin 指定或用 apt 安装 llvm-18）\n")
        return 2

    cases, errors = load_manifest(args.manifest, args.root)
    if errors:
        sys.stderr.write("[ERROR] manifest 有问题（%d 条），前 5 条:\n" % len(errors))
        for e in errors[:5]:
            sys.stderr.write("  " + e + "\n")
        return 2
    picked = select(cases, args)
    if not picked:
        sys.stderr.write("[ERROR] 没有选中任何用例（manifest=%s）\n" % args.manifest)
        return 2

    if args.dry_run:
        for c in picked:
            sys.stdout.write("%s\t%s\t%s\t%s\n"
                             % (c.key, os.path.join(args.root, c.sy),
                                ("-" if c.stdin == "-" else os.path.join(args.root, c.stdin)),
                                c.skip or "-"))
        return 0

    if args.verbose:
        sys.stderr.write("[info] manifest=%s 用例=%d target=%s jobs=%d timeout=%ss\n"
                         % (args.manifest, len(picked), args.target, args.jobs, args.timeout))
        sys.stderr.write("[info] llvm-bin=%s work=%s\n" % (args.llvm_bin, work_root))

    results = []
    lock = threading.Lock()

    def worker(idx_case):
        idx, case = idx_case
        if STOP.is_set() and not args.keep_going:
            return None
        wd = os.path.join(work_root, "%04d" % idx)
        os.makedirs(wd, exist_ok=True)
        try:
            r = evaluate(case, args, wd, args.llvm_bin)
        except Exception as e:            # 测试框架自身出错也不许拖垮整轮回归
            import traceback
            r = {"key": case.key, "suite": case.suite, "category": case.category,
                 "name": case.name, "status": BUILD_FAIL, "failure": "HARNESS_ERROR",
                 "detail": "测试框架异常: %s\n%s" % (e, traceback.format_exc()[-800:]),
                 "seconds": 0.0, "candidate": "", "skip_reason": ""}
        with lock:
            results.append(r)
            if args.verbose:
                sys.stderr.write("  [%-14s] %s (%.2fs)%s\n"
                                 % (r["status"], r["key"], r["seconds"],
                                    ("  " + r["detail"].splitlines()[0]) if r["detail"] else ""))
        return r

    t0 = time.time()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
            list(ex.map(worker, list(enumerate(picked))))
    except KeyboardInterrupt:
        STOP.set()
        sys.stderr.write("\n[中断] 已停止调度新用例（已完成 %d 个）\n" % len(results))
        _emit(results, args, time.time() - t0, work_root)
        return 130

    return _emit(results, args, time.time() - t0, work_root)


def _emit(results, args, elapsed, work_root):
    order = {PASS: 0, FORMAT_WARN: 1, FORMAT_NOTE: 1, COMPILE_FAIL: 2,
             BUILD_FAIL: 3, WRONG_OUTPUT: 4, TIMEOUT: 5, SKIP: 6}
    results.sort(key=lambda r: (r["suite"], r["category"], r["name"]))

    groups = {}
    for r in results:
        g = groups.setdefault("%s/%s" % (r["suite"], r["category"]),
                              {"total": 0, "pass": 0, "fail": 0, "timeout": 0, "skip": 0})
        g["total"] += 1
        st = r["status"]
        if st == SKIP:
            g["skip"] += 1
        elif st == TIMEOUT:
            g["timeout"] += 1
        elif st in (PASS, FORMAT_WARN, FORMAT_NOTE):
            g["pass"] += 1
        else:
            g["fail"] += 1

    n_pass = sum(1 for r in results if r["status"] in (PASS, FORMAT_WARN, FORMAT_NOTE))
    n_fail = sum(1 for r in results if r["status"] in
                 (COMPILE_FAIL, BUILD_FAIL, WRONG_OUTPUT))
    n_timeout = sum(1 for r in results if r["status"] == TIMEOUT)
    n_skip = sum(1 for r in results if r["status"] == SKIP)
    n_fmt = sum(1 for r in results if r["status"] in (FORMAT_WARN, FORMAT_NOTE))

    out = sys.stdout
    out.write("\n=== 汇总 ===\n")
    out.write("%-28s %6s %6s %6s %6s %6s\n"
              % ("suite/category", "总数", "通过", "失败", "超时", "跳过"))
    for key in sorted(groups):
        g = groups[key]
        out.write("%-28s %6d %6d %6d %6d %6d\n"
                  % (key, g["total"], g["pass"], g["fail"], g["timeout"], g["skip"]))
    out.write("-" * 70 + "\n")
    total_all = len(results)
    tail_note = ("（跳过 %d 个 tensor/无 .out 用例）" % n_skip) if n_skip else ""
    out.write("总计: %d / %d 通过    (失败 %d，超时 %d，跳过 %d，耗时 %.1fs) %s\n"
              % (n_pass, total_all - n_skip, n_fail, n_timeout, n_skip, elapsed, tail_note))
    out.write("target=%s  工作目录=%s\n" % (args.target, work_root))
    if n_fmt:
        out.write("注: %d 个用例按 C1–C3 命中（.out 格式差异，非语义错误）: %s\n"
                  % (n_fmt, ", ".join(r["key"] for r in results
                                      if r["status"] in (FORMAT_WARN, FORMAT_NOTE))[:400]))

    bad = [r for r in results if r["status"] in
           (COMPILE_FAIL, BUILD_FAIL, WRONG_OUTPUT, TIMEOUT)]
    if bad:
        out.write("\n失败的用例:\n")
        for r in sorted(bad, key=lambda r: (order.get(r["status"], 9), r["key"])):
            line = "  %s  [%s]" % (r["key"], r["status"])
            if r["detail"]:
                first = r["detail"].splitlines()[0]
                line += "  " + first
            out.write(line + "\n")
            for extra in r["detail"].splitlines()[1:10]:
                out.write("      " + extra + "\n")
    else:
        out.write("\n失败的用例: 无\n")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"target": args.target, "elapsed": elapsed,
                       "summary": {"total": total_all, "pass": n_pass, "fail": n_fail,
                                   "timeout": n_timeout, "skip": n_skip,
                                   "format_diff": n_fmt},
                       "results": results}, f, ensure_ascii=False, indent=1)
        out.write("\nJSON 明细: %s\n" % args.json)

    return 0 if (n_fail == 0 and n_timeout == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
