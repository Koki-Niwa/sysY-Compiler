// S19: dominator-scoped global value numbering for pure instructions.
#ifndef SYSY_OPT_GVN_H
#define SYSY_OPT_GVN_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class GVNPass final : public FunctionPass {
 public:
  GVNPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
