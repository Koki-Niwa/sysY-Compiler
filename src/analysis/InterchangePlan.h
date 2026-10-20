// S23: legality/benefit plan for adjacent loop interchange.
#ifndef SYSY_ANALYSIS_INTERCHANGEPLAN_H
#define SYSY_ANALYSIS_INTERCHANGEPLAN_H
#include <cstddef>
#include <cstdint>
#include <vector>
namespace sysy { namespace flat {
struct MemoryModel {
  size_t cacheLineBytes = 64;
  // Lines touched per element for a stream with the given byte stride.
  double linesPerElement(int64_t strideBytes) const;
};
struct InterchangePlan {
  bool legal = false;
  bool profitable = false;
  size_t outer = 0;
  size_t inner = 1;
  double hotness = 0.0;
  int64_t strideBefore = 0;
  int64_t strideAfter = 0;
  double trafficBefore = 0.0;
  double trafficAfter = 0.0;
  const char* reason = "";
};
// `dependenceDistances` are exact source->sink distance vectors for the nest;
// any non-exact dependence must pass empty/`exact=false`. Strides are bytes per
// dimension of the innermost access. A pair is legal when every dependence
// vector is uniformly nonnegative or nonpositive, so swapping preserves its
// direction. Benefit compares cache-line traffic of the old/new inner stride.
InterchangePlan planInterchange(const std::vector<std::vector<int64_t>>& distances,
                                bool exact, double hotness, size_t outer,
                                size_t inner, const std::vector<int64_t>& strides,
                                size_t elements, const MemoryModel& model);
}}  // namespace sysy::flat
#endif
