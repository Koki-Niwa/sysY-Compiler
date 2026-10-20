// S25b: pair-unrolling for a canonical even-trip loop.
#ifndef SYSY_OPT_LOOPUNROLL_H
#define SYSY_OPT_LOOPUNROLL_H
#include "analysis/UnrollPlan.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
// One i32 induction recurrence, one body block, no loop-defined live-outs.
// The body is cloned once and the loop advances by two steps; odd trips are
// declined so no tail loop is needed. Not in the default pipeline.
class LoopUnrollPass final : public FunctionPass {
 public:
  explicit LoopUnrollPass(UnrollProfile profile = {});
  PassResult run(Function& function, PassContext& context) override;
 private:
  UnrollProfile profile_;
};
}}  // namespace sysy::flat
#endif
