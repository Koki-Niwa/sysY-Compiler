// S25a: BFI-driven advice for canonical constant-trip loops.
#ifndef SYSY_ANALYSIS_UNROLLADVISOR_H
#define SYSY_ANALYSIS_UNROLLADVISOR_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "analysis/UnrollPlan.h"
namespace sysy { namespace flat {
class Function;
struct UnrollLoopAdvice {
  size_t header = 0;
  size_t tripCount = 0;
  size_t bodyInstructions = 0;
  double hotness = 0.0;
  UnrollPlan plan;
};
struct UnrollAdvice {
  std::vector<UnrollLoopAdvice> loops;
};
// Consumes BlockFrequencyAnalysis and LoopInfoAnalysis. Only canonical
// constant-trip loops are advised; unsupported shapes are skipped. This is a
// planning query, not a MutationAuthority.
std::optional<UnrollAdvice> adviseUnrolls(const Function& function,
                                          AnalysisManager& manager,
                                          const UnrollProfile& profile);
}}  // namespace sysy::flat
#endif
