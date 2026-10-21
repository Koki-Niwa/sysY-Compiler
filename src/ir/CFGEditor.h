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
  // Append an unreachable-terminated block to an owned function definition.
  // Reject null/foreign/declaration functions without changing IR. A successful
  // creation records one CFG + DataFlow mutation; all later edits are tracked.
  BasicBlock* createBlock(Function* function);
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
  // Replace a terminator while preserving the rest of the block. New targets
  // are allowed only when they have no phi; a target that loses this
  // predecessor must also have no phi. Phi rewrites are the caller's job.
  MutationResult replaceTerminator(BasicBlock* block, Instruction* newTerminator);
  // Same, but for each newly added successor supplies one value per target phi
  // in block order. `incomingValues` is indexed by successor and may be empty;
  // an existing successor must have an empty vector.
  MutationResult replaceTerminator(
      BasicBlock* block, Instruction* newTerminator,
      const std::vector<std::vector<Value*>>& incomingValues);

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
