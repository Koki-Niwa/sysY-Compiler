// S17: fold constant conditional branches and duplicate targets.
#ifndef SYSY_OPT_SIMPLIFYCFG_H
#define SYSY_OPT_SIMPLIFYCFG_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class SimplifyCFGPass final : public FunctionPass {
 public:
  SimplifyCFGPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
