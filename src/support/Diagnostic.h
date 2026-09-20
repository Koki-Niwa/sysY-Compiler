// ============================================================================
// Diagnostic —— 诊断引擎
//
// 输出格式（P00 prompt §3.2，比赛要求"准确识别、定位"）：
//
//   foo.sy:12:5: error: expected ';' after expression
//       int x = 1
//           ^
//   foo.sy:15:3: warning: unused variable 'y'
//
// 后置条件：report* 只记录，【不抛异常、不中断】——继续分析以发现更多错误。
// 真正决定退出码的是 main.cpp 里的 hasError()。
// ============================================================================
#ifndef SYSY_SUPPORT_DIAGNOSTIC_H
#define SYSY_SUPPORT_DIAGNOSTIC_H

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

#include "support/SourceLoc.h"

namespace sysy {

class SourceFile;

enum class DiagLevel { Note, Warning, Error };

// ★ 每文件诊断上限（**沿用 S02 的数值 100**；Parser.cpp 里的同名常量是它的
//   前身，这里提升到公共头，让 Sema / ConstEval 共用同一个上限）。
//   超限后**仍然继续遍历、仍然退出 1**，只是不再记录 —— 病态输入
//   （例如整份文件都是错误）不许刷出上万条诊断。
inline constexpr size_t kMaxDiagErrors = 100;

const char* toString(DiagLevel level);

struct Diagnostic {
  DiagLevel level = DiagLevel::Error;
  SourceLoc loc;
  std::string message;
  // ★ S03：稳定的**诊断编号**（如 "E-TYPE"），空表示"无编号"。
  //   渲染成 `file:line:col: error: [E-TYPE] 消息` —— `error:` 这个子串**不动**，
  //   因为 tools/selftest/check_parser.py 用它判别"确实报了诊断"（见 prompt §3.5）。
  //   语法/词法阶段的既有诊断**不带编号**，于是它们的输出逐字节不变。
  std::string code;
  // reportWithSnippet 时在记录时刻快照的源码行（避免 SourceFile 生命周期问题）
  bool hasSnippet = false;
  std::string snippetLine;
};

class DiagnosticEngine {
 public:
  // 追加一个诊断源（用于把文件名写进 "file:line:col:"，以及渲染源码行）。
  // 【前置】指针在引擎使用期间保持有效（driver 里 SourceFile 活得比引擎久）。
  void setSourceFile(const SourceFile* file) { source_ = file; }

  // 【前置】无。【后置】返回已设置的诊断源；未设置时为 nullptr。
  // 用途：调用方需要把 token 原文取出来拼进诊断消息（tokenText 需要一个 SourceFile）。
  const SourceFile* sourceFile() const { return source_; }

  // 【前置】无。【后置】记录一条诊断；不抛异常、不中断。
  void report(DiagLevel level, SourceLoc loc, const std::string& msg);

  // 带编号的重载（S03 起）：渲染为 `... error: [code] msg`。
  // 与三参数版本不构成重载歧义（参数个数不同）。
  void report(DiagLevel level, SourceLoc loc, const char* code, const std::string& msg);

  // 【前置】SourceFile 已加载。【后置】输出含源码行与 ^ 指示列位置。
  void reportWithSnippet(DiagLevel level, SourceLoc loc, const std::string& msg);

  bool hasError() const { return errorCount_ != 0; }
  size_t errorCount() const { return errorCount_; }
  size_t warningCount() const { return warningCount_; }
  size_t totalCount() const { return diags_.size(); }
  const std::vector<Diagnostic>& all() const { return diags_; }

  void printAll(std::ostream& os) const;

  // 诊断默认写在 stderr；默认按 isatty 决定是否上色（重定向时无色，便于 diff）
  void printAll(std::ostream& os, bool color) const;

  void clear() {
    diags_.clear();
    errorCount_ = 0;
    warningCount_ = 0;
  }

 private:
  std::string renderHeader(const Diagnostic& d) const;
  void printSnippet(std::ostream& os, const Diagnostic& d) const;

  std::vector<Diagnostic> diags_;
  const SourceFile* source_ = nullptr;
  size_t errorCount_ = 0;
  size_t warningCount_ = 0;
};

}  // namespace sysy

#endif  // SYSY_SUPPORT_DIAGNOSTIC_H
