// ============================================================================
// AstSexpLayout.h —— 打印布局的**唯一实现**（内部头，不对外公开）
//
// ── 为什么要有这个文件（prompt §一、§六 第 4 条）─────────────────────────
//   `--emit=sema` 的格式是 `--emit=ast` 的**加法**：S02 已经打印的东西
//   **一个都不改**，只多出类型注解、`Cast` 节点与 `(RuntimeLib ...)` 头。
//   验收里有一条**机械判据**（去注解还原）：
//
//       把 --emit=sema 的输出 ① 删掉 (RuntimeLib ...) 头
//                            ② 删掉所有 :t / :obj / :<类型> 记号
//                            ③ 把 (Cast <kind> <目标类型> <子节点> :<类型>)
//                               换成它的 <子节点>
//       得到的文本必须与 --emit=ast 的输出**逐字节相同**。
//
//   如果 SemaDump 自己再写一份布局逻辑，任何缩进/括号位置/记号拼写的偏差
//   都会立刻暴露 —— 而那正是"两份真相"这类隐性缺陷的典型症状。
//   ⇒ 布局只有这一份：`SexpLayout<Anno>` 模板，`Anno` 只负责**追加注解**，
//     不参与任何换行/缩进/括号决策。`--emit=ast` 用 `NoAnno`（全空实现），
//     `--emit=sema` 用 `SemaAnno`。于是"加法"是**结构上**成立的，不是靠自觉。
//
// ── 格式模型（先读这一段，否则会改错）──────────────────────────────────
//   输出 = 先序 DFS，每个节点占一行：
//       line(depth)  →  换行 + 缩进 2*depth + "(" + 头部
//   (a) 叶子（没有下钻过的节点）的右括号 ⇒ 跟在**自己这一行**的内容后面
//   (b) 有子节点的节点 ⇒ 它自己的 `)` 与"还在栈上的祖先们"的 `)` 一起，
//       全部堆在**最后一行（= 最深那一行）的末尾**
//
//   ★ S03 的注解全部是**就地插入的记号**（prompt §五）：只在**已有的行**上
//      插入 `:t` / `:obj` / `:<类型>`，**绝不**因为加记号而把子节点搬到另一行、
//      改变缩进、或挪动括号位置。⇒ `closeNode` / `line()` / `deferred_` 的
//      逻辑与 S02 **逐字节相同**，本模板对 `--emit=ast` 而言是恒等变换。
//      （这正是"去注解还原"能成立的结构性原因：还原 = 删掉那些记号，
//        行的划分与括号的落位根本没被碰过。）
//
// ── 遍历必须是**迭代**的（不要把这里改回递归！）────────────────────────
//   语料实测：86_long_code2.sy 的打印树深 4007；而 `1+1+…`（6 万个 `+`）
//   在递归版下 rc=139（SIGSEGV）。"不崩"是硬验收要求，所以用显式工作栈，
//   内存 O(树深)。详见 S02 报告 §3.1 bug#7 与 AstPrinter.h 的历史注记。
// ============================================================================
#ifndef SYSY_FRONTEND_ASTSEXPLAYOUT_H
#define SYSY_FRONTEND_ASTSEXPLAYOUT_H

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/AstSexpFormat.h"

namespace sysy {
namespace sexp {

// ============================================================================
// NoAnno —— `--emit=ast` 的注解策略：**全部空实现**
//   它必须让 Layout 的输出与 S02 时期逐字节相同（check_ast_baseline.py 是门禁）。
// ============================================================================
struct NoAnno {
  static void preamble(std::string&) {}
  static void varDefHead(std::string&, const VarDef&) {}
  static void paramHead(std::string&, const Param&) {}
  static void lvalHead(std::string&, const LVal&) {}
  static void valueType(std::string&, const Expr&) {}
  static void castHead(std::string&, const Cast&) {}
};

// ============================================================================
// SexpLayout —— 布局引擎（迭代先序；`Anno` 只追加注解）
// ============================================================================
template <class Anno>
class SexpLayout {
 public:
  // 单行模式（**S04 增加，默认关闭**）：把"换行 + 缩进"换成"一个空格"，
  // 于是同一棵树被折叠成一行。用途只有一个 —— `--emit=initplan` 里
  // `StoreExpr` 的动作行（§4.3）。**默认关闭 ⇒ `--emit=ast`/`--emit=sema`
  // 的输出一个字节都不会变**（这两个 emit 的既有路径从不调用它）。
  void setSingleLine(bool on) { singleLine_ = on; }

