// S23: interchange legality and cache-line benefit planning.
#include <cstdio>
#include <vector>
#include "analysis/InterchangePlan.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
void testLegality() {
  MemoryModel model;
  auto plan = planInterchange({{1, 1}}, true, 1.0, 0, 1, {4096, 4}, 1024, model);
  check(plan.legal && !plan.profitable && plan.strideBefore == 4 &&
        plan.strideAfter == 4096 && plan.trafficAfter > plan.trafficBefore,
        "transpose-like (1,1) is legal but row-major swap raises traffic");
  plan = planInterchange({{1, -1}}, true, 1.0, 0, 1, {4, 8}, 1024, model);
  check(!plan.legal, "mixed-direction dependence forbids interchange");
  plan = planInterchange({{1, 1}}, false, 1.0, 0, 1, {4, 8}, 1024, model);
  check(!plan.legal, "non-exact dependence forbids interchange");
  plan = planInterchange({{0, 1}}, true, 0.1, 0, 1, {4, 8}, 1024, model);
  check(plan.legal && !plan.profitable, "cold nest is legal but not profitable");
}
void testTargetMemoryModel() {
  const std::vector<int64_t> strides = {4, 128};
  MemoryModel line64;
  line64.cacheLineBytes = 64;
  auto p64 = planInterchange({{1, 1}}, true, 1.0, 0, 1, strides, 1024, line64);
  check(p64.legal && p64.profitable && p64.trafficAfter < p64.trafficBefore,
        "64-byte line: swap moves the 128-byte stride out of the inner loop");
  MemoryModel line128;
  line128.cacheLineBytes = 128;
  auto p128 = planInterchange({{1, 1}}, true, 1.0, 0, 1, strides, 1024, line128);
  check(p128.legal && !p128.profitable,
        "128-byte line: both strides fit a line, so the plan is not profitable");
}
void testEquivalenceOracle() {
  // The iteration permutation preserves every legal dependence direction; a
  // hand-checked oracle over the small distance set.
  const std::vector<std::vector<int64_t>> legal = {{0, 1}, {1, 0}, {1, 1}, {0, 0}};
  const std::vector<std::vector<int64_t>> illegal = {{-1, 1}, {2, -3}};
  bool legalOk = true;
  for (const auto& d : legal)
    legalOk = legalOk &&
              planInterchange({d}, true, 1.0, 0, 1, {4, 8}, 16, MemoryModel()).legal;
  bool illegalOk = true;
  for (const auto& d : illegal)
    illegalOk = illegalOk &&
               !planInterchange({d}, true, 1.0, 0, 1, {4, 8}, 16, MemoryModel()).legal;
  check(legalOk && illegalOk, "equivalence oracle accepts exactly the legal directions");
}
}  // namespace
int main() {
  testLegality();
  testTargetMemoryModel();
  testEquivalenceOracle();
  std::printf("S23_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
