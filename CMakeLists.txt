# ============================================================================
# SysY 编译器前中端 —— 构建脚本
#
#   cmake -S compiler -B compiler/build -G Ninja && cmake --build compiler/build
#   → compiler/build/compiler      （可执行文件必须叫 compiler，比赛规定）
#
# 约束（D1/D7）：
#   * C++17，禁用 GNU 扩展（-std=c++17 而不是 gnu++17）
#   * 零第三方依赖（解析器生成器也不用，词法/语法全部手写）
#   * 零警告是硬要求（-Wall -Wextra，verify.sh 会 grep 构建日志）
#
# 静态链接（默认开，见文末 SYSY_STATIC）：比赛是"每个用例起一个编译器进程"，
# 动态链接的加载器开销在 540 次调用里累积成主要成本（实测 0.854 s → 0.485 s）。
# 探测失败会自动回退到动态链接，不影响可移植性。
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(sysy_compiler
        VERSION 0.1.0
        DESCRIPTION "SysY -> LLVM IR compiler (front/middle end)"
        LANGUAGES CXX)

include(CheckCXXSourceCompiles)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # 禁用 GCC 专有扩展
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# 零警告是硬门禁：开发期把警告直接升级为错误，防止"警告悄悄漂移"。
# 若某个第三方头文件逼出无法消除的警告，用 SYSTEM 包含目录而不是关掉这个开关。
add_compile_options(-Werror)

add_executable(compiler
  src/main/main.cpp
  src/main/EmitText.cpp
  src/support/Diagnostic.cpp
  src/support/SourceFile.cpp
  src/frontend/Lexer.cpp
  src/frontend/Parser.cpp
  src/frontend/ParserStmtExpr.cpp
  src/frontend/AstPrinter.cpp
  src/frontend/AstReader.cpp
  src/frontend/ConstEval.cpp
  src/frontend/RuntimeLib.cpp
  src/frontend/Sema.cpp
  src/frontend/SemaStmt.cpp
  src/frontend/SemaExpr.cpp
  src/frontend/SemaDecl.cpp
  src/frontend/SemaDump.cpp
  src/frontend/InitLowering.cpp
  src/frontend/InitLoweringDump.cpp
  # ── S06：平面 IR 的容器（类型 / 值 / 指令 / 块 / 函数 / 模块 + dump + 读回）
  src/ir/Type.cpp
  src/ir/Instruction.cpp
  src/ir/IR.cpp
  src/ir/FlatDump.cpp
  src/ir/FlatReader.cpp
  # ⚠️ `.inc` 必须显式列成依赖：CMake **看不见** `#include "*.inc"`，
  #   改了它而不同时碰 `.cpp` 时**不会重编** —— 实测踩到：修好读回器的
  #   条件分支解析后 `.o` 还是旧的，白跑了一轮验证。
  src/ir/FlatReaderInst.inc
  src/ir/FlatReaderFunc.inc
  src/ir/FlatVerifier.cpp
  # ── S05：两层 IR 的第一层（结构化 IR）────────────────────────────────────
  src/structured/StructuredIR.cpp
  src/structured/StructuredDump.cpp
  src/structured/StructuredReader.cpp
  src/structured/StructuredVerifier.cpp
  src/structured/IRGenTypes.cpp
  src/structured/IRGen.cpp
  # ── S05b：第一个**变换**（LoopNormalize）+ alloca 提升 ────────────────────
  src/structured/LoopNormalize.cpp
  src/structured/LoopAnalysis.cpp
  src/structured/LoopRewrite.cpp
  src/structured/AllocaHoist.cpp
  src/structured/IRGenStmt.cpp
  # ── S06：层边界（结构化 → 平面）──────────────────────────────────────────
  src/structured/FlattenCFG.cpp
  src/structured/FlattenLower.cpp
  src/structured/FlattenLoops.cpp
)

target_include_directories(compiler PRIVATE src)

# 版本字符串（--version 用）
target_compile_definitions(compiler PRIVATE
  SYSY_COMPILER_VERSION="${PROJECT_VERSION}"
)

# 保证产物落在 compiler/build/compiler（verify.sh 与 run_tests.sh 的默认路径）
set_target_properties(compiler PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME "compiler"
)

# ── 静态链接（默认开；探测失败自动回退）─────────────────────────────────────
# 为什么值得做：评测对每个用例都起一个新进程，动态加载器 + libstdc++ 的重定位
# 是纯固定成本。实测 540 个文件串行 token 化：
#     动态链接 1.046 s   →   静态链接 0.647 s（S01 要求 < 1 s）
# 关掉的方式：-DSYSY_STATIC=OFF
option(SYSY_STATIC "静态链接编译器（减少每次进程启动的加载器开销）" ON)
if(SYSY_STATIC)
  set(CMAKE_REQUIRED_LINK_OPTIONS -static)
  check_cxx_source_compiles("int main(){return 0;}" SYSY_HAVE_STATIC)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(SYSY_HAVE_STATIC)
    target_link_options(compiler PRIVATE -static)
    message(STATUS "SYSY_STATIC=ON：已启用 -static")
  else()
    message(STATUS "SYSY_STATIC=ON 但本机不支持静态链接 → 回退为动态链接")
  endif()
endif()

# 本阶段没有 CMake 测试目标：验证靠 compiler/tools/ 下的脚本
#   * tools/selftest/unit/run.sh          单元测试（含 S01 的 test_lexer、S02 的 test_parser）
#   * tools/selftest/check_lexer.py       ★★ 词法自校验（490 个用例的拼接不变式）
#   * tools/selftest/check_tokens_meta.py 逐 token 的 loc+length 反查原文（540 个）
#   * tools/selftest/check_crlf.py        CRLF 与 LF 词法结果一致性
#   * tools/selftest/check_parser.py      ★★ 语法往返验证（490 个用例，S02）
#   * tools/selftest/perf_tokens.py       性能实测（比赛调用口径）
#   * tools/selftest/run.sh               三种 target 的端到端链路自检
