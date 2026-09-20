// ============================================================================
// Ast —— SysY 抽象语法树节点定义（S02）
//
// 设计约束（phases/S02-parser.md §3）：
//
//   * **节点类型按文法一一对应**，不自作主张合并/省略。
//     每个非终结符有一到多个节点类型；节点之间只有"文法上的父子"关系。
//
//   * **所有节点携带 SourceLoc**（D11：位置要一路带到 IR —— `_sysy_starttime`
//     需要调用点行号）。SourceLoc 是 2×uint32，按值传递，无开销。
//
//   * **所有权一律用 std::unique_ptr**，不用裸指针、不做共享。
//     因此整棵树是纯树（无 DAG 共享）：删除根节点即释放全部内存。
//
//   * **只做语法，不做语义**（明确不做清单）：
//       - 数组维度长度【原样存表达式】—— 不求值、不检查正负、不检查是否为常数
//       - 不查类型、不查重复定义、不查作用域、不插隐式转换
//       - `!` 只应出现在 Cond 里，但语法层统一按 UnaryExp 接受（见 Parser.cpp 头注）
//     这些留给 S03（语义）/ S04（常量求值）。
//
//   * 数值字段（IntLit::value / FloatLit::value）只是**语法层顺手算出的辅助值**
//     （供调试与 S03 使用），【不是】常量求值：
//       - 溢出/不可表示时置 parsed=false 且 value 保持 0，同时报一条 warning
//       - 副作用：**即使 value 不可用，AST 仍是完整的**（原文 text 永远保留）
//     S04 的 ConstEval 才是语义上的唯一权威。
//
//   * 名字用 std::string（不是 string_view）：标识符是小串，而树的生命期可能
//     长于 SourceFile（例如以后 AST 缓存/跨阶段流水线）。`text` 字段则仍是 view ——
//     它只在打印与诊断时需要，且调用点一定持有 SourceFile。
//
// 与 §3.1 骨架的对应关系（**不要删减语义**，只增不改）：
//   TypeSpec.dims 是 std::vector<Dim>（Dim 内含可空 Expr），而不是
//   vector<unique_ptr<Expr>> —— 理由是形参第一维可以【为空】，nullptr 表达不出
//   "这里有一个写成 `[]` 的维"与"根本没有这一维"的区别；那会让 AST 有歧义，
//   而歧义的树无法做无歧义序列化（轨 A 的前提）。见 Dim 的注释。
// ============================================================================
#ifndef SYSY_FRONTEND_AST_H
#define SYSY_FRONTEND_AST_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/Token.h"
#include "support/SourceLoc.h"

namespace sysy {

// ── 前置声明 ────────────────────────────────────────────────────────────
// Dim 比 Expr 先定义（类型段在表达式段之前），但它的成员是 unique_ptr<Expr>。
// ⚠️ 即使 unique_ptr 允许不完整类型，**也必须**先声明 Expr：
//    否则 `std::unique_ptr<Expr>` 的默认删除器在实例化时找不到 Expr，
//    报错是 "use of undeclared identifier"（S02 实际踩过）。
struct Expr;

// 迭代式释放一棵（子）表达式树。**定义在文件末尾**（那时所有节点类型都已定义）。
// 为什么必须迭代、不这么写会怎样，见文件末尾的详细说明。
void destroyTree(Expr* root);

// ============================================================================
// 0. 基类
//
// 唯一根类。Item / Expr / Stmt / InitVal / ... 都是它的派生类，
// 所以 `std::unique_ptr<Node>` 能持有任何一个节点（顶层列表、块内列表用得上）。
// ============================================================================
struct Node {
  SourceLoc loc;   // 1-based；SourceLoc() 表示"无位置"

  Node() = default;
  explicit Node(SourceLoc l) : loc(l) {}
  virtual ~Node() = default;

  // ⚠️ 这里【故意不写】 `Node(const Node&) = delete;`
  //    显式删除拷贝会连带把派生类的**隐式移动构造**也抑制掉（用户声明了拷贝构造
  //    就不再隐式生成移动构造），结果是 `std::vector<Dim>` / `std::vector<Param>`
  //    这种"按值持有含 unique_ptr 的节点"连 push_back 都编译不过 —— 报错还会
  //    指向 libstdc++ 的 allocator 深处，非常难查（S02 实际踩过）。
  //    不删也安全：含 unique_ptr 的派生类本来就不可拷贝，而基类 Node 不会被
  //    单独拷贝（每个具体节点都有自己的成员语义）。

