// ============================================================================
// ir/Instruction.cpp —— opcode 表 + 指令工厂（`ir/Instruction.h` 的实现）
//
//   本文件只做两件事：① **印刷/识别**（dump 与读回共用一张表）；
//   ② 造指令时把"结果类型怎么定"收敛到一处。
//   一条业务语义都没有（没有常量折叠、没有窥孔、没有化简）——
//   平面层的优化在 S17 之后，本关**不做任何优化**（prompt §十.3）。
// ============================================================================
#include "ir/Instruction.h"

#include <cstring>

namespace sysy {
namespace flat {
namespace {

// opcode ↔ 文本拼写。**唯一实现**（与 S05 的 `kKindNames` 同一手法）。
// ⚠️ `Nop` **故意没有拼法**（它是形参占位，不是 iset.txt 里的指令）：
//    查它会拿到 "?"，而 dump 永远不会打印它。
struct OpName {
  Opcode op;
  const char* name;
};
const OpName kOpNames[] = {
    {Opcode::Nop, "?"},
    {Opcode::ConstantInt, "i32"},
    {Opcode::ConstantFP, "f32"},
    {Opcode::Ret, "ret"},
    {Opcode::Br, "br"},
    {Opcode::Unreachable, "unreachable"},
    {Opcode::Alloca, "alloca"},
    {Opcode::Load, "load"},
    {Opcode::Store, "store"},
    {Opcode::GEP, "getelementptr"},
    {Opcode::Add, "add"},
    {Opcode::Sub, "sub"},
    {Opcode::Mul, "mul"},
    {Opcode::SDiv, "sdiv"},
    {Opcode::SRem, "srem"},
    {Opcode::Shl, "shl"},
    {Opcode::LShr, "lshr"},
    {Opcode::AShr, "ashr"},
    {Opcode::And, "and"},
    {Opcode::Or, "or"},
    {Opcode::Xor, "xor"},
    {Opcode::FAdd, "fadd"},
    {Opcode::FSub, "fsub"},
    {Opcode::FMul, "fmul"},
    {Opcode::FDiv, "fdiv"},
    {Opcode::FNeg, "fneg"},
    {Opcode::ICmp, "icmp"},
    {Opcode::FCmp, "fcmp"},
    {Opcode::SIToFP, "sitofp"},
    {Opcode::FPToSI, "fptosi"},
    {Opcode::FPExt, "fpext"},
    {Opcode::SExt, "sext"},
    {Opcode::ZExt, "zext"},
    {Opcode::Trunc, "trunc"},
    {Opcode::BitCast, "bitcast"},
    {Opcode::Call, "call"},
    {Opcode::LLVMMemCpy, "llvm.memcpy"},
    {Opcode::LLVMMemSet, "llvm.memset"},
    {Opcode::Phi, "phi"},
    {Opcode::Select, "select"},
};constexpr size_t kNumOpNames = sizeof(kOpNames) / sizeof(kOpNames[0]);
// 表里有 5 个"容器内部"的条目（Nop + 两个常量 + 两个 intrinsic），
//   它们**不计入** iset.txt 的 35 个 opcode —— 见 Instruction.h 的说明。
//   kCount 的值 = 表层条目数 - 5（把 35 个真 opcode 数一遍：见 check_flat.py 的
//   独立手写清单，那里是权威）。
static_assert(kNumOpNames == static_cast<size_t>(Opcode::kCount),
              "kOpNames 必须与 Opcode 一一对应（表里含 5 个容器内部条目）");

const char* kIPredNames[] = {"eq", "ne", "slt", "sle", "sgt", "sge"};
const char* kFPredNames[] = {"oeq", "une", "olt", "ole", "ogt", "oge"};
constexpr size_t kNumPreds = 6;
static_assert(kNumPreds == static_cast<size_t>(IPred::kCount) &&
                  kNumPreds == static_cast<size_t>(FPred::kCount),
              "谓词表必须与 IPred/FPred 一一对应");

}  // namespace

const char* opcodeName(Opcode k) {
  for (const OpName& e : kOpNames) {
    if (e.op == k) return e.name;
  }
  return "?";
}

bool opcodeFromName(const std::string& name, Opcode& out) {
  for (const OpName& e : kOpNames) {
    if (e.op == Opcode::Nop) continue;   // 占位：不接受读回
    if (name == e.name) { out = e.op; return true; }
  }
  return false;
}

bool isTerminatorOpcode(Opcode k) {
  return k == Opcode::Ret || k == Opcode::Br || k == Opcode::Unreachable;
}

const Type* fixedResultType(Opcode k) {
  switch (k) {
    // i32 类运算与比较
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::SDiv:
    case Opcode::SRem: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::ICmp: case Opcode::FCmp:
      return typePool().i32();
    // f32 类运算
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
    case Opcode::FNeg: case Opcode::SIToFP:
      return typePool().f32();
    // 转换
    case Opcode::FPToSI: case Opcode::Trunc:
      return typePool().i32();
    case Opcode::SExt: case Opcode::ZExt:
      return typePool().i64();
    case Opcode::FPExt:
      // `putf` 的变参要求 f32 → double。我们**不支持 double 值**（SysY 没有
      // double），所以这条指令在本关**不产出**；结果类型保守取 f32，
      // 若将来真的要发射变参调用，这里必须与 S07 的发射器一起改。
      return typePool().f32();
    default:
      return nullptr;
  }
}

bool resultTypeFromOperands(Opcode k) {
  switch (k) {
    case Opcode::Alloca: case Opcode::Load: case Opcode::GEP:
    case Opcode::BitCast: case Opcode::Call: case Opcode::Phi:
    case Opcode::Select:
      return true;
    default:
      return false;
  }
}

const char* ipredName(IPred p) {
  const size_t i = static_cast<size_t>(p);
  return i < kNumPreds ? kIPredNames[i] : "?";
}
const char* fpredName(FPred p) {
  const size_t i = static_cast<size_t>(p);
  return i < kNumPreds ? kFPredNames[i] : "?";
}
bool ipredFromName(const std::string& s, IPred& out) {
  for (size_t i = 0; i < kNumPreds; ++i) {
    if (s == kIPredNames[i]) { out = static_cast<IPred>(i); return true; }
  }
  return false;
}
bool fpredFromName(const std::string& s, FPred& out) {
  for (size_t i = 0; i < kNumPreds; ++i) {
    if (s == kFPredNames[i]) { out = static_cast<FPred>(i); return true; }
  }
  return false;
}

IPred invertIPred(IPred p) {
  switch (p) {
    case IPred::Eq:  return IPred::Ne;
    case IPred::Ne:  return IPred::Eq;
    case IPred::Slt: return IPred::Sge;
    case IPred::Sle: return IPred::Sgt;
    case IPred::Sgt: return IPred::Sle;
    case IPred::Sge: return IPred::Slt;
    case IPred::kCount: break;
  }
  return IPred::Eq;
}

// ============================================================================
// 判别
// ============================================================================
bool Instruction::isConstant() const {
  return op_ == Opcode::ConstantInt || op_ == Opcode::ConstantFP;
}

bool Instruction::isBinOp() const {
  switch (op_) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::SDiv: case Opcode::SRem:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
      return true;
    default:
      return false;
  }
}

// ============================================================================
// 工厂
// ============================================================================
Instruction* createInst(Opcode op, const Type* resultTy, SourceLoc loc) {
  return new Instruction(op, resultTy, loc);
}

Instruction* createBin(Opcode op, Value* a, Value* b, SourceLoc loc) {
  const Type* ty = fixedResultType(op);
  Instruction* i = new Instruction(op, ty, loc);
  i->addOperand(a);
  i->addOperand(b);
  return i;
}

Instruction* createICmp(IPred p, Value* a, Value* b, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::ICmp, typePool().i32(), loc);
  i->setIPred(p);
  i->addOperand(a);
  i->addOperand(b);
  return i;
}

