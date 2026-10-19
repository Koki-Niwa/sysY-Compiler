#include "opt/SimplifyCFG.h"
#include <vector>
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
SimplifyCFGPass::SimplifyCFGPass() : FunctionPass("simplifycfg") {}

PassResult SimplifyCFGPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  std::vector<BasicBlock*> blocks = function.blocks();
  for (BasicBlock* block : blocks) {
    Instruction* terminator = block->terminator();
    if (terminator == nullptr || terminator->op() != Opcode::Br || terminator->numSuccs() != 2) continue;
    BasicBlock* first = terminator->succ(0);
    BasicBlock* second = terminator->succ(1);
    BasicBlock* target = nullptr;
    BasicBlock* dropped = nullptr;
    if (first == second) {
      target = first;
    } else {
      const Value* condition = terminator->operand(0);
      if (condition == nullptr || !condition->isConstant() ||
          !condition->type()->isInt())
        continue;
      const int64_t value = static_cast<const Constant*>(condition)->intVal();
      target = value != 0 ? first : second;
      dropped = value != 0 ? second : first;
    }
    if (target == nullptr) continue;
    // Dropping the last predecessor would make the other successor
    // unreachable; its outgoing phi edges would then lose dominance.
    if (dropped != nullptr && dropped->numPreds() <= 1) {
      ++result.stats.counters["declined"];
      continue;
    }
    Instruction* branch = createBr(target, terminator->loc());
    if (context.cfg().replaceTerminator(block, branch) != MutationResult::Applied) {
      ++result.stats.counters["declined"];
      continue;
    }
    ++result.stats.counters["folded"];
    changed = true;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
