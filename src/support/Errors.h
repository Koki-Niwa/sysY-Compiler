// ============================================================================
// Errors —— 支撑层异常
//
// 原则：支撑层【不决定进程退出码】，只抛异常；main.cpp 统一翻译成退出码。
// 这样单测可以直接 try/catch，而不必 fork 子进程。
// ============================================================================
#ifndef SYSY_SUPPORT_ERRORS_H
#define SYSY_SUPPORT_ERRORS_H

#include <stdexcept>
#include <string>

namespace sysy {

// 命令行用法错误（未知选项、缺 -o、缺输入文件……）→ main 翻译成退出码 2
class UsageError : public std::runtime_error {
 public:
  explicit UsageError(const std::string& what) : std::runtime_error(what) {}
};

// 文件读写失败（源文件不存在/不可读、输出文件写不出）→ main 翻译成退出码 3
class IOError : public std::runtime_error {
 public:
  explicit IOError(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace sysy

#endif  // SYSY_SUPPORT_ERRORS_H
