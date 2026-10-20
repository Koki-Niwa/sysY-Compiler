// S25: unroll-factor planning with target registers and a nonlinear pressure
// penalty. No per-test block factors.
#ifndef SYSY_ANALYSIS_UNROLLPLAN_H
#define SYSY_ANALYSIS_UNROLLPLAN_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
namespace sysy { namespace flat {
enum class UnrollTarget : uint8_t { Generic, AArch64, RV64 };
struct UnrollProfile {
  UnrollTarget target = UnrollTarget::Generic;
  size_t scalarRegisters = 16;
  // Caller-selected VF; the baseline planner only caps UF for this width.
  size_t vectorWidth = 1;
};
struct UnrollContext {
  size_t tripCount = 0;
  size_t bodyInstructions = 0;
  size_t liveValues = 0;
  double hotness = 0.0;
};
struct UnrollPlan {
  bool unroll = false;
  size_t factor = 0;
  size_t projectedLiveValues = 0;
  double pressurePenalty = 0.0;
  size_t vectorFactor = 1;
  bool probeUsed = false;
  const char* reason = "";
};
using UnrollProbe =
    std::function<std::optional<bool>(size_t factor, size_t projectedLive)>;
// Baseline heuristic only: hotness is caller-supplied, and no critical-path
// model, joint VF/UF search or machine-register-bank estimate is implemented.
// A successful plan is a cost candidate, not proof of transform legality.
UnrollPlan planUnroll(const UnrollContext& context, const UnrollProfile& profile,
                      const UnrollProbe& probe = {}, size_t probeBudget = 2);
}}  // namespace sysy::flat
#endif
