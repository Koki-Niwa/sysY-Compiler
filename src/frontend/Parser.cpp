// ============================================================================
// Parser.cpp —— SysY 语法分析器（S02）：基础设施 + 顶层 + 声明
//
// 本文件是 Parser 的**前一半**：构造与 token 访问、诊断、错误恢复、递归深度
// 防护、顶层（CompUnit → Decl | FuncDef）、声明与初始化器。
// 后一半（语句与表达式，含优先级爬升与字面量辅助值换算）在 ParserStmtExpr.cpp。
// 两半共用同一个类声明 Parser.h —— 成员函数分别在这两个 .cpp 里定义，
// 拆分为**纯移动**（没有函数体、字段、接口被改动）。
//
// ── 三个必须遵守的契约（phases/S02-parser.md §3.2/§9）──────────────────────
//
//  1. **无回溯**：每个文法决策都由**有界前瞻**（最多 2 个 token）确定，
//     不存在"先试着解析，失败再退回重来"。只有两处需要看第二步：
//       * `Ident` 后面是不是 `(`   → LVal 还是 Call      （parsePrimary）
//       * `Ident` 后面是不是 `[` `=` → 语句还是赋值语句 （parseStmt）
//     理由：回溯会让最坏复杂度爆炸，而 540 个用例里最长一行 86 KB。
//
//  2. **二元运算用循环而不是递归**：`a+b+c+...` 若按 `AddExp → AddExp '+' MulExp`
//     直接递归实现，树会退化成 N 层递归（86 KB 单行实测约 5000 个运算符）→ 栈溢出。
//     所以六个二元层用同一张优先级表 + 一个 while 循环（优先级爬升），
//     实现在 ParserStmtExpr.cpp（该文件头有完整的优先级表）。
//     **只有真正有嵌套语义的地方才递归**：括号、一元链、语句/块。
//
//  3. **出错不抛异常、不中断**：报诊断 → 恢复到同步点 → 继续解析，
//     最终仍返回一棵**尽可能完整的** AST（非 nullptr）。这是比赛"准确识别、
//     定位错误"的要求：一个文件里的多个错误要一次全报出来。
//     恢复的收敛性由 Parser::recoverToStmtEnd 保证（永远前进，不会死循环）。
//
// ── 与 S01 的接口 ─────────────────────────────────────────────────────────
//   Parser 在构造时用 `lexer.tokenize()` 一次性取完全部 token 并缓存，
//   于是"任意前瞻"只是一次数组下标（**仍然是零回溯**：决策只看 token，
//   不做"先解析、失败再回退"）。见 Parser.cpp 里 parseTopLevelItem 的注释。
//
// ── 递归深度防护 ─────────────────────────────────────────────────────────
//   递归下降对**畸形输入**可能自我放大（例如 `((((((...` 或 `!!!!...`）。
//   本实现用一个显式计数器（kMaxDepth）在超限时【报错并照常返回】——
//   **不依赖栈溢出（那是崩溃，验收要求"不崩"）**。见 Parser::DepthGuard。
//   ⚠️ kMaxDepth 量的是**递归深度**，而二元链靠循环建树、不消耗这个计数 ——
//      这条隐式耦合写在 Parser.h 的 kMaxDepth 注释里，**动 parseExp 之前必读**。
// ============================================================================

#include "frontend/Parser.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>

#include "frontend/Lexer.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace sysy {

// ============================================================================
// 构造与 token 访问
// ============================================================================

// ── 为什么用"缓冲全部 token"而不是"按需拉取" ───────────────────────────
// 语法层的分支判定需要**任意前瞻**，最有代表性的一处是：
//
//     int a[3] = {...};        ┐ 都以 `KwInt Ident` 开头，
//     int f(int a) { ... }     ┘ 必须看完 `[3]` 之后的 token 才能分流
//
// 而"下标里可能有括号"（`int a[f(1)]`）意味着前瞻深度不能硬编码成 2。
// 缓冲全部 token 之后，前瞻就是一次数组下标，**仍然是零回溯**
// （决策只看 token，绝不"先解析再回退"）。
// 额外好处：advance() 不必再关心"词法器是否已经结束"。
// 代价：540 个文件合计约 43 万行，token 缓冲的内存完全可控
// （远小于 D8 的 8 GB 上限）。
Parser::Parser(Lexer& lexer, DiagnosticEngine& diag) : lexer_(lexer), diag_(diag) {
  toks_ = lexer_.tokenize();
  if (toks_.empty() || toks_.back().kind != TokKind::EndOfFile) {
    Token eof;   // 契约保险：Lexer 保证末尾有 EndOfFile，这里只是防御
    toks_.push_back(eof);
  }
}

