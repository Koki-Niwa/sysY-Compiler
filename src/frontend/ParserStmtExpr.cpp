// ============================================================================
// ParserStmtExpr.cpp —— SysY 语法分析器（S02）：语句与表达式
//
// 本文件是 Parser 的**后一半**：语句（parseStmt / parseBlock / parseCond）、
// 表达式（优先级爬升 parseExp + parseUnary / parsePostfix / parsePrimary）、
// 以及字面量的辅助值换算（parseIntLiteral / parseFloatLiteral）。
// 前一半（构造与 token 访问、诊断、错误恢复、深度防护、顶层、声明）在 Parser.cpp；
// 两半共用同一个类声明 Parser.h，拆分为**纯移动**。
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
// ── ⚠️ parseExp 的循环建树与 Parser.h 的 kMaxDepth 是**隐式耦合**的 ──────
//   深度计数 depth_ 在 parseExp 入口只 +1（不随二元链的层数增长），所以
//   `1+1+…+1` 这种 6 万项的链既不会撞上 kMaxDepth、也不占调用栈。
//   语料 86_long_code2.sy 的树深 4007 > kMaxDepth=4000 却仍然合法，唯一
//   原因就是这条。**不要把二元层改成"每层一个递归函数"** —— 那会立刻把这
//   个合法用例判成"嵌套过深"，即改变能接受的程序集合。详见 Parser.h。
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
#include <string_view>
#include <utility>

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
// 语句
// ============================================================================
std::unique_ptr<Stmt> Parser::parseStmt() {
  DepthGuard guard(*this);
  if (depthExceeded()) {
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
  if (depthExceeded()) return std::make_unique<IntLit>(cur().loc, std::string_view{});

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
  if (depthExceeded()) return std::make_unique<IntLit>(cur().loc, std::string_view{});

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

  bool ok = (i < s.size());
  // ★ 溢出**回绕**，与权威实现（`ConstEvaluator::parseIntLit`）**完全同一套语义**。
  //   为什么不报"超出 32 位"：本项目对 int 的既定策略是"32 位二进制补码、溢出回绕、
  //   不产生 UB"（见 AGENT-CONTEXT 铁律 6）。字面量只是常量，不会因为写在源码里
  //   就比算出来的常量更特殊；而且回绕是**跨目标确定**的，正合跨目标一致性的要求。
  //   ⚠️ 曾经这里用 uint64 累加并在超 2^32-1 时判 overflow、置 parsed=false 并报警告，
  //   而 S03 的权威实现是回绕——两层对同一个字面量给出不同判断，警告还指错了原因
  //   （把 `09` 这种"非法数字"也说成"超出 32 位范围"）。**两层必须只有一套语义。**
  uint32_t acc = 0;
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
      ok = false;   // `09`：八进制里没有数字 9
      break;
    }
    acc = acc * static_cast<uint32_t>(base) + static_cast<uint32_t>(d);   // 回绕
  }

  if (!ok) {
    lit.parsed = false;
    lit.value = 0;
    // 只有"非法数字"才走到这里（`09` / `0x` / `0xG`）。消息必须说对原因：
    // 比赛要评"编译错误的准确定位与描述"，指错原因的诊断等于没有诊断。
    warn(lit.loc, "invalid digit in integer constant '" + std::string(s) +
                      "' (base " + std::to_string(base) + ")");
    return;
  }
  lit.value = acc;
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
