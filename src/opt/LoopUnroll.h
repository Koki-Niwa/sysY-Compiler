// S25b: plan-sized chain unrolling for a canonical loop.
#ifndef SYSY_OPT_LOOPUNROLL_H
#define SYSY_OPT_LOOPUNROLL_H
#include "analysis/UnrollPlan.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
// One i32 induction recurrence, one body block, no loop-defined live-outs.
// The chain length is min(plan.factor, 4); a single tail body handles the
// remainder. Not in the default pipeline; no vectorization is claimed.
class LoopUnrollPass final : public FunctionPass {
 public:
  explicit LoopUnrollPass(UnrollProfile profile = {});
  PassResult run(Function& function, PassContext& context) override;
 private:
  UnrollProfile profile_;
};
}}  // namespace sysy::flat
#endif