const Token& Parser::peek() { return peekAhead(1); }

TokKind Parser::peekKind() { return peek().kind; }

const Token& Parser::peekAhead(size_t k) {
  const size_t i = pos_ + k;
  return i < toks_.size() ? toks_[i] : toks_.back();   // 越界 → EndOfFile 哨兵
}

void Parser::advance() {
  if (cur().kind == TokKind::EndOfFile) return;   // 停在文件末尾，绝不越过
  ++pos_;
  // 词法错误（Invalid，如 `@`）在这里报出来 —— 这是语法阶段遇到它的第一个机会。
  // ⚠️ 必须**去重**：同一位置的词法错误只能报一次。否则错误恢复（skip）
  //    会把同一个 `@` 反复变成"当前 token"，一个字符刷出 3 条诊断
  //    （S02 实测：invalid character + 上层 expected… 各报一遍）。
  if (cur().kind == TokKind::Invalid) {
    bool seen = false;
    for (size_t i : invalidReported_) {
      if (i == pos_) { seen = true; break; }
    }
    if (!seen) {
      invalidReported_.push_back(pos_);
      error(cur().loc, "invalid character '" + curText() + "'");
    }
  }
}

bool Parser::accept(TokKind k) {
  if (cur().kind != k) return false;
  advance();
  return true;
}

bool Parser::expect(TokKind k, const char* what) {
  if (cur().kind == k) {
    advance();
    return true;
  }
  expected(what);
  return false;
}

// ============================================================================
// 诊断
// ============================================================================

// 单文件诊断上限。理由：
//   * 比赛要求"准确识别、定位"——前若干条就足以定位问题
//   * 病态输入（实测 20 万层括号）会让恢复路径反复报同一条错，刷出上万条诊断，
//     既淹没真正有用的信息，也把时间花在字符串拼接上
//   * 上限之后仍然继续解析（只是不再记录），所以出口码与"不崩"都不受影响
static constexpr size_t kMaxReportedErrors = 100;

void Parser::error(SourceLoc loc, const std::string& msg) {
  ++errors_;
  if (errors_ > kMaxReportedErrors) {
    if (errors_ == kMaxReportedErrors + 1) {
      diag_.report(DiagLevel::Error, loc,
                   "too many syntax errors (more than " +
                       std::to_string(kMaxReportedErrors) +
                       "); further diagnostics are suppressed");
    }
    return;
  }
  diag_.report(DiagLevel::Error, loc, msg);
}

void Parser::warn(SourceLoc loc, const std::string& msg) {
  diag_.report(DiagLevel::Warning, loc, msg);
}

void Parser::errorHere(const std::string& msg) { error(cur().loc, msg); }

std::string Parser::curText() {
  const SourceFile* src = diag_.sourceFile();
  if (src == nullptr) return std::string(tokKindName(cur().kind));
  const std::string_view text = tokenText(cur(), *src);
  if (text.empty()) return std::string(tokKindName(cur().kind));
  return std::string(text);
}

void Parser::expected(const char* what) {
  std::string got = "end of file";
  if (cur().kind != TokKind::EndOfFile) {
    got = std::string(tokKindName(cur().kind)) + " '" + curText() + "'";
  }
  errorHere(std::string("expected ") + what + ", but got " + got);
}

// ============================================================================
// 错误恢复
//
// 目标：**永远前进**。这是"不崩、不死循环"的最后一道保险。
// 停止点（括号深度为 0 时）：
//   `;` —— 本语句结束（消费掉它）
//   `}` —— 本语句所在的块结束（**不消费**，留给块的解析器去配对）
//   EOF —— 文件结束
// 若一个都没遇到，就一路吃到 EOF（保证收敛）。
// ============================================================================
// 保证：只要没到 EndOfFile 就**至少前进一个 token**。
// ⚠️ 这条不是"保险起见"，而是必须的：当前 token 恰好是"深度 0 的 `}`"时，
//    循环体第一件事就是消费它（见函数末尾的兜底），否则
//    `int main(){ return 0; }}` 这种多一个右括号的输入会让
//    parseCompUnit 的主循环**永远不前进** → 死循环（S02 实测卡死）。
void Parser::recoverToStmtEnd() {
  const size_t startPos = pos_;
  int depth = 0;
  while (cur().kind != TokKind::EndOfFile) {
    switch (cur().kind) {
      case TokKind::LParen:
      case TokKind::LBracket:
      case TokKind::LBrace:
        ++depth;
        break;
      case TokKind::RParen:
      case TokKind::RBracket:
        if (depth > 0) --depth;
        break;
      case TokKind::RBrace:
        if (depth == 0) {
          // 块的结束 —— 交给上层配对。但若我们从一开始就停在这里
          // （调用方本来就站在一个多余的 `}` 上），就必须消费掉它，
          // 否则调用方的主循环不会前进。
          if (pos_ == startPos) advance();
          return;
        }
        --depth;
        break;
      case TokKind::Semicolon:
        if (depth == 0) {
          advance();   // 消费分号：这条语句到此为止
          return;
        }
        break;
      default:
        break;
    }
    advance();   // ★ 无条件前进
  }
}

