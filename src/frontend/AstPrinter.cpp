// ============================================================================
// AstPrinter.cpp —— S-表达式打印器（实现）
//
// 详细格式说明见 AstPrinter.h 的文件头（含"为什么打印器与读取器必须严格互逆"）。
// 读取器（--from-ast）在 AstReader.cpp；两者共用 astformat 里的记号表。
//
// ── ★ 遍历必须是**迭代**的（不要把这里改回递归！）───────────────────────
//
//   本文件的 printExpr / printStmt / printInitVal 原先都是**递归**遍历自己，
//   递归深度 = AST 深度 ⇒ 深树必崩（实测，编排方复现）：
//
//     int main(){ int a; a = 1+1+…+1; return a; }      // 6 万个 `+1`
//       --emit=tokens  → rc=0
//       --emit=ast     → rc=139（SIGSEGV）
//       ulimit -s unlimited 后 rc=0  ⇒ **确证是栈溢出**，不是逻辑/堆问题
//     5 万个 `+1` 时侥幸 rc=0 —— 也就是说余量只有约 14 倍：
//     真实语料 tests/final_arm/functional/86_long_code2.sy 的打印树深
//     已经是 4007 层（3999 个 `+`），离崩溃点并不远。
//
//   "不崩"是硬验收要求，而**加深度上限不是解法**：语料里已经有 4007 层的
//   合法程序，任何阈值都只是把崩溃点往后推 —— 低于它就误伤合法输入，
//   高于它仍然会崩（Parser.h 的 kMaxDepth 注释里有同类教训）。
//   ⇒ 这里用**显式工作栈**做先序遍历：内存 O(树深)，深度不设人为上限。
//
//   ⚠️ 迭代化之后打印器**只受内存限制**，而输出体积是**平方增长**的：
//      每行缩进 = 2 × 深度 ⇒ 一条 n 项的链要写 ≈ **2.008 × n²** 字节
//      （实测 n=1000/2000/4000 → 2 033 173 / 8 066 173 / 32 132 173 字节，
//       严格符合这条二次律；n=50000 → 5.0 GB，n=100000 → 20.1 GB）。
//      ⇒ "能打到多深"最终由内存决定，与调用栈无关 —— 这正是本轮修复的目标。
//
//   至此项目里四处深树风险点里，打印器这一处不再是无界递归；
//   Parser::parseExp（循环建树）、Ast.h 的 destroyTree（显式栈析构）本来
//   就是迭代的，而 **--from-ast 读取器仍是递归的**（它靠一个实测定标的
//   深度哨兵兜底，见 AstReader.h —— 两侧的约束不对称，别照抄）。
// ============================================================================
#include "frontend/AstPrinter.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "frontend/AstSexpFormat.h"

namespace sysy {
namespace {

// ============================================================================
// 一、打印器
//
// ── 格式模型（先读这一段，否则会改错）──────────────────────────────────
//
//   输出 = 先序 DFS，每个节点占一行：
//
//       line(depth)  →  换行 + 缩进 2*depth + "(" + 头部
//
//   (a) 叶子（没有下钻过的节点）的右括号 ⇒ 跟在**自己这一行**的内容后面：
//         `(IntLit 1)` / `(params)`
//   (b) 有子节点的节点 ⇒ 它自己的 `)` 与"还在栈上的祖先们"的 `)` 一起，
//       全部堆在**最后一行（= 最深那一行）的末尾**：
//         `(IntLit 3)))))))`
//
//   ── 为什么 (b) 是对的（两条观察，缺一不可）──
//
//   ① 先序 DFS 里，右括号的出现顺序**恰好**是左括号的逆序。所以只要按
//      "遇到节点就记一个待补的 `)`"，最后把所有待补的 `)` 一次追加，
//      括号配对与嵌套顺序就是对的 —— 不需要在树上到处插收尾逻辑。
//   ② 每行末尾补上的 `)` 属于"从这一行继续下钻时被留在这里的那些祖先"。
//      所有 `)` 都在最后一行补，于是最深的那个叶子所在的行（也就是最后
//      一行）自然承载了全部祖先的收尾。
//
//   实现：deferred_ = "待补的 `)` 计数"；line() 换行前**先把它们落下去**。
//   ⚠️ 落不下去就会被带到下一行 ⇒ 括号配错位置 —— 这是本格式唯一容易漏的一步。
//
//   ⚠️ 这个形态是**格式契约**：--emit=ast 的输出要能被 --from-ast 重读，
//      重读后再打印必须【逐字节相同】（轨 A），并且被 test_parser.cpp 里
//      "与 prompt §5 例子逐字节比对"那条断言钉死 —— 例子长什么样就只能
//      长什么样，**缩进/换行/括号位置一个字节都不能动**。
//
//   ⚠️ prompt §2.2 的教训：序列化器与反序列化器必须**严格互逆**。原型在
//      「空参数列表」「字面量节点」「%」三处出过不对称，都是"某些节点多补/
//      少补了一个括号"。本实现把收尾集中在一个地方（closeNode → deferred_），
//      并且"有没有下钻过"统一由 mark_ > depth 判定，**不再手写
//      `if (kids.empty())` 分支**，从结构上消掉这类错误。
// ============================================================================
class Printer {
 public:
  std::string run(const CompUnit& unit) {
    out_.reserve(4096);
    mark_ = -1;     // 还没有任何一行
    deferred_ = 0;
    stack_.clear();

    pushNode(unit, 0);
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

    // ★ 根节点的右括号也落在"最深那一行"（与树内所有节点的规则一致：
    //   括号落在哪一行，只取决于"它最后一个子节点结束在哪一行"）。
    //   收尾时把还欠着的括号一次补上，末尾换行（与 §5 例子逐字节一致）。
    out_.append(static_cast<size_t>(deferred_), ')');
    out_ += '\n';   // 结尾换行
    return std::move(out_);
  }

