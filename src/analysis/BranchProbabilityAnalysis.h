// S14b: static branch probabilities from loop structure.
#ifndef SYSY_ANALYSIS_BRANCHPROBABILITYANALYSIS_H
#define SYSY_ANALYSIS_BRANCHPROBABILITYANALYSIS_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
// `edges[block][successorIndex]`; each row sums to 1 for a block with
// successors. Heuristic sources: a loop header favours staying inside (0.9),
// a back-edge target is rare (0.1), other conditional branches are even.
struct BranchProbabilityInfo {
  std::vector<std::vector<double>> edges;
  double probability(size_t block, size_t successor) const {
    return block < edges.size() && successor < edges[block].size()
               ? edges[block][successor] : 0.0;
  }
};
struct BranchProbabilityAnalysis {
  using Result = BranchProbabilityInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
