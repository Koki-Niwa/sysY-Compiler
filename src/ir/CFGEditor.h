#ifndef SYSY_IR_CFGEDITOR_H
#define SYSY_IR_CFGEDITOR_H
#include <utility>
#include <vector>
#include "ir/IRMutator.h"

namespace sysy { namespace flat {
// Atomic per-operation phi/edge editing. Shares the mutator's revision tracker.
class CFGEditor {
 public:
  explicit CFGEditor(IRMutator& mutator) : mutator_(mutator) {}
  MutationResult rewritePhiValue(Instruction* phi, BasicBlock* pred, Value* value);
  MutationResult insertPhi(BasicBlock* block, Instruction* phi);
  MutationResult insertPhiBatch(
      const std::vector<std::pair<BasicBlock*, Instruction*>>& phis);
  MutationResult erasePhi(Instruction* phi);
  MutationResult redirectEdge(
      BasicBlock* from, size_t successorIndex, BasicBlock* newTarget,
      const std::vector<Value*>& newTargetPhiValues = {});
  struct SplitResult {
    MutationResult result;
    BasicBlock* block;
  };
  SplitResult splitEdge(BasicBlock* from, size_t successorIndex);
  // Replace a terminator while preserving the rest of the block. The new
  // successors must be a subset of the old ones (constant branch / duplicate
  // target); a target that loses this predecessor must have no phi.
  MutationResult replaceTerminator(BasicBlock* block, Instruction* newTerminator);

 private:
  IRMutator& mutator_;
  bool readPhis(BasicBlock* block, std::vector<Instruction*>& phis) const;
  bool hasSuccessor(BasicBlock* from, BasicBlock* target,
                    size_t excludeIndex) const;
  static void updatePreds(BasicBlock* block, BasicBlock* remove,
                          BasicBlock* add);
};
}} // namespace sysy::flat
#endif
