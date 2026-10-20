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
size_t saturatingAdd(size_t a, size_t b) {
  if (a > std::numeric_limits<size_t>::max() - b)
    return std::numeric_limits<size_t>::max();
  return a + b;
}
size_t floorPow2(size_t value) {
  if (value == 0) return 0;
  size_t result = 1;
  while (result <= value / 2) result *= 2;
  return result;
}
size_t lineFootprint(size_t outer, size_t inner, size_t outerStride,
                     size_t innerStride, size_t elementBytes, size_t lineBytes) {
  if (lineBytes == 0) lineBytes = 1;
  // Choose the smaller-stride axis as the per-row span; the other axis's
  // iterations are counted as separate rows. This is an upper bound and never
  // undercounts lines shared by a strided rectangle.
  size_t rows = 0, span = 0;
  if (innerStride <= outerStride) {
    rows = outer;
    span = saturatingMul(
        saturatingMul(inner > 1 ? inner - 1 : 0, innerStride), elementBytes);
  } else {
    rows = inner;
    span = saturatingMul(
        saturatingMul(outer > 1 ? outer - 1 : 0, outerStride), elementBytes);
  }
  // Include the last element's own bytes; an element larger than a line must
  // be charged all of its lines.
  span = saturatingAdd(span, elementBytes);
  const size_t linesPerRow =
      span == 0 ? 0 : 1 + (span - 1) / lineBytes;
  return saturatingMul(rows, saturatingMul(linesPerRow, lineBytes));
}
size_t tileBytes(size_t outer, size_t inner, const BlockingContext& context,
                 const MemoryModel& model) {
  return lineFootprint(outer, inner, context.outerStride, context.innerStride,
                       context.elementBytes, model.cacheLineBytes);
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
      tileBytes(context.outerTrip, context.innerTrip, context, model);
  plan.workingSetBytes = fullBytes;
  const size_t budget = model.cacheBytes / 2;
  if (fullBytes <= budget) {
    plan.reason = "fits";
    return plan;
  }
  // Start from the largest power-of-two tile per dimension and shrink the
  // larger tile until the tile footprint fits the half-cache budget.
  size_t outer = std::min(floorPow2(context.outerTrip), context.outerTrip);
  size_t inner = std::min(floorPow2(context.innerTrip), context.innerTrip);
  if (outer == 0 || inner == 0) {
    plan.reason = "small";
    return plan;
  }
  while (outer > 1 || inner > 1) {
    if (tileBytes(outer, inner, context, model) <= budget) break;
    if (outer >= inner && outer > 1) outer /= 2;
    else if (inner > 1) inner /= 2;
    else outer /= 2;
  }
  if (tileBytes(outer, inner, context, model) > budget) {
    plan.reason = "capacity";
    return plan;
  }
  if (outer < 8 || inner < 8) {
    plan.reason = "minimum tile";
    return plan;
  }
  if (outer >= context.outerTrip && inner >= context.innerTrip) {
    plan.reason = "fits";
    return plan;
  }
  plan.block = true;
  plan.tileOuter = outer;
  plan.tileInner = inner;
  plan.workingSetBytes = tileBytes(outer, inner, context, model);
  plan.outerTileFirst = true;
  if (context.outerStride == 1 && context.innerStride >= context.outerTrip)
    plan.outerTileFirst = false;  // column-major: keep i innermost
  plan.reason = "blocked";
  return plan;
}
}}  // namespace sysy::flat