  std::string run(const CompUnit& unit) {
    reset();
    Anno::preamble(out_);   // `--emit=sema` 的 `(RuntimeLib ...)` 头（ast 为空实现）
    runRoot(unit);
    flushPending();
    out_ += '\n';           // 结尾换行（与 §五 例子逐字节一致）
    return std::move(out_);
  }

  // ── ★ S04 增加：只打印一棵**子树的正文**，供 `--emit=initplan` 复用 ──────
  //   为什么必须复用而不是自己写一份：prompt §4.3 要求 `StoreExpr` 的子节点
  //   "沿用 `--emit=sema` 的表达式格式（同一个打印器、同一套类型记号）"。
  //   于是这里的输出与 `--emit=sema` 里同一棵子树的正文**逐字节相同**，
  //   包括类型注解 —— 这份"同一性"由单元测试机械核对。
  //
  //   【调用方契约】返回的文本**是一个不完整的片段**：
  //     * 不含 `Anno::preamble`
  //     * 不含节点头与它自己的类型注解（调用方按输出顺序自己写，顺序见 §4.3）
  //     * **含**节点自己的右括号，以及所有子节点的完整文本（含结尾换行）
  //     调用方直接 append，不要自己补行首缩进（布局已经带了）。
  //   【后置】`setSingleLine(true)` 时整棵子树折叠成一行（**没有**结尾换行）。
  std::string runExpr(const Expr& e) {
    reset();
    out_.clear();           // 丢弃 preamble（本函数只返回子树正文）
    runRoot(e);
    flushPending();
    return std::move(out_);
  }

 private:
  void reset() {
    out_.clear();
    mark_ = -1;             // 还没有任何一行
    deferred_ = 0;
    stack_.clear();
  }

  // 打开根节点并跑完整个工作栈（不收尾 —— 收尾由调用方决定加不加换行）。
  void runRoot(const Node& root) {
    pushNode(root, 0);
    while (!stack_.empty()) {
      // ⚠️ 必须**按值**取帧再 pop：下面几乎每个分支都会往 stack_ 里压东西，
      //    而 std::vector 扩容会让任何指向元素的引用/指针失效。
      const Frame f = stack_.back();
      stack_.pop_back();
      switch (f.step) {
        case Step::OpenNode:   openNode(*f.node, f.depth); break;
        case Step::CloseNode:  closeNode(f.depth); break;
        case Step::OpenParams: openParams(*static_cast<const FuncDef*>(f.node), f.depth); break;
        case Step::OpenParam:  openParam(*f.param, f.depth); break;
        case Step::OpenElse:   openElse(*static_cast<const Stmt*>(f.node), f.depth); break;
        case Step::EmptyLine:  line(f.depth); break;
      }
    }
  }

  enum class Step {
    OpenNode,    // 打开一个 AST 节点（node 有效）
    CloseNode,   // 给一个节点收账（depth/node 有效）
    OpenParams,  // 参数节 `(params ...)`（node = 它所属的 FuncDef）
    OpenParam,   // 一个形参（param 有效；Param 不是 Expr/Stmt 那类节点，单独走）
    OpenElse,    // if 的 `(Else ...)` 节（node = else 分支的语句）
    EmptyLine,   // 只写一个空行（函数体缺失时的占位）
  };
  struct Frame {
    Step step = Step::OpenNode;
    const Node* node = nullptr;
    const Param* param = nullptr;
    int depth = 0;
  };

  // 压入一个节点：先压"收账"帧，再压"打开"帧 —— 栈后进先出，于是处理顺序是
  //     打开本节点 → （本节点压入的子树）→ 收账
  void pushNode(const Node& n, int depth) {
    stack_.push_back(Frame{Step::CloseNode, &n, nullptr, depth});
    stack_.push_back(Frame{Step::OpenNode, &n, nullptr, depth});
  }

  // 形参单独一个重载：Param 不在 openNode 的分派表里。
  // ⚠️ 必须**成对**压帧（收账 + 打开）：只压"打开"会丢掉形参自己的右括号，
  //    而少一个括号 = 基线（check_ast_baseline.py）与往返立刻失败。
  void pushParam(const Param& p, int depth) {
    stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth});
    stack_.push_back(Frame{Step::OpenParam, nullptr, &p, depth});
  }

