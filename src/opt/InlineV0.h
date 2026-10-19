// S15: hot-call inlining V0; single-block callees without alloca/phi only.
#ifndef SYSY_OPT_INLINEV0_H
#define SYSY_OPT_INLINEV0_H
#include <cstddef>
#include <string>
#include "opt/CostModel.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
// Input: one verified Function. Output: tracked insertions of a callee's
// single-block body and replacement/erasure of the call. Callees with phi,
// alloca, multiple blocks or self recursion are declined. Only calls at
// frequency >= 0.5 of the caller entry are candidates. No target facts means
// the ConservativeCostModel ceiling (2 instructions) applies.
class InlineV0Pass final : public FunctionPass {
 public:
  explicit InlineV0Pass(const TargetCostModel* model = nullptr,
                        CostContext context = {});
  PassResult run(Function& function, PassContext& context) override;
 private:
  const TargetCostModel* model_;
  CostContext costContext_;
};
}}  // namespace sysy::flat
#endif
