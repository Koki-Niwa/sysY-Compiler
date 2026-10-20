// S25: unroll-factor planning with register pressure and target profiles.
#include <cstdio>
#include <cstring>
#include <limits>
#include "analysis/UnrollPlan.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
UnrollContext context(size_t trip, size_t body, size_t live, double hot = 1.0) {
  UnrollContext c;
  c.tripCount = trip;
  c.bodyInstructions = body;
  c.liveValues = live;
  c.hotness = hot;
  return c;
}
void testTargets() {
  UnrollProfile arm;
  arm.target = UnrollTarget::AArch64;
  arm.scalarRegisters = 32;
  auto plan = planUnroll(context(100, 6, 4), arm);
  check(plan.unroll && plan.factor == 4 && plan.projectedLiveValues == 16 &&
        plan.pressurePenalty > 0.0,
        "AArch64 scalar unroll derives factor 4 from registers");
  plan = planUnroll(context(100, 6, 5), arm);
  check(plan.unroll && plan.factor == 4, "register limit still allows factor 4");
  UnrollProfile rv;
  rv.target = UnrollTarget::RV64;
  rv.scalarRegisters = 32;
  plan = planUnroll(context(100, 6, 4), rv);
  check(plan.unroll && plan.factor == 2 && plan.vectorFactor == 1,
        "RV64 uses a scalar unroll factor of 2");
  UnrollProfile armVec = arm;
  armVec.vectorWidth = 4;
  plan = planUnroll(context(100, 6, 4), armVec);
  check(plan.unroll && plan.factor == 2 && plan.vectorFactor == 4,
        "AArch64 VF x UF budget caps the joint factor at 8");
}
void testRejections() {
  UnrollProfile p;
  p.scalarRegisters = 8;
  auto plan = planUnroll(context(100, 6, 6), p);
  check(!plan.unroll && plan.factor == 0 &&
        plan.pressurePenalty == 0.0 &&
        std::strstr(plan.reason, "no useful unroll factor") != nullptr,
        "register cap rejects before evaluating the pressure cliff");
  p.scalarRegisters = 16;
  plan = planUnroll(context(100, 6, 4, 0.1), p);
  check(!plan.unroll && std::strstr(plan.reason, "cold") != nullptr,
        "cold loop is not unrolled");
  plan = planUnroll(context(1, 6, 4), p);
  check(!plan.unroll, "trip count below two is not unrolled");
  check(!planUnroll(context(100, 0, 4), p).unroll, "empty body is rejected");
}
void testPressureCliff() {
  UnrollProfile p;
  p.target = UnrollTarget::AArch64;
  // Both profiles permit UF=4 via registers/(live+1). Only the nonlinear
  // penalty separates them: (36/40)^2 = 0.81; (36/41)^2 is about 0.771.
  p.scalarRegisters = 40;
  bool called = false;
  auto plan = planUnroll(context(100, 6, 9), p,
      [&](size_t, size_t) -> std::optional<bool> {
        called = true;
        return true;
      });
  check(!plan.unroll && plan.projectedLiveValues == 36 &&
        plan.pressurePenalty > 0.80 && plan.pressurePenalty < 0.82 &&
        std::strstr(plan.reason, "cross the pressure cliff") != nullptr &&
        !called && !plan.probeUsed,
        "pressure above 0.8 rejects even a probe that would approve");
  p.scalarRegisters = 41;
  plan = planUnroll(context(100, 6, 9), p);
  check(plan.unroll && plan.factor == 4 && plan.projectedLiveValues == 36 &&
        plan.pressurePenalty > 0.77 && plan.pressurePenalty < 0.78,
        "pressure below 0.8 retains a register-permitted factor");
}
void testRegisterBoundaries() {
  const size_t max = std::numeric_limits<size_t>::max();
  UnrollProfile p;
  p.target = UnrollTarget::AArch64;
  p.scalarRegisters = max;
  auto plan = planUnroll(context(100, 6, max), p);
  check(!plan.unroll && plan.factor == 0 && plan.projectedLiveValues == 0 &&
        plan.pressurePenalty == 0.0 &&
        std::strstr(plan.reason, "no useful unroll factor") != nullptr,
        "SIZE_MAX live values cannot wrap the register-cap denominator");
  plan = planUnroll(context(100, 6, max / 4), p);
  check(plan.unroll && plan.factor == 3 &&
        plan.projectedLiveValues == (max / 4) * 3 &&
        plan.pressurePenalty > 0.56 && plan.pressurePenalty < 0.57,
        "large representable register budgets retain a valid capped candidate");
}
void testProbe() {
  UnrollProfile p;
  p.scalarRegisters = 32;
  bool called = false;
  auto plan = planUnroll(context(100, 6, 4), p,
      [&](size_t factor, size_t live) -> std::optional<bool> {
        called = true;
        return factor == 2 && live == 8 ? false : true;
      });
  check(!plan.unroll && called && plan.probeUsed &&
        std::strstr(plan.reason, "probe rejected") != nullptr,
        "bounded probe can reject the unroll candidate");
  called = false;
  plan = planUnroll(context(100, 6, 4), p,
      [&](size_t factor, size_t live) -> std::optional<bool> {
        called = true;
        return factor >= 2 && live >= 8;
      });
  check(plan.unroll && called && plan.probeUsed,
        "bounded probe can accept the unroll candidate");
}
}  // namespace
int main() {
  testTargets();
  testRejections();
  testPressureCliff();
  testRegisterBoundaries();
  testProbe();
  std::printf("S25_CHECKS=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
