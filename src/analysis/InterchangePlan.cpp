#include "analysis/InterchangePlan.h"
#include <algorithm>
namespace sysy { namespace flat {
double MemoryModel::linesPerElement(int64_t strideBytes) const {
  if (strideBytes <= 0) return 0.0;
  if (cacheLineBytes == 0) return 1.0;
  const int64_t stride = strideBytes;
  const int64_t line = static_cast<int64_t>(cacheLineBytes);
  return stride <= line ? 1.0 : static_cast<double>(stride) / static_cast<double>(line);
}
InterchangePlan planInterchange(const std::vector<std::vector<int64_t>>& distances,
                                bool exact, double hotness, size_t outer,
                                size_t inner, const std::vector<int64_t>& strides,
                                size_t elements, const MemoryModel& model) {
  InterchangePlan plan;
  plan.outer = outer;
  plan.inner = inner;
  plan.hotness = hotness;
  if (!exact) {
    plan.reason = "dependence is not exact";
    return plan;
  }
  for (const std::vector<int64_t>& distance : distances) {
    bool nonnegative = true;
    bool nonpositive = true;
    for (int64_t component : distance) {
      nonnegative = nonnegative && component >= 0;
      nonpositive = nonpositive && component <= 0;
    }
    if (!nonnegative && !nonpositive) {
      plan.reason = "mixed-direction dependence";
      return plan;
    }
  }
  plan.legal = true;
  if (hotness < 0.5) {
    plan.reason = "loop nest is cold";
    return plan;
  }
  if (inner >= strides.size() || outer >= strides.size()) {
    plan.reason = "stride vector does not cover the pair";
    return plan;
  }
  plan.strideBefore = strides[inner];
  plan.strideAfter = strides[outer];
  plan.trafficBefore =
      model.linesPerElement(plan.strideBefore) * static_cast<double>(elements);
  plan.trafficAfter =
      model.linesPerElement(plan.strideAfter) * static_cast<double>(elements);
  plan.profitable = plan.trafficAfter < plan.trafficBefore;
  plan.reason = plan.profitable ? "lower line traffic after interchange"
                                : "line traffic does not improve";
  return plan;
}
}}  // namespace sysy::flat
