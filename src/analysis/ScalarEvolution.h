// S20: scalar evolution for simple loop induction variables.
#ifndef SYSY_ANALYSIS_SCALAREVOLUTION_H
#define SYSY_ANALYSIS_SCALAREVOLUTION_H
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
class Value;
struct SCEV {
  enum class Kind : uint8_t { Unknown, Constant, AddRec, Add, Mul };
  Kind kind = Kind::Unknown;
  int64_t constant = 0;
  size_t header = 0;          // AddRec loop header block index
  std::shared_ptr<SCEV> left;
  std::shared_ptr<SCEV> right;
  bool isConstant() const { return kind == Kind::Constant; }
  std::string describe() const;
};
struct SCEVInfo {
  std::unordered_map<const Value*, std::shared_ptr<SCEV>> expressions;
  std::shared_ptr<SCEV> of(const Value* value) const {
    const auto it = expressions.find(value);
    return it == expressions.end() ? nullptr : it->second;
  }
};
struct ScalarEvolutionAnalysis {
  using Result = SCEVInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::DataFlow | MutationKind::CFG; }
  static std::optional<Result> run(const Function&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif
