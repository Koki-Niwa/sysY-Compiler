#!/usr/bin/env bash
# ============================================================================
# unit/run.sh —— 支撑层 + 前端单元测试
#   test_sourcefile  P00 验证标准 §6.8（CRLF 行列号一致性）
#   test_diagnostic  P00 验证标准 §6.7（诊断输出格式）
#   test_lexer       S01 验证标准 §7.9（13 个边界用例 + 转储格式 + 拼接不变式）
#
# 用 clang++ --std=c++17（与 CMake 构建同一套标准）直接编译，不需要任何测试框架。
# 通过标准：每个测试程序都退出 0。退出码：0 = 全通过，1 = 有失败。
#
# 用法: bash compiler/tools/selftest/unit/run.sh [--keep]
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
SRC="$ROOT/compiler/src"
CXX="${CXX:-clang++}"
WORK="${WORK:-$(mktemp -d /tmp/sysy_unit_XXXXXX)}"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1
mkdir -p "$WORK"

# clang++ 会因 "unused command line argument" 对 mingw 目标告警，与本项目无关；
# 这里只加项目自己的严格开关（-Werror 保证零警告是硬门禁）。
FLAGS=(--std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -I"$SRC")
SUPPORT=("$SRC/support/SourceFile.cpp" "$SRC/support/Diagnostic.cpp")
FRONTEND=("$SRC/frontend/Lexer.cpp")

# 每个测试：名字 | 需要的源文件（--bench 条目只构建，不参与"通过"判定）
TESTS=(
  "test_sourcefile|${SUPPORT[*]}"
  "test_diagnostic|${SUPPORT[*]}"
  "test_lexer|${SUPPORT[*]} ${FRONTEND[*]}"
)
BENCHES=(
  "bench_lexer|${SUPPORT[*]} ${FRONTEND[*]}"
)

echo "════════════════════════════════════════════════════════════"
echo " 单元测试    $("$CXX" --version | head -1)"
echo " 工作目录: $WORK"
echo "════════════════════════════════════════════════════════════"

build_one() {   # $1 = 目标名, $2 = 依赖源文件
  local t="$1" extra="$2"
  # shellcheck disable=SC2206  # 这里就是要按空格拆开源文件列表
  local deps=($extra)
  local cmd=("$CXX" "${FLAGS[@]}" "$HERE/$t.cpp" "${deps[@]}" -o "$WORK/$t")
  echo "\$ ${cmd[*]}"
  if ! "${cmd[@]}" 2>"$WORK/$t.build.log"; then
    echo "  ✘ 编译失败:"; sed 's/^/    /' "$WORK/$t.build.log"; return 1
  fi
  echo "  （编译零警告：-Wall -Wextra -Wpedantic -Werror）"
  return 0
}

FAIL=0
for entry in "${TESTS[@]}"; do
  t="${entry%%|*}"; extra="${entry#*|}"
  echo
  echo "──── $t ────"
  build_one "$t" "$extra" || { FAIL=1; continue; }
  "$WORK/$t"
  rc=$?
  if [ "$rc" = 0 ]; then
    echo "  ✔ $t 通过"
  else
    echo "  ✘ $t 失败（退出码 $rc）"; FAIL=1
  fi
done

for entry in "${BENCHES[@]}"; do
  t="${entry%%|*}"; extra="${entry#*|}"
  echo
  echo "──── $t（仅构建；用法见文件头，验证标准 §7.5 由它的输出佐证）────"
  build_one "$t" "$extra" || FAIL=1
done

echo
echo "════════════════════════════════════════════════════════════"
if [ "$FAIL" = 0 ]; then echo " 单元测试结果：✔ 全部通过"; else echo " 单元测试结果：✘ 有失败"; fi
echo "════════════════════════════════════════════════════════════"
[ "$KEEP" = 1 ] && echo "保留工作目录: $WORK"
[ "$KEEP" = 1 ] || rm -rf "$WORK"
exit "$FAIL"
