// ============================================================================
// AstReader.h —— S-表达式读取器（`--from-ast`）的内部接口
//
// 这一半原来是塞在 AstPrinter.cpp 里的（打印器 + 读取器共 1135 行），
// 现按"打印 / 读取"拆开：本头声明、AstReader.cpp 定义。
// 对外接口仍然只有 AstPrinter.h 里的 parseAstText()（这里出现的
// SexpReader / Builder 都是实现细节，外部不应使用）。
//
// ── 文法（极小，约 100 行）──────────────────────────────────────────────
//
//   program := form*
//   form    := '(' symbol form* ')'      ; 列表：第一个元素是节点类型
//            | '(' ')'                   ; 空表（本格式里不出现，读进来报"未知节点"）
//   symbol  := 非空白、非括号的任意字节串（节点名 / 运算符 / 标识符 / 字面量 / :记号）
//
// 读取器**不认识 SysY 语法**，只认识这棵括号树 —— 这正是 prompt §6.1 的建议：
// 不要把 AST 文本做成合法 SysY。
//
// ── ⚠️ 与打印器的深度约束**不对称**（别以为两边对称）──────────────────
//
//   * 打印器：深度就是 AST 深度，与输入文本无关 ⇒ 已改成**显式工作栈**的
//     迭代遍历（内存 O(树深)），不受调用栈限制，见 AstPrinter.cpp 的文件头。
//   * 读取器：**仍然是递归的**（SexpReader::parseForm、Builder::buildXxx，
//     外加 Sexp 自身的析构链），三条链的深度都等于 S-表达式的**嵌套深度**
//     ⇒ 它的上限就是**调用栈**，不是"文件太大装不下"。
//     ⚠️ "缩进使文本体积随树深平方增长"**不能**当护栏：16 万层的紧凑文本只有
//     不到 1 MB，照样能把 8 MB 栈打爆（实测见 kMaxNestingDepth 的注释）。
//   ⇒ 所以读取器这一侧靠一个**实测定标**的深度哨兵把"崩"变成"报错"，
//     而不是像打印器那样彻底不受栈限制 —— 这个不对称是**已知的**。
// ============================================================================
#ifndef SYSY_FRONTEND_ASTREADER_H
#define SYSY_FRONTEND_ASTREADER_H

#include <cstddef>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "support/SourceLoc.h"

namespace sysy {

class DiagnosticEngine;

// ── 一、S-表达式树 ───────────────────────────────────────────────────────
// 括号树的直接表示。symbol 只有叶子（非列表）节点用；kids 只有列表节点用。
// ⚠️ 它的析构是**递归**的（vector<Sexp> 套 vector<Sexp>），深度 = 树的嵌套
//    深度 ⇒ 也受下面的 kMaxNestingDepth 保护（哨兵卡在 parseForm 入口，
//    树根本建不了更深，析构自然也不会更深）。
struct Sexp {
  SourceLoc loc;
  bool isList = false;
  std::string symbol;
  std::vector<Sexp> kids;

  // 列表的第一个元素若是符号，就把它当"节点类型/运算符"返回（否则 nullptr）。
  // 这是"节点的子节点个数由类型唯一决定"的入口。
  const Sexp* firstSymbol() const {
    return (isList && !kids.empty() && !kids[0].isList) ? &kids[0] : nullptr;
  }
};

// ── 二、S-表达式文本 → Sexp 树（这一段就是"读取器的词法+语法"）──────────
class SexpReader {
 public:
  SexpReader(const std::string& text, DiagnosticEngine& diag)
      : text_(text), diag_(diag) {}

  // 读入全部顶层 form。出错时**就地恢复**（跳到下一个顶层 '('），
  // 保证一定前进、不在这里死循环；返回已读到的部分（可能是空的）。
  std::vector<Sexp> parseProgram();

  size_t errorCount() const { return errors_; }

