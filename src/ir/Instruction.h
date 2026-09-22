// ============================================================================
// ir/Instruction.h —— 平面层的**指令容器**
//
// ── 与结构化层最重要的一处差别（**类型签名上写清，免得后来者以为
//    结构化层的 0..N 是多余的**）────────────────────────────────────────────
//   平面层**每条指令恰好一个结果**（`Value*`，没有"结果表"）。
//   这不是"结构化层的 0..N 是浪费"，而是**LLVM IR 子集本身的性质**：
//     ① 我们发射的目标方言里，除了 `store`/`br`/`ret`/`unreachable`（无结果）
//        之外，每条指令都恰好一个结果 —— 这是 LLVM 的设计，不是我们的选择；
//     ② 平面层的消费者（mem2reg/GVN/后端）全部按"指令 = 一个值"写；
//     ③ 若这里也叫 `results()`，`S09` 的每个 pass 都要先解包一次。
//   反向的证据在 `StructuredIR.h` 的文件头：**那里** `results` 必须是 0..N，
//   因为设计文档 §1.1 记的教训是"把统计当结构"（一个 Op 的两个结果退化成
//   同一个 Value，是**表达不出来**，不是不方便）。两层各自承担自己的表达力。
//
// ── 操作数的三种形态（prompt §三.2 要求三种都要能表达）────────────────────
//   `Value*` 覆盖两种：**指令结果**（`Instruction` 自己就是 `Value`）与
//   **常量**（`Constant`）；第三种是**基本块**（分支目标），
//   由 `succs()`（`std::vector<BasicBlock*>`）表达。
//   为什么块不塞进 `operands`：块不是值（没有类型），而 `iset.txt` 的
//   `br`/`phi` 在文本里也是分开写的 —— 混在一起会让"操作数"这个概念失去类型。
//
// ── 指令集封闭（**一条铁律，两个方向都查**）──────────────────────────────
//   `Opcode` 与 `docs/handoff/iset.txt` **一一对应**：35 个 opcode / 37 种形式。
//   **不得多、不得少**。轨 B 用一份**独立手写**的清单核对（手写是关键：
//   从 C++ 枚举生成等于自己证明自己）。
//   ⚠️ 特别地：**`neg` 不是 LLVM 指令**（S00 实测踩过），取负一律用
//      `sub 0, x`（`Fold` 那类"顺手造一条 neg"的想法是违规的）。
//   `Nop` 是**参数占位**（形参在平面层也是一个 `Value`，但不是指令），
//   它在 §5 的清单里**故意没有对应拼法** ⇒ 不可能被 dump 出来。
//
// 【冻结】S06 结束后 `ir/` 的全部类接口**只增不改**（SESSION-PLAN §5）。
// ============================================================================
#ifndef SYSY_IR_INSTRUCTION_H
#define SYSY_IR_INSTRUCTION_H

#include <cstdint>
#include <string>
#include <vector>

#include "ir/Type.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

class BasicBlock;
class Function;
class Module;

