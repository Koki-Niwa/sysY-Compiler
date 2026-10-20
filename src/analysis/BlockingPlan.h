// S24c-a: 2D blocked-order planning. No IR is rewritten here.
#ifndef SYSY_ANALYSIS_BLOCKINGPLAN_H
#define SYSY_ANALYSIS_BLOCKINGPLAN_H
#include <cstddef>
#include "analysis/TilePlan.h"
namespace sysy { namespace flat {
struct BlockingContext {
  size_t outerTrip = 0;
  size_t innerTrip = 0;
  size_t outerStride = 0;  // elements per outer step
  size_t innerStride = 1;  // elements per inner step
  size_t elementBytes = 4;
  double hotness = 1.0;
  // Caller must prove the loop nest has no dependence that the reordering
  // would violate; this planner does not run dependence analysis itself.
  bool dependenceFree = false;
};
struct BlockingPlan {
  bool block = false;
  size_t tileOuter = 0;
  size_t tileInner = 0;
  // true: i0, j0, i, j (row-major blocked); false: j0, i0, j, i.
  bool outerTileFirst = true;
  // Upper-bound cache-line footprint of the tile (or of the full nest when
  // block is false), not raw element bytes.
  size_t workingSetBytes = 0;
  const char* reason = "small";
};
BlockingPlan planBlocking(const BlockingContext& context,
                          const MemoryModel& model = {});
}}  // namespace sysy::flat
#endif
