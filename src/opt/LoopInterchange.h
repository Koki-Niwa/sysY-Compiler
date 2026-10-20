// S23b: canonical two-level loop interchange (load-only bodies).
#ifndef SYSY_OPT_LOOPINTERCHANGE_H
#define SYSY_OPT_LOOPINTERCHANGE_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class LoopInterchangePass final : public FunctionPass {
 public:
  LoopInterchangePass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