  // 调试/诊断用的可读名字。**不参与**任何对外格式（S-表达式有各自的字面量）。
  virtual const char* nodeKind() const { return "Node"; }
};

// ============================================================================
// 1. 类型
// ============================================================================

enum class BType { Int, Float };

inline const char* toString(BType t) { return t == BType::Int ? "int" : "float"; }

// 数组的一个维度。
//   expr != nullptr → 写明了长度（`[2+3*4]`，**原样存表达式**，不求值）
//   expr == nullptr → 写成空括号 `[]`（只允许出现在形参第一维）
struct Dim : Node {
  std::unique_ptr<Expr> expr;

  Dim() = default;
  explicit Dim(SourceLoc l) : Node(l) {}
  // ⚠️ 这里【故意不提供】 `Dim(std::unique_ptr<Expr>)` 构造函数：
  //    它与编译器生成的移动构造 `Dim(Dim&&)` 在重载上会打架
  //    （`Dim d(std::move(uptr))` 两边都能匹配 → ambiguous）。
  //    正确写法是先构造再赋值：`Dim d(loc); d.expr = std::move(e);`
  Dim(Dim&&) = default;
  Dim& operator=(Dim&&) = default;

  const char* nodeKind() const override { return "Dim"; }
};

// 变量/形参/返回值的类型描述。
//   dims 为空  → 标量
//   dims 非空  → 数组，dims.size() 即维数
//   isFuncParam → 形参中的数组，其第一维在语法上允许为空（规范：`int a[]` / `int a[][5]`）
//
// 返回类型不用 dims（SysY 不允许返回数组），但复用同一个结构便于比较。
struct TypeSpec : Node {
  BType base = BType::Int;
  bool isFuncParam = false;
  std::vector<Dim> dims;

  TypeSpec() = default;
  explicit TypeSpec(SourceLoc l) : Node(l) {}
  TypeSpec(TypeSpec&&) = default;
  TypeSpec& operator=(TypeSpec&&) = default;

  bool isArray() const { return !dims.empty(); }
  const char* nodeKind() const override { return "TypeSpec"; }
};

// ============================================================================
// 2. 表达式（Expr）
//
// 层次严格对应文法：LOrExp → LAndExp → EqExp → RelExp → AddExp → MulExp
//                    → UnaryExp → (PrimaryExp / Call / LVal)
// 括号不是节点（`(a)` 与 `a` 产出同一棵树）—— 见 Parser::parsePrimary。
// ============================================================================
// 表达式基类。**显式析构**（各语句/声明节点也各自实现了析构），
// 理由见文件末尾的 destroyTree()。
struct Expr : Node {
  Expr() = default;
  explicit Expr(SourceLoc l) : Node(l) {}
  ~Expr() override = default;
  const char* nodeKind() const override { return "Expr"; }
};

// PrimaryExp → Number
//
// ⚠️ text 是 **std::string（自有）**，不是 string_view。这是 S02 用往返验证
//    抓出来的一个真实 bug：最初用 string_view 指向 SourceFile::text()，
//    在 `int f(){ return 1; }` 这种**字面量刚好写在语句末尾**的情形下会悬垂
//    （`1` 的 view 恰好落在一个被覆盖的 std::string 缓冲上 → 打印出来是乱码），
//    表现为"同一份 AST 打印两次结果不同"。字面量很短、数量有限，
//    自有一份文本的代价可以忽略，换来的是 AST 与源缓冲**完全解耦**。
struct IntLit : Expr {
  uint32_t value = 0;        // 辅助值；溢出时 parsed=false
  std::string text;          // ★ 原文（打印与往返靠它，永不丢失）
  bool parsed = true;        // value 是否可用

  IntLit() = default;
  IntLit(SourceLoc l, std::string_view t) : Expr(l), text(t) {}
  IntLit(IntLit&&) = default;
  const char* nodeKind() const override { return "IntLit"; }
};

struct FloatLit : Expr {
  float value = 0.0f;        // 辅助值；不可表示时 parsed=false
  std::string text;          // ★ 原文（如 `1e-3` / `0x1.921fb6p+1`）
  bool parsed = true;

  FloatLit() = default;
  FloatLit(SourceLoc l, std::string_view t) : Expr(l), text(t) {}
  FloatLit(FloatLit&&) = default;
  const char* nodeKind() const override { return "FloatLit"; }
};

// PrimaryExp → LVal | Ident '(' [FuncRParams] ')'
struct LVal : Expr {
  std::string name;
  std::vector<std::unique_ptr<Expr>> indices;   // a[i][j] → 2 个下标
  // 赋值语句左侧标记（仅供 S03 判断"可否被赋值"；不影响树形与打印）
  bool isAssignTarget = false;