 private:
  // ── 嵌套深度防护（与 Parser::kMaxDepth 同一套取舍，但阈值是**实测**来的）──
  //
  // 为什么一个哨兵就够：三条递归链（parseForm / Builder::buildXxx / ~Sexp）
  // 的深度**都等于 S-表达式的嵌套深度**，而嵌套深度是在 parseForm 里逐层
  // 长出来的 ⇒ 在这里卡住，后面两条链自然也不会更深。
  //
  // ⚠️ 8100 这个数字是怎么定的（实测，g++ -O3 -DNDEBUG + 默认 8 MB 栈，
  //    探针直接调 parseAstText / Builder，输入是紧凑深链转储）：
  //      * 单独 parseForm 能到 ≈28 881 层（它自己的帧更小）
  //      * 一旦进 Builder（buildExpr 的递归帧最大），边界是
  //        **16 351 层通过 / 16 357 层段错误** ⇒ D ≈ 16 354
  //      * 语料最大**打印树深**是 4007（86_long_code2.sy）⇒ 下限取 2×4007 = 8014
  //      * 取 floor(D/2) 到百位 = **8100**：既 ≥ 8014（语料最大值的 2.02 倍），
  //        又 ≤ D/2（对实测崩溃点留 2.02 倍余量）
  //    ⇒ 超过它：**报一条 error 诊断**（退出码因此非 0）并**迭代**跳过该节点，
  //      绝不静默返回半棵树，更不靠段错误收场。
  //    ⚠️ 想把这个上限抬高，唯一办法是把 Builder 的表达式递归也迭代化
  //      （那会把 D 抬到 parseForm 的 ≈28 881 层）；**不要**只是把数字改大。
  static constexpr int kMaxNestingDepth = 8100;
  int depth_ = 0;               // 当前 parseForm 的嵌套深度
  bool depthReported_ = false;  // 同一条诊断只报一次（病态输入否则能刷屏）
  int openDepth_ = 0;           // 从文本开头起尚未闭合的 `(` 个数（见 skipRestOfForm）

  struct DepthGuard {
    SexpReader& r;
    explicit DepthGuard(SexpReader& reader) : r(reader) { ++r.depth_; }
    ~DepthGuard() { --r.depth_; }
  };

  // 超限：报一次诊断 + **迭代**跳过到本顶层 form 的末尾（保证前进且不刷屏），返回 true。
  // 调用方（parseForm）看到 true 必须立刻返回 false，不再下钻。
  bool depthExceeded() {
    if (depth_ <= kMaxNestingDepth) return false;
    if (!depthReported_) {
      depthReported_ = true;
      error(locAt(pos_), "AST nesting is too deep (limit " +
                             std::to_string(kMaxNestingDepth) +
                             " levels); the rest of this top-level form is skipped");
    }
    skipRestOfForm();
    return true;
  }

  // 把当前 form 剩下的部分**迭代**吃掉（到配平的 ')' 或 EOF）；定义在 .cpp。
  // ⚠️ 必须保证前进：否则 parseProgram 的恢复循环会反复重试同一个 '('，
  //    每次都重新下钻到上限（O(n·limit) 的活锁），病态输入能卡上几分钟。
  void skipRestOfForm();

  // 行号/列号：行首索引**按需建立到 pos 为止**，再做二分。
  // ⚠️ 不要"每个 token 都从头线性扫一遍 lineStart_" —— 那是 O(n²)：
  //    86_long_code2.sy 的 AST 文本有 1.29 亿字节、数百万行，
  //    实测因此从 0.2 s 恶化到 0.67 s（S02 优化过这一点）。
  SourceLoc locAt(size_t pos);

  void error(SourceLoc loc, const std::string& msg);
  void skipTrivia();          // 跳过空白
  bool parseForm(Sexp& out);  // 读一个 form；失败返回 false（已报诊断）

  const std::string& text_;
  DiagnosticEngine& diag_;
  size_t pos_ = 0;
  size_t errors_ = 0;
  std::vector<size_t> lineStart_;   // 行首 offset（按需增长）
  size_t lineEnd_ = 0;              // 已经扫到哪个 offset（避免重复扫描）
};

// ── 三、Sexp 树 → AST ───────────────────────────────────────────────────
//
// 每个节点的**子节点个数与顺序由类型唯一决定**（不是靠缩进猜的）。
// 出错时：报诊断 + 就地恢复（能建多少建多少），**永远返回可用的节点**。
class Builder {
 public:
  explicit Builder(DiagnosticEngine& diag) : diag_(diag) {}

