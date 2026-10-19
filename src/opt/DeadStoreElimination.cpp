#include "opt/DeadStoreElimination.h"
#include <unordered_map>
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
DeadStoreEliminationPass::DeadStoreEliminationPass()
    : FunctionPass("dse") {}

PassResult DeadStoreEliminationPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (BasicBlock* block : function.blocks()) {
    std::unordered_map<const Value*, Instruction*> pending;
    std::vector<Instruction*> instructions;
    for (Instruction* inst : block->insts())
      if (inst != nullptr) instructions.push_back(inst);
    for (size_t i = instructions.size(); i-- > 0;) {
      Instruction* inst = instructions[i];
      if (inst->op() == Opcode::Load) {
        // A load may read any pending store through an alias; keep all.
        pending.clear();
        continue;
      }
      if (inst->op() == Opcode::Call) {
        pending.clear();
        continue;
      }
      if (inst->op() != Opcode::Store || inst->numOperands() < 2) continue;
      const Value* pointer = inst->operand(1);
      const auto it = pending.find(pointer);
      if (it != pending.end()) {
        if (context.mutator().eraseInstruction(it->second) == MutationResult::Applied) {
          ++result.stats.counters["dse.removed"];
          changed = true;
          pending.erase(it);
        }
      }
      pending[pointer] = inst;
    }
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