  LVal() = default;
  explicit LVal(SourceLoc l) : Expr(l) {}
  const char* nodeKind() const override { return "LVal"; }
};

struct Call : Expr {
  std::string callee;
  std::vector<std::unique_ptr<Expr>> args;

  Call() = default;
  explicit Call(SourceLoc l) : Expr(l) {}
  const char* nodeKind() const override { return "Call"; }
};

// UnaryExp → ('+'|'-'|'!') UnaryExp
// op ∈ {Plus, Minus, Not}；右结合（前缀），由 Parser::parseUnary 递归实现。
struct Unary : Expr {
  TokKind op = TokKind::Plus;
  std::unique_ptr<Expr> operand;

  Unary() = default;
  Unary(SourceLoc l, TokKind o, std::unique_ptr<Expr> e)
      : Expr(l), op(o), operand(std::move(e)) {}
  const char* nodeKind() const override { return "Unary"; }
};

// 二元：op ∈ {Plus,Minus,Star,Slash,Percent,Less,Greater,LessEq,GreaterEq,EqEq,NotEq,AmpAmp,PipePipe}
// 全部**左结合**，由 Parser::parseBinary 的循环实现（不是递归。
// 若对 a+b+c+...（29_long_line 实测最长一行约 5000 个运算符）用递归，
// 树形会退化成 5000 层递归 → 栈溢出）。
struct Binary : Expr {
  TokKind op = TokKind::Plus;
  std::unique_ptr<Expr> lhs, rhs;

  Binary() = default;
  Binary(SourceLoc l, TokKind o, std::unique_ptr<Expr> a, std::unique_ptr<Expr> b)
      : Expr(l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
  const char* nodeKind() const override { return "Binary"; }
};

// ============================================================================
// 3. 初始化器
//
// InitVal → Exp | '{' [InitVal {',' InitVal}] '}'
//
// 【不变式】expr 与 list **互斥**：expr != nullptr 时 list 必为空，反之亦然。
//   标量形式 `= 1`      → expr
//   花括号形式 `= {…}`  → list（**扁平表示**：`{1,2,3}` 就是 3 个标量 InitVal，
//                        而不是"一个 list 里再套一个 list"）
//   空花括号 `= {}`     → expr == nullptr 且 list.empty()
//
// ⚠️ **故意不区分** `= {1}` 与 `= 1`（也故意不记录"用户写没写花括号"）。
//    原因：花括号的**嵌套**在 AST 里如果是"以第一个元素为形状"的，
//    序列化就会有歧义 —— 例如"标量 InitVal 后面还跟着兄弟"这种形态，
//    读回来时无法判断那是不是同一个列表的元素（S02 的往返实测抓到过）。
//    而 SysY 的语义里，`{1}` 与 `1` 对**标量对象**等价（作用都是"首元素 = 1"）；
//    对数组对象，是否带花括号只影响展开方式，由 S04 的 InitLowering 按
//    "扁平序列 + 维度"统一处理即可，不需要语法层预先建立嵌套。
//    ⇒ 这是一个**刻意的表示决定**，已写进 S02 报告。
// ============================================================================
struct InitVal : Node {
  std::unique_ptr<Expr> expr;                    // 标量形式 `= 1`（与 list 互斥）
  std::vector<std::unique_ptr<InitVal>> list;    // 列表形式 `= {1, {2,3}, {}}`（扁平）

  InitVal() = default;
  explicit InitVal(SourceLoc l) : Node(l) {}
  // 花括号可以无限嵌套（`int a = {{{…}}};`），所以列表必须**迭代**释放。
  // ⚠️ 这里只做"拆链 + 逐个 delete"：每个节点在 delete 之前已经把子节点
  //    release 干净了，于是它自己的析构不会向下递归。
  //    不要再调用 destroyTree —— 那是给表达式用的，在这里会互相递归。
  ~InitVal() override {
    destroyTree(expr.release());
    std::vector<InitVal*> stack;
    for (auto& c : list) stack.push_back(c.release());
    while (!stack.empty()) {
      InitVal* n = stack.back();
      stack.pop_back();
      if (n == nullptr) continue;
      destroyTree(n->expr.release());
      for (auto& c : n->list) stack.push_back(c.release());
      delete n;
    }
  }
  const char* nodeKind() const override { return "InitVal"; }
};

// ============================================================================
// 4. 声明
// ============================================================================

// VarDef → Ident {'[' Exp ']'} ['=' InitVal]
// dims 为空 → 标量；dims 非空 → 数组（dims.size() 为维数）。
// ⚠️ 非形参的数组**每一维都必须写长度**（规范）；`int a[];` 由 Parser 报错，
//    但为了诊断恢复仍然把空维存下来（AST 保持"照实记录"）。
struct VarDef : Node {
  std::string name;
  std::vector<Dim> dims;
  std::unique_ptr<InitVal> init;   // nullptr = 没有初始化器

