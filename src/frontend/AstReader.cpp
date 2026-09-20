// ============================================================================
// AstReader.cpp —— S-表达式读取器（实现）：文本 → Sexp 树 → AST
// 接口、深度防护与"与打印器不对称"的说明都在 AstReader.h（对外接口在
// AstPrinter.h）；本文件原与打印器同在 AstPrinter.cpp。
// ============================================================================
#include "frontend/AstReader.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "frontend/AstPrinter.h"      // parseAstText 的声明（对外接口）
#include "frontend/AstSexpFormat.h"   // tokFromSymbol：只有一份真相的记号表
#include "support/Diagnostic.h"

namespace sysy {

std::vector<Sexp> SexpReader::parseProgram() {
  std::vector<Sexp> out;
  for (;;) {
    skipTrivia();
    if (pos_ >= text_.size()) break;
    Sexp form;
    if (!parseForm(form)) {
      const size_t before = pos_;
      while (pos_ < text_.size() && text_[pos_] != '(') ++pos_;
      if (pos_ == before) ++pos_;
      continue;
    }
    out.push_back(std::move(form));
  }
  return out;
}

SourceLoc SexpReader::locAt(size_t pos) {
  while (lineEnd_ < pos || lineStart_.empty()) {
    if (lineStart_.empty()) {
      lineStart_.push_back(0);
      lineEnd_ = 0;
    }
    const size_t nl = text_.find('\n', lineEnd_);
    if (nl == std::string::npos) {
      lineEnd_ = text_.size();
      break;
    }
    if (nl >= pos) {
      lineEnd_ = nl;
      break;
    }
    lineStart_.push_back(nl + 1);
    lineEnd_ = nl + 1;
  }
  size_t lo = 0, hi = lineStart_.size();
  while (lo + 1 < hi) {          // 找最后一个 <= pos 的行首
    const size_t mid = (lo + hi) / 2;
    if (lineStart_[mid] <= pos) lo = mid; else hi = mid;
  }
  return SourceLoc(static_cast<uint32_t>(lo + 1),
                   static_cast<uint32_t>(pos - lineStart_[lo] + 1));
}

void SexpReader::error(SourceLoc loc, const std::string& msg) {
  ++errors_;
  diag_.report(DiagLevel::Error, loc, msg);
}

void SexpReader::skipTrivia() {
  while (pos_ < text_.size()) {
    const char c = text_[pos_];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' || c == '\n') {
      ++pos_;
      continue;
    }
    return;
  }
}

bool SexpReader::parseForm(Sexp& out) {
  DepthGuard guard(*this);
  if (depthExceeded()) return false;   // 已报诊断 + 已迭代跳过本节点（见头文件）

  skipTrivia();
  if (pos_ >= text_.size()) {
    error(locAt(pos_), "unexpected end of AST text");
    return false;
  }
  if (text_[pos_] != '(') {
    error(locAt(pos_), "expected '(' to start a node, but found '" +
                           std::string(1, text_[pos_]) + "'");
    return false;
  }
  out.loc = locAt(pos_);
  out.isList = true;
  ++pos_; ++openDepth_;   // '('

  for (;;) {
    skipTrivia();
    if (pos_ >= text_.size()) {
      error(locAt(pos_), "unexpected end of AST text (unbalanced '(')");
      return false;
    }
    const char c = text_[pos_];
    if (c == ')') {
      ++pos_; --openDepth_;
      return true;
    }
    if (c == '(') {
      Sexp kid;
      if (!parseForm(kid)) return false;
      out.kids.push_back(std::move(kid));
      continue;
    }
    // 符号：读到空白或括号为止
    const size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] != '(' && text_[pos_] != ')' &&
           text_[pos_] != ' ' && text_[pos_] != '\t' && text_[pos_] != '\n' &&
           text_[pos_] != '\r' && text_[pos_] != '\v' && text_[pos_] != '\f') {
      ++pos_;
    }
    Sexp kid;
    kid.loc = locAt(start);
    kid.isList = false;
    kid.symbol = text_.substr(start, pos_ - start);
    out.kids.push_back(std::move(kid));
  }
}