// ============================================================================
// 递归深度防护
// ============================================================================
// 递归深度超限的统一处理。
// ⚠️ 三个必须做到的点（S02 在 5 万层括号 / 5 千层块上实测踩过）：
//   ① 只报**一条**诊断、只算**一个** error —— 否则一个病态输入能刷出几万条
//   ② 调用方必须**立刻返回**，不能再下钻
//   ③ 调用方返回后**必须仍然前进**：parseStmt 在超限时返回空语句而不消费
//      任何 token，若所在层（parseBlock）不额外推进，就会死循环
//      ⇒ 所以调用方要用 skipToMatchingBrace() 把 token 推过去
bool Parser::depthExceeded() {
  if (depth_ <= kMaxDepth) return false;
  if (!depthReported_) {
    depthReported_ = true;
    error(cur().loc, "nesting is too deep (limit " + std::to_string(kMaxDepth) +
                         " levels); the rest of this construct is skipped");
  }
  return true;
}

// 从当前 token 起，跳到"与已消费内容配平"的 `}` 之后（含），保证前进。
void Parser::skipToMatchingBrace() {
  int depth = 0;
  while (cur().kind != TokKind::EndOfFile) {
    if (cur().kind == TokKind::LBrace) {
      ++depth;
    } else if (cur().kind == TokKind::RBrace) {
      if (depth == 0) {
        advance();   // 消费掉配平的 `}`
        return;
      }
      --depth;
    }
    advance();
  }
}

// ============================================================================
// 顶层：CompUnit → {Decl | FuncDef}
//
// 最多 2 个 token 就能决定分支（无需回溯）：
//   'const' KwInt/KwFloat KwVoid → 声明或函数定义
//   KwInt/KwFloat/KwVoid Ident   → 看 Ident 后面是 '('（函数）还是别的（声明）
// ============================================================================
std::unique_ptr<CompUnit> Parser::parseCompUnit() {
  auto unit = std::make_unique<CompUnit>(cur().loc);

  // CompUnit → {Decl | FuncDef}
  // 只有这 4 个关键字能开启一个顶层项；其余一律报错并恢复到语句边界。
  // （`tensor int a[4];` 的 `tensor` 是 Ident → 落到 default 分支报结构化错误；
  //   `@` 已被词法报为 Invalid，advance() 时就会报出来。）
  while (cur().kind != TokKind::EndOfFile) {
    switch (cur().kind) {
      case TokKind::KwConst:
      case TokKind::KwInt:
      case TokKind::KwFloat:
      case TokKind::KwVoid:
        parseTopLevelItem(*unit);
        break;
      default:
        if (cur().kind == TokKind::Ident) {
          errorHere("expected a declaration or function definition, but got identifier '" +
                    std::string(cur().text) + "'");
        } else {
          expected("a declaration or function definition");
        }
        recoverToStmtEnd();
        break;
    }
  }

  return unit;
}

