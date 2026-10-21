#include "opt/AllocaGEPCanonicalization.h"
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool zeroConstant(const Value* value) {
  if (value == nullptr) return false;
  if (value->isConstant())
    return value->type() == typePool().i64() &&
           static_cast<const Constant*>(value)->intVal() == 0;
  if (!value->isInst()) return false;
  const auto* inst = static_cast<const Instruction*>(value);
  if (inst->op() != Opcode::SExt && inst->op() != Opcode::ZExt) return false;
  const Value* inner = inst->operand(0);
  return inner != nullptr && inner->isConstant() &&
         inner->type() == typePool().i32() &&
         static_cast<const Constant*>(inner)->intVal() == 0;
}
}  // namespace
AllocaGEPCanonicalizationPass::AllocaGEPCanonicalizationPass()
    : FunctionPass("alloca-gep-canonicalization") {}
PassResult AllocaGEPCanonicalizationPass::run(Function& function,
                                              PassContext& context) {
  PassResult result;
  std::vector<Instruction*> candidates;
  for (BasicBlock* block : function.blocks())
    for (Instruction* inst : block->insts()) {
      if (inst->op() != Opcode::GEP || inst->numOperands() != 2) continue;
      const Value* base = inst->operand(0);
      if (base == nullptr || !base->isInst() ||
          static_cast<const Instruction*>(base)->op() != Opcode::Alloca)
        continue;
      if (!zeroConstant(inst->operand(1)) || inst->type() != base->type())
        continue;
      candidates.push_back(inst);
    }
  for (Instruction* inst : candidates) {
    Value* base = inst->operand(0);
    const MutationResult replaced = context.mutator().replaceUses(inst, base);
    if (replaced != MutationResult::Applied && replaced != MutationResult::NoChange)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "alloca-gep: replaceUses rejected"; return p; }();
    if (context.mutator().eraseInstruction(inst) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "alloca-gep: erase rejected"; return p; }();
    ++result.stats.counters["folded-geps"];
    result.status = PassStatus::Applied;
  }
  return result;
}
}}  // namespace sysy::flat
