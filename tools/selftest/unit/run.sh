#!/usr/bin/env bash
# ============================================================================
# unit/run.sh —— 支撑层单元测试（P00 验证标准 §6.7 诊断格式 / §6.8 CRLF 一致性）
#
# 用 clang++ --std=c++17（与 CMake 构建同一套标准）直接编译，不需要任何测试框架。
# 通过标准：两个测试程序都退出 0。退出码：0 = 全通过，1 = 有失败。
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

FLAGS=(--std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -I"$SRC")
SUPPORT=("$SRC/support/SourceFile.cpp" "$SRC/support/Diagnostic.cpp")

echo "════════════════════════════════════════════════════════════"
echo " 支撑层单元测试    $("$CXX" --version | head -1)"
echo " 工作目录: $WORK"
echo "════════════════════════════════════════════════════════════"

FAIL=0
for t in test_sourcefile test_diagnostic; do
  echo
  echo "──── $t ────"
  build_cmd=("$CXX" "${FLAGS[@]}" "$HERE/$t.cpp" "${SUPPORT[@]}" -o "$WORK/$t")
  echo "\$ ${build_cmd[*]}"
  if ! "${build_cmd[@]}" 2>"$WORK/$t.build.log"; then
    echo "  ✘ 编译失败:"; sed 's/^/    /' "$WORK/$t.build.log"; FAIL=1; continue
  fi
  echo "  （编译零警告：-Wall -Wextra -Wpedantic -Werror）"
  if "$WORK/$t"; then
    echo "  ✔ $t 通过"
  else
    echo "  ✘ $t 失败（退出码 $?）"; FAIL=1
  fi
done

echo
echo "════════════════════════════════════════════════════════════"
if [ "$FAIL" = 0 ]; then echo " 单元测试结果：✔ 全部通过"; else echo " 单元测试结果：✘ 有失败"; fi
echo "════════════════════════════════════════════════════════════"
[ "$KEEP" = 1 ] && echo "保留工作目录: $WORK"
[ "$KEEP" = 1 ] || rm -rf "$WORK"
exit "$FAIL"
