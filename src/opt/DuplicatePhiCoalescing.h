// Merge structurally identical phis in the same block. Runs only for opt-in
// s24/s25 pipelines after mem2reg, whose generated duplicate induction phis
// otherwise prevent loop matchers from seeing a canonical header.
#ifndef SYSY_OPT_DUPLICATEPHICOALESCING_H
#define SYSY_OPT_DUPLICATEPHICOALESCING_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class DuplicatePhiCoalescingPass final : public FunctionPass {
 public:
  DuplicatePhiCoalescingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
