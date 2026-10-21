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
# F05：源码清单只在这里维护一次。组件 OBJECT 库表达
#   support / frontend / ir / analysis / opt / structured 的依赖方向；
#   单元测试同样链接这些目标，不再由 run.sh 手抄 SOURCES。
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
set(CMAKE_CXX_EXTENSIONS OFF)          # 禁用 GNU 专有扩展
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# 零警告是硬门禁：开发期把警告直接升级为错误，防止"警告悄悄漂移"。
add_compile_options(-Werror)

# ── 组件边界（F05）──────────────────────────────────────────────────────────
# 每个组件是一个 STATIC 库：目标依赖就是允许的 include 方向，新增文件只改
# 对应组件一处，链接时按 PUBLIC 依赖传递。解释器是独立自测目标（S10a），不进入
# 产品链接。
add_library(sysy_support STATIC
  src/support/Diagnostic.cpp
  src/support/SourceFile.cpp)
target_include_directories(sysy_support PUBLIC src)

add_library(sysy_frontend STATIC
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
  src/frontend/InitLoweringDump.cpp)
target_include_directories(sysy_frontend PUBLIC src)
target_link_libraries(sysy_frontend PUBLIC sysy_support)

add_library(sysy_ir STATIC
  src/ir/Type.cpp
  src/ir/Instruction.cpp
  src/ir/Value.cpp
  src/ir/IR.cpp
  src/ir/IRMutator.cpp
  src/ir/CFGEditor.cpp
  src/ir/FlatDump.cpp
  src/ir/FlatReader.cpp
  src/ir/LLVMTextEmitter.cpp
  src/ir/DominatorTree.cpp
  src/ir/FlatSemantics.cpp
  src/ir/FlatVerifier.cpp)
target_include_directories(sysy_ir PUBLIC src)
target_link_libraries(sysy_ir PUBLIC sysy_support)

add_library(sysy_analysis STATIC
  src/analysis/AnalysisManager.cpp
  src/analysis/ReachabilityAnalysis.cpp
  src/analysis/CallGraph.cpp
  src/analysis/FunctionSummary.cpp
  src/analysis/AliasAnalysis.cpp
  src/analysis/DominatorAnalysis.cpp
  src/analysis/LoopAnalysis.cpp
  src/analysis/BranchProbabilityAnalysis.cpp
  src/analysis/BlockFrequencyAnalysis.cpp
  src/analysis/ValueFacts.cpp
  src/analysis/DependenceAnalysis.cpp
  src/analysis/ScalarEvolution.cpp
  src/analysis/TailCallAnalysis.cpp
  src/analysis/InterchangePlan.cpp
  src/analysis/TilePlan.cpp
  src/analysis/BlockingPlan.cpp
  src/analysis/UnrollPlan.cpp
  src/analysis/UnrollAdvisor.cpp)
target_include_directories(sysy_analysis PUBLIC src)
target_link_libraries(sysy_analysis PUBLIC sysy_ir)

add_library(sysy_opt STATIC
  src/opt/FlatPassManager.cpp
  src/opt/Mem2Reg.cpp
  src/opt/Mem2RegAnalysis.cpp
  src/opt/InlineV0.cpp
  src/opt/ConstantPropagation.cpp
  src/opt/DeadCodeElimination.cpp
  src/opt/SimplifyCFG.cpp
  src/opt/RewriteEngine.cpp
  src/opt/GVN.cpp
  src/opt/DeadStoreElimination.cpp
  src/opt/DeadLoadElimination.cpp
  src/opt/LICM.cpp
  src/opt/AddressPlan.cpp
  src/opt/StrengthReduce.cpp
  src/opt/LoopRotate.cpp
  src/opt/TailRecursion.cpp
  src/opt/LoopInterchange.cpp
  src/opt/AllocaGEPCanonicalization.cpp
  src/opt/DuplicatePhiCoalescing.cpp
  src/opt/LoopTiling.cpp
  src/opt/LoopUnroll.cpp
  src/opt/LoopBlocking.cpp)
target_include_directories(sysy_opt PUBLIC src)
target_link_libraries(sysy_opt PUBLIC sysy_analysis)

add_library(sysy_structured STATIC
  src/structured/StructuredIR.cpp
  src/structured/StructuredDump.cpp
  src/structured/StructuredReader.cpp
  src/structured/StructuredVerifier.cpp
  src/structured/IRGenTypes.cpp
  src/structured/IRGen.cpp
  src/structured/IRGenStmt.cpp
  src/structured/LoopNormalize.cpp
  src/structured/LoopAnalysis.cpp
  src/structured/LoopRewrite.cpp
  src/structured/AllocaHoist.cpp
  src/structured/FlattenCFG.cpp
  src/structured/FlattenLower.cpp
  src/structured/FlattenLoops.cpp)
target_include_directories(sysy_structured PUBLIC src)
target_link_libraries(sysy_structured PUBLIC sysy_frontend sysy_ir)

# ── 产品可执行文件 ─────────────────────────────────────────────────────────
add_executable(compiler
  src/main/main.cpp
  src/main/CompilationSession.cpp
  src/main/FlatPipeline.cpp
  src/main/EmitText.cpp)
target_include_directories(compiler PRIVATE src)
target_link_libraries(compiler PRIVATE
  sysy_support sysy_frontend sysy_ir sysy_analysis sysy_opt sysy_structured)

