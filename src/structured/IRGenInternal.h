// ============================================================================
// IRGenInternal.h —— IRGen 的**实现私有**细节（不对外）
//
//   ⚠️ 命名约定：本文件里的一切都属于 IRGen 的实现细节。`IRGen.h` 只暴露
//      `buildModule`（唯一入口）；`Gen`、`Sym`、`GlobalRef` 都在这里 —— 这样
//      "改 IRGen 的内部结构"不会变成对外接口变更（D9：对外 API = 纯数据结构
//      + 遍历器）。
//
//   为什么需要这个头：`IRGen.cpp`（表达式与操作码工厂）、`IRGenStmt.cpp`
//   （语句与初始化降级）**都是 `Gen` 的成员函数定义**，必须共享同一个类声明。
//   拆成两个文件是为了 §C4 的单文件行数上限 —— 一个 1000 行的实现文件读不动。
// ============================================================================
#ifndef SYSY_STRUCTURED_IRGENINTERNAL_H
#define SYSY_STRUCTURED_IRGENINTERNAL_H

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/InitPlan.h"
#include "frontend/RuntimeLib.h"
#include "structured/IRGen.h"
#include "structured/IRGenImpl.h"
#include "support/Diagnostic.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace sir {

inline const Type* i32() { return typePool().i32(); }
inline const Type* i64() { return typePool().i64(); }
inline const Type* f32() { return typePool().f32(); }
inline const Type* voidTy() { return typePool().voidTy(); }
inline const Type* ptrTo(const Type* e) { return typePool().ptrTo(e); }
inline const Type* arrOf(const Type* e, int64_t n) { return typePool().arrayOf(e, n); }

// ── 递归深度上限（**内部安全阀，不是新诊断编号**）─────────────────────────
//   Parser 的 kMaxDepth 已经限过一次；这里是第二道闸。触发时**先报 error**
//   再降级（§九.6：任何"超限就放弃"的分支必须先报 error；本关不允许静默
//   产出空 IR）。6000 远大于语料实测（`86_long_code2` 的打印树深 4007）。
inline constexpr int kDepthLimit = 6000;
inline constexpr const char* kIrDiag = "E-IRGEN";

// ── 两个阈值（**通用代码结构判据，不是用例特判**）─────────────────────────
//   `Zero n`：n 小时逐元素 store 比 memset 便宜（少一次调用 + 指针转换）。
//   32 字节 = 8 个 int，是"调用开销 vs 8 条 store"的保守分界。
inline constexpr int64_t kZeroInlineBytes = 32;
//   常量池元素数上限：超过就**报 error 并逐元素降级**（不静默）。
inline constexpr int64_t kMaxPoolElements = 1 << 20;

// ============================================================================
// 符号表项
// ============================================================================
// 一个全局对象在 IR 里的"引用形式"：GetGlobalOp 的结果 + 它的 Sema 类型。
struct GlobalRef {
  Value slot = nullptr;
  const sysy::Type* objType = nullptr;
};

struct Sym {
  Value slot = nullptr;                // Alloca 结果 / 指针值 / GetGlobal 结果
  const sysy::Type* objType = nullptr; // **Sema 的**对象类型（数组形参已退化）
  bool byValuePtr = false;             // true ⇒ slot 本身就是指针（数组形参）
};

// ── 局部初始化计划的下标：**每个名字一个"按出现顺序的队列"** ──────────────
//   ★ 为什么不能只按名字查（这是一个用 bug 换来的教训）：
//     `InitPlan::locals` 的记录名是 `"<函数名>/<变量名>"`（S04 的契约），
//     而 **同名遮蔽**（`int g = 9; { int g = 11; }`）会产生**两条同名记录、
//     按源码顺序排列**。若按名字建 `map<string,size_t>`，后面的会被丢掉/覆盖，
//     内层的 `g` 就会拿到**外层**的初始化值 —— 而轨 A（往返相同）与轨 B/C
//     （结构合法）对"值取错了"**全盲**，只有轨 D（第二份独立实现）抓得住。
//   ⇒ 这里存 `名字 → 记录下标的队列` + 一个游标。`genVarDef` 按源码顺序被调用，
//     每次取队首并推进游标，于是"第 k 次声明同名变量"必然配到"第 k 条同名记录"。
//     ★ 另一层保险：每条动作的 `offset` 必须落在**这个对象**的字节范围内
//     （见 IRGenStmt.cpp 的 genVarDef）—— 错配通常会在那里露出来。
struct LocalIndex {
  std::unordered_map<std::string, std::vector<size_t>> slots;
  std::unordered_map<std::string, size_t> cursor;   // 名字 → 已消费到第几条