  VarDef() = default;
  explicit VarDef(SourceLoc l) : Node(l) {}
  ~VarDef() override {
    for (Dim& d : dims) destroyTree(d.expr.release());
  }
  const char* nodeKind() const override { return "VarDef"; }
};

// Decl → ['const'] BType VarDef {',' VarDef} ';'
struct Decl : Node {
  bool isConst = false;
  BType base = BType::Int;
  std::vector<std::unique_ptr<VarDef>> defs;

  Decl() = default;
  explicit Decl(SourceLoc l) : Node(l) {}
  const char* nodeKind() const override { return "Decl"; }
};

// ============================================================================
// 5. 语句
// ============================================================================
struct Stmt : Node {
  Stmt() = default;
  explicit Stmt(SourceLoc l) : Node(l) {}
  const char* nodeKind() const override { return "Stmt"; }
};

// Stmt → LVal '=' Exp ';'
// 先用 Exp 解析整条语句，若形如 `LVal = Exp` 再改写成 AssignStmt（SysY 没有
// 逗号表达式/复合赋值，所以这条改写是安全的，且不需要回溯 —— 见 Parser::tryAssign）。
struct AssignStmt : Stmt {
  std::unique_ptr<LVal> lhs;
  std::unique_ptr<Expr> rhs;

  AssignStmt() = default;
  explicit AssignStmt(SourceLoc l) : Stmt(l) {}
  ~AssignStmt() override {
    destroyTree(lhs.release());
    destroyTree(rhs.release());
  }
  const char* nodeKind() const override { return "AssignStmt"; }
};

// Stmt → [Exp] ';'      expr == nullptr 即空语句 `;`
struct ExprStmt : Stmt {
  std::unique_ptr<Expr> expr;

  ExprStmt() = default;
  explicit ExprStmt(SourceLoc l) : Stmt(l) {}
  ~ExprStmt() override { destroyTree(expr.release()); }
  const char* nodeKind() const override { return "ExprStmt"; }
};

// Stmt → Block
// items 元素是 Decl 或 Stmt（S03 起可能扩到别的 Item，故按 Node 存）。
struct BlockStmt : Stmt {
  std::vector<std::unique_ptr<Node>> items;

  BlockStmt() = default;
  explicit BlockStmt(SourceLoc l) : Stmt(l) {}
  const char* nodeKind() const override { return "BlockStmt"; }
};

// Stmt → 'if' '(' Cond ')' Stmt ['else' Stmt]
// ⚠️ else 与【最近的】未配对 if 结合 —— 由递归下降天然保证（parseStmt 返回后
//    只有当前这一层会去吃掉 else）。见 test_parser.cpp 的 27/28 条。
struct IfStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> thenS;
  std::unique_ptr<Stmt> elseS;   // nullptr = 没有 else

  IfStmt() = default;
  explicit IfStmt(SourceLoc l) : Stmt(l) {}
  // 语句只持有"语句/表达式"各一个，链的深度受 Parser 的 kMaxDepth 约束
  // （每层 if/while 都要经过 parseStmt，深度计数必然增长），所以这里直接析构。
  ~IfStmt() override { destroyTree(cond.release()); }
  const char* nodeKind() const override { return "IfStmt"; }
};

// Stmt → 'while' '(' Cond ')' Stmt
struct WhileStmt : Stmt {
  std::unique_ptr<Expr> cond;
  std::unique_ptr<Stmt> body;

  WhileStmt() = default;
  explicit WhileStmt(SourceLoc l) : Stmt(l) {}
  ~WhileStmt() override { destroyTree(cond.release()); }
  const char* nodeKind() const override { return "WhileStmt"; }
};

struct BreakStmt : Stmt {
  BreakStmt() = default;
  explicit BreakStmt(SourceLoc l) : Stmt(l) {}
  const char* nodeKind() const override { return "BreakStmt"; }
};

struct ContinueStmt : Stmt {
  ContinueStmt() = default;
  explicit ContinueStmt(SourceLoc l) : Stmt(l) {}
  const char* nodeKind() const override { return "ContinueStmt"; }
};

// Stmt → 'return' [Exp] ';'      value == nullptr 即 `return;`
struct ReturnStmt : Stmt {
  std::unique_ptr<Expr> value;

  ReturnStmt() = default;
  explicit ReturnStmt(SourceLoc l) : Stmt(l) {}
  ~ReturnStmt() override { destroyTree(value.release()); }
  const char* nodeKind() const override { return "ReturnStmt"; }
};

// ============================================================================
// 6. 顶层
// ============================================================================

// FuncFParam → BType Ident ['[' ']' {'[' Exp ']'}]
//   int n          → dims 为空（标量）
//   int a[]        → dims = [Dim()]          （第一维为空）
//   int a[][5]     → dims = [Dim(), Dim(5)]
struct Param : Node {
  std::string name;
  TypeSpec type;   // type.isFuncParam = true 恒成立（在形参位置上）

  Param() = default;
  explicit Param(SourceLoc l) : Node(l) {}
  Param(Param&&) = default;              // 同上：TypeSpec 内含 vector<Dim>，必须可移动
  Param& operator=(Param&&) = default;
  ~Param() override {
    for (Dim& d : type.dims) destroyTree(d.expr.release());
  }
  const char* nodeKind() const override { return "Param"; }
};

// FuncDef → BType Ident '(' [FuncFParams] ')' Block
// 参数列表按值存 Param（每个 Param 自带 unique_ptr 成员，故 Param 不可拷贝，
// 用 vector<Param> + emplace_back 就地构造）。
struct FuncDef : Node {
  BType retType = BType::Int;   // 返回类型（isVoid 时该字段无意义，见下）
  bool isVoid = false;          // `void f()`：isVoid=true，retType 保持 Int 占位
  std::string name;
  std::vector<Param> params;
  std::unique_ptr<BlockStmt> body;   // 语法上必须有函数体（SysY 无函数声明语法）

  FuncDef() = default;
  explicit FuncDef(SourceLoc l) : Node(l) {}
  const char* nodeKind() const override { return "FuncDef"; }
};

// 顶层元素（CompUnit 与 Block 里可能出现的"项"）。Decl / FuncDef 都是 Item。
// 用一个空基类是为了以后（S03+）能往列表里放新的顶层节点而不改容器类型。
struct Item : Node {
  Item() = default;
  explicit Item(SourceLoc l) : Node(l) {}
  const char* nodeKind() const override { return "Item"; }
};

// CompUnit → {Decl | FuncDef}
struct CompUnit : Node {
  std::vector<std::unique_ptr<Node>> items;   // 元素是 Decl 或 FuncDef

  CompUnit() = default;
  explicit CompUnit(SourceLoc l) : Node(l) {}
  const char* nodeKind() const override { return "CompUnit"; }
};

// ============================================================================
// ★ destroyTree —— **迭代式**地释放一棵（子）树
//
// 为什么不能直接 `delete`（S02 实测到的崩溃）：
//   `unique_ptr` 的析构是**递归**的。对左结合的二元运算链，parser 用
//   *循环*建树，所以解析阶段不占栈；但树本身是 20 万层深的左倾链，
//   `delete root` 会递归 20 万层 → **栈溢出段错误**。
//   实测：`int main(){ int a; a = 1+1+1+…; }`（20 万个 `+1`）
//   在 `n≈3 万` 以上必崩，而"540 个文件全部不崩"是验收标准。
//
// 做法：显式后序遍历，用显式栈代替调用栈（O(树深) 堆内存）。
//   一次只 `delete` 真正无子节点的容器 —— 此时它的析构不会向下递归。
//   ⚠️ 以后新增"持有 Expr 子节点"的节点类型时，**必须**在这里登记，
//      否则那种形状的深树仍会崩（这是本函数唯一的维护点）。
// ============================================================================
inline void destroyTree(Expr* root) {
  if (root == nullptr) return;
  std::vector<Expr*> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    Expr* n = stack.back();
    stack.pop_back();
    if (n == nullptr) continue;

    const std::string_view k = n->nodeKind();
    if (k == "Binary") {
      auto* p = static_cast<Binary*>(n);
      stack.push_back(p->lhs.release());
      stack.push_back(p->rhs.release());
    } else if (k == "Unary") {
      auto* p = static_cast<Unary*>(n);
      stack.push_back(p->operand.release());
    } else if (k == "LVal") {
      auto* p = static_cast<LVal*>(n);
      for (auto& c : p->indices) stack.push_back(c.release());
    } else if (k == "Call") {
      auto* p = static_cast<Call*>(n);
      for (auto& c : p->args) stack.push_back(c.release());
    }
    delete n;   // 到这一步 n 已无子节点 ⇒ 析构不会向下递归
  }
}

}  // namespace sysy
#endif  // SYSY_FRONTEND_AST_H
