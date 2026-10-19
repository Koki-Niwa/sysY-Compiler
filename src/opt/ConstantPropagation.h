// S17: fold pure instructions whose operands are canonical constants.
#ifndef SYSY_OPT_CONSTANTPROPAGATION_H
#define SYSY_OPT_CONSTANTPROPAGATION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class ConstantPropagationPass final : public FunctionPass {
 public:
  ConstantPropagationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
