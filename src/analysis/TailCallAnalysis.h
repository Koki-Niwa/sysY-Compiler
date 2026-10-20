// S22: classify tail calls and prove stack-safety of self tail recursion.
#ifndef SYSY_ANALYSIS_TAILCALLANALYSIS_H
#define SYSY_ANALYSIS_TAILCALLANALYSIS_H
#include <cstdint>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
class Instruction;
enum class TailCallKind : uint8_t { NotTail, DirectSelfTail, MutualTail, GeneralTail };
struct TailCallInfo {
  const Instruction* call = nullptr;
  TailCallKind kind = TailCallKind::NotTail;
  bool stackSafe = false;     // only meaningful for DirectSelfTail
  bool hasStackObject = false;
  const char* reason = "";
};
struct TailCallResult {
  std::vector<TailCallInfo> calls;
  bool hasDirectSelfTail = false;
  bool allDirectSelfTailsStackSafe = true;
};
struct TailCallAnalysis {
  using Result = TailCallResult;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::Calls | MutationKind::CFG | MutationKind::Memory;
  }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
