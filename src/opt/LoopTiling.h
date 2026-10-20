// S24b: order-preserving strip-mining of a canonical loop, sized by TilePlan.
// One positive-step i32 recurrence, no loop-defined live-outs or exit phis.
// T counts iterations; unsupported arithmetic/shapes decline before mutation.
// Transforms at most one loop per invocation and invalidates CFG/DataFlow
// analyses. Unexpected editor failure aborts the pipeline (no rollback).
#ifndef SYSY_OPT_LOOPTILING_H
#define SYSY_OPT_LOOPTILING_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class LoopTilingPass final : public FunctionPass {
 public:
  LoopTilingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