// 解析一个顶层项（声明或函数定义）。返回 true 表示成功消费了一个项。
// 当前 token 必须是 `const` / `int` / `float` / `void` 之一。
//
// ★ 关键：`int a = 3;` 与 `int f(int a) {…}` 都以 `KwInt Ident` 开头。
//   判据是 Ident 之后**跳过所有数组维度**再看的那个 token：
//     '('  → 函数定义
//     其它 → 变量声明（`=`, `,`, `;`, `[`）
//   因为下标里可能有括号（`int a[f(1)];`），前瞻必须"跳过配对的中括号"，
//   不能简单地看第二个 token —— 这就是本实现用 token 缓冲的原因。
bool Parser::parseTopLevelItem(CompUnit& unit) {
  const SourceLoc loc = cur().loc;
  const bool isConst = accept(TokKind::KwConst);

  bool isVoid = false;
  BType base = BType::Int;
  if (at(TokKind::KwVoid)) {
    if (isConst) {   // `const void` 不是合法类型
      error(loc, "'void' cannot be used with 'const'");
    }
    isVoid = true;
    advance();
  } else if (at(TokKind::KwInt)) {
    base = BType::Int;
    advance();
  } else if (at(TokKind::KwFloat)) {
    base = BType::Float;
    advance();
  } else {
    expected("'int', 'float' or 'void'");
    recoverToStmtEnd();
    return true;   // 已经消费了 `const`，算处理过一个项
  }

  if (!at(TokKind::Ident)) {
    expected(isVoid ? "function name" : "variable or function name");
    recoverToStmtEnd();
    return true;
  }
  const std::string name(cur().text);

  // ── 前瞻：跳过 Ident 之后的所有 `[...]`，看落点是 '(' 还是别的 ──
  const bool looksLikeFunc = isVoid || tokenAfterDimsIsLParenAt(pos_ + 1);
  if (looksLikeFunc) {
    advance();   // Ident
    auto fn = parseFuncDef(loc, base, name);
    fn->isVoid = isVoid;
    unit.items.push_back(std::move(fn));
    return true;
  }
  if (isVoid) {   // `void x;` —— 上面已按函数处理，这里不可达；防御性保留
    error(loc, "'void' is only valid as a function return type");
    recoverToStmtEnd();
    return true;
  }

  auto decl = parseDeclRest(loc, isConst, base);
  if (decl != nullptr) unit.items.push_back(std::move(decl));
  return true;
}

// 从 toks_[i] 开始，跳过成对的 `[ ... ]`，返回其后的第一个 token 是否为 '('。
// 只看 token、不建树，所以不会引入回溯（S02 的"无回溯"要求）。
bool Parser::tokenAfterDimsIsLParenAt(size_t i) {
  int bracketDepth = 0;
  for (size_t k = i; k < toks_.size(); ++k) {
    const TokKind t = toks_[k].kind;
    if (t == TokKind::EndOfFile) return false;
    if (t == TokKind::LBracket) {
      ++bracketDepth;
    } else if (t == TokKind::RBracket) {
      if (bracketDepth > 0) --bracketDepth;
    } else if (bracketDepth == 0) {
      return t == TokKind::LParen;
    }
  }
  return false;
}

std::unique_ptr<FuncDef> Parser::parseFuncDef(SourceLoc loc, BType retType,
                                              const std::string& name) {
  auto fn = std::make_unique<FuncDef>(loc);
  fn->retType = retType;
  fn->name = name;

  expect(TokKind::LParen, "'('");
  if (!at(TokKind::RParen)) fn->params = parseParams();
  expect(TokKind::RParen, "')'");

  if (at(TokKind::LBrace)) {
    fn->body = parseBlock();
  } else {
    // SysY 没有函数声明语法 —— 头文件里的原型式写法在这里报结构化错误
    expected("'{' to start the function body");
    recoverToStmtEnd();
    fn->body = std::make_unique<BlockStmt>(cur().loc);
  }
  return fn;
}

std::vector<Param> Parser::parseParams() {
  std::vector<Param> params;
  parseParam(params);
  while (accept(TokKind::Comma)) {
    if (!parseParam(params)) recoverToStmtEnd();
  }
  return params;
}

bool Parser::parseParam(std::vector<Param>& out) {
  if (!at(TokKind::KwInt) && !at(TokKind::KwFloat)) {
    expected("parameter type ('int' or 'float')");
    return false;
  }
  Param p(cur().loc);
  p.type.base = at(TokKind::KwInt) ? BType::Int : BType::Float;
  p.type.isFuncParam = true;
  advance();

  if (!at(TokKind::Ident)) {
    expected("parameter name");
    return false;
  }
  p.name = std::string(cur().text);
  advance();

  if (at(TokKind::LBracket)) parseParamArrayDims(p.type);

  out.push_back(std::move(p));
  return true;
}

// FuncFParam → BType Ident '[' ']' {'[' Exp ']'}
// 第一维【必须】为空（`int a[5]` 作为形参不是合法文法，但这里宽容地接受并报错，
// 这样后面照常建树、错误仍然被准确指出位置）。
void Parser::parseParamArrayDims(TypeSpec& type) {
  bool first = true;
  while (at(TokKind::LBracket)) {
    const SourceLoc lb = cur().loc;
    advance();
    if (at(TokKind::RBracket)) {
      Dim d(lb);   // 空维 `[]`
      advance();
      type.dims.push_back(std::move(d));
      if (!first) {
        error(lb, "only the first dimension of an array parameter may be empty");
      }
    } else {
      auto e = parseExp(1);
      if (first) {
        error(lb, "the first dimension of an array parameter must be empty ('[]')");
      }
      expect(TokKind::RBracket, "']'");
      Dim d(lb);
      d.expr = std::move(e);
      type.dims.push_back(std::move(d));
    }
    first = false;
  }
}