Instruction* createFCmp(FPred p, Value* a, Value* b, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::FCmp, typePool().i32(), loc);
  i->setFPred(p);
  i->addOperand(a);
  i->addOperand(b);
  return i;
}

Instruction* createAlloca(const Type* objTy, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Alloca, typePool().ptrTo(objTy), loc);
  i->setSrcElemType(objTy);
  return i;
}

Instruction* createLoad(const Type* ty, Value* ptr, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Load, ty, loc);
  i->addOperand(ptr);
  return i;
}

Instruction* createStore(const Type* ty, Value* v, Value* ptr, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Store, nullptr, loc);
  i->setSrcElemType(ty);          // 存的是"被写元素的类型"
  i->addOperand(v);
  i->addOperand(ptr);
  return i;
}

Instruction* createGEP(const Type* elemTy, Value* ptr, Value* idx, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::GEP, typePool().ptrTo(elemTy), loc);
  i->setSrcElemType(elemTy);
  i->addOperand(ptr);
  i->addOperand(idx);
  return i;
}

Instruction* createCall(const std::string& callee, const Type* retTy,
                        const std::vector<Value*>& args, SourceLoc loc) {
  const bool isVoid = (retTy == nullptr || retTy->isVoid());
  Instruction* i = new Instruction(Opcode::Call, isVoid ? nullptr : retTy, loc);
  i->setCallee(callee);
  for (Value* a : args) i->addOperand(a);
  return i;
}

Instruction* createPhi(const Type* ty, const std::vector<Value*>& vals,
                       const std::vector<BasicBlock*>& preds, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Phi, ty, loc);
  for (Value* v : vals) i->addOperand(v);
  for (BasicBlock* b : preds) i->addSucc(b);
  return i;
}

Instruction* createBr(BasicBlock* dst, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Br, nullptr, loc);
  i->addSucc(dst);
  return i;
}

Instruction* createCondBr(Value* cond, BasicBlock* t, BasicBlock* f, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Br, nullptr, loc);
  i->addOperand(cond);
  i->addSucc(t);
  i->addSucc(f);
  return i;
}

Instruction* createRet(Value* v, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Ret, nullptr, loc);
  if (v != nullptr) i->addOperand(v);
  return i;
}

Instruction* createUnreachable(SourceLoc loc) {
  return new Instruction(Opcode::Unreachable, nullptr, loc);
}

Instruction* createCast(Opcode op, Value* v, SourceLoc loc) {
  const Type* ty = fixedResultType(op);
  Instruction* i = new Instruction(op, ty, loc);
  i->addOperand(v);
  return i;
}

Instruction* createBitCast(const Type* dstTy, Value* v, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::BitCast, dstTy, loc);
  i->setSrcElemType(dstTy);   // bitcast 的"源元素类型"就是目标类型本身
  i->addOperand(v);
  return i;
}

Instruction* createSelect(Value* c, Value* a, Value* b, SourceLoc loc) {
  Instruction* i = new Instruction(Opcode::Select,
                                   a != nullptr ? a->type() : typePool().i32(), loc);
  i->addOperand(c);
  i->addOperand(a);
  i->addOperand(b);
  return i;
}

Instruction* createParam(const Type* ty, SourceLoc loc) {
  return new Instruction(Opcode::Nop, ty, loc);
}

}  // namespace flat
}  // namespace sysy
