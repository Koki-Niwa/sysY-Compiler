#include "analysis/BlockingPlan.h"
#include <algorithm>
#include <cstdint>
#include <limits>
namespace sysy { namespace flat {
namespace {
size_t saturatingMul(size_t a, size_t b) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
    return std::numeric_limits<size_t>::max();
  return a * b;
}
size_t floorPow2(size_t value) {
  if (value == 0) return 0;
  size_t result = 1;
  while (result <= value / 2) result *= 2;
  return result;
}
size_t tileBytes(size_t outer, size_t inner, size_t elementBytes) {
  return saturatingMul(saturatingMul(outer, inner), elementBytes);
}
}  // namespace
BlockingPlan planBlocking(const BlockingContext& context, const MemoryModel& model) {
  BlockingPlan plan;
  if (!context.dependenceFree) {
    plan.reason = "dependence";
    return plan;
  }
  if (context.hotness < 0.5) {
    plan.reason = "cold";
    return plan;
  }
  if (context.outerTrip < 2 || context.innerTrip < 2 || context.elementBytes == 0) {
    plan.reason = "small";
    return plan;
  }
  const size_t fullBytes =
      tileBytes(context.outerTrip, context.innerTrip, context.elementBytes);
  plan.workingSetBytes = fullBytes;
  const size_t budget = model.cacheBytes / 2;
  if (fullBytes <= budget) {
    plan.reason = "fits";
    return plan;
  }
  // Start from the largest power-of-two tile per dimension and shrink the
  // larger tile until the tile footprint fits the half-cache budget.
  size_t outer = std::min(floorPow2(context.outerTrip),
                          std::min(context.outerTrip, static_cast<size_t>(4096)));
  size_t inner = std::min(floorPow2(context.innerTrip),
                          std::min(context.innerTrip, static_cast<size_t>(4096)));
  if (outer == 0 || inner == 0) {
    plan.reason = "small";
    return plan;
  }
  while (outer > 1 || inner > 1) {
    if (tileBytes(outer, inner, context.elementBytes) <= budget) break;
    if (outer >= inner && outer > 1) outer /= 2;
    else if (inner > 1) inner /= 2;
    else outer /= 2;
  }
  if (tileBytes(outer, inner, context.elementBytes) > budget) {
    plan.reason = "capacity";
    return plan;
  }
  if (outer >= context.outerTrip && inner >= context.innerTrip) {
    plan.reason = "fits";
    return plan;
  }
  plan.block = true;
  plan.tileOuter = outer;
  plan.tileInner = inner;
  plan.workingSetBytes = tileBytes(outer, inner, context.elementBytes);
  plan.outerTileFirst = true;
  if (context.outerStride == 1 && context.innerStride >= context.outerTrip)
    plan.outerTileFirst = false;  // column-major: keep i innermost
  plan.reason = "blocked";
  return plan;
}
}}  // namespace sysy::flat
