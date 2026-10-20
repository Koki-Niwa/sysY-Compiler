// S24: cache-capacity tile planning derived from the memory model.
#include <cstdio>
#include <limits>
#include <string>
#include "analysis/TilePlan.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
MemoryModel cache(size_t bytes, size_t line = 64) {
  MemoryModel model;
  model.cacheBytes = bytes;
  model.cacheLineBytes = line;
  return model;
}
void testMatmulAndStencil() {
  // 8 MiB working set, 32 KiB cache: 32x32 tile fits 8 KiB.
  auto plan = planTiling(1024, 2, 4, cache(32768), 1.0);
  check(plan.tile && plan.tileSize == 32 && plan.workingSetAfter == 8192 &&
        plan.workingSetBefore == 8388608,
        "matmul tile is derived from the cache budget");
  plan = planTiling(1024, 2, 4, cache(65536), 1.0);
  check(plan.tile && plan.tileSize == 64, "larger cache allows the next tile size");
  plan = planTiling(1024, 2, 4, cache(16384), 1.0);
  check(plan.tile && plan.tileSize == 32, "smaller cache keeps the derived tile");
  // 3-D stencil, 256^3, 4-byte elements: cube-root bound gives an 8 tile.
  plan = planTiling(256, 3, 4, cache(32768), 1.0);
  check(plan.tile && plan.tileSize == 8 && plan.workingSetAfter == 4096,
        "3-D stencil tile is derived from the same budget");
}
void testCapacityBoundaries() {
  // Hand-computed two-stream footprints: 2 * T^d * 4 bytes.
  // Each exact fit must select T; one byte less cache must select T/2, or
  // decline when T/2 is below the minimum. No floating-point oracle is used.
  struct Case {
    size_t dimensions;
    size_t tile;
    size_t footprint;
  };
  const Case cases[] = {{2, 64, 32768}, {3, 16, 32768}, {7, 8, 16777216}};
  for (const auto& c : cases) {
    auto plan = planTiling(c.tile * 2, c.dimensions, 4,
                           cache(c.footprint * 2), 1.0);
    check(plan.tile && plan.tileSize == c.tile &&
          plan.workingSetAfter == c.footprint,
          "an exact integer capacity boundary accepts the largest fitting tile");
    plan = planTiling(c.tile * 2, c.dimensions, 4,
                      cache(c.footprint * 2 - 1), 1.0);
    check(c.tile == 8 ? !plan.tile
                     : plan.tile && plan.tileSize == c.tile / 2 &&
                       plan.workingSetAfter <= c.footprint - 1,
          "one byte below the capacity boundary cannot accept the larger tile");
  }
  auto plan = planTiling(16, 7, 4, cache(33554433), 1.0);
  check(plan.tile && plan.tileSize == 8 && plan.workingSetAfter == 16777216,
        "odd cache capacities use the integer half-cache budget");
}
void testOverflowBoundaries() {
  auto plan = planTiling(std::numeric_limits<size_t>::max(), 3, 4,
                         cache(32768), 1.0);
  check(plan.tile && plan.tileSize == 8 && plan.workingSetBefore == UINT64_MAX &&
        plan.workingSetAfter == 4096,
        "overflowing full extents still select an exactly fitting small tile");
  plan = planTiling(16, 2, UINT64_MAX, cache(32768), 1.0);
  check(!plan.tile && plan.workingSetBefore == UINT64_MAX,
        "overflowing element footprints never appear to fit the cache");
  plan = planTiling(1, std::numeric_limits<size_t>::max(), 4, cache(32768), 1.0);
  check(!plan.tile && plan.workingSetBefore == 8,
        "a unit extent has a bounded calculation even for a huge dimension count");
}
void testRejections() {
  auto plan = planTiling(32, 2, 4, cache(32768), 1.0);
  check(!plan.tile && std::string(plan.reason).find("already fits") != std::string::npos,
        "working set that already fits is left untiled");
  plan = planTiling(1024, 2, 4, cache(32768), 0.2);
  check(!plan.tile && std::string(plan.reason).find("cold") != std::string::npos,
        "cold nest is not tiled");
  plan = planTiling(1024, 2, 4, cache(512), 1.0);
  check(!plan.tile && std::string(plan.reason).find("minimum tile") != std::string::npos,
        "too-small cache is rejected instead of guessing a tile");
  plan = planTiling(0, 2, 4, cache(32768), 1.0);
  check(!plan.tile, "empty nest is rejected");
}
void testProbe() {
  bool called = false;
  auto plan = planTiling(1024, 2, 4, cache(32768), 1.0,
      [&](const TileCandidate& candidate) -> std::optional<bool> {
        called = true;
        return candidate.tile == 32 ? false : true;
      });
  check(!plan.tile && called && plan.probeUsed &&
        std::string(plan.reason).find("probe rejected") != std::string::npos,
        "a bounded probe can reject the cache-derived candidate");
  called = false;
  plan = planTiling(1024, 2, 4, cache(32768), 1.0,
      [&](const TileCandidate& candidate) -> std::optional<bool> {
        called = true;
        return candidate.tile == 32;
      });
  check(plan.tile && called && plan.probeUsed,
        "a bounded probe can accept the cache-derived candidate");
}
}  // namespace
int main() {
  testMatmulAndStencil();
  testCapacityBoundaries();
  testOverflowBoundaries();
  testRejections();
  testProbe();
  std::printf("S24_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
