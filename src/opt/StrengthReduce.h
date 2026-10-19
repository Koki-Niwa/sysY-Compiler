// S20: strength reduction for power-of-two signed division and multiply.
#ifndef SYSY_OPT_STRENGTHREDUCE_H
#define SYSY_OPT_STRENGTHREDUCE_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class StrengthReducePass final : public FunctionPass {
 public:
  StrengthReducePass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
