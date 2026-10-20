// S15: hot-call inlining V0; single-block callees without alloca/phi only.
#ifndef SYSY_OPT_INLINEV0_H
#define SYSY_OPT_INLINEV0_H
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "opt/CostModel.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class Function;
class Instruction;

struct InlineProbeRequest {
  const Instruction* call = nullptr;
  const Function* callee = nullptr;
  double frequency = 0.0;
  size_t instructions = 0;
};
struct InlineProbeResult {
  bool profitable = false;
  double latencyDelta = 0.0;
  double codeGrowth = 0.0;
};
// Optional bounded backend probe. Returning nullopt means "no budget/no
// backend"; the caller then uses the conservative cost fallback.
using InlineProbe =
    std::function<std::optional<InlineProbeResult>(const InlineProbeRequest&)>;

struct InlineRecord {
  const Instruction* call = nullptr;
  bool inlined = false;
  double frequency = 0.0;
  size_t instructions = 0;
  double codeGrowth = 0.0;
  double latencyDelta = 0.0;
  bool probeUsed = false;
  std::string reason;
};

// Input: one verified Function. Output: tracked insertions of a callee's
// single-block body and replacement/erasure of the call. Callees with phi,
// alloca, multiple blocks or self recursion are declined. Only calls at
// frequency >= 0.5 of the caller entry are candidates. No target facts means
// the ConservativeCostModel ceiling (2 instructions) applies.
class InlineV0Pass final : public FunctionPass {
 public:
  explicit InlineV0Pass(const TargetCostModel* model = nullptr,
                        CostContext context = {}, InlineProbe probe = {},
                        size_t probeBudget = 4);
  PassResult run(Function& function, PassContext& context) override;
  const std::vector<InlineRecord>& records() const { return records_; }
 private:
  const TargetCostModel* model_;
  CostContext costContext_;
  InlineProbe probe_;
  size_t probeBudget_;
  std::vector<InlineRecord> records_;
};
}}  // namespace sysy::flat
#endif
