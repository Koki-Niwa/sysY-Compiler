// S14: Function-scope dominator analysis over the single ir/DominatorTree.
#ifndef SYSY_ANALYSIS_DOMINATORANALYSIS_H
#define SYSY_ANALYSIS_DOMINATORANALYSIS_H
#include <cstddef>
#include <optional>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "ir/FlatVerifier.h"
namespace sysy { namespace flat {
// Immutable copy of the shared Cooper-Harvey-Kennedy result. This wrapper does
// not reimplement dominators; FlatVerifier and analyses use one algorithm.
class DominatorInfo {
 public:
  bool reachable(size_t block) const { return tree_.reachable(block); }
  const BasicBlock* idom(size_t block) const { return tree_.idom(block); }
  bool dominates(const BasicBlock* a, const BasicBlock* b) const {
    return tree_.dominates(a, b);
  }
  const std::vector<int>& postOrder() const { return tree_.postOrder(); }
 private:
  friend struct DominatorAnalysis;
  explicit DominatorInfo(const Function& function) : tree_(function) {}
  DominatorTree tree_;
};
struct DominatorAnalysis {
  using Result = DominatorInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
