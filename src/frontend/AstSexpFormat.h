// ============================================================================
// AstSexpFormat.h —— 打印器与读取器**共用**的记号表（内部头，不对外公开）
//
//   打印器（AstPrinter.cpp）用 tokOpText 把 TokKind 变回原文、用 btypeSymbol
//   打印类型记号；读取器（AstReader.cpp）用 tokFromSymbol 反向还原。
//   两者共用同一张表 —— 这正是"严格互逆"的实现手段：**只有一份真相**。
//
//   ⚠️ 加一个运算符 / 改一个记号时**只改这一行**：若要给某一侧另写一份表，
//      两份表一旦漂移，往返（轨 A，check_parser.py）会立刻失败 —— 这正是
//      把它抽成共享头、而不是在两个 .cpp 里各留一份的原因。
//
//   ⚠️ 为什么这半边与打印器不在同一个文件里：见 AstPrinter.h 的文件头。
//      打印器与读取器是同一份格式契约的两面，但**深度约束不对称**
//      （打印器受栈限制、读取器受输入体积限制），实现上互不依赖，
//      所以按"打印 / 读取"一分为二，只把共用的记号表放在这里。
// ============================================================================
#ifndef SYSY_FRONTEND_ASTSEXPFORMAT_H
#define SYSY_FRONTEND_ASTSEXPFORMAT_H

#include <string>

#include "frontend/Ast.h"     // BType
#include "frontend/Token.h"   // TokKind

namespace sysy {

// 运算符 → 原文（打印器用）
inline const char* tokOpText(TokKind k) {
  switch (k) {
    case TokKind::Plus:      return "+";
    case TokKind::Minus:     return "-";
    case TokKind::Star:      return "*";
    case TokKind::Slash:     return "/";
    case TokKind::Percent:   return "%";
    case TokKind::Less:      return "<";
    case TokKind::Greater:   return ">";
    case TokKind::LessEq:    return "<=";
    case TokKind::GreaterEq: return ">=";
    case TokKind::EqEq:      return "==";
    case TokKind::NotEq:     return "!=";
    case TokKind::AmpAmp:    return "&&";
    case TokKind::PipePipe:  return "||";
    case TokKind::Not:       return "!";
    default:                 return "?";
  }
}

// 原文 → 运算符（读取器用）。失败返回 false（调用方当作"未知节点"处理）。
inline bool tokFromSymbol(const std::string& s, TokKind& out) {
  if (s == "+")  { out = TokKind::Plus;      return true; }
  if (s == "-")  { out = TokKind::Minus;     return true; }
  if (s == "*")  { out = TokKind::Star;      return true; }
  if (s == "/")  { out = TokKind::Slash;     return true; }
  if (s == "%")  { out = TokKind::Percent;   return true; }
  if (s == "<")  { out = TokKind::Less;      return true; }
  if (s == ">")  { out = TokKind::Greater;   return true; }
  if (s == "<=") { out = TokKind::LessEq;    return true; }
  if (s == ">=") { out = TokKind::GreaterEq; return true; }
  if (s == "==") { out = TokKind::EqEq;      return true; }
  if (s == "!=") { out = TokKind::NotEq;     return true; }
  if (s == "&&") { out = TokKind::AmpAmp;    return true; }
  if (s == "||") { out = TokKind::PipePipe;  return true; }
  if (s == "!")  { out = TokKind::Not;       return true; }
  return false;
}

// 类型记号（打印器用）：所有关键字/类型记号一律带 `:` 前缀，而 `:` 在 SysY
// 标识符里是非法字符 ⇒ 任何标识符都不可能被误认成关键字（格式无歧义性）。
inline const char* btypeSymbol(BType t) { return t == BType::Int ? ":int" : ":float"; }

}  // namespace sysy

#endif  // SYSY_FRONTEND_ASTSEXPFORMAT_H