// ============================================================================
// 声明：`[const] BType VarDef {',' VarDef} ';'`（类型前缀已被消费）
// ============================================================================
std::unique_ptr<Decl> Parser::parseDeclRest(SourceLoc loc, bool isConst, BType base) {
  if (!at(TokKind::Ident)) {
    // ⚠️ 这里【不消费】token、也不恢复 —— 交给 parseCompUnit / parseBlock 的统一
    //    恢复逻辑。这样 `tensor int a[4];` 只产生一条诊断（在 `int` 处报
    //    "expected ';' after expression"），而不是两条互相干扰的诊断。
    expected("variable name");
    return nullptr;
  }

  auto decl = std::make_unique<Decl>(loc);
  decl->isConst = isConst;
  decl->base = base;

  while (true) {
    auto def = std::make_unique<VarDef>(cur().loc);
    def->name = std::string(cur().text);
    advance();

    // ⚠️ "是不是数组"看的是**有没有方括号**，不是"维度解析成没解析成"。
    //    否则 `int a[;` 这类输入会让 AST 把一个数组记成标量（错误被掩埋），
    //    而 S03 的类型检查会就此得出错误的类型。
    if (at(TokKind::LBracket)) {
      parseVarDefDims(def->dims);
      for (const Dim& d : def->dims) {
        if (d.expr == nullptr) {
          error(d.loc, "array '" + def->name +
                           "' must specify the length of every dimension here");
        }
      }
    }

    if (accept(TokKind::Assign)) def->init = parseInitVal();

    decl->defs.push_back(std::move(def));

    if (!accept(TokKind::Comma)) break;
    if (!at(TokKind::Ident)) {
      expected("variable name after ','");
      return decl;
    }
  }

  expect(TokKind::Semicolon, "';' after declaration");
  return decl;
}

// Ident {'[' Exp ']'}（调用方已确认当前是 '['）
void Parser::parseVarDefDims(std::vector<Dim>& dims) {
  while (at(TokKind::LBracket)) {
    const SourceLoc lb = cur().loc;
    advance();
    if (at(TokKind::RBracket)) {
      Dim d(lb);   // 空维：非形参不允许，由调用方报错（这里只照实记录）
      advance();
      dims.push_back(std::move(d));
      continue;
    }
    Dim d(lb);
    d.expr = parseExp(1);
    expect(TokKind::RBracket, "']'");
    dims.push_back(std::move(d));
  }
}

// InitVal → Exp | '{' [InitVal {',' InitVal}] '}'
//
// ⚠️ 花括号是**唯一**可以无限嵌套、而每层又几乎不耗 token 的结构
//    （`int a = {{{{{{…}}}}}};`）。所以这里必须加深度防护，而且超限后
//    要把已经吃掉的那一串 `{` **排空**，否则 token 流会停在原地：
//      * 若直接返回空 InitVal，调用方 parseDeclRest 会继续去 expect `,`/`;`，
//        而那是一个 `{` → 报错 → recoverToStmtEnd 一路吃到 `;`
//      * 用递归地 reset 子树不行 —— 那正是我们要避免的深递归
//    所以用一个 while 循环把 `{`/`,` 全部吃掉，遇到 `}` 就配对消费。
std::unique_ptr<InitVal> Parser::parseInitVal() {
  DepthGuard guard(*this);
  auto iv = std::make_unique<InitVal>(cur().loc);
  // ⚠️ 必须调 depthExceeded()（会**报错**）而不是 guard.exceeded()（只查询）。
  //    返回空节点而不报错 = 静默产出错误的 AST 且退出码 0，比崩溃更糟。
  if (depthExceeded()) {
    int depth = 0;
    while (cur().kind != TokKind::EndOfFile) {
      if (cur().kind == TokKind::LBrace) {
        ++depth;
      } else if (cur().kind == TokKind::RBrace) {
        if (depth == 0) break;
        --depth;
      } else if (cur().kind == TokKind::Semicolon && depth == 0) {
        break;
      }
      advance();
    }
    return iv;
  }
  if (at(TokKind::LBrace)) {
    advance();
    while (!at(TokKind::RBrace) && !at(TokKind::EndOfFile)) {
      iv->list.push_back(parseInitVal());
      if (!accept(TokKind::Comma)) break;   // 尾随逗号 `{1,2,}` 被宽容接受
    }
    expect(TokKind::RBrace, "'}' to close the initializer list");
    return iv;
  }
  iv->expr = parseExp(1);
  return iv;
}

}  // namespace sysy