 private:
  // ── 工作栈的一帧 ───────────────────────────────────────────────────────
  // 递归版的"调用帧"在这里被显式化：打开一个节点 = 压帧，处理完子树 =
  // 弹出收账帧。深度不设上限，内存 O(树深)。
  enum class Step {
    OpenNode,    // 打开一个 AST 节点（node 有效）
    CloseNode,   // 给一个节点收账（depth 有效）—— 原 emitParens 的迭代版
    OpenParams,  // 参数节 `(params ...)`（node = 它所属的 FuncDef）
    OpenParam,   // 一个形参（param 有效；Param 不是 Expr/Stmt 那类节点，单独走）
    OpenElse,    // if 的 `(Else ...)` 节（node = else 分支的语句）
    EmptyLine,   // 只写一个空行（函数体缺失时的占位，见 openFuncDef）
  };
  struct Frame {
    Step step = Step::OpenNode;
    const Node* node = nullptr;
    const Param* param = nullptr;
    int depth = 0;
  };

  // 压入一个节点：先压"收账"帧，再压"打开"帧 —— 栈是后进先出，于是处理顺序是
  //     打开本节点 → （本节点压入的子树）→ 收账
  // 与递归版的"先写自己这一行，再依次下钻子节点，最后补自己的括号"逐一对应。
  void pushNode(const Node& n, int depth) {
    stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth});
    stack_.push_back(Frame{Step::OpenNode, &n, nullptr, depth});
  }

