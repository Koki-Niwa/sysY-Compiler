#include "opt/DeadCodeElimination.h"
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool removable(const Instruction& inst) {
  switch (inst.op()) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::SDiv: case Opcode::SRem:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::Select:
    case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
    case Opcode::BitCast: case Opcode::GEP: case Opcode::Alloca:
    case Opcode::Load: case Opcode::Phi:
      return true;
    default:
      return false;
  }
}
}  // namespace

DeadCodeEliminationPass::DeadCodeEliminationPass()
    : FunctionPass("dce") {}

PassResult DeadCodeEliminationPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (size_t round = 0; round < 256; ++round) {
    bool roundChanged = false;
    std::vector<Instruction*> instructions;
    for (BasicBlock* block : function.blocks())
      for (Instruction* inst : block->insts())
        if (inst != nullptr) instructions.push_back(inst);
    for (Instruction* inst : instructions) {
      if (!removable(*inst)) continue;
      if (inst->op() == Opcode::Phi) {
        if (context.cfg().erasePhi(inst) == MutationResult::Applied) {
          ++result.stats.counters["removed"];
          roundChanged = changed = true;
        }
        continue;
      }
      if (inst->hasUses()) continue;
      if (context.mutator().eraseInstruction(inst) == MutationResult::Applied) {
        ++result.stats.counters["removed"];
        roundChanged = changed = true;
      }
    }
    if (!roundChanged) break;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
