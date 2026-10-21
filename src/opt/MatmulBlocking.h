// S24c-c: recognize a canonical three-level matmul-style accumulation nest.
// Matching is side-effect free; the transform lands separately.
#ifndef SYSY_OPT_MATMULBLOCKING_H
#define SYSY_OPT_MATMULBLOCKING_H
#include <cstddef>
#include <optional>
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class Function;
struct MatmulCandidate {
  size_t outerBound = 0;
  size_t innerBound = 0;
  size_t reductionBound = 0;
};
// Returns nullopt unless O/J/K are canonical positive constant-trip loops,
// K has a local accumulator, and C[i][j] is the only canonical output store.
std::optional<MatmulCandidate> matchMatmulNest(const Function& function);

class MatmulBlockingPass : public FunctionPass {
 public:
  MatmulBlockingPass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
