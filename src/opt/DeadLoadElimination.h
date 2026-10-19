// S19: remove loads whose result is unused.
#ifndef SYSY_OPT_DEADLOADELIMINATION_H
#define SYSY_OPT_DEADLOADELIMINATION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class DeadLoadEliminationPass final : public FunctionPass {
 public:
  DeadLoadEliminationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
