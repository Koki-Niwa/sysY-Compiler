// ============================================================================
// AstPrinter.cpp —— S-表达式打印器 + 读取器（实现）
//
// 详细格式说明见 AstPrinter.h 的文件头（含"为什么两者必须严格互逆"）。
// ============================================================================
#include "frontend/AstPrinter.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "support/Diagnostic.h"

namespace sysy {
namespace {

// ============================================================================
// 一、运算符 ⇄ 原文
//
// 打印器用 tokOpText 把 TokKind 变回原文；读取器用 tokFromSymbol 反向还原。
// 两者共用同一张表 —— 这正是"严格互逆"的实现手段：**只有一份真相**。
// ============================================================================
const char* tokOpText(TokKind k) {
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

bool tokFromSymbol(const std::string& s, TokKind& out) {
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

const char* btypeSymbol(BType t) { return t == BType::Int ? ":int" : ":float"; }

// ============================================================================
// 二、打印器
//
// ── 格式模型（先读这一段）──────────────────────────────────────────────
//
//   每个节点占一行，子节点另起一行、缩进 +2。右括号的位置遵循一条规则：
//
//       节点的右括号 ⇒ 落在"它最后一个子节点所在那一行"的末尾
//       叶子的右括号 ⇒ 落在自己那一行末尾
//
//   于是"最深的那个叶子所在的行"会承载沿途所有祖先的右括号 ——
//   这正是 prompt §5 例子里 `(IntLit 3)))))))` 的成因。
//
//   实现：owned_ 是一个"每层自己欠几个右括号"的栈。
//     * 起一行（pushed）：pending_ 归零（新行末尾还没有括号）
//     * 下钻子节点：祖先把自己那一层的账留在栈上（后面用 pushOwned 记上）
//     * 上浮收尾（popFrame → release）：把本层的账并进"当前最后一行"。
//       若那一行已经是更深的一行，就等于把本层的右括号堆到了那一行末尾
//       —— 这正是上面那条规则的实现。
//     * 叶子：没有子节点，所以必须把栈上所有祖先的账一起补在自己这一行
//       （release(openFrame() + 1)）。**叶子忘了收账 = 少右括号 = 往返必失败**。
//
//   ⚠️ 这个形态是**格式契约**：--emit=ast 要能被 --from-ast 重读，重读后再打印
//      必须【逐字节相同】（轨 A）。括号本身就表达了树形，缩进只是可读性，
//      但既然 prompt §5 给了例子，就严格照例子来（test_parser.cpp 钉死这一条）。
//
//   ⚠️ 最容易出不对称的是"有没有子节点"的三处（原型在「空参数列表」
//      「字面量节点」「%」上各栽过一次）：本实现统一走 owned_ 栈，
//      **不再手写 `if (kids.empty())` 分支**，从结构上消掉这类错误。
// ============================================================================
// ============================================================================
// 二、打印器
//
// ── 格式模型（先读这一段）──────────────────────────────────────────────
//
//   每个节点占一行，子节点另起一行、缩进 +2。右括号的位置遵循**两条**规则：
//
//     (a) 叶子的右括号 ⇒ 跟在**自己这一行**的内容后面   `(IntLit 1)`
//     (b) 有子节点的节点 ⇒ 在**它最后一个子节点结束的那一行**、按自己的缩进
//         另起一行放右括号
//
//   (b) 的成因：最后一个子节点结束后，物理行已经停在子树的深处；父节点要收回
//   自己的括号，就得在**那一行之下**、回到自己的缩进位置再写一个 `)`。
//   于是一串祖先的右括号会依次堆在最深处叶子的下面 ——
//   prompt §5 例子最后两行的样子：
//
//          (IntLit 3)
//        )          ← 深度 5 的 `*`
//      )            ← 深度 4 的 `+`   ……
//
//   ⚠️ 这正是原型栽过跟头的地方（prompt §2.2）：**序列化器与反序列化器必须
//      严格互逆**。所以本函数的输出形态被 test_parser.cpp 里"§5 例子逐字节
//      比对"那条断言钉死，任何改动都会立刻暴露。
//
// ── 实现：每个 printXxx 返回"本子树最后一行所在的行号(深度)" ─────────────
//     * -1 表示"没有写任何行"（空子树）
//     * 父节点取所有子节点返回值的最大值
//     * 收尾统一走 emitCloser(end, depth)
//   把"最后一行在哪"显式返回出来，就不用维护"当前行号"这种隐式状态 ——
//   原型在「空参数列表」上出的不对称，本质就是隐式状态漏更新。
// ============================================================================
// ============================================================================
// 二、打印器
//
// ── 格式模型（先读这一段，否则会改错）──────────────────────────────────
//
//   输出 = 先序 DFS，每个节点一行：
//
//       line(depth)  →  换行 + 缩进 2*depth + "(" + 头部
//
//   而右括号**全部堆在最后一行**（`(IntLit 3)))))))`）。
//
//   ── 为什么可以这样（两条观察，缺一不可）──
//
//   ① 先序 DFS 里，右括号的出现顺序**恰好**是左括号的逆序。所以只要按
//      "遇到节点就记一个待补的 `)`"，最后把所有待补的 `)` 一次追加，
//      括号配对与嵌套顺序就是对的 —— 不需要在树上到处插收尾逻辑。
//
//   ② 每行末尾补上的 `)` 属于"从这一行继续下钻时被留在这里的那些祖先"。
//      由于所有 `)` 都在最后一行补，最深的那个叶子所在的行
//      （也就是最后一行）自然承载了全部祖先的收尾。
//
//   ⚠️ 这让实现变得很短，但**这个形态是格式契约**：
//      --emit=ast 的输出要能被 --from-ast 重读，重读后再打印必须
//      【逐字节相同】（轨 A）。所以它被 test_parser.cpp 里"与 prompt §5
//      例子逐字节比对"那条断言钉死 —— 例子长什么样就只能长什么样。
//
//   ⚠️ prompt §2.2 的教训：序列化器与反序列化器必须**严格互逆**。
//      原型在「空参数列表」「字面量节点」「%」三处出过不对称，都是
//      "某些节点多补/少补了一个括号"。本实现把收尾集中到一个地方
//      （deferred 计数），从结构上消掉这类错误。
// ============================================================================
class Printer {
 public:
  std::string run(const CompUnit& unit) {
    out_.reserve(4096);
    mark_ = -1;     // 还没有任何一行
    maxDepth_ = 0;
    deferred_ = 0;
    printNode(unit, 0);
    // ★ 根节点的右括号也落在"最深那一行"（与树内所有节点的规则一致：
    //   括号落在哪一行，只取决于"它最后一个子节点结束在哪一行"）。
    // 若游标已经在那一行（maxDepth_ == mark_），就**不要再换行** —— 否则会
    // 多出一个空白行。只有根节点自身也需要收尾（deferred_ > 0）时才补这一行。
    out_.append(static_cast<size_t>(deferred_), ')');
    out_ += '\n';   // 结尾换行（与 §5 例子逐字节一致）
    return std::move(out_);
  }

 private:
  // 起一行：换行 + 缩进 2*depth。
  // 不做任何"复用行"的优化 —— 每个节点都占独立的一行（格式要求）。
  // ⚠️ 换行之前必须先把"攒在同一行末尾的右括号"落下去 —— 否则它们会被
  //    带到下一行去（括号就配错了位置）。这是本格式里唯一容易漏的一步。
  void line(int depth) {
    if (mark_ >= 0) {
      out_.append(static_cast<size_t>(deferred_), ')');   // ★ 收掉当前行欠的括号
      out_ += '\n';
    }
    deferred_ = 0;
    out_.append(static_cast<size_t>(depth) * 2, ' ');
    mark_ = depth;
    if (depth > maxDepth_) maxDepth_ = depth;
  }

  // ── 收尾（本设计的核心，只有这一处逻辑）────────────────────────────
  // end = 最后一个子节点结束所在的行（-1 = 没有任何子节点）。
  //   * end < 0  → 本节点**没有下钻**：括号就地补在当前行内容后面 `(IntLit 1)`
  //   * end >= 0 → 下钻过了：本节点（以及所有还在递归栈上的祖先）的括号
  //                都要落在 end 那一行 —— 追加到 deferred_，最后统一输出
  // 返回"本子树最后一行"：父节点用它决定自己的括号落在哪一行。
  int emitParens(int end) {
    if (end < 0) {
      out_ += ')';           // 就地补：括号落在**当前行**
      return mark_;
    }
    ++deferred_;             // 延后补：括号落在"最后一行"（= 最深行）
    return maxDepth_ > mark_ ? maxDepth_ : mark_;
  }

  // 所有 printXxx 都返回"本子树最后一行"（-1 = 没有下钻；否则 >= depth）
  int printNode(const Node& n, int depth) { return dispatch(n, depth); }
  int dispatch(const Node& n, int depth);

  int printCompUnit(const CompUnit& n, int depth);
  int printDecl(const Decl& n, int depth);
  int printVarDef(const VarDef& n, int depth);
  int printDim(const Dim& n, int depth);
  int printFuncDef(const FuncDef& n, int depth);
  int printParam(const Param& p, int depth);
  int printInitVal(const InitVal& n, int depth);
  int printBlock(const BlockStmt& n, int depth);
  int printStmt(const Stmt& s, int depth);
  int printExpr(const Expr& e, int depth);

  std::string out_;
  int mark_ = -1;      // 当前行号（-1 = 还没有任何一行）
  int maxDepth_ = 0;   // 已经写出的最深行号（最后一行用它）
  int deferred_ = 0;   // 攒在"最后一行"的右括号总数
};

int Printer::dispatch(const Node& n, int depth) {
  const std::string_view k = n.nodeKind();
  if (k == "CompUnit")  return printCompUnit(static_cast<const CompUnit&>(n), depth);
  if (k == "FuncDef")   return printFuncDef(static_cast<const FuncDef&>(n), depth);
  if (k == "Decl")      return printDecl(static_cast<const Decl&>(n), depth);
  if (k == "VarDef")    return printVarDef(static_cast<const VarDef&>(n), depth);
  if (k == "Dim")       return printDim(static_cast<const Dim&>(n), depth);
  if (k == "InitVal")   return printInitVal(static_cast<const InitVal&>(n), depth);
  if (k == "BlockStmt") return printBlock(static_cast<const BlockStmt&>(n), depth);
  if (k == "AssignStmt" || k == "ExprStmt" || k == "IfStmt" || k == "WhileStmt" ||
      k == "BreakStmt" || k == "ContinueStmt" || k == "ReturnStmt") {
    return printStmt(static_cast<const Stmt&>(n), depth);
  }
  if (k == "LVal" || k == "Call" || k == "Unary" || k == "Binary" ||
      k == "IntLit" || k == "FloatLit") {
    return printExpr(static_cast<const Expr&>(n), depth);
  }
  // 未知节点：照常打印并收尾（少一个括号 = 往返必失败）
  line(depth);
  out_ += "(UnknownNode :";
  out_ += std::string(k);
  return emitParens(-1);
}

// 父节点取"所有子节点返回的最大行号"
namespace {
inline void bump(int& cur, int v) { if (v > cur) cur = v; }
}  // namespace

// ── 顶层 ────────────────────────────────────────────────────────────────
int Printer::printCompUnit(const CompUnit& n, int depth) {
  line(depth);
  out_ += "(CompUnit";
  int end = -1;
  for (const auto& item : n.items) {
    if (item != nullptr) bump(end, printNode(*item, depth + 1));
  }
  return emitParens(end);
}

int Printer::printDecl(const Decl& n, int depth) {
  line(depth);
  out_ += "(Decl";
  if (n.isConst) out_ += " :const";
  out_ += ' ';
  out_ += btypeSymbol(n.base);
  int end = -1;
  for (const auto& d : n.defs) {
    if (d != nullptr) bump(end, printVarDef(*d, depth + 1));
  }
  return emitParens(end);
}

int Printer::printVarDef(const VarDef& n, int depth) {
  line(depth);
  out_ += "(VarDef ";
  out_ += n.name;
  int end = -1;
  for (const Dim& d : n.dims) bump(end, printDim(d, depth + 1));
  if (n.init != nullptr) bump(end, printInitVal(*n.init, depth + 1));
  return emitParens(end);
}

int Printer::printDim(const Dim& n, int depth) {
  line(depth);
  out_ += "(Dim";
  int end = -1;
  if (n.expr != nullptr) end = printNode(*n.expr, depth + 1);
  return emitParens(end);
}

int Printer::printFuncDef(const FuncDef& n, int depth) {
  line(depth);
  out_ += "(FuncDef ";
  out_ += n.name;
  out_ += ' ';
  out_ += n.isVoid ? ":void" : btypeSymbol(n.retType);
  int end = -1;

  // ── 参数表节 `(params ...)` ──
  // **总是**打印成显式的一节（空表也打印 `(params)`）：原型在"空参数列表"
  // 上出过不对称，所以这里不做任何"空就省掉"的优化。
  // 它是 FuncDef 的**第一个子节点**，所以它结束时所在的行就是 FuncDef 的
  // "最后一个子节点结束行"的起点（后面若有函数体再更新）。
  {
    line(depth + 1);
    out_ += "(params";
    int pend = -1;
    for (const Param& p : n.params) bump(pend, printParam(p, depth + 2));
    // 参数节的右括号：空表时就地补 → `(params)`；否则落在最后一个形参那一行
    const int pEnd = emitParens(pend);
    bump(end, pEnd);
    // NOTE: pend>=0 ⇒ 括号记到 deferred_（落在最后一个形参那一行）；
    //       pend<0  ⇒ 就地补在当前行 → `(params)`。
    //
    // ⚠️ 函数体必须另起一行：printBlock 的第一件事就是 line(depth+1)，
    //    它一定会换行，所以 `(params)` 与 `(Block` 不会粘在一起
    //    （S02 踩过 `(params)(Block` —— 那次是因为 line() 有"复用空行"的优化）。
  }

  if (n.body != nullptr) bump(end, printBlock(*n.body, depth + 1));
  else line(depth + 1);

  return emitParens(end);
}

int Printer::printParam(const Param& p, int depth) {
  line(depth);
  out_ += "(Param ";
  out_ += p.name;
  int end = -1;
  for (const Dim& d : p.type.dims) bump(end, printDim(d, depth + 1));
  return emitParens(end);
}

int Printer::printInitVal(const InitVal& n, int depth) {
  line(depth);
  out_ += "(InitVal";
  int end = -1;
  if (n.expr != nullptr) bump(end, printNode(*n.expr, depth + 1));
  for (const auto& e : n.list) {
    if (e != nullptr) bump(end, printInitVal(*e, depth + 1));
  }
  return emitParens(end);
}

int Printer::printBlock(const BlockStmt& n, int depth) {
  line(depth);
  out_ += "(Block";
  int end = -1;
  for (const auto& item : n.items) {
    if (item != nullptr) bump(end, printNode(*item, depth + 1));
  }
  return emitParens(end);
}

// ── 语句 ────────────────────────────────────────────────────────────────
int Printer::printStmt(const Stmt& s, int depth) {
  const std::string_view k = s.nodeKind();

  if (k == "BlockStmt") return printBlock(static_cast<const BlockStmt&>(s), depth);

  line(depth);
  int end = -1;

  if (k == "AssignStmt") {
    const auto& n = static_cast<const AssignStmt&>(s);
    out_ += "(=";
    if (n.lhs != nullptr) bump(end, printExpr(*n.lhs, depth + 1));
    if (n.rhs != nullptr) bump(end, printExpr(*n.rhs, depth + 1));
    return emitParens(end);
  }
  if (k == "ExprStmt") {
    const auto& n = static_cast<const ExprStmt&>(s);
    out_ += "(ExprStmt";
    // 空语句 `;`：只打印节点，不打印 `;`（它不承载信息）
    if (n.expr != nullptr) end = printExpr(*n.expr, depth + 1);
    return emitParens(end);
  }
  if (k == "IfStmt") {
    const auto& n = static_cast<const IfStmt&>(s);
    out_ += "(If";
    if (n.cond != nullptr) bump(end, printExpr(*n.cond, depth + 1));
    if (n.thenS != nullptr) bump(end, printStmt(*n.thenS, depth + 1));
    if (n.elseS != nullptr) {
      // else 用一节显式的 `(Else ...)` 包住 —— 这样"有没有 else"在文本里是
      // **结构上可见**的，不依赖"子节点个数"的巧合（有歧义轨 A 就不成立）。
      line(depth + 1);
      out_ += "(Else";
      const int e = printStmt(*n.elseS, depth + 2);
      bump(end, emitParens(e));
    }
    return emitParens(end);
  }
  if (k == "WhileStmt") {
    const auto& n = static_cast<const WhileStmt&>(s);
    out_ += "(While";
    if (n.cond != nullptr) bump(end, printExpr(*n.cond, depth + 1));
    if (n.body != nullptr) bump(end, printStmt(*n.body, depth + 1));
    return emitParens(end);
  }
  if (k == "BreakStmt")    { out_ += "(Break";    return emitParens(-1); }
  if (k == "ContinueStmt") { out_ += "(Continue"; return emitParens(-1); }
  if (k == "ReturnStmt") {
    const auto& n = static_cast<const ReturnStmt&>(s);
    out_ += "(Return";
    if (n.value != nullptr) end = printExpr(*n.value, depth + 1);
    return emitParens(end);
  }
  out_ += "(UnknownStmt :";
  out_ += std::string(k);
  return emitParens(-1);
}

// ── 表达式 ──────────────────────────────────────────────────────────────
int Printer::printExpr(const Expr& e, int depth) {
  const std::string_view k = e.nodeKind();

  line(depth);
  if (k == "IntLit") {
    const auto& n = static_cast<const IntLit&>(e);
    out_ += "(IntLit ";
    out_.append(n.text.data(), n.text.size());   // ★ 原文，不是重新格式化的数字
    return emitParens(-1);
  }
  if (k == "FloatLit") {
    const auto& n = static_cast<const FloatLit&>(e);
    out_ += "(FloatLit ";
    out_.append(n.text.data(), n.text.size());
    return emitParens(-1);
  }
  int end = -1;
  if (k == "LVal") {
    const auto& n = static_cast<const LVal&>(e);
    out_ += "(LVal ";
    out_ += n.name;
    for (const auto& idx : n.indices) {
      if (idx != nullptr) bump(end, printExpr(*idx, depth + 1));
    }
  } else if (k == "Call") {
    const auto& n = static_cast<const Call&>(e);
    out_ += "(Call ";
    out_ += n.callee;
    for (const auto& arg : n.args) {
      if (arg != nullptr) bump(end, printExpr(*arg, depth + 1));
    }
  } else if (k == "Unary") {
    const auto& n = static_cast<const Unary&>(e);
    out_ += '(';
    out_ += tokOpText(n.op);
    if (n.operand != nullptr) end = printExpr(*n.operand, depth + 1);
  } else if (k == "Binary") {
    const auto& n = static_cast<const Binary&>(e);
    out_ += '(';
    out_ += tokOpText(n.op);
    if (n.lhs != nullptr) bump(end, printExpr(*n.lhs, depth + 1));
    if (n.rhs != nullptr) bump(end, printExpr(*n.rhs, depth + 1));
  } else {
    out_ += "(UnknownExpr :";
    out_ += std::string(k);
  }
  return emitParens(end);
}
// ============================================================================
// 三、S-表达式读取器
//
// 文法（极小，约 100 行）：
//
//   program := form*
//   form    := '(' symbol form* ')'      ; 列表：第一个元素是节点类型
//            | '(' ')'                   ; 空表（本格式里不出现，读进来报"未知节点"）
//   symbol  := 非空白、非括号的任意字节串（节点名 / 运算符 / 标识符 / 字面量 / :记号）
//
// 读取器**不认识 SysY 语法**，只认识这棵括号树 —— 这正是 prompt §6.1 的建议：
// 不要把 AST 文本做成合法 SysY。
// ============================================================================
struct Sexp {
  SourceLoc loc;
  bool isList = false;
  std::string symbol;
  std::vector<Sexp> kids;

  const Sexp* firstSymbol() const {
    return (isList && !kids.empty() && !kids[0].isList) ? &kids[0] : nullptr;
  }
};

class SexpReader {
 public:
  SexpReader(const std::string& text, DiagnosticEngine& diag)
      : text_(text), diag_(diag) {}

  std::vector<Sexp> parseProgram() {
    std::vector<Sexp> out;
    for (;;) {
      skipTrivia();
      if (pos_ >= text_.size()) break;
      Sexp form;
      if (!parseForm(form)) {
        // 恢复：跳到下一个顶层 '('，保证一定前进（不在这里死循环）
        const size_t before = pos_;
        while (pos_ < text_.size() && text_[pos_] != '(') ++pos_;
        if (pos_ == before) ++pos_;
        continue;
      }
      out.push_back(std::move(form));
    }
    return out;
  }

  size_t errorCount() const { return errors_; }

 private:
  // 行号/列号：行首索引**按需建立到 pos 为止**，再做二分。
  // ⚠️ 不要"每个 token 都从头线性扫一遍 lineStart_" —— 那是 O(n²)：
  //    86_long_code2.sy 的 AST 文本有 1.29 亿字节、数百万行，
  //    实测因此从 0.2 s 恶化到 0.67 s（S02 优化过这一点）。
  SourceLoc locAt(size_t pos) {
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

  void error(SourceLoc loc, const std::string& msg) {
    ++errors_;
    diag_.report(DiagLevel::Error, loc, msg);
  }

  void skipTrivia() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' || c == '\n') {
        ++pos_;
        continue;
      }
      return;
    }
  }

  bool parseForm(Sexp& out) {
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
    ++pos_;   // '('

    for (;;) {
      skipTrivia();
      if (pos_ >= text_.size()) {
        error(locAt(pos_), "unexpected end of AST text (unbalanced '(')");
        return false;
      }
      const char c = text_[pos_];
      if (c == ')') {
        ++pos_;
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

  const std::string& text_;
  DiagnosticEngine& diag_;
  size_t pos_ = 0;
  size_t errors_ = 0;
  std::vector<size_t> lineStart_;   // 行首 offset（按需增长）
  size_t lineEnd_ = 0;              // 已经扫到哪个 offset（避免重复扫描）
};

// ============================================================================
// 四、S-表达式 → AST
//
// 每个节点的**子节点个数与顺序由类型唯一决定**（不是靠缩进猜的）。
// 出错时：报诊断 + 就地恢复（能建多少建多少），**永远返回可用的节点**。
// ============================================================================
class Builder {
 public:
  Builder(DiagnosticEngine& diag) : diag_(diag) {}

  // ⚠️ forms 必须**拷进本对象**（或至少活到 AST 用完）：
  //    AST 里 IntLit/FloatLit 的 text 是指向 Sexp::symbol(std::string) 内部的
  //    string_view。若 forms 只是个临时对象，函数一返回这些 view 就悬垂
  //    —— 实测表现是"字面量原文随机变成空串"（round-trip 大面积不相等）。
  //    所以 Builder 按值持有 forms，并在 run() 之后仍保留它。
  std::unique_ptr<CompUnit> run(std::vector<Sexp> forms) {
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

  size_t errorCount() const { return errors_; }

 private:
  void error(SourceLoc loc, const std::string& msg) {
    ++errors_;
    diag_.report(DiagLevel::Error, loc, msg);
  }

  static const Sexp* at(const Sexp& f, size_t i) {
    return (f.isList && i < f.kids.size()) ? &f.kids[i] : nullptr;
  }

  // 取第 i 个子节点并断言它是符号；失败返回空串（并报错）
  std::string expectSymbol(const Sexp& f, size_t i, const char* what) {
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

  static void setLoc(Node& n, SourceLoc loc) { n.loc = loc; }

  // ── 顶层项 ────────────────────────────────────────────────────────────
  std::unique_ptr<Node> buildItem(const Sexp& f) {
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

  std::unique_ptr<Decl> buildDecl(const Sexp& f) {
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

  std::unique_ptr<VarDef> buildVarDef(const Sexp& f) {
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

  // 注意：Dim 内含 unique_ptr，不可拷贝 ⇒ 返回**值**（移动构造）
  Dim buildDim(const Sexp& f) {
    Dim d(f.loc);
    if (f.kids.size() > 1) {
      d.expr = buildExpr(f.kids[1]);
    } else if (f.kids.size() == 0) {
      error(f.loc, "malformed Dim node");
    }
    return d;
  }

  std::unique_ptr<InitVal> buildInitVal(const Sexp& f) {
    auto iv = std::make_unique<InitVal>();
    setLoc(*iv, f.loc);
    // 与打印器严格对称：
    //   0 个子节点      → `{}`（空列表）
    //   子节点是 InitVal → 列表形式（扁平，逐个搬过来）
    //   否则            → 标量形式（那唯一一个子节点就是表达式）
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

  std::unique_ptr<FuncDef> buildFuncDef(const Sexp& f) {
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

  Param buildParam(const Sexp& f) {
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

  std::unique_ptr<BlockStmt> buildBlock(const Sexp& f) {
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

  // ── 语句 ──────────────────────────────────────────────────────────────
  std::unique_ptr<Stmt> buildStmt(const Sexp& f) {
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

  // ── 表达式 ────────────────────────────────────────────────────────────
  std::unique_ptr<Expr> buildExpr(const Sexp& f) {
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
    // ── 运算符节点 ──
    // ⚠️ 打印器输出的**就是运算符原文**（prompt §5："运算符用其原文"），
    //    所以这里不能去找 "Binary"/"Unary" 这样的节点名，而要把 head 当成
    //    运算符符号去认。元数由子节点个数决定（1 = 一元、2 = 二元）。
    //    这条曾经写错过：既打印 `(+ ...)` 又去匹配 "Binary"，导致
    //    --from-ast 对任何算术表达式都报 "unknown expression node '+'"
    //    —— 正是轨 A 存在的意义（printer 与 reader 不互逆会被立刻抓到）。
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

  // 字面量原文：`(IntLit 1)` 的第 2 个子节点（若缺，报错并返回空串）
  // 返回**副本**（不是 view）：字面量节点的 text 必须是自有的，
  // 否则 AST 的生命期就被绑死在 S-表达式缓冲上（S02 实测踩过这个悬垂）。
  std::string textOf(const Sexp& f, const char* what) {
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

  // 与 Parser::parseIntLiteral 等价的辅助值换算（**不参与往返**，只供 S03 使用）
  void parseIntText(IntLit& lit) {
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

  void parseFloatText(FloatLit& lit) {
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

  DiagnosticEngine& diag_;
  std::vector<Sexp> forms_;   // ★ 拥有 S-表达式树（字面量 text 的 string_view 指向它）
  size_t errors_ = 0;
};

}  // namespace

// ============================================================================
// 对外接口
// ============================================================================

std::string printAst(const CompUnit& unit) {
  Printer p;
  return p.run(unit);
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
