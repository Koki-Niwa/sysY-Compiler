#include "analysis/UnrollPlan.h"
#include <algorithm>
#include <cmath>
#include <vector>
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
  // There must be room for at least one additional value. This guard also
  // proves live+1 cannot overflow, even for size_t boundary inputs.
  if (live >= registers) return 1;
  // One extra register for the induction/bounds of the unrolled code.
  return std::max<size_t>(1, registers / std::max<size_t>(1, live + 1));
}
double penaltyFor(size_t live, size_t factor, size_t registers) {
  const double ratio = static_cast<double>(live * factor) /
                       std::max<size_t>(1, registers);
  return ratio * ratio;
}
struct Candidate {
  size_t factor = 0;
  size_t vector = 1;
  double gain = 0.0;
  double penalty = 0.0;
};
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
  if (context.criticalPath > 0) {
    const size_t ilpCap =
        std::max<size_t>(1, context.bodyInstructions / context.criticalPath);
    factor = std::min(factor, ilpCap);
  }
  if (factor < 2) {
    plan.reason = "register pressure or critical path leaves no useful unroll factor";
    return plan;
  }
  // Preserve the conservative cliff on the scalar candidate before searching
  // smaller vector/unroll combinations; a plan under this pressure is refused.
  plan.pressurePenalty = penaltyFor(context.liveValues, factor,
                                    profile.scalarRegisters);
  plan.projectedLiveValues = context.liveValues * factor;
  if (plan.pressurePenalty > 0.8) {
    plan.reason = "projected live values cross the pressure cliff";
    return plan;
  }
  std::vector<size_t> vectors{1};
  if (profile.target == UnrollTarget::AArch64 && profile.vectorWidth > 1) {
    // A useful candidate has UF >= 2, so VF above four cannot satisfy the
    // joint VF*UF budget. Bound the search and guard the doubling step so a
    // malformed profile cannot turn it into an overflowing loop.
    const size_t maxVector = std::min<size_t>(profile.vectorWidth, 8 / 2);
    for (size_t vf = 2; vf <= maxVector;) {
      vectors.push_back(vf);
      if (vf > maxVector / 2) break;
      vf *= 2;
    }
  }
  Candidate best;
  for (size_t vector : vectors) {
    const size_t productBudget =
        vector > 0 ? std::max<size_t>(1, 8 / vector) : 1;
    const size_t maxFactor = std::min(factor, productBudget);
    for (size_t candidate = 2; candidate <= maxFactor; ++candidate) {
      const double penalty = penaltyFor(context.liveValues, candidate,
                                        profile.scalarRegisters);
      if (penalty > 0.8) continue;
      const double gain = static_cast<double>(vector * candidate) /
                          (1.0 + penalty);
      if (gain > best.gain ||
          (gain == best.gain && (vector > best.vector ||
                                 (vector == best.vector && candidate > best.factor)))) {
        best = Candidate{candidate, vector, gain, penalty};
      }
    }
  }
  if (best.factor < 2) {
    plan.reason = "no VF/UF candidate satisfies the pressure cliff";
    return plan;
  }
  plan.projectedLiveValues = context.liveValues * best.factor;
  plan.pressurePenalty = best.penalty;
  if (probe && probeBudget > 0) {
    plan.probeUsed = true;
    const std::optional<bool> accepted =
        probe(best.factor, plan.projectedLiveValues);
    if (accepted.has_value() && !*accepted) {
      plan.reason = "probe rejected the unroll candidate";
      return plan;
    }
  }
  plan.unroll = true;
  plan.factor = best.factor;
  plan.vectorFactor = best.vector;
  plan.estimatedGain = best.gain;
  plan.reason = "hot loop with a register-safe VF/UF candidate";
  return plan;
}
}}  // namespace sysy::flat