void SexpReader::skipRestOfForm() {
  // ⚠️ 要跳到【本顶层 form 的末尾】，而不是"当前这一个节点"的末尾：
  //    返回 false 会冒泡到 parseProgram 的顶层循环，剩下的祖先右括号会被当成
  //    "节点头"，刷出成百上千条假诊断（实测 9000 层的链刷了 1900+ 条）。
  //    openDepth_ = 尚未闭合的 `(` 个数，归零即回到顶层边界 ⇒ 只报一条。
  while (pos_ < text_.size() && openDepth_ > 0) {
    const char c = text_[pos_++];
    if (c == '(') ++openDepth_;
    else if (c == ')') --openDepth_;
  }
}

// ── 三、Sexp 树 → AST ─────────────────────────────────────────────────────

std::unique_ptr<CompUnit> Builder::run(std::vector<Sexp> forms) {
  forms_ = std::move(forms);
  auto unit = std::make_unique<CompUnit>();
  for (const Sexp& f : forms_) {
    if (!f.isList) {
      error(f.loc, "expected a top-level node list, but found symbol '" + f.symbol + "'");
      continue;
    }
    const Sexp* head = f.firstSymbol();
    if (head == nullptr) {
      error(f.loc, "empty node list at top level");
      continue;
    }
    if (head->symbol == "CompUnit") {
      unit->loc = f.loc;
      for (size_t i = 1; i < f.kids.size(); ++i) {
        auto item = buildItem(f.kids[i]);
        if (item != nullptr) unit->items.push_back(std::move(item));
      }
    } else {
      auto item = buildItem(f);
      if (item != nullptr) unit->items.push_back(std::move(item));
    }
  }
  return unit;
}

void Builder::error(SourceLoc loc, const std::string& msg) {
  ++errors_;
  diag_.report(DiagLevel::Error, loc, msg);
}

std::string Builder::expectSymbol(const Sexp& f, size_t i, const char* what) {
  const Sexp* k = at(f, i);
  if (k == nullptr) {
    error(f.loc, std::string("malformed node: missing ") + what);
    return std::string();
  }
  if (k->isList) {
    error(k->loc, std::string("malformed node: expected ") + what +
                      ", but found a nested node");
    return std::string();
  }
  return k->symbol;
}

std::unique_ptr<Node> Builder::buildItem(const Sexp& f) {
  const Sexp* head = f.firstSymbol();
  if (head == nullptr) {
    error(f.loc, "malformed node: empty list");
    return nullptr;
  }
  if (head->symbol == "Decl")     return buildDecl(f);
  if (head->symbol == "FuncDef")  return buildFuncDef(f);
  error(f.loc, "unknown top-level node '" + head->symbol + "'");
  return nullptr;
}

std::unique_ptr<Decl> Builder::buildDecl(const Sexp& f) {
  auto d = std::make_unique<Decl>();
  setLoc(*d, f.loc);
  size_t i = 1;
  if (const Sexp* k = at(f, i); k != nullptr && !k->isList && k->symbol == ":const") {
    d->isConst = true;
    ++i;
  }
  const std::string ty = expectSymbol(f, i, "a type marker (:int or :float)");
  ++i;
  if (ty == ":int") {
    d->base = BType::Int;
  } else if (ty == ":float") {
    d->base = BType::Float;
  } else {
    error(f.loc, "malformed Decl: expected ':int' or ':float', but found '" + ty + "'");
    d->base = BType::Int;
  }
  for (; i < f.kids.size(); ++i) {
    auto v = buildVarDef(f.kids[i]);
    if (v != nullptr) d->defs.push_back(std::move(v));
  }
  return d;
}

