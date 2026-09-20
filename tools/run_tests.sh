#!/usr/bin/env bash
# ============================================================================
# run_tests.sh —— 全量测试运行器（P00 prompt §4.3）
#
# 用法: run_tests.sh [选项]
#   --compiler <path>     编译器可执行文件（默认 compiler/build/compiler）
#   --manifest <path>     用例清单（默认 tests/manifest.tsv）
#   --case <name>         只跑一个用例（可用逗号分隔；支持 name 或 suite/category/name）
#   --suite <name>        只跑某个 suite（final_arm / prelim_riscv / ...）
#   --category <name>     functional / h_functional / performance
#   --filter <glob>       名字匹配（如 'matmul*'）
#   --target <t>          x86（默认）| aarch64 | riscv64
#   --jobs <n>            并行数（默认 nproc）
#   --timeout <sec>       单用例超时（默认 60；性能用例建议 300）
#   --no-structured       传给编译器一个开关（S06 之后才有效）
#   --toy-backend         走自研降级器而非 clang（S11b 之后才有效）
#   --verbose             打印每个用例的命令
#   --keep-going          兼容用；本运行器【总是】跑完整个选择集（不会早停）
#   --list                只列出会被选中的用例，不执行
#   --json <path>         把每个用例的结果写成 JSON
#   --dry-run             同 --list
#   -h, --help
#
# 退出码：全通过 → 0；有失败/超时 → 1（verify.sh 依赖它）。用法错误 → 2。
#
# ┌──────────────────────────────────────────────────────────────────────────┐
# │ ★ 契约（P00 prompt §4.4，实测得出，不要改动）                              │
# │                                                                          │
# │ 我们的编译器 dump 的 .ll 是【目标无关】的：                                │
# │   · 不带 target triple、不带 target datalayout                            │
# │   · 函数属性里不带 target-cpu / target-features                           │
# │ 目标由【降级阶段】决定（llc -mtriple / 自研后端）。                        │
# │                                                                          │
# │ 理由：同一份 .ll 要跑三种 target 做验证；带 triple 就无法重定向。          │
# │ 这条契约写在 BACKEND-HANDOFF.md 里（S07 之后适用）。                       │
# │                                                                          │
# │ 五个必须记住的细节（实测）：                                               │
# │   1. .ll 必须"干净"（无 triple/features）——否则换 target 时 clang 拒绝翻译 │
# │   2. riscv64 必须 -mattr=+m,+a,+f,+d,+c ——否则链接报 d 扩展缺失            │
# │   3. -w / -lm / -static ——警告 / 链接 / qemu 加载器                        │
# │   4. -mcmodel=medany（riscv64 链接）——比赛要求                             │
# │   5. -fno-addrsig（若用 clang 出汇编）——binutils 2.42 不认 .addrsig         │
# └──────────────────────────────────────────────────────────────────────────┘
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PY="${PYTHON:-python3}"

usage() { sed -n '2,35p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

COMPILER="$ROOT/compiler/build/compiler"
MANIFEST="$ROOT/tests/manifest.tsv"
TARGET="x86"
JOBS="$(nproc 2>/dev/null || echo 4)"
TIMEOUT="60"
CASE=""; SUITE=""; CATEGORY=""; FILTER=""
VERBOSE=0; KEEP_GOING=0; DRY=0; NO_STRUCT=0; TOY=0; OPT=0; JSON=""; WORK=""
COMPILE_TIMEOUT="60"

while [ $# -gt 0 ]; do
  case "$1" in
    --compiler)  COMPILER="$2"; shift 2 ;;
    --manifest)  MANIFEST="$2"; shift 2 ;;
    --case)      CASE="$2"; shift 2 ;;
    --suite)     SUITE="$2"; shift 2 ;;
    --category)  CATEGORY="$2"; shift 2 ;;
    --filter)    FILTER="$2"; shift 2 ;;
    --target)    TARGET="$2"; shift 2 ;;
    --jobs|-j)   JOBS="$2"; shift 2 ;;
    --timeout)   TIMEOUT="$2"; shift 2 ;;
    --compile-timeout) COMPILE_TIMEOUT="$2"; shift 2 ;;
    --work)      WORK="$2"; shift 2 ;;
    --json)      JSON="$2"; shift 2 ;;
    --llvm-bin)  LLVM_BIN_ARG="$2"; shift 2 ;;
    --no-structured) NO_STRUCT=1; shift ;;
    --toy-backend)   TOY=1; shift ;;
    -O1|--O1)        OPT=1; shift ;;
    --verbose|-v)    VERBOSE=1; shift ;;
    --keep-going|-k) KEEP_GOING=1; shift ;;
    --dry-run|--list) DRY=1; shift ;;
    -h|--help)   usage; exit 0 ;;
    *) echo "run_tests.sh: 未知参数 '$1'（--help 看用法）" >&2; exit 2 ;;
  esac
done

case "$TARGET" in
  x86|aarch64|riscv64) ;;
  *) echo "run_tests.sh: 未知 target '$TARGET'（x86 | aarch64 | riscv64）" >&2; exit 2 ;;
esac
case "$TARGET" in
  aarch64) command -v qemu-aarch64 >/dev/null || { echo "run_tests.sh: 缺少 qemu-aarch64" >&2; exit 2; } ;;
  riscv64) command -v qemu-riscv64 >/dev/null || { echo "run_tests.sh: 缺少 qemu-riscv64" >&2; exit 2; } ;;
esac

[ -f "$MANIFEST" ] || {
  echo "run_tests.sh: 找不到清单 $MANIFEST" >&2
  echo "  先生成: python3 compiler/tools/selftest/gen_manifest.py" >&2
  exit 2
}

ARGS=( "$HERE/_run_tests.py"
       --compiler "$COMPILER" --manifest "$MANIFEST" --root "$ROOT"
       --target "$TARGET" --jobs "$JOBS" --timeout "$TIMEOUT"
       --compile-timeout "$COMPILE_TIMEOUT" )
[ -n "$CASE" ]     && ARGS+=( --case "$CASE" )
[ -n "$SUITE" ]    && ARGS+=( --suite "$SUITE" )
[ -n "$CATEGORY" ] && ARGS+=( --category "$CATEGORY" )
[ -n "$FILTER" ]   && ARGS+=( --filter "$FILTER" )
[ -n "$JSON" ]     && ARGS+=( --json "$JSON" )
[ -n "$WORK" ]     && ARGS+=( --work "$WORK" )
[ -n "${LLVM_BIN_ARG:-}" ] && ARGS+=( --llvm-bin "$LLVM_BIN_ARG" )
[ "$VERBOSE" = 1 ]    && ARGS+=( --verbose )
[ "$KEEP_GOING" = 1 ] && ARGS+=( --keep-going )
[ "$DRY" = 1 ]        && ARGS+=( --dry-run )
[ "$NO_STRUCT" = 1 ]  && ARGS+=( --no-structured )
[ "$TOY" = 1 ]        && ARGS+=( --toy-backend )
[ "$OPT" = 1 ]        && ARGS+=( --optimize )
# 注：--no-structured 与 --toy-backend 目前只是【透传给编译器】的开关，
#     S06 / S11b 之前编译器会接受但忽略它们（见 P00 prompt §4.3）。

exec "$PY" "${ARGS[@]}"
