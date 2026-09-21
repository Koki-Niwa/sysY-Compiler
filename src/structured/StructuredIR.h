// ============================================================================
// StructuredIR —— 结构化 IR 的**容器**（S05 交付物 #1）
//
// 规格来源：`docs/handoff/03-设计/结构化IR设计.md` §1.1 / §1.2（决策 D18）。
// 本文件只定义**数据结构与容器原语**，不含任何语义（IRGen 在 IRGen.cpp，
// 不变量检查在 StructuredVerifier.cpp，文本格式在 StructuredDump.cpp）。
//
// ── 这个文件里最不可逆的一条：`results` 是 0..N，不是"恰好 1" ─────────────
//   设计文档 §1.1 记了一次血的教训：把"75.4% 的 Op 恰好一个结果"这个**统计**
//   当成**结构**（`class Value { Op* defining; }`），代价是整个 IR 层 ——
//   一个 Op 的两个结果会退化成**同一个 Value**（不是不方便，是**表达不出来**）。
//   所以这里：
//     * `std::vector<Result> results` —— **值存储**（POD，16 字节/个，1 次分配），
//       而不是 `vector<Result*>`（1+N 次分配）。理由见设计文档 §1.1。
//     * `using Value = Result*` —— **值与定义是两个对象、一对多**。
//     * `Result` 里带 `index` ⇒ `op.result(0) != op.result(1)` 是**可判定**的。
//   容器**支持** 0..N；SysY 实际只产出 0 或 1。`unit/test_structured.cpp` 里有一条
//   手工构造双结果 Op 的断言，把"支持 0..N"从注释变成**可执行的契约**。
//
// ── 为什么没有基本块 / 没有 φ / 没有 SSA ─────────────────────────────────
//   * 变量**全在内存**：`alloca` + `load`/`store`（铁律 1：前端禁止值传播，
//     所有变量读出必须经过 `load` —— 自研 mem2reg(S09) 的输入就是这些 load）。
//   * 控制流是**树**：`WhileOp`/`IfOp`/`ForOp` 各带自己的 Region。
//
// ── 生命周期：Arena（这是 `Value = Result*` 能成立的前提）─────────────────
//   所有 `Op` 与 `Region` 由 `Arena` 分配、由 Arena 统一释放。于是：
//     * 指向 `Result` 的裸指针（= `Value`）在整个编译过程中**不会失效**；
//     * 释放是**迭代**的（`Op` 不持有 `Region`/`Op` 的所有权，Arena 逐个 delete），
//       所以 4000 层深的 IR 也不会在析构时爆栈（S02 的 `destroyTree` 教训）。
//
// ── 类型表示（prompt §五.12 要求写清"我们的指针类型是怎么表示的"）─────────
//   `Type` 是**结构化 IR 自己的类型**（与 AST 的 `sysy::Type` 无关）：
//       i32 · f32 · void · ptr[T] · [N x T]
//   指针类型**显式携带元素类型**（`ptr[i32]` / `ptr[[3 x [2 x i32]]]`），不用
//   LLVM 的 opaque `ptr`。理由：typed pointer 语义下 S06/S07 必须知道每次
//   `load`/`store`/`gep` 的元素类型才能发射，而"从 alloca 的原始类型再算一遍"
//   在**子数组传参**（`getarray(A)` 把 `int[1400][1400]` 当 `int*`）这条路径上
//   会算错 —— 那时指针的类型已经不是任何对象的类型了。显式携带 ⇒ 转换点只有
//   一处（IRGen 发射 `BitcastOp`），后端照结果类型发射即可。
// ============================================================================
#ifndef SYSY_STRUCTURED_STRUCTUREDIR_H
#define SYSY_STRUCTURED_STRUCTUREDIR_H

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "support/SourceLoc.h"

namespace sysy {
namespace sir {

class Op;
class Region;

// ============================================================================
// 0. Op 集合（**初版冻结**，设计文档 §1.2 / prompt §4.1）
//
//   ★ SysY **没有**移位/位运算符（540 个用例实测 0 次）⇒ 这里**故意没有**
//     AndI/OrI/XorI/LShift/RShift。**多一个就是违规**：下游后端是按这张表实现的，
//     `check_structured.py` 的轨 B 会检查"用到的 OpKind 全部在这张表里"。
// ============================================================================
enum class OpKind : uint8_t {
  // ── 函数 / 模块 ──────────────────────────────────────────────────────
  Module,       // (Module <源文件基名>)   —— 整个 dump 的根，1 个
  GlobalVar,    // 全局对象：attrs = [name, kind("zero"/"data"), data?]
  Func,         // 函数：attrs = [name, retType?, paramTypes...]，regions = [body]
  GetArg,       // 取第 i 个形参：attrs = [index]
  Return,       // 0 或 1 个操作数
  Call,         // attrs = [callee]，操作数 = 实参；0 或 1 个结果

