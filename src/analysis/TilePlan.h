// S24: cache-capacity driven tile planning (no case-specific block sizes).
#ifndef SYSY_ANALYSIS_TILEPLAN_H
#define SYSY_ANALYSIS_TILEPLAN_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include "analysis/InterchangePlan.h"
namespace sysy { namespace flat {
struct TileCandidate {
  size_t tile = 0;
  uint64_t workingSetBytes = 0;
};
// Optional bounded probe: true accepts the candidate, false rejects it.
using TileProbe = std::function<std::optional<bool>(const TileCandidate&)>;
struct TilePlan {
  bool tile = false;
  size_t tileSize = 0;
  uint64_t workingSetBefore = 0;
  uint64_t workingSetAfter = 0;
  double hotness = 0.0;
  bool probeUsed = false;
  const char* reason = "";
};
// `dimensions` is the number of tiled loop levels (2 for matmul/stencil,
// 3 for a 3-D stencil). Working set assumes two streams (read + write) of
// `elementBytes`; the tile is the largest power of two whose working set fits
// half the cache, bounded by the iteration count. A full-size working set that
// already fits half the cache is left untiled. Capacity comparisons are integer
// exact; reported working sets saturate at UINT64_MAX on overflow. Search is
// bounded by the size_t bit width, independent of the numeric iteration count.
TilePlan planTiling(size_t iterations, size_t dimensions, uint64_t elementBytes,
                    const MemoryModel& model, double hotness,
                    const TileProbe& probe = {}, size_t probeBudget = 2);
}}  // namespace sysy::flat
#endif
