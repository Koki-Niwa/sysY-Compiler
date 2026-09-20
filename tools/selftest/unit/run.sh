#!/usr/bin/env bash
# ============================================================================
# unit/run.sh —— 支撑层 + 前端单元测试
#   test_sourcefile  P00 验证标准 §6.8（CRLF 行列号一致性）
#   test_diagnostic  P00 验证标准 §6.7（诊断输出格式）
#   test_lexer       S01 验证标准 §7.9（13 个边界用例 + 转储格式 + 拼接不变式）
#   test_parser      S02 §10 第 4/6/9 项（34 条树形断言 + §5 例子逐字节 + 往返 + 恢复）
#   test_sema        S03 §10 第 ⑨ 项（转换形状、遮蔽、★诊断条数探针、深树不崩）
#   test_initlowering S04 §10 第 ⑨ 项（"当前对象"试金石、规模锚点、病态输入）
# 用 clang++ --std=c++17 直接编译，无第三方框架；每个程序退出 0 即通过。
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

# -Werror：零警告是硬门禁（clang++ 对 mingw 目标的告警与本项目无关，已避开）。
FLAGS=(--std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -I"$SRC")
SUPPORT=("$SRC/support/SourceFile.cpp" "$SRC/support/Diagnostic.cpp")
FRONTEND=("$SRC/frontend/Lexer.cpp" "$SRC/frontend/Parser.cpp"
          "$SRC/frontend/ParserStmtExpr.cpp" "$SRC/frontend/AstPrinter.cpp"
          "$SRC/frontend/AstReader.cpp"
          "$SRC/frontend/ConstEval.cpp" "$SRC/frontend/RuntimeLib.cpp"
          "$SRC/frontend/Sema.cpp" "$SRC/frontend/SemaStmt.cpp"
          "$SRC/frontend/SemaExpr.cpp" "$SRC/frontend/SemaDecl.cpp"
          "$SRC/frontend/SemaDump.cpp"
          "$SRC/frontend/InitLowering.cpp" "$SRC/frontend/InitLoweringDump.cpp")
# ⚠️ 上面必须与 CMakeLists.txt 的源文件列表一致，少一个就是 undefined reference。

# 每个测试：名字 | 需要的源文件（--bench 条目只构建，不参与"通过"判定）
TESTS=(
  "test_sourcefile|${SUPPORT[*]}"
  "test_diagnostic|${SUPPORT[*]}"
  "test_lexer|${SUPPORT[*]} ${FRONTEND[*]}"
  "test_parser|${SUPPORT[*]} ${FRONTEND[*]}"
  "test_sema|${SUPPORT[*]} ${FRONTEND[*]}"
  "test_initlowering|${SUPPORT[*]} ${FRONTEND[*]}")
BENCHES=(
  "bench_lexer|${SUPPORT[*]} ${FRONTEND[*]}"
)

echo "════════════════════════════════════════════════════════════"
echo " 单元测试    $("$CXX" --version | head -1)"; echo " 工作目录: $WORK"
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
  # test_parser 需要真实编译器二进制做 CLI 往返冒烟（可选参数：二进制 + 工作目录）
  if [ "$t" = "test_parser" ]; then
    "$WORK/$t" "$ROOT/compiler/build/compiler" "$WORK"
  else
    "$WORK/$t"
  fi
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
