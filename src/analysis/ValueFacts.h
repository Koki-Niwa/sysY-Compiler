// S14b: lightweight integer ranges; unknown is conservative.
#ifndef SYSY_ANALYSIS_VALUEFACTS_H
#define SYSY_ANALYSIS_VALUEFACTS_H
#include <cstdint>
#include <optional>
#include <unordered_map>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
class Value;
struct ValueRange {
  bool known = false;
  int64_t lo = 0;
  int64_t hi = 0;
};
class ValueFactsInfo {
 public:
  ValueRange rangeOf(const Value* value) const {
    const auto it = ranges_.find(value);
    return it == ranges_.end() ? ValueRange{} : it->second;
  }
 private:
  friend struct ValueFactsAnalysis;
  std::unordered_map<const Value*, ValueRange> ranges_;
};
struct ValueFactsAnalysis {
  using Result = ValueFactsInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::DataFlow | MutationKind::CFG;
  }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