  // 起一行：换行 + 缩进 2*depth（**单行模式下**：一个空格）。
  // ⚠️ 换行之前必须先把"攒在同一行末尾的注解与右括号"落下去 —— 否则它们
  //    会被带到下一行去（括号就配错了位置）。
  // ⚠️ 单行模式的这三个分支必须**逐字**对应多行版：`singleLine_ == false` 时
  //    它逐字节等于 S02 的实现（`check_ast_baseline.py` 是门禁）。
  void line(int depth) {
    if (singleLine_) {
      if (mark_ >= 0) flushPending();
      out_ += ' ';
      mark_ = depth;
      return;
    }
    if (mark_ >= 0) {
      flushPending();
      out_ += '\n';
    }
    out_.append(static_cast<size_t>(depth) * 2, ' ');
    mark_ = depth;
  }

  // 写一个节点的头部（`(Name` 或 `(`+运算符 …）。所有 `openXxx` 都走这里，
  // 于是"行首怎么起"只有一份实现 —— S04 加单行模式时不必改二十处调用点。
  void writeHead(int depth, const char* head) {
    line(depth);
    out_ += head;
  }
  void writeHead(int depth, const std::string& head) { line(depth); out_ += head; }
  void writeHead(int depth, const std::string_view& head) {
    line(depth);
    out_.append(head.data(), head.size());
  }

  void flushPending() { out_.append(static_cast<size_t>(deferred_), ')'); deferred_ = 0; }

  // ── 收账（本设计的核心，只有这一处逻辑）──────────────────────────────
  //   * 下钻过（mark_ 已经比本节点更深）⇒ 括号**延后**：++deferred_，
  //     它会在下一次 line() 或收尾时落到"最后一行"的末尾
  //   * 没下钻（mark_ 仍是本节点自己那一行）⇒ 就地补在自己行尾
  //
  // ⚠️ 判定"有没有下钻过"用 mark_ > depth，它与递归版 emitParens(end<0)
  //    完全等价：每个节点都先写自己那一行（line(depth) ⇒ mark_ == depth），
  //    而子节点一律在 depth+1 ⇒ 只要打印过任何一个子节点，mark_ 必然 > depth。
  //    **S03 的注解一概走"节点头"，所以这里一个字符都不用改** —— 这正是
  //    "--emit=ast 逐字节不变"与"去注解还原"能同时成立的原因。
  void closeNode(int depth) {
    if (mark_ > depth) ++deferred_;
    else out_ += ')';
  }

  void openNode(const Node& n, int depth);

  // ⚠️ 子节点一律**逆序**压栈：栈是后进先出，只有逆序压才能保证
  //    "最左边的子节点最先打印"。顺序错了就是树形变了 —— 往返与基线会立刻抓到。
  void openCompUnit(const CompUnit& n, int depth) {
    writeHead(depth, "(CompUnit");
    for (size_t i = n.items.size(); i-- > 0;) {
      if (n.items[i] != nullptr) pushNode(*n.items[i], depth + 1);
    }
  }

  void openDecl(const Decl& n, int depth) {
    writeHead(depth, "(Decl");
    if (n.isConst) out_ += " :const";
    out_ += ' ';
    out_ += btypeSymbol(n.base);
    for (size_t i = n.defs.size(); i-- > 0;) {
      if (n.defs[i] != nullptr) pushNode(*n.defs[i], depth + 1);
    }
  }

  void openVarDef(const VarDef& n, int depth) {
    writeHead(depth, "(VarDef ");
    out_ += n.name;
    Anno::varDefHead(out_, n);          // ★ S03 的加法点：`:t <类型>`
    // 打印顺序：dims… → init。逆序压 ⇒ 先压 init，再倒着压 dims。
    if (n.init != nullptr) pushNode(*n.init, depth + 1);
    for (size_t i = n.dims.size(); i-- > 0;) pushNode(n.dims[i], depth + 1);
  }

  void openDim(const Dim& n, int depth) {
    writeHead(depth, "(Dim");
    if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
  }

