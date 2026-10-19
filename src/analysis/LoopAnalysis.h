// S14: natural loops from the shared dominator tree, with a Loop-scope view.
#ifndef SYSY_ANALYSIS_LOOPANALYSIS_H
#define SYSY_ANALYSIS_LOOPANALYSIS_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;

// Natural loop: header dominates every back-edge tail and the block set is
// closed under predecessors except at the header. Loops are ordered by header
// index; `parent` is the smallest enclosing loop, or -1.
struct LoopEntry {
  size_t header = 0;
  std::vector<size_t> blocks;   // sorted block indices, header included
  long parent = -1;
  size_t depth = 0;
};

struct LoopInfoResult {
  std::vector<LoopEntry> loops;
};

struct LoopInfoAnalysis {
  using Result = LoopInfoResult;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};

// First real Loop-scope consumer: one cached result per loop slot.
struct LoopView {
  size_t loop = 0;
  size_t header = 0;
  long parent = -1;
  size_t depth = 0;
  size_t blockCount = 0;
};

struct LoopViewAnalysis {
  using Result = LoopView;
  static constexpr AnalysisScope scope = AnalysisScope::Loop;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, size_t loopIndex,
                                   AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
