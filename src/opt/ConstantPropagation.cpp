#include "opt/ConstantPropagation.h"
#include <cstdint>
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool intConstant(const Value* value, int64_t& out) {
  if (value == nullptr || !value->isConstant()) return false;
  const auto* constant = static_cast<const Constant*>(value);
  if (constant->type() == nullptr || !constant->type()->isInt()) return false;
  out = constant->intVal();
  return true;
}
int64_t wrapI32(int64_t value) {
  return static_cast<int32_t>(static_cast<uint32_t>(value));
}
Value* fold(const Instruction& inst, Module& module) {
  if (inst.type() == nullptr || !inst.type()->isInt()) return nullptr;
  if (inst.op() == Opcode::Select) {
    int64_t condition = 0;
    if (!intConstant(inst.operand(0), condition)) return nullptr;
    return inst.operand(condition != 0 ? 1 : 2);
  }
  if (inst.op() == Opcode::ICmp) {
    int64_t a = 0, b = 0;
    if (!intConstant(inst.operand(0), a) || !intConstant(inst.operand(1), b))
      return nullptr;
    bool result = false;
    switch (inst.ipred()) {
      case IPred::Eq: result = a == b; break;
      case IPred::Ne: result = a != b; break;
      case IPred::Slt: result = a < b; break;
      case IPred::Sle: result = a <= b; break;
      case IPred::Sgt: result = a > b; break;
      case IPred::Sge: result = a >= b; break;
      default: return nullptr;
    }
    return module.getIntConst(inst.type(), result ? 1 : 0);
  }
  int64_t a = 0, b = 0;
  if (inst.numOperands() < 1 || !intConstant(inst.operand(0), a)) return nullptr;
  const bool binary = inst.numOperands() > 1;
  if (binary && !intConstant(inst.operand(1), b)) return nullptr;
  int64_t value = 0;
  switch (inst.op()) {
    case Opcode::Add: value = wrapI32(a + b); break;
    case Opcode::Sub: value = wrapI32(a - b); break;
    case Opcode::Mul: value = wrapI32(a * b); break;
    case Opcode::SDiv:
      if (b == 0) return nullptr;
      value = wrapI32(static_cast<int64_t>(a) / b);
      break;
    case Opcode::SRem:
      if (b == 0) return nullptr;
      value = wrapI32(static_cast<int64_t>(a) % b);
      break;
    case Opcode::Shl:
      value = wrapI32(static_cast<uint32_t>(a) << (static_cast<uint32_t>(b) & 31u));
      break;
    case Opcode::LShr:
      value = wrapI32(static_cast<uint32_t>(a) >> (static_cast<uint32_t>(b) & 31u));
      break;
    case Opcode::AShr:
      value = wrapI32(static_cast<int32_t>(a) >> (static_cast<uint32_t>(b) & 31u));
      break;
    case Opcode::And: value = wrapI32(a & b); break;
    case Opcode::Or: value = wrapI32(a | b); break;
    case Opcode::Xor: value = wrapI32(a ^ b); break;
    case Opcode::SExt:
      if (inst.type()->kind != TypeKind::I64) return nullptr;
      value = static_cast<int32_t>(a);
      break;
    case Opcode::ZExt:
      if (inst.type()->kind != TypeKind::I64) return nullptr;
      value = static_cast<uint32_t>(a);
      break;
    case Opcode::Trunc:
      if (inst.type()->kind != TypeKind::I32) return nullptr;
      value = wrapI32(a);
      break;
    default:
      return nullptr;
  }
  return module.getIntConst(inst.type(), value);
}
}  // namespace

ConstantPropagationPass::ConstantPropagationPass()
    : FunctionPass("constprop") {}

PassResult ConstantPropagationPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (size_t round = 0; round < 64; ++round) {
    bool roundChanged = false;
    std::vector<Instruction*> instructions;
    for (BasicBlock* block : function.blocks())
      for (Instruction* inst : block->insts())
        if (inst != nullptr) instructions.push_back(inst);
    for (Instruction* inst : instructions) {
      Value* folded = fold(*inst, context.analyses().module());
      if (folded == nullptr || folded == inst) continue;
      if (context.mutator().replaceAndErase(inst, folded) != MutationResult::Applied)
        continue;
      ++result.stats.counters["folded"];
      roundChanged = changed = true;
    }
    if (!roundChanged) break;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