  void openFuncDef(const FuncDef& n, int depth) {
    writeHead(depth, "(FuncDef ");
    out_ += n.name;
    out_ += ' ';
    out_ += n.isVoid ? ":void" : btypeSymbol(n.retType);
    // 打印顺序：参数节 → 函数体 ⇒ 函数体先压（它最后打印）。
    if (n.body != nullptr) {
      pushNode(*n.body, depth + 1);
    } else {
      // 没有函数体（语法上不该出现；读取畸形文本时才可能）：照旧写一个空行。
      stack_.push_back(Frame{Step::EmptyLine, nullptr, nullptr, depth + 1});
    }
    // 参数表节 `(params ...)`：**总是**打印（空表也打印 `(params)`）。
    stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth + 1});
    stack_.push_back(Frame{Step::OpenParams, &n, nullptr, depth + 1});
  }

  void openParams(const FuncDef& n, int depth) {
    writeHead(depth, "(params");
    for (size_t i = n.params.size(); i-- > 0;) pushParam(n.params[i], depth + 1);
  }

  void openParam(const Param& p, int depth) {
    writeHead(depth, "(Param ");
    out_ += p.name;
    Anno::paramHead(out_, p);           // ★ S03 的加法点：`:t <类型>`
    for (size_t i = p.type.dims.size(); i-- > 0;) pushNode(p.type.dims[i], depth + 1);
  }

  void openInitVal(const InitVal& n, int depth) {
    writeHead(depth, "(InitVal");
    for (size_t i = n.list.size(); i-- > 0;) {
      if (n.list[i] != nullptr) pushNode(*n.list[i], depth + 1);
    }
    if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
  }

  void openBlock(const BlockStmt& n, int depth) {
    writeHead(depth, "(Block");
    for (size_t i = n.items.size(); i-- > 0;) {
      if (n.items[i] != nullptr) pushNode(*n.items[i], depth + 1);
    }
  }

  // ── 语句 ──────────────────────────────────────────────────────────────
  void openStmt(const Stmt& s, int depth) {
    const std::string_view k = s.nodeKind();

    if (k == "BlockStmt") { openBlock(static_cast<const BlockStmt&>(s), depth); return; }

    line(depth);

    if (k == "AssignStmt") {
      const auto& n = static_cast<const AssignStmt&>(s);
      out_ += "(=";
      if (n.rhs != nullptr) pushNode(*n.rhs, depth + 1);
      if (n.lhs != nullptr) pushNode(*n.lhs, depth + 1);
      return;
    }
    if (k == "ExprStmt") {
      const auto& n = static_cast<const ExprStmt&>(s);
      out_ += "(ExprStmt";
      if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
      return;
    }
    if (k == "IfStmt") {
      const auto& n = static_cast<const IfStmt&>(s);
      out_ += "(If";
      // else 用一节显式的 `(Else ...)` 包住 —— 这样"有没有 else"在文本里是
      // **结构上可见**的，不依赖"子节点个数"的巧合。
      if (n.elseS != nullptr) {
        stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth + 1});
        stack_.push_back(Frame{Step::OpenElse, n.elseS.get(), nullptr, depth + 1});
      }
      if (n.thenS != nullptr) pushNode(*n.thenS, depth + 1);
      if (n.cond != nullptr) pushNode(*n.cond, depth + 1);
      return;
    }
    if (k == "WhileStmt") {
      const auto& n = static_cast<const WhileStmt&>(s);
      out_ += "(While";
      if (n.body != nullptr) pushNode(*n.body, depth + 1);
      if (n.cond != nullptr) pushNode(*n.cond, depth + 1);
      return;
    }
    if (k == "BreakStmt")    { out_ += "(Break";    return; }
    if (k == "ContinueStmt") { out_ += "(Continue"; return; }
    if (k == "ReturnStmt") {
      const auto& n = static_cast<const ReturnStmt&>(s);
      out_ += "(Return";
      if (n.value != nullptr) pushNode(*n.value, depth + 1);
      return;
    }
    out_ += "(UnknownStmt :";
    out_ += std::string(k);
  }

  // `(Else ...)` 这一节不是 AST 节点（只是把 else 分支包起来），所以单独一帧。
  void openElse(const Stmt& s, int depth) {
    writeHead(depth, "(Else");
    pushNode(s, depth + 1);
  }

  // ── 表达式 ────────────────────────────────────────────────────────────
  void openExpr(const Expr& e, int depth) {
    const std::string_view k = e.nodeKind();

    writeHead(depth, "");   // 只起一行；头部由下面各分支按节点种类追加
    // ★ S03 的加法点全部在"节点头之后、第一个子节点之前"（prompt §五(b)）：
    //   它只往**已有的行**上插记号，行的划分与括号的落位一动不动。
    if (k == "IntLit") {
      const auto& n = static_cast<const IntLit&>(e);
      out_ += "(IntLit ";
      out_.append(n.text.data(), n.text.size());   // ★ 原文，不是重新格式化的数字
      Anno::valueType(out_, n);
      return;                                      // 叶子：收账帧会补 `)`
    }
    if (k == "FloatLit") {
      const auto& n = static_cast<const FloatLit&>(e);
      out_ += "(FloatLit ";
      out_.append(n.text.data(), n.text.size());
      Anno::valueType(out_, n);
      return;
    }
    if (k == "LVal") {
      const auto& n = static_cast<const LVal&>(e);
      out_ += "(LVal ";
      out_ += n.name;
      Anno::lvalHead(out_, n);          // ★ `:obj <对象类型> :<值类型>`（都紧跟名字）
      for (size_t i = n.indices.size(); i-- > 0;) {
        if (n.indices[i] != nullptr) pushNode(*n.indices[i], depth + 1);
      }
      return;
    }
    if (k == "Call") {
      const auto& n = static_cast<const Call&>(e);
      out_ += "(Call ";
      out_ += n.callee;
      Anno::valueType(out_, n);         // ★ 值类型 = 返回类型，紧跟被调函数名
      for (size_t i = n.args.size(); i-- > 0;) {
        if (n.args[i] != nullptr) pushNode(*n.args[i], depth + 1);
      }
      return;
    }
    if (k == "Unary") {
      const auto& n = static_cast<const Unary&>(e);
      out_ += '(';
      out_ += tokOpText(n.op);
      Anno::valueType(out_, n);
      if (n.operand != nullptr) pushNode(*n.operand, depth + 1);
      return;
    }
    if (k == "Binary") {
      const auto& n = static_cast<const Binary&>(e);
      out_ += '(';
      out_ += tokOpText(n.op);
      Anno::valueType(out_, n);
      if (n.rhs != nullptr) pushNode(*n.rhs, depth + 1);
      if (n.lhs != nullptr) pushNode(*n.lhs, depth + 1);
      return;
    }
    if (k == "Cast") {
      const auto& n = static_cast<const Cast&>(e);
      out_ += "(Cast";
      Anno::castHead(out_, n);          // ★ `:种类 :目标类型`（值类型由种类决定）
      if (n.operand != nullptr) pushNode(*n.operand, depth + 1);
      return;
    }
    out_ += "(UnknownExpr :";
    out_ += std::string(k);
  }

  std::string out_;
  int mark_ = -1;                 // 当前行号（-1 = 还没有任何一行）
  int deferred_ = 0;              // 攒在"最后一行"的右括号总数
  bool singleLine_ = false;       // ★ S04：折叠成一行（只给 --emit=initplan 用）
  std::vector<Frame> stack_;      // ★ 显式工作栈：迭代遍历的全部状态，内存 O(树深)
};

