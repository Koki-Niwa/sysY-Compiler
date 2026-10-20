#include "analysis/UnrollPlan.h"
#include <algorithm>
#include <cmath>
namespace sysy { namespace flat {
namespace {
size_t targetLimit(UnrollTarget target) {
  switch (target) {
    case UnrollTarget::AArch64: return 4;
    case UnrollTarget::RV64: return 2;
    case UnrollTarget::Generic: return 2;
  }
  return 1;
}
size_t registerLimit(size_t registers, size_t live) {
  if (registers == 0) return 1;
  // One extra register for the induction/bounds of the unrolled code.
  return std::max<size_t>(1, registers / std::max<size_t>(1, live + 1));
}
}  // namespace
UnrollPlan planUnroll(const UnrollContext& context, const UnrollProfile& profile,
                      const UnrollProbe& probe, size_t probeBudget) {
  UnrollPlan plan;
  if (context.tripCount < 2 || context.bodyInstructions == 0) {
    plan.reason = "trip count or body cannot benefit";
    return plan;
  }
  if (context.hotness < 0.5) {
    plan.reason = "loop is cold";
    return plan;
  }
  size_t factor = targetLimit(profile.target);
  factor = std::min(factor, context.tripCount);
  factor = std::min(factor, registerLimit(profile.scalarRegisters,
                                          context.liveValues));
  if (profile.target == UnrollTarget::AArch64 && profile.vectorWidth > 1) {
    // Joint VF x UF budget: keep the product at or below eight lanes.
    const size_t vectorBudget =
        std::max<size_t>(1, 8 / std::max<size_t>(1, profile.vectorWidth));
    factor = std::min(factor, vectorBudget);
    plan.vectorFactor = profile.vectorWidth;
  }
  if (factor < 2) {
    plan.reason = "register pressure leaves no useful unroll factor";
    return plan;
  }
  // Nonlinear pressure penalty: grow with (live*factor / registers)^2.
  const double ratio = static_cast<double>(context.liveValues * factor) /
                       std::max<size_t>(1, profile.scalarRegisters);
  plan.pressurePenalty = ratio * ratio;
  plan.projectedLiveValues = context.liveValues * factor;
  if (plan.pressurePenalty > 0.8) {
    plan.reason = "projected live values cross the pressure cliff";
    return plan;
  }
  if (probe && probeBudget > 0) {
    plan.probeUsed = true;
    const std::optional<bool> accepted =
        probe(factor, plan.projectedLiveValues);
    if (accepted.has_value() && !*accepted) {
      plan.reason = "probe rejected the unroll candidate";
      return plan;
    }
  }
  plan.unroll = true;
  plan.factor = factor;
  plan.reason = "hot loop with register-safe unroll factor";
  return plan;
}
}}  // namespace sysy::flat