  // ── 循环与分支（控制流**树**）────────────────────────────────────────
  For,          // attrs = [iv]，操作数 = [lower, upper, step]，regions = [body]
  While,        // regions = [cond, body]
  If,           // 操作数 = [cond]，regions = [then] 或 [then, else]
  Goto,         // 无条件转移（结构化层未用；平面化的预留形态）
  Yield,        // Region 终结：继续（while 条件 / 循环体 / then / else 都用它）
  Break,        // Region 终结：跳出循环（S05 里也承担 `continue`，S05b 消解）

  // ── 内存 ────────────────────────────────────────────────────────────
  Alloca,       // attrs = [type(被分配对象)]，1 个结果 ptr[type]
  Load,         // attrs = [type(元素)]，操作数 = [指针]
  Store,        // attrs = [type(元素)]，操作数 = [值, 指针]
  GetElementPtr,// attrs = [elemType, indexType, affinity]，操作数 = [指针, 下标]
  GetGlobal,    // attrs = [全局名, 对象类型]，1 个结果 ptr[对象类型]
  Bitcast,      // attrs = [dstType]，操作数 = [指针]（子数组传参用的指针转换）

  // ── 整数（i32，二进制补码、溢出回绕；**永不打 nsw/nuw**，铁律 6）─────
  AddI, SubI, MulI, DivI, ModI, MinusI,

  // ── 浮点（f32；**永不加** fast/reassoc/nsz/contract，实测 FMA 收缩会改判分）─
  AddF, SubF, MulF, DivF, MinusF,

  // ── 比较（结果一律 i32；浮点用有序/无序谓词，见 IRGenImpl 的映射表）──
  Eq, Ne, Lt, Le, Gt, Ge,

  // ── 转换 ────────────────────────────────────────────────────────────
  //   ★ `Sext` 是 prompt §五.2 明文要求的：数组下标在 **i32** 里算完（会回绕），
  //     再 `sext i32→i64`，最后 `getelementptr`。**不许用 zext**（负下标会错）。
  //     结果类型 = i64（结构化层唯一的 64 位整数类型，只用于地址算术）。
  I2F, F2I, Sext,

  // ── 常量 ────────────────────────────────────────────────────────────
  Int,          // attrs = [i32 十进制值]
  Float,        // attrs = [f32 十六进制浮点（精确、可读回）]

  // ── 其他（S05 不产出；容器与检查器支持）──────────────────────────────
  Select, Phi,  // Phi **只在平面层用**（S06）
  Unreachable,  // Region 终结：不可达（F2I 的饱和链用它做 dead-end）
};

// 【后置】返回 OpKind 的**文本拼写**（dump 与读回共用；**唯一实现**）。
//         合法范围内一定返回值；越界返回 "?"（调用方应视为内部错误）。
const char* opKindName(OpKind k);

// 【后置】按文本拼写反查 OpKind（读回用）。不认识 → 返回 false。
bool opKindFromName(const std::string& name, OpKind& out);

// 【后置】该 OpKind 是不是"终结 Op"（I4：每个 Region 恰好一个终结 Op，且在最后）。
bool isTerminator(OpKind k);

// 【后置】该 OpKind 是不是"控制流容器"（I5：只允许这几种；其余 Op 不许带 Region）。
bool isControlFlowContainer(OpKind k);

// ============================================================================
// 1. 类型（结构化 IR 自己的类型）
// ============================================================================
enum class TypeKind : uint8_t { Void, I32, I64, F32, Ptr, Array };

struct Type {
  TypeKind kind = TypeKind::I32;
  const Type* elem = nullptr;   // Ptr 的元素类型 / Array 的元素类型
  int64_t len = 0;              // 仅 Array：元素个数（>= 0）

  bool isPtr() const { return kind == TypeKind::Ptr; }
  bool isArray() const { return kind == TypeKind::Array; }
};

// 进程内唯一的类型池（interned：同构类型只有一个实例 ⇒ 指针相等 ⇔ 结构相等）。
// 生命期 = 进程（比任何 Module 长）。理由与 `sysy::typeContext()` 相同。
class TypePool {
 public:
  const Type* voidTy();
  const Type* i32();
  const Type* i64();
  const Type* f32();
  // 【前置】elem != nullptr。len >= 0。
  const Type* ptrTo(const Type* elem);
  const Type* arrayOf(const Type* elem, int64_t len);