// ============================================================================
// 0. opcode（**与 iset.txt 一一对应**）
//
//   打印名 = LLVM 子集里的拼写。`kOpcodeText[]`（Instruction.cpp）是**唯一**
//   的"印刷/识别"实现，dump 与读回都走它 —— 与 S05 的 `opKindName` 同一个手法：
//   印刷与识别不可能漂移。
// ============================================================================
enum class Opcode : uint8_t {
  Nop = 0,      // 形参占位（**不是指令**，没有文本拼法）
  // ── 常量 ────────────────────────────────────────────────────────────
  //   ⚠️ 常量**不是 iset.txt 里的 opcode**（它是文本层的"字面量"）。
  //     平面层的**值**表示里必须有一种"常量值"，而"平面层每条指令恰好一个
  //      结果"又要求它有一个落点 ⇒ 这里给两个**容器内部**的 opcode：
  //     它们**不计入 35 个 opcode**，dump 时打印成 `%7 = i32 5`，
  //      轨 B 的封闭性检查**显式**把它们排除（并且独立手写清单里也没有它们）。
  ConstantInt,  // i32 / i64 字面量（类型由 `type()` 决定）
  ConstantFP,   // f32 字面量（位模式在 `fbits_`）
  // ── 终结符（每个块恰好一条，且在最后）────────────────────────────────
  Ret,          // ret void | ret <ty> %v
  Br,           // br label %L | br i1 %c, label %A, label %B
  Unreachable,  // unreachable
  // ── 内存 ────────────────────────────────────────────────────────────
  Alloca,       // alloca <ty>                     → ptr[ty]
  Load,         // load <ty>, ptr[ty] %p           → ty
  Store,        // store <ty> %v, ptr[ty] %p       → （无结果）
  GEP,          // getelementptr <ty>, ptr[ty] %p, i64 %i → ptr[ty]
  // ── 整数（i32，二进制补码、溢出回绕；**永不打 nsw/nuw**，D6）─────────
  Add, Sub, Mul, SDiv, SRem,
  Shl, LShr, AShr, And, Or, Xor,
  // ── 浮点（f32；**永不加** fast/reassoc/nsz/contract）──────────────────
  FAdd, FSub, FMul, FDiv, FNeg,
  // ── 比较（结果一律 i32）──────────────────────────────────────────────
  ICmp,         // eq ne slt sle sgt sge
  FCmp,         // oeq une olt ole ogt oge
  // ── 转换 ────────────────────────────────────────────────────────────
  SIToFP,       // i32 → f32
  FPToSI,       // f32 → i32
  FPExt,        // f32 → double（`putf` 的变参）
  SExt,         // i32 → i64（数组下标）
  ZExt,         // i32 → i64
  Trunc,        // i64 → i32
  BitCast,      // ptr[A] → ptr[B]
  // ── 调用 / SSA ──────────────────────────────────────────────────────
  Call,         // call <retty> @f(args) | call void @f(args)
  //   两个内建（`llvm.memcpy`/`llvm.memset`）**不是 opcode**，是 `Call` 的
  //   被调名；但它们要能同"普通函数名"区分，所以单列两个 opcode 值。
  //   它们**同样不计入 35 个 opcode**（`iset.txt` 把它们列为 2 个 intrinsic）。
  LLVMMemCpy,
  LLVMMemSet,
  Phi,          // phi <ty> [(%v L0) ...]
  Select,       // select i1 %c, <ty> %a, <ty> %b
  //   `Nop` / 两个常量 / 两个 intrinsic 是**容器内部**的 opcode，
  //   不计入 iset.txt 的 35 个（轨 B 的清单显式排除它们）。
  kCount
};

// 比较谓词。icmp 与 fcmp 各有 6 个（`iset.txt` 明文列出 fcmp 的 6 个谓词；
// icmp 取 LLVM 里与 SysY 语义对应的有符号/有序子集）。
enum class IPred : uint8_t { Eq = 0, Ne, Slt, Sle, Sgt, Sge, kCount };
enum class FPred : uint8_t { Oeq = 0, Une, Olt, Ole, Ogt, Oge, kCount };

// 【后置】opcode 的文本拼写（dump 与读回共用；**唯一实现**）。
//         越界 / `Nop` → "?"（调用方应视为内部错误）。
const char* opcodeName(Opcode k);
// 【后置】按文本拼写反查。不认识 → false。
bool opcodeFromName(const std::string& name, Opcode& out);
// 【后置】该 opcode 是不是终结符（`ret`/`br`/`unreachable`）。
bool isTerminatorOpcode(Opcode k);
// 【后置】该 opcode 的固定结果类型是否可**只由 opcode** 决定（`i32` 类运算、
//         `f32` 类运算、比较、转换、`alloca`/`gep` 需要看操作数）。
//         用途：读回器重建结果类型 + 检查器校验结果类型。
const Type* fixedResultType(Opcode k);
// 【后置】该 opcode 的结果类型是否**取决于操作数/属性**
//         （`load`/`gep`/`bitcast`/`call`/`phi`/`select`/`alloca`）。
bool resultTypeFromOperands(Opcode k);
// 【后置】`icmp`/`fcmp` 谓词的文本（`slt` / `une` …）。
const char* ipredName(IPred p);
const char* fpredName(FPred p);
bool ipredFromName(const std::string& s, IPred& out);
bool fpredFromName(const std::string& s, FPred& out);
// 【后置】`icmp` 谓词的**补**谓词（`eq`↔`ne`、`slt`↔`sge` …）。
//         平面层**不主动**用它（那属于算术改写），只给检查器/调试用。
IPred invertIPred(IPred p);

