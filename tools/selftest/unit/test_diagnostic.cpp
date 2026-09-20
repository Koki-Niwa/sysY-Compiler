// ============================================================================
// test_diagnostic.cpp —— 诊断引擎单元测试（P00 验证标准 §6.7）
//
// 输出格式必须是【严格】这样（比赛要求"编译错误的准确识别、定位"）：
//
//   foo.sy:12:5: error: expected ';' after expression
//       int x = 1
//           ^
//   foo.sy:15:3: warning: unused variable 'y'
//
// 判据：把整段输出做【逐字节】比较，不是 grep 关键词。
//
// 期望值【全部程序化构造】，不写成含长串空格的字符串字面量：
//   * 行首缩进一律用 ind(n)（std::string(n,' ')）显式生成；
//   * 源码行直接取 src.lineText(k)，保证与"被测对象看到的同一份文本"；
//   * "^" 行的列偏移用 caret(n) 生成。
// 这样期望值与代码缩进、编辑器行为完全无关（本项目踩过"字面量空格被改动"的坑）。
//
//   "^" 行的对齐规则（见 support/Diagnostic.cpp）：
//       前导空格 = 4（引擎统一缩进） + (col-1)（列偏移；TAB 先按 4 空格展开）
//
// 依赖：compiler/tools/selftest/unit/run.sh（不需要第三方测试框架）
// ============================================================================
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::DiagnosticEngine;
using sysy::DiagLevel;
using sysy::SourceFile;
using sysy::SourceLoc;

static int g_failed = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) { ++g_failed; std::cout << "  X " << what << "\n"; }
  else     { std::cout << "  . " << what << "\n"; }
}

static void checkEq(const std::string& what, const std::string& got,
                    const std::string& want) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::cout << "  X " << what << "\n---- 期望 ----\n" << want
              << "\n---- 实际 ----\n" << got << "\n--------------\n";
  } else {
    std::cout << "  . " << what << "\n";
  }
}

// ── 期望值构造（全部程序化，见文件头说明）─────────────────────────────────
static const char* kIndent = "    ";          // 引擎给源码行/^ 行的统一缩进

static std::string ind(size_t n) { return std::string(n, ' '); }

// 诊断头：<path>:<line>:<col>: <level>: <msg>
static std::string hdr(const std::string& path, unsigned line, unsigned col,
                       const char* level, const std::string& msg) {
  std::ostringstream os;
  os << path << ":" << line << ":" << col << ": " << level << ": " << msg;
  return os.str();
}

// 带源码行的诊断：头 + 缩进后的源码行 + "^" 行
// col 是 1-based 列号；^ 行前导空格 = kIndentLen + (col-1)
static std::string withSnippet(const std::string& path, unsigned line, unsigned col,
                               const char* level, const std::string& msg,
                               const std::string& sourceLine) {
  std::string s = hdr(path, line, col, level, msg) + "\n";
  s += kIndent + sourceLine + "\n";
  s += kIndent + ind(col - 1 >= 0 ? col - 1 : 0) + "^\n";
  return s;
}

static std::string render(const DiagnosticEngine& d) {
  std::ostringstream os;
  d.printAll(os, false);
  return os.str();
}

