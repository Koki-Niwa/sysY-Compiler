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
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(sysy_compiler
        VERSION 0.1.0
        DESCRIPTION "SysY -> LLVM IR compiler (front/middle end)"
        LANGUAGES CXX)

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
  src/support/Diagnostic.cpp
  src/support/SourceFile.cpp
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

# 本阶段没有测试目标：验证靠 compiler/tools/ 下的脚本（见 P00 prompt §4/§6）
