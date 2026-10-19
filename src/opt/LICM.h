// S20: loop-invariant code motion for pure instructions.
#ifndef SYSY_OPT_LICM_H
#define SYSY_OPT_LICM_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class LICMPass final : public FunctionPass {
 public:
  LICMPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