# 版本字符串（--version 用）
target_compile_definitions(compiler PRIVATE
  SYSY_COMPILER_VERSION="${PROJECT_VERSION}")

# 保证产物落在 compiler/build/compiler（verify.sh 与 run_tests.sh 的默认路径）
set_target_properties(compiler PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME "compiler")

add_executable(toy_backend
  tools/toy_backend/main.cpp
  tools/toy_backend/ToyIR.cpp
  tools/toy_backend/ToyParser.cpp
  tools/toy_backend/X86Lowering.cpp
  tools/toy_backend/X86Ops.cpp)
target_include_directories(toy_backend PRIVATE src)
set_target_properties(toy_backend PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ── 单元测试（F05：链接组件目标，不复制 SOURCES）──────────────────────────
# 只有顶层构建才注册测试；作为子目录嵌入时不污染宿主工程。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  function(sysy_unit_test name)
    add_executable(${name} tools/selftest/unit/${name}.cpp)
    target_include_directories(${name} PRIVATE src)
    target_link_libraries(${name} PRIVATE ${ARGN})
    set_target_properties(${name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit")
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  sysy_unit_test(test_runtime_catalog sysy_frontend)
  sysy_unit_test(test_sourcefile sysy_support)
  sysy_unit_test(test_diagnostic sysy_support)
  sysy_unit_test(test_lexer sysy_frontend)
  sysy_unit_test(test_parser sysy_frontend)
  sysy_unit_test(test_sema sysy_frontend)
  sysy_unit_test(test_initlowering sysy_structured)
  sysy_unit_test(test_const_remainder sysy_structured)
  sysy_unit_test(test_structured sysy_structured)
  sysy_unit_test(test_loopnorm sysy_structured)
  sysy_unit_test(test_flat sysy_structured sysy_ir)
  sysy_unit_test(test_flat_reader sysy_ir)
  sysy_unit_test(test_ir_mutation sysy_ir)
  sysy_unit_test(test_value_use sysy_ir)
  sysy_unit_test(test_flat_f2i sysy_structured sysy_ir)
  sysy_unit_test(test_flat_dead_phi sysy_structured sysy_ir)
  sysy_unit_test(test_llvm_emitter sysy_ir)
  sysy_unit_test(test_analysis_manager sysy_analysis)
  sysy_unit_test(test_interprocedural sysy_analysis)
  sysy_unit_test(test_alias sysy_analysis)
  sysy_unit_test(test_cfg_analysis sysy_analysis)
  sysy_unit_test(test_hotness sysy_analysis)
  sysy_unit_test(test_dependence sysy_analysis)
  sysy_unit_test(test_foundation_audit sysy_analysis sysy_opt)
  sysy_unit_test(test_pass_manager sysy_opt)
  sysy_unit_test(test_mem2reg sysy_opt)
  sysy_unit_test(test_mem2reg_pass sysy_opt)
  sysy_unit_test(test_inline_v0 sysy_opt)
  sysy_unit_test(test_s17 sysy_opt)
  sysy_unit_test(test_rewrite sysy_opt)
  sysy_unit_test(test_s19 sysy_opt)
  sysy_unit_test(test_s20 sysy_opt)
  sysy_unit_test(test_looprotate sysy_opt)
  sysy_unit_test(test_tailcall sysy_analysis)
  sysy_unit_test(test_tco sysy_opt)
  sysy_unit_test(test_interchange sysy_analysis)
  sysy_unit_test(test_tile sysy_analysis)
  sysy_unit_test(test_blocking sysy_analysis)
  sysy_unit_test(test_loop_canonicalization sysy_opt)
  sysy_unit_test(test_unroll sysy_analysis)
  sysy_unit_test(test_unroll_advisor sysy_analysis)
  sysy_unit_test(test_interchange_transform sysy_opt)
  sysy_unit_test(test_cfg_phi_edges sysy_opt)
  sysy_unit_test(test_tiling_transform sysy_opt)
  sysy_unit_test(test_unroll_transform sysy_opt)
  sysy_unit_test(test_blocking_transform sysy_opt)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME test_tiling_equivalence
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/selftest/tiling_equivalence.py"
      $<TARGET_FILE:test_tiling_transform>)
  set_tests_properties(test_tiling_equivalence PROPERTIES TIMEOUT 60)
  add_test(NAME test_unroll_equivalence
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/selftest/unroll_equivalence.py"
      $<TARGET_FILE:test_unroll_transform>)
  set_tests_properties(test_unroll_equivalence PROPERTIES TIMEOUT 60)
  add_test(NAME test_blocking_equivalence
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/selftest/blocking_equivalence.py"
      $<TARGET_FILE:test_blocking_transform>)
  set_tests_properties(test_blocking_equivalence PROPERTIES TIMEOUT 60)
  add_test(NAME test_cli_help
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/selftest/cli_help.py"
      $<TARGET_FILE:compiler>)

  add_executable(bench_lexer tools/selftest/unit/bench_lexer.cpp)
  target_include_directories(bench_lexer PRIVATE src)
  target_link_libraries(bench_lexer PRIVATE sysy_frontend)
  set_target_properties(bench_lexer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unit")
endif()

# ── 静态链接（默认开；探测失败自动回退）─────────────────────────────────────
# 为什么值得做：评测对每个用例都起一个新进程，动态加载器 + libstdc++ 的重定位
# 是纯固定成本。实测 540 个文件串行 token 化：
#     动态链接 1.046 s   → 静态链接 0.647 s（S01 要求 < 1 s）
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
