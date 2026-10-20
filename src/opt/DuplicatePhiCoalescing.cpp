#include "opt/DuplicatePhiCoalescing.h"
#include <vector>
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
DuplicatePhiCoalescingPass::DuplicatePhiCoalescingPass()
    : FunctionPass("duplicate-phi-coalescing") {}
PassResult DuplicatePhiCoalescingPass::run(Function& function,
                                           PassContext& context) {
  PassResult result;
  for (BasicBlock* block : function.blocks()) {
    std::vector<Instruction*> phis;
    for (Instruction* inst : block->insts()) {
      if (inst->op() != Opcode::Phi) break;
      phis.push_back(inst);
    }
    for (size_t i = 0; i < phis.size(); ++i) {
      if (phis[i] == nullptr) continue;
      for (size_t j = i + 1; j < phis.size(); ++j) {
        Instruction* a = phis[i];
        Instruction* b = phis[j];
        if (b == nullptr || a->type() != b->type() ||
            a->numOperands() != b->numOperands() ||
            a->numSuccs() != b->numSuccs())
          continue;
        bool same = true;
        for (size_t k = 0; k < a->numOperands(); ++k)
          if (a->operand(k) != b->operand(k) || a->succ(k) != b->succ(k)) {
            same = false;
            break;
          }
        if (!same) continue;
        if (context.mutator().replaceUses(b, a) != MutationResult::Applied)
          return [] { PassResult p; p.status = PassStatus::Failed;
                      p.diagnostic = "phi-coalescing: replaceUses rejected"; return p; }();
        if (context.cfg().erasePhi(b) != MutationResult::Applied)
          return [] { PassResult p; p.status = PassStatus::Failed;
                      p.diagnostic = "phi-coalescing: erase rejected"; return p; }();
        phis[j] = nullptr;
        ++result.stats.counters["coalesced-phis"];
        result.status = PassStatus::Applied;
      }
    }
  }
  return result;
}
}}  // namespace sysy::flat