std::unique_ptr<VarDef> Builder::buildVarDef(const Sexp& f) {
  const Sexp* head = f.firstSymbol();
  if (head == nullptr || head->symbol != "VarDef") {
    error(f.loc, "malformed Decl: expected a (VarDef ...) child");
    return nullptr;
  }
  auto v = std::make_unique<VarDef>();
  setLoc(*v, f.loc);
  v->name = expectSymbol(f, 1, "a variable name");
  for (size_t i = 2; i < f.kids.size(); ++i) {
    const Sexp& k = f.kids[i];
    const Sexp* h = k.firstSymbol();
    if (h != nullptr && h->symbol == "Dim") {
      v->dims.push_back(buildDim(k));
    } else if (h != nullptr && h->symbol == "InitVal") {
      v->init = buildInitVal(k);
    } else {
      error(k.loc, "malformed VarDef: unexpected child");
    }
  }
  return v;
}

Dim Builder::buildDim(const Sexp& f) {
  Dim d(f.loc);
  if (f.kids.size() > 1) {
    d.expr = buildExpr(f.kids[1]);
  } else if (f.kids.size() == 0) {
    error(f.loc, "malformed Dim node");
  }
  return d;
}

std::unique_ptr<InitVal> Builder::buildInitVal(const Sexp& f) {
  auto iv = std::make_unique<InitVal>();
  setLoc(*iv, f.loc);
  // 三种形态的判据见 AstReader.h 的声明处（与打印器严格对称）
  if (f.kids.size() <= 1) return iv;
  const Sexp* first = at(f, 1);
  const Sexp* fh = (first != nullptr) ? first->firstSymbol() : nullptr;
  const bool isList = (fh != nullptr && fh->symbol == "InitVal");
  if (!isList) {
    if (f.kids.size() > 2) {
      error(f.loc, "malformed InitVal: a scalar initializer cannot have siblings");
    }
    iv->expr = buildExpr(f.kids[1]);
    return iv;
  }
  for (size_t i = 1; i < f.kids.size(); ++i) {
    const Sexp& k = f.kids[i];
    const Sexp* h = k.firstSymbol();
    if (h == nullptr || h->symbol != "InitVal") {
      error(k.loc, "malformed InitVal: expected a nested (InitVal ...) child");
      continue;
    }
    iv->list.push_back(buildInitVal(k));
  }
  return iv;
}

std::unique_ptr<FuncDef> Builder::buildFuncDef(const Sexp& f) {
  auto fn = std::make_unique<FuncDef>();
  setLoc(*fn, f.loc);
  fn->name = expectSymbol(f, 1, "a function name");
  const std::string ret = expectSymbol(f, 2, "a return type marker");
  if (ret == ":void") {
    fn->isVoid = true;
    fn->retType = BType::Int;
  } else if (ret == ":int") {
    fn->retType = BType::Int;
  } else if (ret == ":float") {
    fn->retType = BType::Float;
  } else {
    error(f.loc, "malformed FuncDef: unexpected return type '" + ret + "'");
  }

  for (size_t i = 3; i < f.kids.size(); ++i) {
    const Sexp& k = f.kids[i];
    const Sexp* h = k.firstSymbol();
    if (h == nullptr) {
      error(k.loc, "malformed FuncDef: unexpected child");
      continue;
    }
    if (h->symbol == "params") {
      for (size_t j = 1; j < k.kids.size(); ++j) {
        fn->params.push_back(buildParam(k.kids[j]));
      }
      continue;
    }
    if (h->symbol == "Block") {
      fn->body = buildBlock(k);
      continue;
    }
    error(k.loc, "malformed FuncDef: unexpected child '" + h->symbol + "'");
  }
  if (fn->body == nullptr) {
    error(f.loc, "malformed FuncDef: missing function body");
    fn->body = std::make_unique<BlockStmt>();
  }
  return fn;
}

Param Builder::buildParam(const Sexp& f) {
  Param p(f.loc);
  p.type.isFuncParam = true;
  const Sexp* head = f.firstSymbol();
  if (head == nullptr || head->symbol != "Param") {
    error(f.loc, "malformed params: expected a (Param ...) child");
    return p;
  }
  p.name = expectSymbol(f, 1, "a parameter name");
  for (size_t i = 2; i < f.kids.size(); ++i) {
    const Sexp& k = f.kids[i];
    const Sexp* h = k.firstSymbol();
    if (h != nullptr && h->symbol == "Dim") {
      p.type.dims.push_back(buildDim(k));
    } else {
      error(k.loc, "malformed Param: unexpected child");
    }
  }
  return p;
}

