// S24c-a: deterministic 2D blocked-order planning.
#include <cstdio>
#include <limits>
#include <string>
#include "analysis/BlockingPlan.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
MemoryModel model() { return MemoryModel{64, 32768}; }
}  // namespace
int main() {
  {  // The exact half-cache tile is accepted; the whole working set is not.
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1024; c.outerStride = 1024;
    c.innerStride = 1;
    BlockingPlan p = planBlocking(c, model());
    check(p.block && p.tileOuter == 64 && p.tileInner == 64,
          "1024x1024 uses a 64x64 element tile");
    check(p.workingSetBytes == 64 * 64 * 4, "tile footprint is exact");
    check(p.outerTileFirst, "row-major keeps i-tile first");
    check(p.reason != nullptr && std::string(p.reason) == "blocked", "reason blocked");
  }
  {
    BlockingContext c; c.outerTrip = 64; c.innerTrip = 64; c.outerStride = 64;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block, "64x64 exactly fits the half-cache budget");
    check(p.workingSetBytes == 64 * 64 * 4, "fits working set recorded");
    check(std::string(p.reason) == "fits", "reason fits");
  }
  {
    BlockingContext c; c.outerTrip = 65; c.innerTrip = 64; c.outerStride = 64;
    BlockingPlan p = planBlocking(c, model());
    check(p.block && p.tileOuter == 64 && p.tileInner == 64,
          "one element over budget blocks with a 64x64 tile");
  }
  {
    BlockingContext c; c.outerTrip = 65; c.innerTrip = 63; c.outerStride = 63;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block && std::string(p.reason) == "fits",
          "one element below budget still fits");
  }
  {
    BlockingContext c; c.outerTrip = 128; c.innerTrip = 128; c.outerStride = 128;
    BlockingPlan p = planBlocking(c, model());
    check(p.block && p.tileOuter == 64 && p.tileInner == 64,
          "128x128 chooses 64x64");
  }
  {
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1024;
    c.outerStride = 1; c.innerStride = 1024;
    BlockingPlan p = planBlocking(c, model());
    check(p.block && !p.outerTileFirst,
          "column-major keeps the unit-stride outer loop innermost");
  }
  {
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1024;
    c.dependenceFree = false;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block && std::string(p.reason) == "dependence",
          "dependence blocks tiling");
  }
  {
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1024; c.hotness = 0.1;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block && std::string(p.reason) == "cold", "cold nest is declined");
  }
  {
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block && std::string(p.reason) == "small", "one trip is declined");
  }
  {
    BlockingContext c; c.outerTrip = std::numeric_limits<size_t>::max();
    c.innerTrip = std::numeric_limits<size_t>::max();
    BlockingPlan p = planBlocking(c, model());
    check(p.block && p.tileOuter >= 1 && p.tileInner >= 1 &&
              p.workingSetBytes <= 32768,
          "SIZE_MAX trips saturate without overflow");
  }
  {
    BlockingContext c; c.outerTrip = 1024; c.innerTrip = 1024;
    c.elementBytes = 32768;
    BlockingPlan p = planBlocking(c, model());
    check(!p.block && std::string(p.reason) == "capacity",
          "an element larger than the budget is rejected");
  }
  std::printf("S24C_BLOCKING_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
