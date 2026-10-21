// S24d: block i/j of a canonical 2-D convolution with nested ki/kj reduction
// loops, retaining both K loops and each output's reduction order.
#ifndef SYSY_OPT_CONVBLOCKING_H
#define SYSY_OPT_CONVBLOCKING_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class ConvBlockingPass : public FunctionPass {
 public:
  ConvBlockingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