 private:
  std::vector<Type*> pool_;
};

// 【后置】返回进程唯一的类型池。
TypePool& typePool();

// 【后置】把类型写成文本（`i32` / `ptr[i32]` / `[3 x [2 x i32]]`）。
void appendTypeText(std::string& out, const Type* t);
std::string typeText(const Type* t);

// 【前置】s 从 pos 起应当是一段类型文本（dump 里打印出来的那种）。
// 【后置】成功 → 返回类型并把 pos 推到类型末尾之后；失败 → 返回 nullptr，
//         pos 不动。**不抛异常**（读回畸形输入要能报错而不是崩，C5）。
const Type* parseTypeText(const std::string& s, size_t& pos);

// 【后置】该类型的**字节大小**（i32/f32 = 4；数组 = len × elem；void/未知 = 0）。
//         结构化层不做布局计算，只用于"同一性/零长度"的判定。
int64_t typeByteSize(const Type* t);

// 【后置】该类型的元素类型（数组 → 元素；非数组 → 自身）。
const Type* typeElem(const Type* t);

// ============================================================================
// 2. 值 / 结果 / 属性
// ============================================================================
// 一个 Op 的一个结果。**POD，16 字节**（见设计文档 §1.1：`results` 值存储）。
struct Result {
  Op* definer = nullptr;    // 哪个 Op 产生的
  uint32_t index = 0;       // 是该 Op 的第几个结果
  const Type* type = nullptr;   // 结果类型（IRGen 填；读回时按 OpKind/attrs 重建）
};
static_assert(sizeof(Result) <= 24, "Result 必须是 POD（16 字节量级）");

// ★ 值 = 指向某个结果的指针（**与 Op 分离**，且一对多）。
using Value = Result*;

// 属性。四种形态够表达全部 Op：名字（字符串）、类型、整数、浮点原始位、数据表。
struct Attr {
  enum class Kind : uint8_t { Str, Type, Int, Float, Data } kind = Kind::Int;
  std::string s;                                   // Str
  const Type* ty = nullptr;                        // Type
  int64_t i = 0;                                   // Int（也用于非仿射标记等枚举）
  uint32_t fbits = 0;                              // Float（IEEE-754 原始位，无损）
  std::vector<std::pair<uint64_t, uint32_t>> data; // Data：字节偏移 → 32 位值

  static Attr ofStr(std::string v);
  static Attr ofType(const Type* t);
  static Attr ofInt(int64_t v);
  static Attr ofFBits(uint32_t bits);
};

// ============================================================================
// 3. Region —— 一个 Op 序列
// ============================================================================
class Region {
 public:
  // 【前置】op != nullptr。
  // 【后置】op 追加到末尾；父 Op / 所属 Region 不变。
  // 【副作用】无（不检查 I4；那是 StructuredVerifier 的职责）。
  void push(Op* op) { ops_.push_back(op); }

  size_t size() const { return ops_.size(); }
  bool empty() const { return ops_.empty(); }
  Op* at(size_t i) const { return ops_[i]; }
  Op* back() const { return ops_.empty() ? nullptr : ops_.back(); }
  const std::vector<Op*>& ops() const { return ops_; }
  // 【后置】Region 里的 Op 列表（只读）。供 dump/读回/检查器遍历。
  const std::vector<Op*>& all() const { return ops_; }

  // 【前置】无（`ops` 里的空指针原样保留）。
  // 【后置】Region 的内容**整体替换**为 `ops`（顺序即给定顺序）。
  // 【副作用】`ops_` 变成 `ops` 的副本。**`Op*` 本身不失效**（Arena 持有）。
  // ★ 为什么必须新增这个方法（S05b 是第一个**变换**）：
  //   S05 只做"构造"（一路 `push`），所以容器只留了"追加"。而 `LoopNormalize`
  //   要"把条件 Region 的边界表达式搬到循环之前"、"把 `while` 换成 `for`"、
  //   "摘掉体内末尾的自增" —— 全是**改写中间位置**。没有这个方法就只能
  //   `const_cast` 绕过容器契约（那会让"顺序由容器保证"这条前提失效）。
  //   加它与 SESSION-PLAN §5 的"S05 结束后结构化 IR 的 Region 接口**只增不改**"
  //   一致：`push` 的语义一个字没动，只是多了一个受控的整体替换入口。
  //   ⚠️ 调用者必须**一次性**给出完整序列：本方法不做任何 I4/use-def 检查
  //      （那是 StructuredVerifier 的职责）。
  void replaceAll(std::vector<Op*> ops) { ops_.swap(ops); }

 private:
  std::vector<Op*> ops_;
};

// ============================================================================
// 4. Op
// ============================================================================
class Op {
 public:
  explicit Op(OpKind k) : kind(k) {}

