#ifndef SYSY_OPT_MATMULBLOCKINGINTERNAL_H
#define SYSY_OPT_MATMULBLOCKINGINTERNAL_H
#include <cstdint>
#include "opt/MatmulBlocking.h"
namespace sysy { namespace flat {
// Read-only proof result. All skipped blocks are empty trampolines or exact
// induction latches; retained instructions form a closed single-entry region.
struct MatmulNest {
  BasicBlock* pre = nullptr;
  BasicBlock* reduction = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* reductionLatch = nullptr;
  BasicBlock* reductionExit = nullptr;
  BasicBlock* exit = nullptr;
  Instruction* outerPhi = nullptr;
  Instruction* innerPhi = nullptr;
  int64_t outerBound = 0;
  int64_t innerBound = 0;
  int64_t reductionBound = 0;
};
bool matchMatmul(const Function& function, MatmulNest& nest);
}}  // namespace sysy::flat
#endif
