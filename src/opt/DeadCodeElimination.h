// S17: conservative DCE for unused pure instructions.
#ifndef SYSY_OPT_DEADCODEELIMINATION_H
#define SYSY_OPT_DEADCODEELIMINATION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class DeadCodeEliminationPass final : public FunctionPass {
 public:
  DeadCodeEliminationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