  void reset() { slots.clear(); cursor.clear(); }
  // 【后置】按"记录顺序"重建同名队列（`InitPlan::locals` 的顺序 = 源文件声明顺序）。
  void buildFrom(const std::vector<std::pair<std::string, size_t>>& ordered) {
    reset();
    for (const auto& kv : ordered) slots[kv.first].push_back(kv.second);
  }
  // 【后置】取该名字下一条未被消费的记录下标；取完/没有 → false。
  bool take(const std::string& name, size_t& out) {
    const auto it = slots.find(name);
    if (it == slots.end()) return false;
    const size_t k = cursor[name];
    if (k >= it->second.size()) return false;
    out = it->second[k];
    cursor[name] = k + 1;
    return true;
  }
};

// ============================================================================
// Gen
// ============================================================================
class Gen {
 public:
  Gen(Arena& a, const InitPlan& p, DiagnosticEngine& d) : arena_(a), plan_(p), diag_(d) {
    // 按**记录顺序**把下标压进"同名队列"（顺序 = 源文件声明顺序，S04 的契约）
    std::vector<std::pair<std::string, size_t>> ordered;
    ordered.reserve(plan_.locals.size());
    for (size_t i = 0; i < plan_.locals.size(); ++i) {
      ordered.emplace_back(plan_.locals[i].name, i);
    }
    localIdx_.buildFrom(ordered);
  }

  // 【后置】返回 ModuleOp（attrs = [源文件基名]，regions = [模块 Region]）。
  Op* build(const CompUnit& unit, const std::string& baseName);

 private:
  // ── 基本 ──────────────────────────────────────────────────────────────
  void emit(Op* op) { if (cur_ != nullptr) cur_->push(op); }
  Op* mk(OpKind k, SourceLoc loc) { return arena_.makeOp(k, loc); }
  Region* mkRegion() { return arena_.makeRegion(); }
  bool depthOk(int depth, SourceLoc loc);
  const Sym* findSym(const std::string& name) const;

  // ── Op 工厂（每个恰好一条指令；不折叠、不优化）────────────────────────
  Value cInt(int32_t v, SourceLoc loc);
  Value cFlt(uint32_t bits, SourceLoc loc);
  Value un(OpKind k, const Type* ty, Value a, SourceLoc loc);
  Value bin(OpKind k, const Type* ty, Value a, Value b, SourceLoc loc);
  Value cmp(OpKind k, Value a, Value b, SourceLoc loc);
  Value emitAlloca(const Type* objTy, SourceLoc loc);
  Value load(const Type* ty, Value p, SourceLoc loc);
  void store(const Type* ty, Value v, Value p, SourceLoc loc);
  Value gep(const Type* elemTy, Value base, Value iv, SourceLoc loc);
  Value bitcast(const Type* dst, Value p, SourceLoc loc);
  Value callTo(const std::string& callee, const std::vector<Value>& args, const Type* ret,
               SourceLoc loc);
  void terminator(OpKind k, const std::vector<Value>& ops, SourceLoc loc);
  // 【后置】i32 → i64（prompt §五.2 的 sext；**不许用 zext**）。
  Value sextI64(Value v, SourceLoc loc);

