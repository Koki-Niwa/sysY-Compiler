#include "analysis/BranchProbabilityAnalysis.h"
#include <algorithm>
#include "analysis/LoopAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
std::optional<BranchProbabilityInfo> BranchProbabilityAnalysis::run(
    const Function& function, AnalysisManager& manager) {
  AnalysisHandle<LoopInfoResult> loops = manager.get<LoopInfoAnalysis>(function);
  if (!loops.isCurrent()) return std::nullopt;
  const size_t count = function.blockCount();
  std::vector<long> headerLoop(count, -1);
  for (size_t i = 0; i < loops.get()->loops.size(); ++i)
    headerLoop[loops.get()->loops[i].header] = static_cast<long>(i);
  BranchProbabilityInfo info;
  info.edges.resize(count);
  for (size_t index = 0; index < count; ++index) {
    BasicBlock* block = function.at(index);
    if (block == nullptr) continue;
    Instruction* terminator = block->terminator();
    if (terminator == nullptr || terminator->numSuccs() == 0) continue;
    const size_t successors = terminator->numSuccs();
    info.edges[index].assign(successors, 0.0);
    if (successors == 1) { info.edges[index][0] = 1.0; continue; }
    const long innerLoop = headerLoop[index];
    if (innerLoop >= 0) {
      const std::vector<size_t>& members = loops.get()->loops[static_cast<size_t>(innerLoop)].blocks;
      size_t inside = 0;
      for (size_t s = 0; s < successors; ++s) {
        BasicBlock* successor = terminator->succ(s);
        if (successor != nullptr && std::binary_search(members.begin(), members.end(),
                                                       successor->index()))
          ++inside;
      }
      const size_t outside = successors - inside;
      for (size_t s = 0; s < successors; ++s) {
        BasicBlock* successor = terminator->succ(s);
        const bool in = successor != nullptr &&
                        std::binary_search(members.begin(), members.end(), successor->index());
        if (inside > 0 && outside > 0) info.edges[index][s] = in ? 0.9 / inside : 0.1 / outside;
        else info.edges[index][s] = in ? 1.0 / inside : 1.0 / outside;
      }
      continue;
    }
    size_t back = 0;
    for (size_t s = 0; s < successors; ++s) {
      BasicBlock* successor = terminator->succ(s);
      if (successor != nullptr && successor->index() < count &&
          headerLoop[successor->index()] >= 0)
        ++back;
    }
    const size_t forward = successors - back;
    for (size_t s = 0; s < successors; ++s) {
      BasicBlock* successor = terminator->succ(s);
      const bool isBack = successor != nullptr && successor->index() < count &&
                          headerLoop[successor->index()] >= 0;
      if (back > 0 && forward > 0) info.edges[index][s] = isBack ? 0.1 / back : 0.9 / forward;
      else info.edges[index][s] = 1.0 / successors;
    }
  }
  return info;
}
}}  // namespace sysy::flat