// ============================================================================
// 1. Value —— 一切"能当操作数用的东西"
//
//   四种形态：
//     * `Instruction`（指令结果；`isParam()` 为真时它只有位置、不是指令）
//     * `Constant`（i32 / f32 / i64 字面量）
//     * `GlobalAddr`（"全局变量的地址"这个**指针值**；每个引用是一个值）
//     * `BlockAddr`（`blockaddress` 形态；本关的 dump **不打印**它，
//        容器支持是为了将来——S11b 的玩具后端与块地址相关的语法）
//   ⚠️ **基本块本身不是 `Value`**：`br`/`phi` 的目标/前驱走
//      `Instruction::succs()`（块没有类型，混进 `operands` 会让
//      "操作数"这个概念失去类型）。
// ============================================================================
enum class ValueKind : uint8_t { Inst, Constant, GlobalAddr, BlockAddr };

class Value {
 public:
  Value(ValueKind k, const Type* t) : kind_(k), type_(t) {}
  virtual ~Value() = default;

  ValueKind kind() const { return kind_; }
  const Type* type() const { return type_; }
  void setType(const Type* t) { type_ = t; }
  bool isInst() const { return kind_ == ValueKind::Inst; }
  // ★★ 注意：这与 `Instruction::isConstant()` **不是一回事**，所以名字必须区分 ★★
  //   * `Value::isInternedConstant()`  —— `ValueKind::Constant`，即**模块级
  //     interned 常量**（`Module::getIntConst`/`getFloatConst` 的产物）；
  //   * `Instruction::isConstant()`    —— **常量形态的指令**（`Opcode::ConstantInt`
  //     /`ConstantFP`），即 dump 里 `%7 = i32 5` 那种。
  //   两者**同名过**（都叫 isConstant），而谓词不同 ⇒ 极易误用：`FlatDump` 曾
  //   因此让常量永远不进定义区、`FlatVerifier` 曾因此把模块级常量误判成
  //   "不属于任何块的指令"。**同类名字必须能一眼区分**，这是今天的教训。
  bool isInternedConstant() const { return kind_ == ValueKind::Constant; }
  bool isGlobalAddr() const { return kind_ == ValueKind::GlobalAddr; }
  bool isBlockAddr() const { return kind_ == ValueKind::BlockAddr; }
  // 【后置】稳定且**与构造顺序无关**的编号，只用于**确定性排序/打印**。
  uint64_t id() const { return id_; }
  void setId(uint64_t v) { id_ = v; }

 private:
  ValueKind kind_;
  const Type* type_;
  uint64_t id_ = 0;
};

// ── 常量 ──────────────────────────────────────────────────────────────────
//   位模式而非"值"：`-0.0` 与 `+0.0` 的位模式不同、`nan != nan`，
//   逐字节往返要求按位去重（`平面IR与dump格式.md` §4.3）。
class Constant : public Value {
 public:
  // 【前置】ty 是 i32 或 i64。
  Constant(const Type* ty, int64_t iv)
      : Value(ValueKind::Constant, ty), ival_(iv) {}
  // 【前置】ty == f32()。
  explicit Constant(const Type* ty) : Value(ValueKind::Constant, ty), ival_(0) {}

  bool isFloat() const { return type()->isFloat(); }
  // 【后置】整数常量的值（i32 常量也在 int64 里存，符号扩展过）。
  //         浮点常量调用它是**内部错误**（返回 0，不崩）。
  int64_t intVal() const { return isFloat() ? 0 : ival_; }
  // 【后置】i32 常量的低 32 位（按补码）。
  int32_t i32Val() const { return static_cast<int32_t>(ival_); }
  // 【后置】f32 常量的 IEEE-754 原始位。
  uint32_t fbits() const { return fbits_; }
  void setFBits(uint32_t b) { fbits_ = b; }
  void setInt(int64_t v) { ival_ = v; }

 private:
  int64_t ival_;
  uint32_t fbits_ = 0;
};

// ============================================================================
// 2. Instruction
// ============================================================================
class Instruction : public Value {
 public:
  Instruction(Opcode op, const Type* resultTy, SourceLoc loc)
      : Value(ValueKind::Inst, resultTy), op_(op), loc_(loc) {}
  ~Instruction() override = default;

  Opcode op() const { return op_; }
  SourceLoc loc() const { return loc_; }
  void setLoc(SourceLoc l) { loc_ = l; }