  // ── 表达式 ────────────────────────────────────────────────────────────
  Value genExpr(const Expr* e, int depth);
  Value genExprRec(const Expr* e, int depth);
  Value genLValAddr(const LVal& lv, int depth);
  Value genLValValue(const LVal& lv, int depth);
  Value genCall(const Call& c, int depth);
  Value genCast(const Cast& c, int depth);
  Value genBinary(const Binary& b, int depth);
  Value genIndexChain(const sysy::Type* startObjTy, const LVal& lv, int depth,
                      Value base);
  Value genLogical(const Binary& b, int depth);
  Value genCmp(OpKind k, Value a, Value b, bool floatOp, SourceLoc loc);
  Value genNormDiv(Value a, Value b, SourceLoc loc);
  Value genNormRem(Value a, Value b, SourceLoc loc);
  Value genSatFptosi(Value v, SourceLoc loc);
  // 【后置】把"按 cond 在 then/else 两个 Region 里各算一个 i32"降级成
  //   **内存形式**：`slot = ...; if (cond) { slot = then } else { slot = else }`，
  //   返回 `load slot`。
  //   ★ 为什么不用"带结果的 IfOp"（MLIR `scf.if` 形态）：那会让 dump 出现
  //     **文本歧义** —— `(If %x @line 5) {` 里的 `%x` 既可能是结果名、也可能是
  //     条件操作数，而两者在文本上都写在 Region 之前，**读回器无法区分**
  //     （实测：往返后条件变成 `%?`）。结构化层本来就是"变量全在内存"
  //     （设计文档 §1.1），用临时槽是**与本层语义一致**的表达，且 mem2reg(S09)
  //     在平面层会把这些槽全部提升掉。
  Value genConditionalI32(Value cond, const std::function<void()>& genThen,
                          const std::function<void()>& genElse, SourceLoc loc);
  // 【后置】把 `then` 里 yield 的 i32 写进 slot（供 genConditionalI32 复用）
  void genYieldInto(Region* r, const std::function<void()>& body, SourceLoc loc);
  // 【后置】在一个**新** Region 里生成 body 的语句，并以终结 Op 收尾；
  //         内部保存/恢复 cur_。返回该 Region（调用方负责挂到 Op 上）。
  Region* makeBodyRegion(const Stmt* body, SourceLoc loc, int depth);
  // 【后置】同上，但内容是"算 cond 然后 `YieldOp <bool>`"（条件 Region）。
  Value genCondValue(const Expr* cond, Region* dst, int depth);

  // ── 语句 ──────────────────────────────────────────────────────────────
  void genStmt(const Node* n, int depth);
  // 【后置】这条语句**无条件终止**当前 Region 的控制流（`return`/`break`/
  //   `continue`，或"两个分支都终止"的 `if`）。用途：块里它之后的语句是
  //   **不可达代码**，IRGen **不生成**它们（I4：终结 Op 必须是 Region 的最后
  //   一行；生成死代码会让它出现在中间）。S06 的展平因此不需要死块清理。
  bool terminates(const Node* n) const;
  // 【后置】把值变成"类型为 want 的值"（类型不符则插转换；`v == nullptr` 补零值）。
  //   用途：让 IRGen 在"上游已报错"的残缺树上也**永不产出坏 IR**
  //   （终结符类型匹配是六条不变式之一）。
  Value coerceTo(Value v, const Type* want, SourceLoc loc);
  // 【后置】返回非空值（`v == nullptr` 时按 `fallbackTy` 补零）。
  Value orZero(Value v, const sysy::Type* fallbackTy, SourceLoc loc);
  void genDecl(const Decl& d, int depth);
  void genVarDef(const VarDef& v);
  void genAction(const InitAction& a, Value slot, const Type* elemTy, SourceLoc loc);

  // ── 顶层 ──────────────────────────────────────────────────────────────
  void genGlobals();
  void genFunction(const FuncDef& f);

  // ── 初始化降级 ────────────────────────────────────────────────────────
  void emitZero(Value dst, int64_t bytes, SourceLoc loc);
  void emitMemset(Value dst, int64_t bytes, SourceLoc loc);
  void emitMemcpyConst(Value dst, const std::vector<ConstValue>& vals, SourceLoc loc);

  Arena& arena_;
  const InitPlan& plan_;
  DiagnosticEngine& diag_;
  LocalIndex localIdx_;

  Region* cur_ = nullptr;        // 当前写入的 Region
  Region* modRegion_ = nullptr;  // 模块 Region（调试用）
  Region* funcEntry_ = nullptr;  // 函数入口 Region（**所有 alloca 都写这里**）
  std::string funcName_;
  const Type* curRetType_ = nullptr;
  std::vector<std::pair<std::string, Sym>> syms_;
  size_t poolSeq_ = 0;           // `<const.N>` 常量池序号（函数内）
  bool overflow_ = false;        // 超限诊断只报一次
  std::vector<Op*> globalOps_;   // 全局 Op（顺序 = 源文件声明顺序）
  std::vector<std::pair<std::string, GlobalRef>> globals_;   // 名字 → 引用形式
};
}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_IRGENINTERNAL_H
