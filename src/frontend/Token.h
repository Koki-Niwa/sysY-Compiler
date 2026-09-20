// ============================================================================
// Token —— 词法单元（S01）
//
// 设计约束（见 docs/handoff/phases/S01-lexer.md §2/§3）：
//
//   * Token 集合【严格】按 SysY2022 规范：10 个关键字 + 23 个运算符/定界符。
//     **没有** tensor / for / do / struct —— 它们出现时是【普通标识符】。
//
//   * 【只识别、不求值】：数值转换留给 S04（ConstEval）。
//     Token 只携带【指向源文本的】原始文本（string_view，不复制、不拥有）。
//
//   * 【性能】Token::text 是 view 而不是 std::string：
//     29_long_line.sy 是 86 KB 单行，逐字符构造字符串会退化成 O(n²)。
//
//   * 【契约】src.text() 的 [offsetOf(loc), offsetOf(loc)+length)
//     必须逐字节等于该 token 的原始文本。check_lexer.py 靠这条做自校验。
// ============================================================================
#ifndef SYSY_FRONTEND_TOKEN_H
#define SYSY_FRONTEND_TOKEN_H

#include <cstdint>
#include <string_view>

#include "support/SourceLoc.h"

namespace sysy {

class SourceFile;

enum class TokKind {
  EndOfFile,
  // 标识符与字面量
  Ident, IntLit, FloatLit,
  // 关键字
  KwConst, KwInt, KwFloat, KwVoid, KwIf, KwElse, KwWhile,
  KwBreak, KwContinue, KwReturn,
  // 运算符与定界符（每个一个枚举值）
  Plus, Minus, Star, Slash, Percent,
  Less, Greater, LessEq, GreaterEq, EqEq, NotEq,
  Assign, Not, AmpAmp, PipePipe,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon,
  // 非法字符（词法错误时产出，便于报错后继续扫描）
  Invalid,
};

struct Token {
  TokKind kind = TokKind::EndOfFile;
  SourceLoc loc;            // 【指向 token 首字符】1-based
  uint32_t length = 0;      // 字节长度（用于自校验与反查原文）
  // 字面量与标识符的原始文本（仅 IntLit/FloatLit 有意义；指向 SourceFile 的 text，不复制）
  std::string_view text{};
};

// —— Token 类别判定（语法阶段常用，放在这里避免每个调用点重复 switch）——
bool isKeyword(TokKind k);
bool isLiteral(TokKind k);

// —— 对外的可读名字（诊断用，形如 "keyword 'int'" / "identifier"）——
// 注意：不叫 toString —— support/Diagnostic.h 里已有同名的 DiagLevel 版本，
//       同签名重载在 C++ 里会直接编译失败。
const char* tokKindName(TokKind k);

// --emit=tokens 的第 3 列：**机器可校验**的稳定短名（全小写，无空格）。
// 例：kw_int / ident / intlit / floatlit / lparen / EOF
//
// 【这是对外格式的一部分】—— 改名字等于改 dump 格式，S02 之后有工具依赖。
const char* tokKindDumpName(TokKind k);

// 【前置】loc/length 指向 src 文本内的合法区间（由 Lexer 保证）。
// 【后置】返回该 token 的原始文本；EndOfFile 返回空 view。
// 【为什么需要它】Token::text 只对字面量/标识符填充（它们有"有意义的名字"），
//   运算符/定界符的原文由本函数按 loc+length 现取 —— 两者都指向 src，零拷贝。
std::string_view tokenText(const Token& tok, const SourceFile& src);

}  // namespace sysy

#endif  // SYSY_FRONTEND_TOKEN_H
