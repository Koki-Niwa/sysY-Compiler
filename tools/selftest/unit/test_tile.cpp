// S24: cache-capacity tile planning derived from the memory model.
#include <cstdio>
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
  check(plan.tile && plan.tileSize == 64, "larger cache halves the tile size boundary");
  plan = planTiling(1024, 2, 4, cache(16384), 1.0);
  check(plan.tile && plan.tileSize == 32, "smaller cache keeps the derived tile");
  // 3-D stencil, 256^3, 4-byte elements: cube-root bound gives an 8 tile.
  plan = planTiling(256, 3, 4, cache(32768), 1.0);
  check(plan.tile && plan.tileSize == 8 && plan.workingSetAfter == 4096,
        "3-D stencil tile is derived from the same budget");
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
  testRejections();
  testProbe();
  std::printf("S24_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
