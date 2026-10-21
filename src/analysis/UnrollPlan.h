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
  // Caller-selected maximum VF. AArch64 planning enumerates power-of-two
  // widths up to this value, bounded by the VF*UF product budget.
  size_t vectorWidth = 1;
};
struct UnrollContext {
  size_t tripCount = 0;
  size_t bodyInstructions = 0;
  size_t liveValues = 0;
  // Length of the longest dependence chain in one iteration. 0 = unknown
  // (no cap); otherwise unrolling cannot exceed bodyInstructions/criticalPath.
  size_t criticalPath = 0;
  double hotness = 0.0;
};
struct UnrollPressure {
  size_t projectedLive = 0;
  size_t excessLive = 0;
  size_t spillSlots = 0;
  size_t stackBytes = 0;
  bool spills = false;
};
// Static register-pressure estimate, not a backend spill measurement. One
// excess live value is modeled as one 8-byte spill slot; multiplication and
// subtraction saturate at SIZE_MAX.
UnrollPressure estimateUnrollPressure(size_t live, size_t factor, size_t registers);
struct UnrollPlan {
  bool unroll = false;
  size_t factor = 0;
  size_t projectedLiveValues = 0;
  double pressurePenalty = 0.0;
  size_t vectorFactor = 1;
  double estimatedGain = 0.0;
  UnrollPressure pressure;
  bool probeUsed = false;
  const char* reason = "";
};
using UnrollProbe =
    std::function<std::optional<bool>(size_t factor, size_t projectedLive)>;
// Heuristic planning model: hotness is caller-supplied, the critical path is
// an input, and VF/UF candidates are enumerated for the target profile. There
// is no per-register-bank machine model or backend calibration; a successful
// plan is a cost candidate, not proof of transform legality.
UnrollPlan planUnroll(const UnrollContext& context, const UnrollProfile& profile,
                      const UnrollProbe& probe = {}, size_t probeBudget = 2);
}}  // namespace sysy::flat
#endif
