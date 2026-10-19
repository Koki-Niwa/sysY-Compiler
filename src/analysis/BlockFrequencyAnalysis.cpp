#include "analysis/BlockFrequencyAnalysis.h"
#include <algorithm>
#include <cmath>
#include "analysis/BranchProbabilityAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
std::optional<BlockFrequencyInfo> BlockFrequencyAnalysis::run(
    const Function& function, AnalysisManager& manager) {
  AnalysisHandle<BranchProbabilityInfo> probabilities =
      manager.get<BranchProbabilityAnalysis>(function);
  if (!probabilities.isCurrent()) return std::nullopt;
  const size_t count = function.blockCount();
  BlockFrequencyInfo info;
  info.frequencies.assign(count, 0.0);
  if (count == 0 || function.entry() == nullptr) return info;
  constexpr double kEntry = 1e6;
  constexpr double kCap = 1e12;
  info.frequencies[function.entry()->index()] = kEntry;
  const size_t rounds = count * 4 + 32;
  for (size_t round = 0; round < rounds; ++round) {
    std::vector<double> next(count, 0.0);
    next[function.entry()->index()] = kEntry;
    for (size_t index = 0; index < count; ++index) {
      const double flow = info.frequencies[index];
      if (flow <= 0.0) continue;
      BasicBlock* block = function.at(index);
      if (block == nullptr) continue;
      Instruction* terminator = block->terminator();
      if (terminator == nullptr) continue;
      for (size_t s = 0; s < terminator->numSuccs(); ++s) {
        BasicBlock* successor = terminator->succ(s);
        if (successor == nullptr || successor->parent() != &function ||
            successor->index() >= count)
          continue;
        next[successor->index()] += flow * probabilities.get()->probability(index, s);
      }
    }
    double delta = 0.0;
    for (size_t i = 0; i < count; ++i) {
      next[i] = std::min(next[i], kCap);
      delta = std::max(delta, std::fabs(next[i] - info.frequencies[i]));
    }
    info.frequencies.swap(next);
    if (delta < 1e-6) break;
  }
  return info;
}
}}  // namespace sysy { namespace flat {
