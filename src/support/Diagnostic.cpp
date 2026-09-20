#include "support/Diagnostic.h"

#include <algorithm>
#include <ostream>
#include <string>

#include "support/SourceFile.h"

namespace sysy {

const char* toString(DiagLevel level) {
  switch (level) {
    case DiagLevel::Note:    return "note";
    case DiagLevel::Warning: return "warning";
    case DiagLevel::Error:   return "error";
  }
  return "error";
}

namespace {

// 终端着色（只在 isatty 时启用；重定向到文件/log 时输出纯文本，便于 diff）
const char* colorFor(DiagLevel level) {
  switch (level) {
    case DiagLevel::Note:    return "\033[36m";   // cyan
    case DiagLevel::Warning: return "\033[33m";   // yellow
    case DiagLevel::Error:   return "\033[31m";   // red
  }
  return "";
}
constexpr const char* kReset = "\033[0m";

// 制表符按 4 空格展开：让 "^" 与源码列在终端里对得上。
// 返回 [0, stopBefore) 的显示宽度（第 col 个字符之前占了多少终端列）。
size_t expandPrefix(std::string& out, const std::string& line, size_t stopBefore) {
  constexpr size_t kTabWidth = 4;
  size_t width = 0;
  for (size_t i = 0; i < line.size() && i < stopBefore; ++i) {
    const char c = line[i];
    if (c == '\t') {
      const size_t pad = kTabWidth - (width % kTabWidth);
      out.append(pad, ' ');
      width += pad;
    } else {
      out.push_back(c);
      ++width;
    }
  }
  return width;
}

}  // namespace

void DiagnosticEngine::report(DiagLevel level, SourceLoc loc, const std::string& msg) {
  Diagnostic d;
  d.level = level;
  d.loc = loc;
  d.message = msg;
  diags_.push_back(std::move(d));
  if (level == DiagLevel::Error) ++errorCount_;
  if (level == DiagLevel::Warning) ++warningCount_;
}

void DiagnosticEngine::report(DiagLevel level, SourceLoc loc, const char* code,
                             const std::string& msg) {
  Diagnostic d;
  d.level = level;
  d.loc = loc;
  d.message = msg;
  if (code != nullptr) d.code = code;
  diags_.push_back(std::move(d));
  if (level == DiagLevel::Error) ++errorCount_;
  if (level == DiagLevel::Warning) ++warningCount_;
}

void DiagnosticEngine::reportWithSnippet(DiagLevel level, SourceLoc loc,
                                         const std::string& msg) {
  Diagnostic d;
  d.level = level;
  d.loc = loc;
  d.message = msg;
  if (source_ != nullptr && loc.line != 0) {
    d.hasSnippet = true;
    d.snippetLine = source_->lineText(loc.line);   // 记录时刻快照
  }
  diags_.push_back(std::move(d));
  if (level == DiagLevel::Error) ++errorCount_;
  if (level == DiagLevel::Warning) ++warningCount_;
}

std::string DiagnosticEngine::renderHeader(const Diagnostic& d) const {
  std::string out;
  if (source_ != nullptr && d.loc.line != 0) {
    out += source_->path();
    out += ":" + std::to_string(d.loc.line);
    if (d.loc.col != 0) out += ":" + std::to_string(d.loc.col);
    out += ": ";
  }
  out += toString(d.level);
  out += ": ";
  // ★ S03：编号插在 `error:` **之后**（见 Diagnostic.h 的 code 字段注释）。
  if (!d.code.empty()) {
    out += '[';
    out += d.code;
    out += "] ";
  }
  out += d.message;
  return out;
}

void DiagnosticEngine::printSnippet(std::ostream& os, const Diagnostic& d) const {
  if (!d.hasSnippet) return;
  // 源码行统一缩进 4 空格；"^" 行必须用同样的缩进 + (col-1) 列的宽度，
  // 这样才能与上一行【逐字符对齐】（TAB 按 4 空格展开后再算宽度）。
  constexpr const char* kIndent = "    ";
  std::string line;
  expandPrefix(line, d.snippetLine, d.snippetLine.size());   // 整行（TAB 已展开）
  const size_t caretCol = (d.loc.col == 0) ? 0 : d.loc.col - 1;
  std::string prefix;
  const size_t width = expandPrefix(prefix, d.snippetLine, caretCol);
  os << kIndent << line << "\n"
     << kIndent << std::string(width, ' ') << "^\n";
}

void DiagnosticEngine::printAll(std::ostream& os) const { printAll(os, false); }

void DiagnosticEngine::printAll(std::ostream& os, bool color) const {
  for (const Diagnostic& d : diags_) {
    if (color) {
      // 只给 "error:"/"warning:" 上色，其余保持纯文本，方便肉眼扫读
      const std::string header = renderHeader(d);
      const std::string prefix = std::string(": ") + toString(d.level) + ": ";
      const size_t at = header.rfind(prefix);
      if (at == std::string::npos) {
        os << header << "\n";
      } else {
        os << header.substr(0, at) << ": " << colorFor(d.level) << toString(d.level)
           << kReset << ": " << d.message << "\n";
      }
    } else {
      os << renderHeader(d) << "\n";
    }
    printSnippet(os, d);
  }
}

}  // namespace sysy
