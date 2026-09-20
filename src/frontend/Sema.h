// ============================================================================
// Sema —— 语义分析主体（S03 交付物 #4）
//
// 职责：作用域与符号表、名字解析、类型检查、**隐式转换插入**（就地改写 AST）。
// 明确不做：常量折叠（只求值用于判定）、IRGen、任何优化（prompt §十一）。
//
// 规范依据逐条写在实现文件里；本文件只描述接口与**遍历架构**。
//
// ============================================================================
// ★★ 遍历架构：显式工作栈的任务机（**不许改成递归**）
// ============================================================================
//   prompt §九 把这条列为硬要求，理由是实测的两次事故：
//     * 打印器递归版在 6 万个 `+` 的链上 rc=139（SIGSEGV，编排方复现）；
//     * `destroyTree` 递归版在 3 万个 `+` 上必崩（S02 bug#7）。
//   语料里最长用例 86_long_code2.sy 的树深已经是 **4007**，而 Sema 还要
//   插入 `Cast` 节点让树更深。所以整棵树的遍历（表达式、语句、初始化器、
//   顶层）**全部**走同一个显式栈：内存 O(树深)，与调用栈无关。
//
//   帧（Frame）就是"递归版的调用帧"，`TK` 是它的种类：
//
//     Items       遍历一个 Node 列表（CompUnit 或 Block），带下标，不一次压满
//     FuncDef     进入函数：建签名 → 声明 → 压参数作用域 → 函数体 → 弹作用域
//     ParamDecl   声明一个形参（含同作用域重名判定）
//     PopScope    退出作用域（与 push 成对出现，**保证错误路径也不失衡**）
//     Decl        处理一个 Decl（逐个 VarDef）
//     VarDef      一个变量/常量定义：求维度 → 建类型 → 声明 → 检查初始化器
//     InitRoot    初始化器入口：分成"标量目标"与"数组目标"两条路
//     InitGroup   花括号组（C 语义的扁平游标；见 SemaDecl.cpp 的算法说明）
//     Stmt        一条语句
//     AssignRhs   赋值：左值已算完，接着算右值（右值的期望类型 = 左值类型）
//     ExprEnter   进入一个表达式（决定要不要下钻子节点）
//     ExprPost    表达式的后序动作（算自己的类型、插转换、套期望类型）
//
//   传递上下文的三个 flag（都随帧下传，不放进全局状态）：
//     kCondCtx       本子树位于 if/while 的条件之内 ⇒ `!` 合法（prompt §3.2-13）
//     kArgPos        本表达式是某个 `Call` 的**直接**实参 ⇒ 允许子数组传递
//     kStmtLevel     本表达式是某条表达式语句的整个表达式 ⇒ 允许 void 调用
//     kInitArrElem   本表达式是**数组**的初始化元素 ⇒ 整型数组不许出现浮点
//     kInitNeedConst 本初始化器必须是常量表达式（全局变量 / const 对象）
//                    —— 该 flag 只用于 InitRoot/InitGroup 帧之间的传递
//
// ============================================================================
// 语义决策（prompt §3.6，必须写进代码注释）
// ============================================================================
//   * `int` 32 位二进制补码、溢出回绕；`float` IEEE-754 单精度。
//   * float→int 越界是 UB；归一化策略（NaN→0；越界→饱和）与后端契约一致，
//     **实现落在 S05 的 IRGen**，S03 只在 ConstEval 的常量折叠里保持同一套
//     数值（`satFptosi`），并作为注释与文档存在。
//   * `x / 0` 与 `INT_MIN / -1` 归一化为 0（守卫在 S05 插）；S03 不报错、不警告。
// ============================================================================
#ifndef SYSY_FRONTEND_SEMA_H
#define SYSY_FRONTEND_SEMA_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/ConstEval.h"
#include "frontend/RuntimeLib.h"
#include "frontend/SemaScope.h"
#include "frontend/Type.h"
#include "support/Diagnostic.h"

