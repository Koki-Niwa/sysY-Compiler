#include "analysis/TilePlan.h"
#include <algorithm>
#include <cmath>
namespace sysy { namespace flat {
namespace {
uint64_t saturatingMul(uint64_t a, uint64_t b) {
  if (a == 0 || b == 0) return 0;
  if (a > UINT64_MAX / b) return UINT64_MAX;
  return a * b;
}
uint64_t power(uint64_t base, size_t exponent) {
  uint64_t result = 1;
  for (size_t i = 0; i < exponent; ++i) {
    if (result > UINT64_MAX / base) return UINT64_MAX;
    result *= base;
  }
  return result;
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
  const uint64_t count = power(iterations, dimensions);
  const uint64_t fullSet = saturatingMul(saturatingMul(2, count), elementBytes);
  plan.workingSetBefore = fullSet;
  const uint64_t budget = model.cacheBytes / 2;
  if (fullSet <= budget) {
    plan.reason = "full working set already fits the cache budget";
    return plan;
  }
  // Solve 2 * T^dimensions * elementBytes <= budget.
  const long double perTile = static_cast<long double>(budget) /
                              (2.0L * static_cast<long double>(elementBytes));
  const long double root =
      std::pow(static_cast<long double>(perTile), 1.0L / dimensions);
  const size_t maxTile = root >= static_cast<long double>(iterations)
                             ? iterations
                             : static_cast<size_t>(root);
  size_t tile = 1;
  while (tile <= maxTile / 2) tile *= 2;
  if (tile < 8) {
    plan.reason = "cache budget cannot hold a useful minimum tile";
    return plan;
  }
  tile = std::min(tile, iterations);
  plan.tileSize = tile;
  plan.workingSetAfter =
      saturatingMul(saturatingMul(2, power(tile, dimensions)), elementBytes);
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
