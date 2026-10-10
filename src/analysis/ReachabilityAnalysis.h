#ifndef SYSY_ANALYSIS_REACHABILITYANALYSIS_H
#define SYSY_ANALYSIS_REACHABILITYANALYSIS_H
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class ReachabilityInfo {
 public:
  explicit ReachabilityInfo(std::vector<char> bits) : bits_(std::move(bits)) {}
  bool reachable(size_t i) const { return i < bits_.size() && bits_[i] != 0; }
  const std::vector<char>& bits() const { return bits_; }
 private:
  std::vector<char> bits_;
};
struct ReachabilityAnalysis {
  using Result = ReachabilityInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}} // namespace sysy::flat
#endif