namespace sysy {

// ============================================================================
// 诊断编号（**稳定契约**，prompt §3.5；名字逐字一致，大小写一致）
//   §八 的最小对照用例集按编号断言，check_sema.py 的坏输入探针也按编号断言。
// ============================================================================
namespace sema_diag {
inline constexpr const char* kUndef       = "E-UNDEF";        // 未定义标识符
inline constexpr const char* kRedef       = "E-REDEF";        // 重复定义
inline constexpr const char* kConstAssign = "E-CONST-ASSIGN"; // 给 const 赋值
inline constexpr const char* kNotVar      = "E-NOT-VAR";      // 赋值左侧不是变量
inline constexpr const char* kNotArray    = "E-NOT-ARRAY";    // 对标量取下标
inline constexpr const char* kArrayRank   = "E-ARRAY-RANK";   // 下标个数与维数不符
inline constexpr const char* kIndexType   = "E-INDEX-TYPE";   // 下标不是 int
inline constexpr const char* kArrayDim    = "E-ARRAY-DIM";    // 维度长度非法
inline constexpr const char* kType        = "E-TYPE";         // 类型不匹配
inline constexpr const char* kArgc        = "E-ARGC";         // 实参个数不符
inline constexpr const char* kArgType     = "E-ARGTYPE";      // 实参类型不符
inline constexpr const char* kCallNonFunc = "E-CALL-NONFUNC"; // 调用的名字不是函数
inline constexpr const char* kVoidValue   = "E-VOID-VALUE";   // void 用在需要值的位置
inline constexpr const char* kRetMissing  = "E-RET-MISSING";  // 非 void 的 return; 无值
inline constexpr const char* kRetValue    = "E-RET-VALUE";    // void 的 return expr;
inline constexpr const char* kNotContext  = "E-NOT-CONTEXT";  // `!` 出现在非条件表达式里
inline constexpr const char* kModFloat    = "E-MOD-FLOAT";    // `%` 的操作数是 float
inline constexpr const char* kConstInit   = "E-CONST-INIT";   // 全局初始化器不是常量
inline constexpr const char* kInitShape   = "E-INIT-SHAPE";   // 初始化器结构与类型不符
inline constexpr const char* kMain        = "E-MAIN";         // main 缺失或签名不符
inline constexpr const char* kCallVarargs = "E-CALL-VARARGS"; // 调用了不可调用的变参函数
}  // namespace sema_diag

// 帧的上下文标志位
enum : uint8_t {
  kCondCtx       = 1u << 0,
  kArgPos        = 1u << 1,
  kStmtLevel     = 1u << 2,
  kInitArrElem   = 1u << 3,
  kInitNeedConst = 1u << 4,   // 本初始化器必须是常量表达式（全局 / const）
};

class Sema : public ConstEnv {
 public:
  explicit Sema(DiagnosticEngine& diag) : diag_(diag), eval_(*this, diag) {}

  // 【前置】unit 是一棵语法上解析出来的树（可能带语法错误）。
  // 【后置】就地完成：填 `Expr::type` / `LVal::objType` / `VarDef::semType` /
  //         `Param::semType`，插入 `Cast` 节点，报出全部语义诊断。
  //         不抛异常；**总是**能返回（错误只影响退出码，不影响产物生成）。
  void run(CompUnit& unit);

  // ConstEnv：给 ConstEvaluator 查"已定义的符号常量"（标量与数组）。
  const ConstObject* findConst(const std::string& name) const override;

  size_t errorCount() const { return diag_.errorCount(); }

 private:
  // ── 任务种类 ──────────────────────────────────────────────────────────
  enum class TK : uint8_t {
    Items, FuncDef, ParamDecl, PopScope, Decl, VarDef,
    InitRoot, InitGroup, Stmt, AssignRhs, ExprEnter, ExprPost, CondFix, VarDefFinal,
  };

  struct Frame {
    TK kind = TK::Items;
    uint8_t state = 0;
    uint8_t flags = 0;
    Node* node = nullptr;
    const Decl* decl = nullptr;
    const std::vector<std::unique_ptr<Node>>* items = nullptr;
    size_t idx = 0;
    Expr* expr = nullptr;
    std::unique_ptr<Expr>* slot = nullptr;
    const Type* expected = nullptr;
    InitVal* init = nullptr;
    const Type* target = nullptr;
    int level = 0;      // 本组填充的维度层号（== 秩 表示作用在标量位置上）
    int depth = 0;      // 花括号的嵌套深度（顶层组 = 1）
    int64_t start = 0;
    int64_t end = 0;
    uint32_t paramIndex = 0;
  };

