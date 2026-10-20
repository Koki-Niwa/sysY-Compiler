// S24c-b: 2D blocked order for an integer-add reduction nest.
#ifndef SYSY_OPT_LOOPBLOCKING_H
#define SYSY_OPT_LOOPBLOCKING_H
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
// Matches a canonical two-level nest whose body is a pure integer reduction
// `g = g + f(i,j)` with one load/store of a scalar global. Addition modulo
// i32 is associative/commutative, so blocked reordering preserves the final
// value. Not in the default pipeline; no runtime measurement is claimed.
class LoopBlockingPass final : public FunctionPass {
 public:
  LoopBlockingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