std::unique_ptr<BlockStmt> Builder::buildBlock(const Sexp& f) {
  auto b = std::make_unique<BlockStmt>();
  setLoc(*b, f.loc);
  for (size_t i = 1; i < f.kids.size(); ++i) {
    const Sexp& k = f.kids[i];
    const Sexp* h = k.firstSymbol();
    if (h == nullptr) {
      error(k.loc, "malformed Block: unexpected child");
      continue;
    }
    if (h->symbol == "Decl") {
      b->items.push_back(buildDecl(k));
      continue;
    }
    auto s = buildStmt(k);
    if (s != nullptr) b->items.push_back(std::move(s));
  }
  return b;
}

std::unique_ptr<Stmt> Builder::buildStmt(const Sexp& f) {
  const Sexp* head = f.firstSymbol();
  if (head == nullptr) {
    error(f.loc, "malformed statement: empty list");
    return nullptr;
  }
  const std::string& s = head->symbol;

  if (s == "Block") return buildBlock(f);

  if (s == "=") {
    auto st = std::make_unique<AssignStmt>();
    setLoc(*st, f.loc);
    if (f.kids.size() != 3) {
      error(f.loc, "malformed assignment: expected (lhs rhs)");
    }
    if (const Sexp* a = at(f, 1)) {
      auto e = buildExpr(*a);
      if (auto* lv = dynamic_cast<LVal*>(e.get())) {
        lv->isAssignTarget = true;
        st->lhs.reset(lv);
        e.release();
      } else if (e != nullptr) {
        error(a->loc, "malformed assignment: left side is not an lvalue");
      }
    }
    if (const Sexp* b2 = at(f, 2)) st->rhs = buildExpr(*b2);
    return st;
  }

  if (s == "ExprStmt") {
    auto st = std::make_unique<ExprStmt>();
    setLoc(*st, f.loc);
    if (f.kids.size() > 1) st->expr = buildExpr(f.kids[1]);
    return st;
  }

  if (s == "If") {
    auto st = std::make_unique<IfStmt>();
    setLoc(*st, f.loc);
    if (const Sexp* c = at(f, 1)) st->cond = buildExpr(*c);
    if (const Sexp* t = at(f, 2)) st->thenS = buildStmt(*t);
    if (const Sexp* e = at(f, 3)) {
      const Sexp* eh = e->firstSymbol();
      if (eh != nullptr && eh->symbol == "Else") {
        if (const Sexp* eb = at(*e, 1)) st->elseS = buildStmt(*eb);
      } else {
        st->elseS = buildStmt(*e);   // 宽容：也接受不带 (Else ...) 包裹的写法
      }
    }
    return st;
  }

  if (s == "While") {
    auto st = std::make_unique<WhileStmt>();
    setLoc(*st, f.loc);
    if (const Sexp* c = at(f, 1)) st->cond = buildExpr(*c);
    if (const Sexp* b2 = at(f, 2)) st->body = buildStmt(*b2);
    return st;
  }

  if (s == "Break") {
    auto st = std::make_unique<BreakStmt>();
    setLoc(*st, f.loc);
    return st;
  }
  if (s == "Continue") {
    auto st = std::make_unique<ContinueStmt>();
    setLoc(*st, f.loc);
    return st;
  }
  if (s == "Return") {
    auto st = std::make_unique<ReturnStmt>();
    setLoc(*st, f.loc);
    if (f.kids.size() > 1) st->value = buildExpr(f.kids[1]);
    return st;
  }

  error(f.loc, "unknown statement node '" + s + "'");
  return nullptr;
}

