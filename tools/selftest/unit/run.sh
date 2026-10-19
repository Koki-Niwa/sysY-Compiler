#!/usr/bin/env bash
# unit/run.sh —— 单元测试入口（F05：不再手抄 SOURCES）
#
# 源码清单只在 CMakeLists.txt 维护一次；这里只负责"构建 + 运行"。
# 每个测试是链接组件 STATIC 库的 CMake 目标，注册在 ctest 中。
# 工作目录默认 mktemp；--keep 保留并打印路径。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
BUILD="${BUILD:-$ROOT/compiler/build}"
CXX="${CXX:-clang++}"
WORK="${WORK:-$(mktemp -d /tmp/sysy_unit_XXXXXX)}"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1
mkdir -p "$WORK"

echo "════════════════════════════════════════════════════════════"
echo " 单元测试    $("$CXX" --version 2>/dev/null | head -1 || echo 'CXX unavailable')"
echo " 构建目录: $BUILD"
echo " 工作目录: $WORK"
echo "════════════════════════════════════════════════════════════"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -S "$ROOT/compiler" -B "$BUILD" -G Ninja -DCMAKE_CXX_COMPILER="$CXX" || {
    echo "  ✘ CMake 配置失败"; exit 1; }
fi

if ! cmake --build "$BUILD" --target compiler test_runtime_catalog test_sourcefile test_diagnostic \
     test_lexer test_parser test_sema test_initlowering test_const_remainder test_flat_reader \
     test_structured test_loopnorm test_flat test_ir_mutation test_value_use \
     test_flat_f2i test_flat_dead_phi test_llvm_emitter test_analysis_manager test_interprocedural test_alias test_cfg_analysis test_hotness test_dependence test_foundation_audit \
     test_pass_manager test_mem2reg test_mem2reg_pass test_inline_v0 test_s17 test_rewrite bench_lexer \
     >"$WORK/build.log" 2>&1; then
  echo "  ✘ 单元测试构建失败："; tail -30 "$WORK/build.log" | sed 's/^/    /'
  exit 1
fi
if [ ! -x "$BUILD/compiler" ]; then
  echo "  ✘ 构建后仍找不到 $BUILD/compiler（CLI 往返无法验证）"
  exit 1
fi
echo "  （构建通过：-Wall -Wextra -Wpedantic -Werror）"

FAIL=0
# ctest covers every test except test_parser, whose CLI round-trip needs the
# product binary and a writable directory.
if ! (cd "$BUILD" && ctest --output-on-failure -E test_parser) >"$WORK/ctest.log" 2>&1; then
  echo "  ✘ ctest 有失败："; tail -40 "$WORK/ctest.log" | sed 's/^/    /'
  FAIL=1
else
  grep -E "tests passed|tests failed" "$WORK/ctest.log" | sed 's/^/  /'
fi

echo
echo "──── test_parser（含 CLI 往返）────"
if "$BUILD/unit/test_parser" "$BUILD/compiler" "$WORK"; then
  echo "  ✔ test_parser 通过"
else
  echo "  ✘ test_parser 失败（退出码 $?）"; FAIL=1
fi

echo
echo "════════════════════════════════════════════════════════════"
if [ "$FAIL" = 0 ]; then echo " 单元测试结果：✔ 全部通过"; else echo " 单元测试结果：✘ 有失败"; fi
echo "════════════════════════════════════════════════════════════"
[ "$KEEP" = 1 ] && echo "保留工作目录: $WORK"
[ "$KEEP" = 1 ] || rm -rf "$WORK"
exit "$FAIL"
