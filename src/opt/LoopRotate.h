// S21b: rotate a phi-free while loop into a guarded do-while.
#ifndef SYSY_OPT_LOOPROTATE_H
#define SYSY_OPT_LOOPROTATE_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class LoopRotatePass final : public FunctionPass {
 public:
  LoopRotatePass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