  // 【前置】type != nullptr。
  // 【后置】追加一个结果并返回**指向它的指针**（Arena 保证永不失效）。
  // 【副作用】改变 results（**只在构造期调用**：结果个数定了就不再变，
  //           这是 `results` 值存储 + 裸指针稳定的前提）。
  Value addResult(const Type* type);

  // 【后置】返回第 i 个结果；越界 → nullptr（**不抛异常、不 assert**：读回
  //         畸形输入时调用方要能报错而不是崩，C5）。
  Value result(size_t i) {
    return i < results_.size() ? &results_[i] : nullptr;
  }
  const Result* result(size_t i) const {
    return i < results_.size() ? &results_[i] : nullptr;
  }
  // 【后置】单一结果（**只在"这个 Op 恰恰只有一个结果"时**可用；SysY 的绝大多数
  //         Op 如此）。多结果时返回 result(0)，调用方必须先自行确认个数。
  Value first() { return results_.empty() ? nullptr : &results_[0]; }

  size_t numResults() const { return results_.size(); }
  size_t numOperands() const { return operands_.size(); }
  size_t numRegions() const { return regions_.size(); }
  size_t numAttrs() const { return attrs_.size(); }

  Value operand(size_t i) const { return i < operands_.size() ? operands_[i] : nullptr; }
  Region* region(size_t i) const { return i < regions_.size() ? regions_[i] : nullptr; }
  const Attr* attr(size_t i) const { return i < attrs_.size() ? &attrs_[i] : nullptr; }
  const Attr* attrAt(size_t i) const { return attr(i); }

  void addOperand(Value v) { operands_.push_back(v); }
  void addRegion(Region* r) { regions_.push_back(r); }
  void addAttr(Attr a) { attrs_.push_back(std::move(a)); }

  // 属性查询（读回/检查器用；越界返回默认值，不崩）。
  const std::string& strAttr(size_t i) const;
  int64_t intAttr(size_t i) const;
  const Type* typeAttr(size_t i) const;

  OpKind kind;
  SourceLoc loc;                      // D11：位置一路带到 IR（`@line` 属性）

  // 容器的三个序列**只读暴露**（dump/读回/检查器要遍历它们，但只允许通过
  // addXxx 增长 —— 这样"结果个数定了就不再变"这条前提不会被外部悄悄破坏）。
  const std::vector<Value>& operands() const { return operands_; }
  const std::deque<Result>& results() const { return results_; }
  const std::vector<Attr>& attrs() const { return attrs_; }
  const std::vector<Region*>& regions() const { return regions_; }

 private:
  std::vector<Value> operands_;
  // ★ 0..N，**值存储**（不是指针数组）—— 但用 `std::deque` 而不是 `vector`：
  //   `addResult` 会返回 `&results[i]`，而 `Value = Result*` 必须**永不失效**。
  //   `vector` 在扩容时会把已有元素搬走（引用/指针全部失效），deque 的
  //   "按块分配 + 不搬动已有元素"正好提供这个保证，且**语义仍是值存储**
  //   （不是 `vector<Result*>`：那会为"0 结果的 Op"也浪费一次分配，
  //    违背设计文档 §1.1 的明文禁止）。
  std::deque<Result> results_;
  std::vector<Attr> attrs_;
  std::vector<Region*> regions_;
};

// ============================================================================
// 5. Arena —— Op/Region 的唯一所有者（迭代释放）
// ============================================================================
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() { clear(); }

  // 【后置】返回一个由 Arena 持有的 Op（指针在 Arena 生命期内稳定）。
  Op* makeOp(OpKind k, SourceLoc loc);
  // 【后置】同上，Region。
  Region* makeRegion();

  // 【后置】释放全部 Op/Region。**迭代**（不用递归）：4000 层深的 IR 也不爆栈。
  void clear();

  size_t opCount() const { return ops_.size(); }
  size_t regionCount() const { return regions_.size(); }

 private:
  std::vector<Op*> ops_;          // Op 的析构是平凡的（vector 成员会自己释放）
  std::vector<Region*> regions_;
};

// ============================================================================
// 6. 遍历原语（**迭代**，显式工作栈 —— S02/S03 的教训：不许递归）
// ============================================================================
// 【后置】对 module 做**先序**遍历，对每个 Op 调用 f（含 Module 自己）。
//         显式工作栈，内存 O(深度)（**与树深无关的调用栈**）。
void forEachOp(Op* root, const std::function<void(Op*)>& f);

// 【后置】统计 root 子树（含自己）里 kind == k 的 Op 个数。
size_t countOps(Op* root, OpKind k);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_STRUCTUREDIR_H