  // 结果：**恰好一个** —— 由 `hasResult()` 决定有没有（`store`/`br`/… 没有）。
  bool hasResult() const { return type() != nullptr && !type()->isVoid(); }
  // 形参占位（`op() == Opcode::Nop`）：它属于入口块开头，但**不是指令**。
  bool isParam() const { return op_ == Opcode::Nop; }

  // 操作数（指令结果或常量）。**只读暴露**，只能通过 `addOperand` 增长。
  const std::vector<Value*>& operands() const { return operands_; }
  size_t numOperands() const { return operands_.size(); }
  Value* operand(size_t i) const { return i < operands_.size() ? operands_[i] : nullptr; }
  // 【前置】v != nullptr。
  // 【后置】v 追加为最后一个操作数；use-def 的**反向边**由 BasicBlock::addInst
  //         之后调用的 `rebuildUseDef`/`addUse` 维护（container 不自动做 ——
  //         理由见 BasicBlock.h 的"use-def 维护点"）。
  void addOperand(Value* v) { operands_.push_back(v); }
  void setOperand(size_t i, Value* v) {
    if (i < operands_.size()) operands_[i] = v;
  }
  void clearOperands() { operands_.clear(); }

  // 后继块 / φ 的前驱块：**块不是值**（见文件头）。
  const std::vector<BasicBlock*>& succs() const { return succs_; }
  size_t numSuccs() const { return succs_.size(); }
  BasicBlock* succ(size_t i) const { return i < succs_.size() ? succs_[i] : nullptr; }
  void addSucc(BasicBlock* b) { succs_.push_back(b); }
  void setSucc(size_t i, BasicBlock* b) {
    if (i < succs_.size()) succs_[i] = b;
  }
  void clearSuccs() { succs_.clear(); }

  // 所属基本块（**唯一所有权**在 BasicBlock 的指令表里）。
  BasicBlock* parent() const { return parent_; }
  void setParent(BasicBlock* b) { parent_ = b; }

  // ── 使用者链（use-def 的**反向**方向）────────────────────────────────
  //   "谁能用我"：从值找使用者。S17–S20 的每个 pass 都要它（改一条指令后
  //   要找到所有使用者）。
  const std::vector<Instruction*>& users() const { return users_; }
  void addUser(Instruction* u) { users_.push_back(u); }
  void removeUser(Instruction* u) {
    for (size_t i = 0; i < users_.size(); ++i) {
      if (users_[i] == u) { users_.erase(users_.begin() + static_cast<long>(i)); return; }
    }
  }
  void clearUsers() { users_.clear(); }

  // 类型判别（**不引入 RTTI**：判别标准就是 opcode，O(1)）。
  bool isBinOp() const;
  bool isICmp() const { return op_ == Opcode::ICmp; }
  bool isFCmp() const { return op_ == Opcode::FCmp; }
  // 【后置】该"指令"是不是**常量定义**（`i32 5` / `f32 0x1p+0`）。
  bool isConstant() const;

  // 属性（`icmp`/`fcmp` 的谓词；`call` 的被调名；…）。
  IPred ipred() const { return ipred_; }
  void setIPred(IPred p) { ipred_ = p; }
  FPred fpred() const { return fpred_; }
  void setFPred(FPred p) { fpred_ = p; }
  const std::string& callee() const { return callee_; }
  void setCallee(std::string s) { callee_ = std::move(s); }
  bool isTailCall() const { return tail_; }
  void setTailCall(bool t) { tail_ = t; }
  // `GEP` 的源元素类型（`<ty>` 属性）；结果类型是 `ptr[<ty>]`。
  const Type* srcElemType() const { return srcElem_; }
  void setSrcElemType(const Type* t) { srcElem_ = t; }
  uint32_t gepFlags() const { return gepFlags_; }
  void setGepFlags(uint32_t f) { gepFlags_ = f; }
  // `ConstantInt` 的值（i32/i64，符号扩展存 int64）。
  int64_t intBits() const { return intBits_; }
  void setIntBits(int64_t v) { intBits_ = v; }
  // `ConstantFP` 的 IEEE-754 原始位。
  uint32_t floatBits() const { return floatBits_; }
  void setFloatBits(uint32_t b) { floatBits_ = b; }

