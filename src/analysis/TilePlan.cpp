#include "analysis/TilePlan.h"
namespace sysy { namespace flat {
namespace {
uint64_t saturatingMul(uint64_t a, uint64_t b) {
  if (a == 0 || b == 0) return 0;
  if (a > UINT64_MAX / b) return UINT64_MAX;
  return a * b;
}
uint64_t power(uint64_t base, size_t exponent) {
  // Callers supply a nonzero base. Avoid walking an arbitrary dimension count
  // for a one-element extent; larger bases saturate within 64 multiplications.
  if (base == 1) return 1;
  uint64_t result = 1;
  for (size_t i = 0; i < exponent; ++i) {
    if (result > UINT64_MAX / base) return UINT64_MAX;
    result *= base;
  }
  return result;
}
uint64_t workingSet(size_t extent, size_t dimensions, uint64_t elementBytes) {
  return saturatingMul(saturatingMul(2, power(extent, dimensions)),
                       elementBytes);
}
}  // namespace
TilePlan planTiling(size_t iterations, size_t dimensions, uint64_t elementBytes,
                    const MemoryModel& model, double hotness,
                    const TileProbe& probe, size_t probeBudget) {
  TilePlan plan;
  plan.hotness = hotness;
  if (iterations == 0 || dimensions == 0 || elementBytes == 0 ||
      model.cacheBytes == 0) {
    plan.reason = "invalid tiling shape";
    return plan;
  }
  const uint64_t fullSet = workingSet(iterations, dimensions, elementBytes);
  plan.workingSetBefore = fullSet;
  const uint64_t budget = model.cacheBytes / 2;
  if (fullSet <= budget) {
    plan.reason = "full working set already fits the cache budget";
    return plan;
  }
  // Compare powers of two using exact integer capacity checks. Floating-point
  // roots can round down at a perfect power and lose an otherwise fitting tile.
  // Saturation is always above budget (at most SIZE_MAX / 2).
  size_t tile = 1;
  while (tile <= iterations / 2) {
    const size_t next = tile * 2;
    if (workingSet(next, dimensions, elementBytes) > budget) break;
    tile = next;
  }
  if (tile < 8) {
    plan.reason = "cache budget cannot hold a useful minimum tile";
    return plan;
  }
  plan.tileSize = tile;
  plan.workingSetAfter = workingSet(tile, dimensions, elementBytes);
  if (hotness < 0.5) {
    plan.reason = "loop nest is cold";
    return plan;
  }
  if (probe && probeBudget > 0) {
    plan.probeUsed = true;
    const std::optional<bool> accepted = probe(TileCandidate{tile, plan.workingSetAfter});
    if (accepted.has_value() && !*accepted) {
      plan.reason = "probe rejected the cache-derived tile";
      return plan;
    }
  }
  plan.tile = true;
  plan.reason = "cache-derived tile fits the working-set budget";
  return plan;
}
}}  // namespace sysy::flat
