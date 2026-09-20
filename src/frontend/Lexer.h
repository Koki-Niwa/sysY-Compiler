// ============================================================================
// Lexer —— SysY 词法分析器（S01）
//
// 实现要点（对应 phases/S01-lexer.md）：
//
//   * 手写单遍扫描，**无回溯**、O(n)。29_long_line.sy 是 86 KB 单行，
//     任何"先切片再判断"的写法都会退化。
//   * 输出用 std::string_view 指向 SourceFile::text()，**不复制源文本**。
//   * 最长匹配：`<=` `>=` `==` `!=` `&&` `||` 先试两字符形式。
//   * 注释不是 token，但要正确跳过（行注释到行尾；块注释到第一个 `*/`）。
//     ⚠️ 注释里会出现引号（`//#include "sylib.h"`）—— **不实现字符串字面量**，
//        看到 `"` 一律按非法字符处理，绝不会因此进入"字符串模式"。
//   * 数字：十进制/八进制/十六进制整数 + 十进制/十六进制浮点。
//     三种"不完整"浮点形式 `.5` / `1.` / `03.14159` 实测都在语料里。
//   * 非法字符：报 error + 产出 Invalid token，**继续扫描**（比赛要求能报多个错误）。
//
// 前端铁律（AGENT-CONTEXT §2）不受影响：本文件不做任何值传播、不求值。
// ============================================================================
#ifndef SYSY_FRONTEND_LEXER_H
#define SYSY_FRONTEND_LEXER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "frontend/Token.h"
#include "support/SourceLoc.h"

namespace sysy {

class SourceFile;
class DiagnosticEngine;

class Lexer {
 public:
  // 【前置】src 的生命期必须长于本对象（Token::text 指向它的 text()）。
  // 【后置】无副作用；不抛异常。
  explicit Lexer(const SourceFile& src, DiagnosticEngine& diag);

  // 【前置】无。【后置】返回下一个 token；文件结束时返回 EndOfFile 并【永远】返回它。
  Token next();

  // 【前置】无。【后置】一次取完所有 token（含末尾的 EndOfFile）。
  std::vector<Token> tokenize();

  // 【前置】无。【后置】词法错误数（不含 warning）。
  size_t errorCount() const;

 private:
  // —— 扫描原语（全部只前进，不回退）——
  bool atEnd() const { return pos_ >= text_.size(); }
  char peek(size_t ahead = 0) const {
    const size_t i = pos_ + ahead;
    return i < text_.size() ? text_[i] : '\0';
  }
  // 跳过空白与注释；返回时 pos_ 停在下一个"真实字符"上（或文件末尾）
  void skipTrivia();
  void skipLineComment();
  void skipBlockComment(SourceLoc startLoc);

  // 构造 token 并推进 pos_ 到 offset（offset > start）
  Token makeToken(TokKind kind, size_t start, size_t offset);
  // 非法字符：报 error + 产出 Invalid（长度恒为 1，保证一定前进）
  Token makeInvalid(size_t start, const char* what);

  // —— 数字扫描（最长匹配）——
  // 整数/浮点的判定在扫描过程中就能确定，**不要**事后靠"文本里有 '.' 吗"去猜
  // （`1e5.5` 实际切成 [FloatLit `1e5`][FloatLit `.5`]，没有词法错误；
  //   若事后靠'整段文本里有没有 .'去判，就会把它当成一个浮点 —— 那是错的）。
  // 关于【畸形数字】的既定行为（语料里未出现，但契约要明确）：
  //   `0x1.`  → [IntLit `0x1`] [Invalid `.`]      （报 1 个词法错误）
  //   `1e5.5` → [FloatLit `1e5`] [FloatLit `.5`]  （无错误；两个连写的浮点）
  //   `1..2`  → [FloatLit `1.`]  [FloatLit `.2`]  （无错误；同上）
  // 判据：**最长匹配优先**，扫到哪算哪；**不做事后合理性检查**（那是语法阶段的活）。
  struct NumberScan {
    size_t end = 0;                 // token 结束 offset（恒 > start，保证一定前进）
    bool isFloat = false;           // 有小数点或指数 → 浮点
  };
  NumberScan scanNumber(size_t start);
  size_t scanDecimalDigits(size_t p) const;   // [0-9]+
  size_t scanHexDigits(size_t p) const;       // [0-9a-fA-F]+
  size_t scanExponent(size_t p, const char* markers) const;   // [eE] / [pP] + 可选符号 + 数字

  const SourceFile& src_;
  DiagnosticEngine& diag_;
  const std::string& text_;   // == src_.text()（含末尾 '\n' 哨兵）
  size_t pos_ = 0;
  size_t errors_ = 0;
};

}  // namespace sysy

#endif  // SYSY_FRONTEND_LEXER_H
