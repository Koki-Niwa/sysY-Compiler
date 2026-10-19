// S12: SCC-level effect/purity summaries, computed bottom-up through the AM.
#ifndef SYSY_ANALYSIS_FUNCTIONSUMMARY_H
#define SYSY_ANALYSIS_FUNCTIONSUMMARY_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "analysis/CallGraph.h"
namespace sysy { namespace flat {
class Function;

// Conservative, independent effect facts: readsMemory means the function may
// observe mutable memory, writesMemory means it may modify memory, and
// callsExternal means it invokes a callee without a module definition.
// `pure == true` requires all three flags to be false, every reachable callee
// to be pure, and makes no claim about termination. Purity is a greatest fixed
// point inside a recursive SCC, so mutual recursion without effects stays
// pure; effects are a least fixed point, so recursion alone never invents one.
struct FunctionSummary {
  const Function* function = nullptr;
  size_t functionIndex = 0;
  size_t scc = 0;
  bool pure = false;
  bool readsMemory = false;
  bool writesMemory = false;
  bool callsExternal = false;
  bool recursive = false;
};

struct SccSummary {
  size_t scc = 0;
  std::vector<FunctionSummary> members;   // CallGraphScc::members order
  bool pure = true;
  bool readsMemory = false;
  bool writesMemory = false;
  bool callsExternal = false;
};

// First real CGSCC consumer: one cached result per SCC. Callee SCCs are
// queried through the AnalysisManager, so their generations become
// dependencies of this result.
struct SccSummaryAnalysis {
  using Result = SccSummary;
  static constexpr AnalysisScope scope = AnalysisScope::CGSCC;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::Calls | MutationKind::Memory | MutationKind::Interface;
  }
  static std::optional<Result> run(const CallGraphInfo&, size_t scc,
                                   AnalysisManager&);
};

struct FunctionSummaryInfo {
  std::vector<FunctionSummary> perFunction;   // CallGraphInfo::nodes order
  std::vector<SccSummary> sccs;
};

// Module-level aggregator over the CGSCC results. Consumers may use either
// this or SccSummaryAnalysis directly; both share one cache.
struct FunctionSummaryAnalysis {
  using Result = FunctionSummaryInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Module;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::Calls | MutationKind::Memory | MutationKind::Interface;
  }
  static std::optional<Result> run(const Module&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif  // SYSY_ANALYSIS_FUNCTIONSUMMARY_H
