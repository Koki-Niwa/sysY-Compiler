#include "opt/DeadLoadElimination.h"
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
DeadLoadEliminationPass::DeadLoadEliminationPass()
    : FunctionPass("dle") {}

PassResult DeadLoadEliminationPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (size_t round = 0; round < 64; ++round) {
    bool roundChanged = false;
    std::vector<Instruction*> loads;
    for (BasicBlock* block : function.blocks())
      for (Instruction* inst : block->insts())
        if (inst != nullptr && inst->op() == Opcode::Load && !inst->hasUses())
          loads.push_back(inst);
    for (Instruction* load : loads) {
      if (context.mutator().eraseInstruction(load) == MutationResult::Applied) {
        ++result.stats.counters["dle.removed"];
        roundChanged = changed = true;
      }
    }
    if (!roundChanged) break;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
