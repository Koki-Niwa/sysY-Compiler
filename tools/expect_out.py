#!/usr/bin/env python3
"""expect_out.py —— .out 比对（P00 prompt §4.5）

用法:
    expect_out.py <stdout文件> <退出码> [候选序号0-3]   → 打到 stdout

.out 的真实格式 = 程序 stdout + main 返回值的文本表示。实测存在细微不一致
（stdout 是否补换行、退出码后是否有换行），所以判定必须【多候选匹配】：

    C0  stdout + sep + code + "\\n"    ← 主规则：stdout 与退出码各占一行
    C1  stdout + code + "\\n"
    C2  stdout + code
    C3  stdout + sep + code

其中 sep 在 stdout 已以换行结尾时为 b''，否则为 b'\\n'。

调用方（run_tests.sh / _run_tests.py）应当按 C0–C3 逐个试，任一命中即通过；
全部不中才判 WRONG_OUTPUT。本脚本用退出码表达结果：

    0 = 候选内容与 <stdout文件>+<退出码> 自洽（永远成立，用于生成候选）
    2 = 用法错误

为了让 shell 侧也能直接用，`--match <期望文件>` 模式会实际做比对：
    0 = 匹配（stdout 打印命中的候选序号 0-3）
    1 = 不匹配
"""

import sys


def candidate_expectations(stdout: bytes, code: int):
    """返回 4 个候选的完整期望输出（bytes），顺序即 C0–C3。"""
    c = str(int(code)).encode()
    sep = b"" if stdout.endswith(b"\n") else b"\n"
    return [
        stdout + sep + c + b"\n",   # C0 主规则：stdout 与退出码各占一行
        stdout + c + b"\n",         # C1
        stdout + c,                 # C2
        stdout + sep + c,           # C3
    ]


CANDIDATE_NAMES = ["C0", "C1", "C2", "C3"]


def match(actual: bytes, stdout: bytes, code: int):
    """返回命中的候选序号（0-3）；全部不中返回 -1。"""
    for idx, cand in enumerate(candidate_expectations(stdout, code)):
        if actual == cand:
            return idx
    return -1


def _usage():
    sys.stderr.write(__doc__ + "\n")
    return 2


def main(argv):
    args = [a for a in argv[1:]]
    match_file = None
    if "--match" in args:
        i = args.index("--match")
        if i + 1 >= len(args):
            return _usage()
        match_file = args[i + 1]
        del args[i:i + 2]

    if len(args) < 2:
        return _usage()
    stdout_path, code_str = args[0], args[1]
    idx = int(args[2]) if len(args) > 2 else 0
    if idx < 0 or idx > 3:
        sys.stderr.write("候选序号必须在 0-3 之间\n")
        return 2

    try:
        with open(stdout_path, "rb") as f:
            stdout = f.read()
    except OSError as e:
        sys.stderr.write("cannot read %s: %s\n" % (stdout_path, e))
        return 2

    try:
        code = int(code_str)
    except ValueError:
        sys.stderr.write("退出码必须是整数，得到 %r\n" % code_str)
        return 2

    if match_file is not None:
        try:
            with open(match_file, "rb") as f:
                actual = f.read()
        except OSError as e:
            sys.stderr.write("cannot read %s: %s\n" % (match_file, e))
            return 2
        hit = match(actual, stdout, code)
        if hit < 0:
            return 1
        sys.stdout.write("%d\n" % hit)
        return 0

    sys.stdout.buffer.write(candidate_expectations(stdout, code)[idx])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
