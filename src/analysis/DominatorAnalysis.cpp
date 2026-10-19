#include "analysis/DominatorAnalysis.h"
namespace sysy { namespace flat {
std::optional<DominatorInfo> DominatorAnalysis::run(
    const Function& function, AnalysisManager&) {
  if (function.blockCount() == 0 || function.entry() == nullptr)
    return std::nullopt;
  return DominatorInfo(function);
}
}}  // namespace sysy::flat