  // 参数表（**只对函数入口块开头的那段 Nop 有意义**）。
  bool isParamList() const { return paramList_; }
  void setParamList(bool v) { paramList_ = v; }

 protected:
  Opcode op_;
  SourceLoc loc_;
  std::vector<Value*> operands_;
  std::vector<BasicBlock*> succs_;
  std::vector<Instruction*> users_;
  BasicBlock* parent_ = nullptr;
  IPred ipred_ = IPred::Eq;
  FPred fpred_ = FPred::Oeq;
  std::string callee_;
  const Type* srcElem_ = nullptr;
  uint32_t gepFlags_ = 0;
  int64_t intBits_ = 0;
  uint32_t floatBits_ = 0;
  bool tail_ = false;
  bool paramList_ = false;
};

// ============================================================================
// 3. 工厂（**每条指令只有一个结果的唯一落点**）
//
//   为什么用工厂而不是"到处 new 子类"：平面层的 pass 会大量造指令
//   （S09 的 φ、S17 的常量折叠…），把"结果类型怎么定"集中在这里，
//   才不会出现"某处造出的 add 结果类型是 f32"这类静默错误。
//   ⚠️ 工厂**不**负责挂到基本块上（那是 `BasicBlock::addInst` 的事），
//      也不维护 use-def 的反向边（`Module::rebuildUseDef` 统一做）。
// ============================================================================
Instruction* createInst(Opcode op, const Type* resultTy, SourceLoc loc);
// 【后置】二元整数运算（结果 i32）。
Instruction* createBin(Opcode op, Value* a, Value* b, SourceLoc loc);
// 【后置】比较（结果 i32）。
Instruction* createICmp(IPred p, Value* a, Value* b, SourceLoc loc);
Instruction* createFCmp(FPred p, Value* a, Value* b, SourceLoc loc);
// 【后置】`alloca <objTy>`（结果 ptr[objTy]）。
Instruction* createAlloca(const Type* objTy, SourceLoc loc);
// 【后置】`load <ty>, ptr[ty] %p`。ty 必须与 p 的元素类型一致（调用方保证）。
Instruction* createLoad(const Type* ty, Value* ptr, SourceLoc loc);
// 【后置】`store <ty> %v, ptr[ty] %p`（无结果）。
Instruction* createStore(const Type* ty, Value* v, Value* ptr, SourceLoc loc);
// 【后置】`getelementptr <elemTy>, ptr[elemTy] %p, i64 %i`。
Instruction* createGEP(const Type* elemTy, Value* ptr, Value* idx, SourceLoc loc);
// 【后置】`call <retTy> @callee(args...)`；`retTy == nullptr` 表示 void。
Instruction* createCall(const std::string& callee, const Type* retTy,
                        const std::vector<Value*>& args, SourceLoc loc);
// 【后置】`phi <ty> [(%v0 B0) (%v1 B1) ...]`；`vals` 与 `preds` 等长。
Instruction* createPhi(const Type* ty, const std::vector<Value*>& vals,
                       const std::vector<BasicBlock*>& preds, SourceLoc loc);
// 【后置】`br label %dst`。
Instruction* createBr(BasicBlock* dst, SourceLoc loc);
// 【后置】`br i1 %cond, label %t, label %f`。
Instruction* createCondBr(Value* cond, BasicBlock* t, BasicBlock* f, SourceLoc loc);
// 【后置】`ret void` 或 `ret <ty> %v`。
Instruction* createRet(Value* v, SourceLoc loc);
Instruction* createUnreachable(SourceLoc loc);
// 【后置】转换指令（结果类型由 opcode 决定：SIToFP→f32、FPToSI→i32、
//         SExt/ZExt→i64、Trunc→i32、FPExt→double 用 f32 表示占位…见实现）。
Instruction* createCast(Opcode op, Value* v, SourceLoc loc);
// 【后置】`bitcast ptr[A] %p to ptr[B]`。
Instruction* createBitCast(const Type* dstTy, Value* v, SourceLoc loc);
// 【后置】`select i1 %c, <ty> %a, <ty> %b`。
Instruction* createSelect(Value* c, Value* a, Value* b, SourceLoc loc);
// 【后置】形参占位（属于入口块开头，`isParam() == true`）。
Instruction* createParam(const Type* ty, SourceLoc loc);

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_INSTRUCTION_H
