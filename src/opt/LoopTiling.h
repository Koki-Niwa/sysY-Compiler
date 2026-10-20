// S24b: order-preserving strip-mining of a canonical loop, sized by TilePlan.
#ifndef SYSY_OPT_LOOPTILING_H
#define SYSY_OPT_LOOPTILING_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class LoopTilingPass final : public FunctionPass {
 public:
  LoopTilingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
