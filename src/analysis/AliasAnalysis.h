// S13: conservative may/must alias over Flat pointer provenance.
#ifndef SYSY_ANALYSIS_ALIASANALYSIS_H
#define SYSY_ANALYSIS_ALIASANALYSIS_H
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
class GlobalVariable;
class Instruction;
class Value;

// One immutable provenance fact for a pointer value. `object` identifies an
// Alloca instruction, a GlobalVariable, a Function (Param, with `index`) or a
// call Instruction (Fresh result). Unknown absorbs every query.
struct AliasBase {
  enum class Kind : uint8_t { Unknown, Alloca, Global, Param, Fresh };
  Kind kind = Kind::Unknown;
  const void* object = nullptr;
  size_t index = 0;
  int64_t offset = 0;
  bool offsetKnown = true;
};

enum class AliasKind : uint8_t { NoAlias, MayAlias, MustAlias };

class AliasInfo {
 public:
  AliasInfo() = default;
  explicit AliasInfo(std::unordered_map<const Value*, AliasBase> bases)
      : bases_(std::move(bases)) {}
  AliasKind query(const Value* a, const Value* b) const;
  const AliasBase* baseOf(const Value* value) const;
 private:
  std::unordered_map<const Value*, AliasBase> bases_;
};

struct AliasAnalysis {
  using Result = AliasInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Module;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::Memory | MutationKind::Calls |
           MutationKind::Globals | MutationKind::Interface;
  }
  static std::optional<Result> run(const Module&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif  // SYSY_ANALYSIS_ALIASANALYSIS_H