  // ── 任务机 ────────────────────────────────────────────────────────────
  void push(Frame f) { stack_.push_back(f); }
  void pushItems(const std::vector<std::unique_ptr<Node>>& items);
  void pushExpr(Expr& e, std::unique_ptr<Expr>* slot, const Type* expected, uint8_t flags);
  void pushStmt(Node* n);
  void pushDecl(Decl* d);
  void pushVarDef(const Decl* d, VarDef* v);
  void pushInitRoot(InitVal* iv, const Type* target, bool needConst);
  void pushInitGroup(InitVal* iv, const Type* target, int level, int depth, int64_t start,
                     uint8_t flags);

  void doItems(Frame& f);
  void doFuncDef(Frame& f);
  void doParamDecl(Frame& f);
  void doDecl(Frame& f);
  void doVarDef(Frame& f);
  void doInitRoot(Frame& f);
  void doInitGroup(Frame& f);
  void doStmt(Frame& f);
  void doAssignRhs(Frame& f);
  void doExprEnter(Frame& f);
  void doExprPost(Frame& f);
  void doCondFix(Frame& f);
  void doVarDefFinal(Frame& f);

  // ── 工具 ──────────────────────────────────────────────────────────────
  void error(SourceLoc loc, const char* code, const std::string& msg);
  void errorExpr(const Expr& e, const char* code, const std::string& msg) {
    error(e.loc, code, msg);
  }

  // 把一个表达式套上"期望类型"：类型不同就插 Cast（标量）或报错（数组等）。
  // flags 携带 kInitArrElem（整型数组不许出现浮点元素）。
  void applyExpected(std::unique_ptr<Expr>* slot, Expr& e, const Type* expected, uint8_t flags);

  // 把一个表达式就地包成转换节点（slot 指向持有它的槽位）。
  void wrapCast(std::unique_ptr<Expr>* slot, CastKind ck, const Type* target);

  // 构造声明/形参的类型（逐维求值）。维度非法时**报错但仍返回一个占位类型**，
  // 这样后续检查与转储都不会因为"类型为空"而崩或漏注解。
  const Type* buildArrayType(BType base, const std::vector<Dim>& dims, bool isArrayParam);

  // 常量性检查（全局初始化器 / const 初始化器必须是常量表达式）与常量求值。
  // 把求出的值按目标元素类型归一化后写进 initScratch_[pos]；失败置 initConstOk_=false。
  void evalConstInto(Expr& e, int64_t pos, const Type* elemType);

  // 作用域与符号
  ScopeStack scopes_;
  DiagnosticEngine& diag_;

  // ── 生命周期由 Sema 持有的池（指针要稳定，故用 deque / unique_ptr）──
  std::deque<FuncSig> sigs_;
  std::deque<ConstObject> consts_;
  ConstEvaluator eval_;

  // ── 运行状态 ──────────────────────────────────────────────────────────
  std::vector<Frame> stack_;
  const Type* curRetType_ = nullptr;   // 当前函数的返回类型（void 用 voidType）
  bool curRetIsVoid_ = false;
  const FuncDef* mainDef_ = nullptr;   // 顶层遇到的名为 main 的函数定义
  int64_t initPos_ = 0;                // InitGroup 的**扁平游标**（行主序）
  int64_t initTotal_ = 0;              // 目标数组的元素总数
  // ── 当前正在处理的初始化器的常量求值状态（**串行**，见 SemaDecl.cpp 的说明）──
  //   ★ S04：占位表示从 `std::vector<ConstValue>` 改成 `ConstObject`
  //     （"默认值 + 稀疏非零表"）。于是 `const int a[50000000] = {1};` 只花
  //     O(1) 内存，而不是 600 MB。**只有 const 对象才需要这份占位**
  //     （initMaterialize_），非 const 对象从来不写它。
  ConstObject initScratch_;              // 扁平元素值（行主序）
  bool initNeedConst_ = false;           // 本初始化器必须是常量表达式（**检查**）
  // 是否把逐元素值物化进 consts_（只有 const 对象才需要 —— 见 SemaDecl.cpp 的详细说明）
  bool initMaterialize_ = false;
  bool initConstOk_ = true;              // 到目前为止每个元素都求值成功
  const Type* initType_ = nullptr;       // 目标类型
  Symbol* initSym_ = nullptr;            // 求值成功后把 cval 挂到这个符号上
};

// ============================================================================
// 对外入口：分析一个编译单元（就地改写 + 诊断）
// 【后置】不抛异常；即使有错也保证 AST 结构完整、每个表达式都有类型。
// ============================================================================
void runSema(CompUnit& unit, DiagnosticEngine& diag);

}  // namespace sysy
#endif  // SYSY_FRONTEND_SEMA_H
