// S19: conservative intra-block dead store elimination.
#ifndef SYSY_OPT_DEADSTOREELIMINATION_H
#define SYSY_OPT_DEADSTOREELIMINATION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class DeadStoreEliminationPass final : public FunctionPass {
 public:
  DeadStoreEliminationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
