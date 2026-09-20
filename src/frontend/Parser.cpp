// ============================================================================
// Parser.cpp —— SysY 语法分析器实现（S02）
//
// 结构（自下而上，与文法同构）：
//
//   parseCompUnit                     CompUnit → {Decl | FuncDef}
//     parseFuncDef / parseParams / parseParam
//     parseDeclRest / parseVarDefDims / parseParamArrayDims
//     parseStmt → parseBlock / parseInitVal / parseCond
//   parseExp(minPrec)                 ★ 优先级爬升（六层二元运算共用一个循环）
//     parseUnary                      UnaryExp → ('+'|'-'|'!') UnaryExp | Postfix
//     parsePostfix                    LVal 的下标链 / 函数调用
//     parsePrimary                    '(' Exp ')' | Number | Ident | Ident '(' args ')'
//
// ── ★ 优先级表（phases/S02-parser.md §4，与规范 sysy_lang.txt 逐条对应）──
//
//   1  ||                      LOrExp       左
//   2  &&                      LAndExp      左
//   3  == !=                   EqExp        左
//   4  < > <= >=               RelExp       左
//   5  + -                     AddExp       左
//   6  * / %                   MulExp       左
//   7  一元 + - !              UnaryExp     右（前缀）
//   8  后缀 [] 与调用 ()        LVal/UnaryExp
//   9  ( )                     PrimaryExp
//
//   ⚠️ 关系(4) 比相等(3) 优先级【高】——`a < b == c` 是 `(a < b) == c`。
//      这张表把 RelExp 放在 EqExp 之上（数字更大 = 结合更紧），与 C 一致。
//
// ── ★ `!` 的处理选择（prompt §4 要求"明确说明并在测试里固化"）──────────
//
//   规范说 `!` 只出现在 Cond 里（`Exp` 不含 `!`），但**语法层不区分**：
//   parseCond 就是 parseExp，parseUnary 对 `+ - !` 一视同仁，所以
//   `int y = !x;` 也会被解析成 Unary(Not, LVal x) 而不报错。理由：
//     ① 二者的【树形完全相同】，文法上本来就无法用"出现在哪个上下文"区分节点；
//     ② 语料实测有 2 处 `!` 出现在一元之外的形态（51_short_circuit3.sy 的
//        `if (i0 == !i1 && ...)` —— 它在 Cond 内，但嵌在二元运算的右操作数里），
//        按 UnaryExp 统一接受最简单、也最不容易漏；
//     ③ 这条本质是**语义**约束（`!` 的操作数必须是条件），不是语法约束。
//   ⇒ **选择：语法层统一接受，留给 S03 语义阶段报错。**
//      这条选择被 test_parser.cpp 的 `!` 用例固化（含 `int y = !x;` 应当被接受）。
//
// ── 不做的事（明确不做清单）─────────────────────────────────────────────
//   * 不求值数组维度（`int a[3+1]` 的维度就是 Binary(+ 3 1)，原样存）
//   * 不查类型、不查作用域、不查重复定义、不插入隐式转换（S03）
//   * 不为 tensor/@ 做任何特判或预留（D3）：`tensor int a[4];` 会自然地
//     被当成 `Ident Ident Ident [ IntLit ] ;` → 报结构化语法错误
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
namespace {

// ── 二元运算符优先级（0 = 不是二元运算符）──────────────────────────────
int binPrec(TokKind k) {
  switch (k) {
    case TokKind::PipePipe:  return 1;   // ||
    case TokKind::AmpAmp:    return 2;   // &&
    case TokKind::EqEq:                        // ==
    case TokKind::NotEq:     return 3;   // !=
    case TokKind::Less:                        // <
    case TokKind::Greater:                     // >
    case TokKind::LessEq:                      // <=
    case TokKind::GreaterEq: return 4;   // >=
    case TokKind::Plus:                        // +
    case TokKind::Minus:     return 5;   // -
    case TokKind::Star:                        // *
    case TokKind::Slash:                       // /
    case TokKind::Percent:   return 6;   // %
    default:                 return 0;
  }
}

// 十六进制浮点字面量规范化：C 的 strtof 要求 `0x1.8p3` 形式（必须有 p 指数），
// 而 SysY 允许 `0x1.8` / `0x.AP-3`。缺 p 时补一个 `p0`。
// 【不改变数值】，只是让 strtof 接得住。
std::string canonicalHexFloat(std::string_view sv) {
  std::string s;
  s.reserve(sv.size() + 2);
  bool hasP = false;
  for (char c : sv) {
    if (c == 'p' || c == 'P') hasP = true;
    s += c;
  }
  if (!hasP) s += "p0";
  return s;
}

}  // namespace

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
                         " levels); the rest of this brace level is skipped");
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
  if (guard.exceeded()) {
    error(cur().loc, "initializer nesting is too deep (limit " +
                         std::to_string(kMaxDepth) + " levels)");
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

// ============================================================================
// 语句
// ============================================================================
std::unique_ptr<Stmt> Parser::parseStmt() {
  DepthGuard guard(*this);
  if (guard.exceeded()) {
    // ⚠️ 超限时必须**消费 token**，不能只是返回一个空语句：调用方
    //    （parseBlock 的循环 / while 的 body）会原地重试同一个 token → 死循环。
    //    S02 在 4100 层嵌套块上实测卡死过一次。
    //    注意 `{` 要**先消费掉**再跳到配平的 `}`：skipToMatchingBrace 是从
    //    "当前 token"开始计数的，留着 `{` 会多跳一层。
    if (accept(TokKind::LBrace)) skipToMatchingBrace();
    return std::make_unique<ExprStmt>(cur().loc);
  }

  const SourceLoc loc = cur().loc;

  switch (cur().kind) {
    case TokKind::LBrace:
      return parseBlock();

    case TokKind::Semicolon: {
      // 空语句 `;` —— expr == nullptr（与"有表达式的语句"共用 ExprStmt 节点）
      auto s = std::make_unique<ExprStmt>(loc);
      advance();
      return s;
    }

    case TokKind::KwIf: {
      auto s = std::make_unique<IfStmt>(loc);
      advance();
      expect(TokKind::LParen, "'(' after 'if'");
      s->cond = parseCond();
      expect(TokKind::RParen, "')' after if-condition");
      s->thenS = parseStmt();
      // ⚠️ else 只在这里被吃掉 —— 所以它必然绑定到【最近的】未配对 if：
      //    内层 if 的 parseStmt 先返回，它已经把自己那份 else 吃掉了。
      if (accept(TokKind::KwElse)) s->elseS = parseStmt();
      return s;
    }

    case TokKind::KwWhile: {
      auto s = std::make_unique<WhileStmt>(loc);
      advance();
      expect(TokKind::LParen, "'(' after 'while'");
      s->cond = parseCond();
      expect(TokKind::RParen, "')' after while-condition");
      ++loopDepth_;   // break/continue 的合法性是语法上下文约束
      s->body = parseStmt();
      --loopDepth_;
      return s;
    }

    case TokKind::KwBreak: {
      auto s = std::make_unique<BreakStmt>(loc);
      advance();
      if (loopDepth_ == 0) error(loc, "'break' outside of a loop");
      expect(TokKind::Semicolon, "';' after 'break'");
      return s;
    }

    case TokKind::KwContinue: {
      auto s = std::make_unique<ContinueStmt>(loc);
      advance();
      if (loopDepth_ == 0) error(loc, "'continue' outside of a loop");
      expect(TokKind::Semicolon, "';' after 'continue'");
      return s;
    }

    case TokKind::KwReturn: {
      auto s = std::make_unique<ReturnStmt>(loc);
      advance();
      if (!at(TokKind::Semicolon)) {
        if (at(TokKind::RBrace) || at(TokKind::EndOfFile)) {
          expected("';' or a return value after 'return'");
        } else {
          s->value = parseExp(1);
        }
      }
      if (!expect(TokKind::Semicolon, "';' after return statement")) recoverToStmtEnd();
      return s;
    }

    default:
      break;
  }

  // ── 赋值语句 vs 表达式语句 ──
  // 赋值的左侧必然是 `Ident` 开头（文法：Stmt → LVal '=' Exp，而 LVal 是
  // `Ident {'[' Exp ']'}`）。所以判定方式是：先按 LVal 的形状解析出"候选左值"，
  // 再看下一个 token 是不是 `=`。**仍然是无回溯的** —— 候选左值的解析路径与
  // 表达式语句的解析路径在前缀上完全一致；若不是赋值，就把这个前缀结果作为
  // parseExp 的 lhs 继续往上爬（见 parseExp 的 lhs 参数）。
  if (at(TokKind::Ident)) {
    auto probe = parsePostfix();
    if (at(TokKind::Assign)) {
      if (auto* lv = dynamic_cast<LVal*>(probe.get())) {
        auto s = std::make_unique<AssignStmt>(loc);
        advance();   // '='
        lv->isAssignTarget = true;
        probe.release();               // 所有权移交 AssignStmt
        s->lhs.reset(lv);
        s->rhs = parseExp(1);
        if (!expect(TokKind::Semicolon, "';' after assignment")) recoverToStmtEnd();
        return s;
      }
      expected("an assignable expression on the left of '='");
      advance();
      recoverToStmtEnd();
      return std::make_unique<ExprStmt>(loc);
    }
    auto s = std::make_unique<ExprStmt>(loc);
    s->expr = parseExp(1, std::move(probe));   // 接上前缀继续爬（不重复解析）
    if (!expect(TokKind::Semicolon, "';' after expression")) recoverToStmtEnd();
    return s;
  }

  // 其余形态的表达式语句（算术、调用、字面量开头……）
  {
    auto s = std::make_unique<ExprStmt>(loc);
    s->expr = parseExp(1);
    if (!expect(TokKind::Semicolon, "';' after expression")) recoverToStmtEnd();
    return s;
  }
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
  auto block = std::make_unique<BlockStmt>(cur().loc);
  expect(TokKind::LBrace, "'{'");
  // 注意：这里**不做**"深度超限就整块跳过"的处理 —— 那会把同一块里后面的
  // 合法语句一起丢掉（S02 实测：`int main(){ <4100 层嵌套> return 0; }`
  // 会因为整块被跳过而静默返回 0）。前进的保证放在 parseStmt 里：
  // 超限时它会消费掉那个 `{` 并跳到配平的 `}`，于是本循环照常继续。
  while (!at(TokKind::RBrace) && !at(TokKind::EndOfFile)) {
    const SourceLoc itemLoc = cur().loc;

    // 块内声明：`const? (int|float) Ident ...`
    if (cur().kind == TokKind::KwConst || cur().kind == TokKind::KwInt ||
        cur().kind == TokKind::KwFloat) {
      const bool isConst = accept(TokKind::KwConst);
      if (!at(TokKind::KwInt) && !at(TokKind::KwFloat)) {
        expected("'int' or 'float'");
        recoverToStmtEnd();
        continue;
      }
      const BType base = at(TokKind::KwInt) ? BType::Int : BType::Float;
      advance();
      auto decl = parseDeclRest(itemLoc, isConst, base);
      if (decl != nullptr) {
        block->items.push_back(std::move(decl));
      } else {
        recoverToStmtEnd();   // `tensor int a[4];` 在这里被跳过
      }
      continue;
    }

    auto stmt = parseStmt();
    if (stmt != nullptr) block->items.push_back(std::move(stmt));
  }
  if (!accept(TokKind::RBrace)) {
    // 块没有收尾：多半是文件提前结束（或误配的括号）。这里不额外恢复 ——
    // 上层会继续；recoverToStmtEnd 也不会跨过 `}`，所以不会吃掉别的块。
    expected("'}' to close the block");
    if (!at(TokKind::EndOfFile)) recoverToStmtEnd();
  }
  return block;
}

// Cond → LOrExp（语法层与 Exp 相同；`!` 不与 Exp 区分，见文件头注）
std::unique_ptr<Expr> Parser::parseCond() { return parseExp(1); }

// ============================================================================
// 表达式：优先级爬升
//
// 六层二元运算共用这一个循环 —— 用的是**迭代**而不是每层一个递归函数。
// 这样 `a+b+c+...`（86 KB 单行实测约 5000 个运算符）只占常数栈空间。
//
// 左结合的实现要点：右操作数要求 `prec + 1` 起（而不是 `prec`），
// 于是同优先级的运算符不会被右操作数吃掉，而是回到本层循环里
// 变成"左边的树 + 新的右操作数" ⇒ a-b-c == (a-b)-c。
// ============================================================================
std::unique_ptr<Expr> Parser::parseExp(int minPrec, std::unique_ptr<Expr> lhs) {
  DepthGuard guard(*this);
  if (guard.exceeded()) return std::make_unique<IntLit>(cur().loc, std::string_view{});

  // lhs == nullptr ⇒ 从头解析一个一元表达式；否则接在调用方给的前缀后面继续爬
  if (lhs == nullptr) {
    lhs = parseUnary();
    if (lhs == nullptr) return lhs;   // 主表达式失败：错误已报，不再级联刷屏
  }

  while (true) {
    const int prec = binPrec(cur().kind);
    if (prec == 0 || prec < minPrec) break;
    const TokKind op = cur().kind;
    const SourceLoc at = cur().loc;
    advance();
    auto rhs = parseExp(prec + 1);
    if (rhs == nullptr) return lhs;   // 右操作数缺失：保留已建好的左树
    lhs = std::make_unique<Binary>(at, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

// UnaryExp → ('+'|'-'|'!') UnaryExp | PostfixExp
std::unique_ptr<Expr> Parser::parseUnary() {
  DepthGuard guard(*this);
  if (guard.exceeded()) return std::make_unique<IntLit>(cur().loc, std::string_view{});

  if (at(TokKind::Plus) || at(TokKind::Minus) || at(TokKind::Not)) {
    const TokKind op = cur().kind;
    const SourceLoc at = cur().loc;
    advance();
    auto operand = parseUnary();   // 右结合（前缀）⇒ 只有这里递归
    if (operand == nullptr) return std::make_unique<IntLit>(at, std::string_view{});
    return std::make_unique<Unary>(at, op, std::move(operand));
  }
  return parsePostfix();
}

// 后缀：LVal 的下标链 `a[i][j]...`
std::unique_ptr<Expr> Parser::parsePostfix() {
  auto base = parsePrimary();
  if (base == nullptr) return base;

  while (at(TokKind::LBracket)) {
    const SourceLoc at = cur().loc;
    advance();
    auto idx = parseExp(1);
    expect(TokKind::RBracket, "']'");
    if (auto* lv = dynamic_cast<LVal*>(base.get())) {
      lv->indices.push_back(std::move(idx));
    } else {
      // `f(x)[i]`：语义非法（S03 会报），语法上在这里指出位置并停止下标链
      error(at, "cannot apply a subscript here (only variables can be subscripted)");
      break;
    }
  }
  return base;
}

// PrimaryExp → '(' Exp ')' | Number | LVal | Ident '(' [Exp {',' Exp}] ')'
//
// ★ `Ident` 后是不是 `(` 用**一个 token 的前瞻**判定（无回溯）：
//     `f(x)` 是调用，`a[i]` / `a` 是 LVal。见 prompt §7.1 第 25 条。
std::unique_ptr<Expr> Parser::parsePrimary() {
  const SourceLoc loc = cur().loc;

  switch (cur().kind) {
    case TokKind::LParen: {
      advance();
      auto inner = parseExp(1);
      expect(TokKind::RParen, "')'");
      // ⚠️ 括号**不是节点**：`(a)` 与 `a` 产出同一棵树（prompt §7.1 第 19 条）。
      //    这是安全的：括号只影响解析顺序，解析完就已经体现在树形里了。
      if (inner != nullptr) return inner;
      return std::make_unique<IntLit>(loc, std::string_view{});
    }

    case TokKind::IntLit: {
      auto lit = std::make_unique<IntLit>(loc, cur().text);
      parseIntLiteral(*lit);
      advance();
      return lit;
    }

    case TokKind::FloatLit: {
      auto lit = std::make_unique<FloatLit>(loc, cur().text);
      parseFloatLiteral(*lit);
      advance();
      return lit;
    }

    case TokKind::Ident: {
      const std::string name(cur().text);
      advance();
      if (at(TokKind::LParen)) {   // ← 唯一的 2-token 决策点
        auto call = std::make_unique<Call>(loc);
        call->callee = name;
        advance();   // '('
        if (!at(TokKind::RParen)) {
          while (true) {
            auto arg = parseExp(1);
            if (arg == nullptr) break;
            call->args.push_back(std::move(arg));
            if (!accept(TokKind::Comma)) break;
          }
        }
        expect(TokKind::RParen, "')' to close the argument list");
        return call;
      }
      auto lval = std::make_unique<LVal>(loc);
      lval->name = name;
      return lval;
    }

    default:
      break;
  }

  // 出错：报一条诊断并返回 nullptr（调用方据此停止级联，不再刷屏）
  expected("an expression");
  return nullptr;
}

// ============================================================================
// 数值（**辅助值**，不是常量求值；失败时只记 parsed=false + 一条 warning）
// ============================================================================

void Parser::parseIntLiteral(IntLit& lit) {
  const std::string_view s = lit.text;
  if (s.empty()) {
    lit.parsed = false;
    return;
  }
  // 进制：0x/0X → 16；前导 0 且长度 > 1 → 8；其余 → 10
  unsigned base = 10;
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() >= 2 && s[0] == '0') {
    base = 8;
    i = 1;
  }

  uint64_t value = 0;
  bool ok = (i < s.size());
  bool overflow = false;
  for (; ok && i < s.size(); ++i) {
    const char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<unsigned>(c - 'A') + 10;
    } else {
      ok = false;   // 不该出现（词法已切分）；保险起见按不可用处理
      break;
    }
    if (d >= base) {
      ok = false;
      break;
    }
    if (value > 0xFFFFFFFFull / base) {   // 先除后乘，避免 uint64 自身溢出
      overflow = true;
      break;
    }
    value = value * base + d;
    if (value > 0xFFFFFFFFull) {
      overflow = true;
      break;
    }
  }

  if (!ok || overflow) {
    lit.parsed = false;
    lit.value = 0;
    warn(lit.loc, "integer constant '" + std::string(s) +
                      "' does not fit in 32 bits (semantic handling is S04's job)");
    return;
  }
  lit.value = static_cast<uint32_t>(value);
  lit.parsed = true;
}

void Parser::parseFloatLiteral(FloatLit& lit) {
  const std::string_view s = lit.text;
  if (s.empty()) {
    lit.parsed = false;
    return;
  }
  // 词法是"最长匹配、不做事后合理性检查"，所以 `1e5.5` 会切成
  // [FloatLit `1e5`][FloatLit `.5`]，而 `1e5` 这个 token 以 'e' 结尾、没有指数数字。
  // 这种"词法上合法、数值上不完整"的串在这里判定为不可表示（语义留给 S04）。
  const char last = s.back();
  if (last == 'e' || last == 'E' || last == 'p' || last == 'P') {
    lit.parsed = false;
    return;
  }

  const bool hex = s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  const std::string buf = hex ? canonicalHexFloat(s) : std::string(s);

  errno = 0;
  char* end = nullptr;
  const float v = std::strtof(buf.c_str(), &end);
  const bool consumedAll = (end != nullptr && *end == '\0');
  if (!consumedAll || errno == ERANGE) {
    lit.parsed = false;
    lit.value = 0.0f;
    warn(lit.loc, "floating constant '" + std::string(s) +
                      "' cannot be represented exactly (semantic handling is S04's job)");
    return;
  }
  lit.value = v;
  lit.parsed = true;
}

}  // namespace sysy
