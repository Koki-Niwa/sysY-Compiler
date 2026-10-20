// Fold zero-index GEPs of scalar allocas to the alloca pointer so the existing
// conservative mem2reg can see direct accesses. Used only by opt-in s24/s25.
#ifndef SYSY_OPT_ALLOCAGEPCANONICALIZATION_H
#define SYSY_OPT_ALLOCAGEPCANONICALIZATION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class AllocaGEPCanonicalizationPass final : public FunctionPass {
 public:
  AllocaGEPCanonicalizationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