  // ⚠️ forms 必须**拷进本对象**（或至少活到 AST 用完）：
  //    AST 里 IntLit/FloatLit 的 text 是指向 Sexp::symbol(std::string) 内部的
  //    string_view。若 forms 只是个临时对象，函数一返回这些 view 就悬垂
  //    —— 实测表现是"字面量原文随机变成空串"（round-trip 大面积不相等）。
  //    所以 Builder 按值持有 forms，并在 run() 之后仍保留它。
  std::unique_ptr<CompUnit> run(std::vector<Sexp> forms);

  size_t errorCount() const { return errors_; }

 private:
  void error(SourceLoc loc, const std::string& msg);

  static const Sexp* at(const Sexp& f, size_t i) {
    return (f.isList && i < f.kids.size()) ? &f.kids[i] : nullptr;
  }

  // 取第 i 个子节点并断言它是符号；失败返回空串（并报错）
  std::string expectSymbol(const Sexp& f, size_t i, const char* what);

  static void setLoc(Node& n, SourceLoc loc) { n.loc = loc; }

  // ── 顶层项 ────────────────────────────────────────────────────────────
  std::unique_ptr<Node> buildItem(const Sexp& f);

  // ── 声明 ──────────────────────────────────────────────────────────────
  std::unique_ptr<Decl> buildDecl(const Sexp& f);
  std::unique_ptr<VarDef> buildVarDef(const Sexp& f);
  // 注意：Dim 内含 unique_ptr，不可拷贝 ⇒ 返回**值**（移动构造）
  Dim buildDim(const Sexp& f);
  // 与打印器严格对称：0 个子节点 → `{}`（空列表）；子节点是 InitVal → 列表
  // 形式（扁平，逐个搬过来）；否则 → 标量形式（那唯一一个子节点就是表达式）
  std::unique_ptr<InitVal> buildInitVal(const Sexp& f);
  std::unique_ptr<FuncDef> buildFuncDef(const Sexp& f);
  Param buildParam(const Sexp& f);

  // ── 语句 ──────────────────────────────────────────────────────────────
  std::unique_ptr<BlockStmt> buildBlock(const Sexp& f);
  std::unique_ptr<Stmt> buildStmt(const Sexp& f);

  // ── 表达式 ────────────────────────────────────────────────────────────
  // ⚠️ 运算符节点：打印器输出的**就是运算符原文**（prompt §5："运算符用其原文"），
  //    所以这里不能去找 "Binary"/"Unary" 这样的节点名，而要把 head 当成
  //    运算符符号去认。元数由子节点个数决定（1 = 一元、2 = 二元）。
  //    这条曾经写错过：既打印 `(+ ...)` 又去匹配 "Binary"，导致
  //    --from-ast 对任何算术表达式都报 "unknown expression node '+'"
  //    —— 正是轨 A 存在的意义（printer 与 reader 不互逆会被立刻抓到）。
  std::unique_ptr<Expr> buildExpr(const Sexp& f);

  // ── 数值（辅助值，不参与往返）────────────────────────────────────────
  // 字面量原文：`(IntLit 1)` 的第 2 个子节点（若缺，报错并返回空串）
  // 返回**副本**（不是 view）：字面量节点的 text 必须是自有的，
  // 否则 AST 的生命期就被绑死在 S-表达式缓冲上（S02 实测踩过这个悬垂）。
  std::string textOf(const Sexp& f, const char* what);
  void parseIntText(IntLit& lit);      // 与 Parser::parseIntLiteral 等价的辅助值换算
  void parseFloatText(FloatLit& lit);  // 与 Parser::parseFloatLiteral 等价

  DiagnosticEngine& diag_;
  std::vector<Sexp> forms_;   // ★ 拥有 S-表达式树（字面量 text 的 string_view 指向它）
  size_t errors_ = 0;
};

}  // namespace sysy

#endif  // SYSY_FRONTEND_ASTREADER_H
