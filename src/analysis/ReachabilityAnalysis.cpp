#include "analysis/ReachabilityAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
std::optional<ReachabilityInfo> ReachabilityAnalysis::run(
    const Function& function, AnalysisManager&) {
  const size_t count = function.blockCount();
  std::vector<char> reachable(count, 0);
  if (count == 0 || function.entry() == nullptr)
    return ReachabilityInfo(std::move(reachable));
  if (function.entry()->parent() != &function ||
      function.entry()->index() >= count) return std::nullopt;
  std::vector<BasicBlock*> stack{function.entry()};
  reachable[function.entry()->index()] = 1;
  while (!stack.empty()) {
    BasicBlock* block = stack.back();
    stack.pop_back();
    Instruction* term = block->terminator();
    if (term == nullptr) continue;
    for (size_t i = 0; i < term->numSuccs(); ++i) {
      BasicBlock* next = term->succ(i);
      if (next == nullptr || next->parent() != &function) continue;
      const size_t index = next->index();
      if (index >= count || reachable[index]) continue;
      reachable[index] = 1;
      stack.push_back(next);
    }
  }
  return ReachabilityInfo(std::move(reachable));
}
}} // namespace sysy::flat
