// ============================================================================
// SourceFile —— 源文件读取与行列映射
//
// CRLF 规范化（P00 prompt §3.3，实测必须做）：
//   * "\r\n" → "\n"，孤立的 '\r' 也 → "\n"
//   * 规范化【只影响内部表示】；行列号按规范化后的文本计算
//   * 为什么必须做：many_mat_cal-*.sy / conv2d-1.in 等含 CRLF，
//     不处理会让列号错位（诊断与 _sysy_starttime 行号都会错）
//
// 约定：text() 保证以 '\n' 结尾（无行尾也会补一个），
//       因此每条逻辑行都能用 (offsetOf(line), offsetOf(line+1)) 切出来。
// ============================================================================
#ifndef SYSY_SUPPORT_SOURCEFILE_H
#define SYSY_SUPPORT_SOURCEFILE_H

#include <cstdint>
#include <string>
#include <vector>

#include "support/SourceLoc.h"

namespace sysy {

// 二进制读取整个文件；【失败】抛 IOError。诊断引擎等也要用它。
std::string readFileOrThrow(const std::string& path);

class SourceFile {
 public:
  // 读文件并把行尾规范化为 '\n'。
  // 【失败】抛 support/Errors.h 的 IOError（绝不 std::exit——便于单测与复用）。
  static SourceFile load(const std::string& path);

  // 从内存构造（单测用；同样做行尾规范化）。
  static SourceFile fromString(const std::string& path, std::string content);

  const std::string& path() const { return path_; }
  const std::string& text() const { return text_; }

  // 逻辑行数（末尾换行不额外产生一行）；文本为空时返回 1。
  // 实现：lineStart_ 末尾有一个哨兵，故行数 = lineStart_.size() - 1。
  uint32_t lineCount() const {
    return lineStart_.empty() ? 1u : static_cast<uint32_t>(lineStart_.size() - 1);
  }

  // 【越界返回 text().size()】
  size_t offsetOf(SourceLoc loc) const;

  // offset 落在哪一行哪一列（1-based，按规范化后文本计算）
  SourceLoc locOf(size_t offset) const;

  // 【1-based，不含行尾】；行号越界返回空串
  std::string lineText(uint32_t line) const;

  // 半开区间 [start,end) 内的文本（用于诊断/调试打印）
  std::string slice(size_t start, size_t end) const;

 private:
  SourceFile() = default;

  // 规范化行尾并建行首索引
  void build(std::string content);

  std::string path_;
  std::string text_;
  // 每条逻辑行的起始 offset（lineStart_[0] == 0），末尾额外放一个哨兵 = text_.size()，
  // 便于用 [lineStart_[i], lineStart_[i+1]) 切出第 i+1 行。
  std::vector<uint32_t> lineStart_;
  size_t end_ = 0;   // == text_.size()；lineCount() = lineStart_.size() - 1
};

}  // namespace sysy

#endif  // SYSY_SUPPORT_SOURCEFILE_H