int main() {
  std::cout << "== DiagnosticEngine ==\n";

  // 测试源（与 prompt 示例同形）
  SourceFile src = SourceFile::fromString(
      "foo.sy",
      "int main() {\n"          // line 1
      "    int x = 1\n"         // line 2 -> 列 5 是 'i'
      "    return x;\n"         // line 3
      "    int y;\n"            // line 4 -> 列 9 是 'y'
      "}\n");                   // line 5

  // ── 1. 不带 snippet 的 report ───────────────────────────────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.report(DiagLevel::Error, SourceLoc(2, 5), "expected ';' after expression");
    checkEq("report: 严格 file:line:col: error: msg（无源码行）",
            render(d),
            hdr("foo.sy", 2, 5, "error", "expected ';' after expression") + "\n");
    check(d.hasError(), "report: hasError() == true");
    check(d.errorCount() == 1, "report: errorCount() == 1");
  }

  // ── 2. reportWithSnippet：头 + 源码行 + ^ 指示 ──────────────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.reportWithSnippet(DiagLevel::Error, SourceLoc(2, 5),
                        "expected ';' after expression");
    checkEq("reportWithSnippet: 严格三段（头/源行/^）", render(d),
            withSnippet("foo.sy", 2, 5, "error", "expected ';' after expression",
                        src.lineText(2)));
  }

  // ── 3. 警告：只记 warning，不置 hasError（不中断分析）──────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.reportWithSnippet(DiagLevel::Warning, SourceLoc(4, 9), "unused variable 'y'");
    checkEq("warning: 格式为 ...: warning: msg + 指示", render(d),
            withSnippet("foo.sy", 4, 9, "warning", "unused variable 'y'",
                        src.lineText(4)));
    check(!d.hasError(), "warning: hasError() == false（警告不算错误）");
    check(d.errorCount() == 0, "warning: errorCount() == 0");
    check(d.warningCount() == 1, "warning: warningCount() == 1");
  }

  // ── 4. 多个诊断：顺序输出、互不干扰（继续分析以发现更多错误）──────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.reportWithSnippet(DiagLevel::Error, SourceLoc(2, 5),
                        "expected ';' after expression");
    d.reportWithSnippet(DiagLevel::Warning, SourceLoc(4, 9), "unused variable 'y'");
    d.report(DiagLevel::Note, SourceLoc(1, 1), "in function 'main'");
    std::string want;
    want += withSnippet("foo.sy", 2, 5, "error", "expected ';' after expression",
                        src.lineText(2));
    want += withSnippet("foo.sy", 4, 9, "warning", "unused variable 'y'",
                        src.lineText(4));
    want += hdr("foo.sy", 1, 1, "note", "in function 'main'") + "\n";  // note 无源码行
    checkEq("多条诊断: 按报告顺序、无颜色时纯文本", render(d), want);
    check(d.errorCount() == 1 && d.warningCount() == 1 && d.totalCount() == 3,
          "多条诊断: 计数正确 (1 error / 1 warning / 3 total)");
  }

  // ── 5. 无位置信息（line==0）→ 只输出 "error: msg" ──────────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.report(DiagLevel::Error, SourceLoc(0, 0), "cannot open input file 'x.sy'");
    checkEq("line==0: 不输出文件名/行号/列号",
            render(d), std::string("error: cannot open input file 'x.sy'\n"));
  }

  // ── 6. 没有 SourceFile 时也不崩（前置条件未满足的兜底）────────────────
  {
    DiagnosticEngine d;
    d.report(DiagLevel::Error, SourceLoc(3, 2), "no source attached");
    checkEq("无 SourceFile: 退化为 'error: msg'",
            render(d), std::string("error: no source attached\n"));
  }

  // ── 7. ^ 的列位置：第 1 列 / 第 2 行 col 2 / 含 TAB ────────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.reportWithSnippet(DiagLevel::Error, SourceLoc(1, 1), "at col 1");
    checkEq("col==1: ^ 行前导空格 = 4（只有缩进）", render(d),
            withSnippet("foo.sy", 1, 1, "error", "at col 1", src.lineText(1)));
  }
  {
    SourceFile t = SourceFile::fromString("t.sy", "ab\ncd\n");
    DiagnosticEngine d;
    d.setSourceFile(&t);
    d.reportWithSnippet(DiagLevel::Error, SourceLoc(2, 2), "second line");
    checkEq("第 2 行 col 2: ^ 行前导空格 = 4 + 1 = 5", render(d),
            withSnippet("t.sy", 2, 2, "error", "second line", t.lineText(2)));
  }
  {
    // TAB 展开为 4 空格：col=2 是 tab 之后的 'i'，
    // ^ 行前导空格 = 4（缩进） + 4（tab 展开）= 8
    SourceFile t = SourceFile::fromString("tab.sy", "\tint z;\n");
    DiagnosticEngine d;
    d.setSourceFile(&t);
    d.reportWithSnippet(DiagLevel::Error, SourceLoc(1, 2), "after tab");
    std::string want = hdr("tab.sy", 1, 2, "error", "after tab") + "\n";
    want += kIndent + ind(4) + "int z;\n";     // 源码行的 \t 展开为 4 空格
    want += kIndent + ind(4) + "^\n";          // col-1 = 1 个字符 = tab 的 4 列
    checkEq("TAB: 源码行按 4 空格展开，^ 对齐到同列（4 + 4 = 8）", render(d), want);
  }

  // ── 8. clear() 与计数 ──────────────────────────────────────────────────
  {
    DiagnosticEngine d;
    d.setSourceFile(&src);
    d.report(DiagLevel::Error, SourceLoc(1, 1), "e1");
    d.report(DiagLevel::Error, SourceLoc(1, 1), "e2");
    check(d.errorCount() == 2, "clear 前: errorCount == 2");
    d.clear();
    check(!d.hasError() && d.totalCount() == 0, "clear 后: 计数归零");
  }

  std::cout << "\n" << (g_failed == 0 ? "全部通过" : "有失败") << ": "
            << (g_checks - g_failed) << "/" << g_checks << "\n";
  return g_failed == 0 ? 0 : 1;
}
