// ============================================================================
// test_sourcefile.cpp —— SourceFile / SourceLoc 单元测试（P00 验证标准 §6.7/§6.8）
//
// 重点验证 CRLF 规范化：CRLF 文件与 LF 文件的行列号必须【完全一致】。
// 这是实测踩过的坑（many_mat_cal-*.sy、conv2d-1.in 含 CRLF）。
//
// 依赖：compiler/tools/selftest/unit/run.sh（不需要第三方测试框架）
// ============================================================================
#include <iostream>
#include <string>
#include <vector>

#include "support/SourceFile.h"

using sysy::SourceFile;
using sysy::SourceLoc;

static int g_failed = 0;
static int g_checks = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::cout << "  ✘ " << what << "\n";
  } else {
    std::cout << "  ✔ " << what << "\n";
  }
}

static void checkEq(const std::string& got, const std::string& want,
                    const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::cout << "  ✘ " << what << "\n      期望: [" << want << "]\n      实际: ["
              << got << "]\n";
  } else {
    std::cout << "  ✔ " << what << "\n";
  }
}

static void checkEqU(uint32_t got, uint32_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::cout << "  ✘ " << what << "  期望 " << want << "，实际 " << got << "\n";
  } else {
    std::cout << "  ✔ " << what << "\n";
  }
}

static void checkEqS(size_t got, size_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::cout << "  ✘ " << what << "  期望 " << want << "，实际 " << got << "\n";
  } else {
    std::cout << "  ✔ " << what << "\n";
  }
}

int main() {
  std::cout << "== SourceFile / SourceLoc ==\n";

  // ── 1. 基本行列映射（LF）────────────────────────────────────────────────
  const std::string lf = "int main() {\n  int x = 1;\n  return x;\n}\n";
  SourceFile a = SourceFile::fromString("lf.sy", lf);
  checkEqU(a.lineCount(), 4, "LF: lineCount == 4（末尾换行不算额外一行）");
  checkEq(a.lineText(1), "int main() {", "LF: lineText(1)");
  checkEq(a.lineText(2), "  int x = 1;", "LF: lineText(2)");
  checkEqU(a.locOf(0).line, 1, "LF: offset 0 → line 1");
  checkEqU(a.locOf(0).col, 1, "LF: offset 0 → col 1");
  checkEqU(a.locOf(13).line, 2, "LF: offset 13（第 2 行首）→ line 2");
  checkEqU(a.locOf(13).col, 1, "LF: offset 13 → col 1");
  checkEq(a.slice(13, 24), "  int x = 1", "LF: slice 覆盖第 2 行内容");
  // 第 2 行第 3 列 = 'i'
  checkEqU(a.offsetOf(SourceLoc(2, 3)), 15, "LF: offsetOf(2,3) == 15");
  checkEqU(a.offsetOf(SourceLoc(0, 0)), lf.size(), "LF: offsetOf(0,0) → text().size()（越界约定）");
  checkEqU(a.offsetOf(SourceLoc(99, 1)), a.text().size(), "LF: 行号越界 → text().size()");
  checkEqU(a.offsetOf(SourceLoc(1, 999)), 12, "LF: 列越界 → 夹到行尾(\n 位置)");

  // ── 2. ★ CRLF：行列号必须与 LF 版本完全一致 ────────────────────────────
  const std::string crlf = "int main() {\r\n  int x = 1;\r\n  return x;\r\n}\r\n";
  SourceFile b = SourceFile::fromString("crlf.sy", crlf);
  checkEqU(b.lineCount(), a.lineCount(), "CRLF: lineCount 与 LF 相同");
  checkEq(b.text(), lf, "CRLF: text() 规范化后与 LF 逐字节相同");
  checkEq(b.lineText(1), a.lineText(1), "CRLF: lineText(1) 与 LF 相同");
  checkEq(b.lineText(2), a.lineText(2), "CRLF: lineText(2) 与 LF 相同（无 \\r 残留）");
  check(b.lineText(2).find('\r') == std::string::npos, "CRLF: lineText 不含 \\r");

  bool same = true;
  for (size_t off = 0; off < lf.size(); ++off) {
    if (a.locOf(off) != b.locOf(off)) same = false;
  }
  check(same, "★ CRLF: 遍历每个 offset，locOf 的行列号与 LF 版本完全一致");
  checkEqU(b.offsetOf(SourceLoc(2, 3)), 15, "CRLF: offsetOf(2,3) 与 LF 相同（列号未错位）");
  checkEqU(b.offsetOf(SourceLoc(3, 3)), 28, "CRLF: offsetOf(3,3) 与 LF 相同");

  // ── 3. 孤立 \r 也当作换行 ───────────────────────────────────────────────
  SourceFile c = SourceFile::fromString("cr.sy", "a\rb\r\nc\n");
  checkEqU(c.lineCount(), 3, "孤立 \\r: lineCount == 3");
  checkEq(c.lineText(1), "a", "孤立 \\r: lineText(1)");
  checkEq(c.lineText(2), "b", "孤立 \\r: lineText(2)");
  checkEq(c.lineText(3), "c", "孤立 \\r: lineText(3)");

  // ── 4. 空文件 / 无末尾换行 / 单行 ───────────────────────────────────────
  SourceFile d = SourceFile::fromString("empty.sy", "");
  checkEqU(d.lineCount(), 1, "空文件: lineCount == 1");
  checkEq(d.lineText(1), "", "空文件: lineText(1) 为空");
  checkEqU(d.locOf(0).line, 1, "空文件: locOf(0) 不崩");

  SourceFile e = SourceFile::fromString("noeol.sy", "int x;");
  checkEqU(e.lineCount(), 1, "无末尾换行: lineCount == 1");
  checkEq(e.lineText(1), "int x;", "无末尾换行: lineText(1) 正确");
  checkEqU(e.locOf(6).col, 7, "无末尾换行: 最后一个字符的列号正确");

  SourceFile f = SourceFile::fromString("blank.sy", "a\n\n\nb\n");
  checkEqU(f.lineCount(), 4, "含空行: lineCount == 4");
  checkEq(f.lineText(2), "", "含空行: lineText(2) 为空");
  checkEq(f.lineText(4), "b", "含空行: lineText(4) 正确");

  // ── 5. 大文件（单行 86 KB，防 O(n²) 或列号溢出）─────────────────────────
  std::string big(86 * 1024, 'x');
  big += "\nnext\n";
  SourceFile g = SourceFile::fromString("long_line.sy", big);
  checkEqU(g.lineCount(), 2, "86KB 单行: lineCount == 2");
  checkEqS(g.lineText(1).size(), 86 * 1024, "86KB 单行: lineText(1) 长度正确");
  checkEqU(g.locOf(86 * 1024 + 1).line, 2, "86KB 单行: 第二行起点定位正确");
  checkEqU(g.offsetOf(SourceLoc(1, 86017)), 86016, "86KB 单行: 列号可到大数");

  std::cout << "\n" << (g_failed == 0 ? "全部通过" : "有失败") << ": "
            << (g_checks - g_failed) << "/" << g_checks << "\n";
  return g_failed == 0 ? 0 : 1;
}