// 按 nodeKind 分派（与 Ast.h 的节点类型一一对应）。
// 未知节点：照常打印并收尾（少一个括号 = 往返必失败）。
template <class Anno>
void SexpLayout<Anno>::openNode(const Node& n, int depth) {
  const std::string_view k = n.nodeKind();
  if (k == "CompUnit")  { openCompUnit(static_cast<const CompUnit&>(n), depth); return; }
  if (k == "FuncDef")   { openFuncDef(static_cast<const FuncDef&>(n), depth); return; }
  if (k == "Decl")      { openDecl(static_cast<const Decl&>(n), depth); return; }
  if (k == "VarDef")    { openVarDef(static_cast<const VarDef&>(n), depth); return; }
  if (k == "Dim")       { openDim(static_cast<const Dim&>(n), depth); return; }
  if (k == "InitVal")   { openInitVal(static_cast<const InitVal&>(n), depth); return; }
  if (k == "BlockStmt") { openBlock(static_cast<const BlockStmt&>(n), depth); return; }
  if (k == "AssignStmt" || k == "ExprStmt" || k == "IfStmt" || k == "WhileStmt" ||
      k == "BreakStmt" || k == "ContinueStmt" || k == "ReturnStmt") {
    openStmt(static_cast<const Stmt&>(n), depth);
    return;
  }
  if (k == "LVal" || k == "Call" || k == "Unary" || k == "Binary" || k == "Cast" ||
      k == "IntLit" || k == "FloatLit") {
    openExpr(static_cast<const Expr&>(n), depth);
    return;
  }
  line(depth);
  out_ += "(UnknownNode :";
  out_ += std::string(k);
}

}  // namespace sexp
}  // namespace sysy
#endif  // SYSY_FRONTEND_ASTSEXPLAYOUT_H