std::unique_ptr<Expr> Builder::buildExpr(const Sexp& f) {
  const Sexp* head = f.firstSymbol();
  if (head == nullptr) {
    error(f.loc, "malformed expression: empty list");
    return nullptr;
  }
  const std::string& s = head->symbol;

  if (s == "IntLit") {
    auto e = std::make_unique<IntLit>();
    setLoc(*e, f.loc);
    e->text = textOf(f, "IntLit");
    e->parsed = false;   // 由下面的数值换算决定（辅助值，不参与打印）
    parseIntText(*e);
    return e;
  }
  if (s == "FloatLit") {
    auto e = std::make_unique<FloatLit>();
    setLoc(*e, f.loc);
    e->text = textOf(f, "FloatLit");
    e->parsed = false;
    parseFloatText(*e);
    return e;
  }
  if (s == "LVal") {
    auto e = std::make_unique<LVal>();
    setLoc(*e, f.loc);
    e->name = expectSymbol(f, 1, "an identifier");
    for (size_t i = 2; i < f.kids.size(); ++i) {
      e->indices.push_back(buildExpr(f.kids[i]));
    }
    return e;
  }
  if (s == "Call") {
    auto e = std::make_unique<Call>();
    setLoc(*e, f.loc);
    e->callee = expectSymbol(f, 1, "a callee name");
    for (size_t i = 2; i < f.kids.size(); ++i) {
      e->args.push_back(buildExpr(f.kids[i]));
    }
    return e;
  }
  // 运算符节点：把 head 当运算符原文认（为什么，见 AstReader.h 的声明处）
  TokKind op = TokKind::Plus;
  if (tokFromSymbol(s, op)) {
    if (f.kids.size() == 2) {   // 一元
      auto e = std::make_unique<Unary>();
      setLoc(*e, f.loc);
      e->op = op;
      e->operand = buildExpr(f.kids[1]);
      return e;
    }
    if (f.kids.size() == 3) {   // 二元
      auto e = std::make_unique<Binary>();
      setLoc(*e, f.loc);
      e->op = op;
      e->lhs = buildExpr(f.kids[1]);
      e->rhs = buildExpr(f.kids[2]);
      return e;
    }
    error(f.loc, "operator '" + s + "' has " + std::to_string(f.kids.size() - 1) +
                     " operands (expected 1 or 2)");
    return nullptr;
  }

  error(f.loc, "unknown expression node '" + s + "'");
  return nullptr;
}

std::string Builder::textOf(const Sexp& f, const char* what) {
  const Sexp* k = at(f, 1);
  if (k == nullptr) {
    error(f.loc, std::string("malformed ") + what + ": missing literal text");
    return std::string();
  }
  if (k->isList) {
    error(k->loc, std::string("malformed ") + what + ": expected literal text");
    return std::string();
  }
  return k->symbol;
}

void Builder::parseIntText(IntLit& lit) {
  const std::string_view s = lit.text;
  if (s.empty()) return;
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
  for (; i < s.size(); ++i) {
    const char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<unsigned>(c - 'A') + 10;
    } else {
      return;
    }
    if (d >= base || value > 0xFFFFFFFFull / base) return;
    value = value * base + d;
    if (value > 0xFFFFFFFFull) return;
  }
  lit.value = static_cast<uint32_t>(value);
  lit.parsed = true;
}

void Builder::parseFloatText(FloatLit& lit) {
  const std::string_view s = lit.text;
  if (s.empty()) return;
  const char last = s.back();
  if (last == 'e' || last == 'E' || last == 'p' || last == 'P') return;
  std::string buf;
  const bool hex = s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  if (hex) {
    buf.assign(s.data(), s.size());
    bool hasP = false;
    for (char c : buf) {
      if (c == 'p' || c == 'P') hasP = true;
    }
    if (!hasP) buf += "p0";
  } else {
    buf.assign(s.data(), s.size());
  }
  char* end = nullptr;
  const float v = std::strtof(buf.c_str(), &end);
  if (end == nullptr || *end != '\0') return;
  lit.value = v;
  lit.parsed = true;
}

std::unique_ptr<CompUnit> parseAstText(const std::string& text, DiagnosticEngine& diag) {
  // 文件名不进这里：诊断渲染走 DiagnosticEngine 的 SourceFile（与 .sy 完全一致）
  SexpReader reader(text, diag);
  Builder builder(diag);
  auto unit = builder.run(reader.parseProgram());
  if (unit->loc.line == 0) unit->loc = SourceLoc(1, 1);
  return unit;
}

}  // namespace sysy
