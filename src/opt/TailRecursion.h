// S22b: direct-self tail recursion to a memory-carried loop (single block).
#ifndef SYSY_OPT_TAILRECURSION_H
#define SYSY_OPT_TAILRECURSION_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class TailRecursionEliminationPass final : public FunctionPass {
 public:
  TailRecursionEliminationPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
