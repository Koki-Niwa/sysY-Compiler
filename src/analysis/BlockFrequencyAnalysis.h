// S14b: block frequencies from branch probabilities.
#ifndef SYSY_ANALYSIS_BLOCKFREQUENCYANALYSIS_H
#define SYSY_ANALYSIS_BLOCKFREQUENCYANALYSIS_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
// Entry frequency is 1e6 for numerical headroom. Cyclic flow is solved by a
// bounded relaxation (4 * blocks + 32 rounds); if it does not converge the
// last iterate is returned and frequency is capped at 1e12.
struct BlockFrequencyInfo {
  std::vector<double> frequencies;
  double frequency(size_t block) const {
    return block < frequencies.size() ? frequencies[block] : 0.0;
  }
};
struct BlockFrequencyAnalysis {
  using Result = BlockFrequencyInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