  // 形参单独一个重载：Param 不在 openNode 的分派表里（它不是 Expr/Stmt 那一类
  // 会被分派到的节点），所以它有自己的 Step::OpenParam。
  // ⚠️ 必须**成对**压帧（收账 + 打开），与 pushNode 同构：
  //    只压"打开"会丢掉形参自己的右括号，而少一个括号 =
  //    基线（check_ast_baseline.py）与往返立刻失败 —— 这个不对称踩过一次。
  void pushParam(const Param& p, int depth) {
    stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth});
    stack_.push_back(Frame{Step::OpenParam, nullptr, &p, depth});
  }

  // 起一行：换行 + 缩进 2*depth。
  // 不做任何"复用行"的优化 —— 每个节点都占独立的一行（格式要求）。
  // ⚠️ 换行之前必须先把"攒在同一行末尾的右括号"落下去 —— 否则它们会被
  //    带到下一行去（括号就配错了位置）。
  void line(int depth) {
    if (mark_ >= 0) {
      out_.append(static_cast<size_t>(deferred_), ')');   // ★ 收掉当前行欠的括号
      out_ += '\n';
    }
    deferred_ = 0;
    out_.append(static_cast<size_t>(depth) * 2, ' ');
    mark_ = depth;
  }

  // ── 收账（本设计的核心，只有这一处逻辑）──────────────────────────────
  //   * 下钻过（mark_ 已经比本节点更深）⇒ 括号**延后**：++deferred_，
  //     它会在下一次 line() 或收尾时落到"最后一行"的末尾
  //   * 没下钻（mark_ 仍是本节点自己那一行）⇒ 就地补在自己行尾
  //
  // ⚠️ 判定"有没有下钻过"用 mark_ > depth，它与递归版 `emitParens(end<0)`
  //    完全等价：每个节点都先写自己那一行（line(depth) ⇒ mark_ == depth），
  //    而子节点一律在 depth+1 ⇒ 只要打印过任何一个子节点，mark_ 必然 > depth。
  //    （原来的实现是在递归返回值里传递"最后一行在哪"，现在不需要了：
  //     返回值只被用来区分"有没有子节点"，而那完全由 mark_ 决定。）
  void closeNode(int depth) {
    if (mark_ > depth) ++deferred_;
    else out_ += ')';
  }

  void openNode(const Node& n, int depth);

  // ⚠️ 子节点一律**逆序**压栈：栈是后进先出，只有逆序压才能保证
  //    "最左边的子节点最先打印"。顺序错了就是树形变了 —— 往返与基线
  //    （check_ast_baseline.py）会立刻抓到，这不是风格问题。
  void openCompUnit(const CompUnit& n, int depth) {
    line(depth);
    out_ += "(CompUnit";
    for (size_t i = n.items.size(); i-- > 0;) {
      if (n.items[i] != nullptr) pushNode(*n.items[i], depth + 1);
    }
  }

  void openDecl(const Decl& n, int depth) {
    line(depth);
    out_ += "(Decl";
    if (n.isConst) out_ += " :const";
    out_ += ' ';
    out_ += btypeSymbol(n.base);
    for (size_t i = n.defs.size(); i-- > 0;) {
      if (n.defs[i] != nullptr) pushNode(*n.defs[i], depth + 1);
    }
  }

  void openVarDef(const VarDef& n, int depth) {
    line(depth);
    out_ += "(VarDef ";
    out_ += n.name;
    // 打印顺序：dims… → init。逆序压 ⇒ 先压 init，再倒着压 dims。
    if (n.init != nullptr) pushNode(*n.init, depth + 1);
    for (size_t i = n.dims.size(); i-- > 0;) pushNode(n.dims[i], depth + 1);
  }

  void openDim(const Dim& n, int depth) {
    line(depth);
    out_ += "(Dim";
    if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
  }

  void openFuncDef(const FuncDef& n, int depth) {
    line(depth);
    out_ += "(FuncDef ";
    out_ += n.name;
    out_ += ' ';
    out_ += n.isVoid ? ":void" : btypeSymbol(n.retType);

    // 打印顺序：参数节 → 函数体 ⇒ 函数体先压（它最后打印）。
    if (n.body != nullptr) {
      pushNode(*n.body, depth + 1);
    } else {
      // 没有函数体（语法上不该出现；读取畸形文本时才可能）：照旧写一个空行，
      // 括号交给上面的收账帧。⚠️ 空行要写，否则 `(FuncDef` 那一节少一行、
      // 收尾括号的位置就变了 —— 与递归版逐字节一致。
      stack_.push_back(Frame{Step::EmptyLine, nullptr, nullptr, depth + 1});
    }

    // ── 参数表节 `(params ...)` ──
    // **总是**打印成显式的一节（空表也打印 `(params)`）：原型在"空参数列表"
    // 上出过不对称，所以这里不做任何"空就省掉"的优化。
    // 它有自己的收账帧（空表 ⇒ 就地补成 `(params)`；非空 ⇒ 落到最后那个
    // 形参所在的行尾），与递归版 emitParens(pend) 的行为一致。
    stack_.push_back(Frame{Step::CloseNode, nullptr, nullptr, depth + 1});
    stack_.push_back(Frame{Step::OpenParams, &n, nullptr, depth + 1});
    // ⚠️ 函数体必须另起一行：openBlock 的第一件事就是 line(depth+1)，
    //    所以 `(params)` 与 `(Block` 不会粘在一起
    //    （S02 踩过 `(params)(Block` —— 那次是因为 line() 有"复用空行"的优化）。
  }

  void openParams(const FuncDef& n, int depth) {
    line(depth);
    out_ += "(params";
    for (size_t i = n.params.size(); i-- > 0;) pushParam(n.params[i], depth + 1);
  }

  void openParam(const Param& p, int depth) {
    line(depth);
    out_ += "(Param ";
    out_ += p.name;
    for (size_t i = p.type.dims.size(); i-- > 0;) pushNode(p.type.dims[i], depth + 1);
  }

  void openInitVal(const InitVal& n, int depth) {
    line(depth);
    out_ += "(InitVal";
    // 打印顺序：expr（若有）→ list 各项。逆序压 ⇒ 先倒着压 list，再压 expr。
    for (size_t i = n.list.size(); i-- > 0;) {
      if (n.list[i] != nullptr) pushNode(*n.list[i], depth + 1);
    }
    if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
  }

  void openBlock(const BlockStmt& n, int depth) {
    line(depth);
    out_ += "(Block";
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
      if (n.rhs != nullptr) pushNode(*n.rhs, depth + 1);   // 逆序：rhs 先压
      if (n.lhs != nullptr) pushNode(*n.lhs, depth + 1);
      return;
    }
    if (k == "ExprStmt") {
      const auto& n = static_cast<const ExprStmt&>(s);
      out_ += "(ExprStmt";
      // 空语句 `;`：只打印节点，不打印 `;`（它不承载信息）
      if (n.expr != nullptr) pushNode(*n.expr, depth + 1);
      return;
    }
    if (k == "IfStmt") {
      const auto& n = static_cast<const IfStmt&>(s);
      out_ += "(If";
      // else 用一节显式的 `(Else ...)` 包住 —— 这样"有没有 else"在文本里是
      // **结构上可见**的，不依赖"子节点个数"的巧合（有歧义轨 A 就不成立）。
      // 打印顺序：cond → then → else ⇒ else 最先压。
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
      if (n.body != nullptr) pushNode(*n.body, depth + 1);   // 逆序：body 先压
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

  // `(Else ...)` 这一节不是 AST 节点（只是把 else 分支包起来），
  // 所以单独一帧：写节名 → 压入分支语句。
  void openElse(const Stmt& s, int depth) {
    line(depth);
    out_ += "(Else";
    pushNode(s, depth + 1);
  }

  // ── 表达式 ────────────────────────────────────────────────────────────
  void openExpr(const Expr& e, int depth) {
    const std::string_view k = e.nodeKind();

    line(depth);
    if (k == "IntLit") {
      const auto& n = static_cast<const IntLit&>(e);
      out_ += "(IntLit ";
      out_.append(n.text.data(), n.text.size());   // ★ 原文，不是重新格式化的数字
      return;                                      // 叶子：收账帧会就地补 `)`
    }
    if (k == "FloatLit") {
      const auto& n = static_cast<const FloatLit&>(e);
      out_ += "(FloatLit ";
      out_.append(n.text.data(), n.text.size());
      return;
    }
    if (k == "LVal") {
      const auto& n = static_cast<const LVal&>(e);
      out_ += "(LVal ";
      out_ += n.name;
      for (size_t i = n.indices.size(); i-- > 0;) {
        if (n.indices[i] != nullptr) pushNode(*n.indices[i], depth + 1);
      }
      return;
    }
    if (k == "Call") {
      const auto& n = static_cast<const Call&>(e);
      out_ += "(Call ";
      out_ += n.callee;
      for (size_t i = n.args.size(); i-- > 0;) {
        if (n.args[i] != nullptr) pushNode(*n.args[i], depth + 1);
      }
      return;
    }
    if (k == "Unary") {
      const auto& n = static_cast<const Unary&>(e);
      out_ += '(';
      out_ += tokOpText(n.op);
      if (n.operand != nullptr) pushNode(*n.operand, depth + 1);
      return;
    }
    if (k == "Binary") {
      const auto& n = static_cast<const Binary&>(e);
      out_ += '(';
      out_ += tokOpText(n.op);
      if (n.rhs != nullptr) pushNode(*n.rhs, depth + 1);   // 逆序：rhs 先压
      if (n.lhs != nullptr) pushNode(*n.lhs, depth + 1);
      return;
    }
    out_ += "(UnknownExpr :";
    out_ += std::string(k);
  }

  std::string out_;
  int mark_ = -1;      // 当前行号（-1 = 还没有任何一行）
  int deferred_ = 0;   // 攒在"最后一行"的右括号总数
  std::vector<Frame> stack_;   // ★ 显式工作栈：迭代遍历的全部状态，内存 O(树深)
};

// 按 nodeKind 分派（与 Ast.h 的节点类型一一对应）。
// 未知节点：照常打印并收尾（少一个括号 = 往返必失败）。
void Printer::openNode(const Node& n, int depth) {
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
  if (k == "LVal" || k == "Call" || k == "Unary" || k == "Binary" ||
      k == "IntLit" || k == "FloatLit") {
    openExpr(static_cast<const Expr&>(n), depth);
    return;
  }
  line(depth);
  out_ += "(UnknownNode :";
  out_ += std::string(k);
}

}  // namespace

// ============================================================================
// 对外接口（声明见 AstPrinter.h）
// ============================================================================
std::string printAst(const CompUnit& unit) {
  Printer p;
  return p.run(unit);
}

}  // namespace sysy
