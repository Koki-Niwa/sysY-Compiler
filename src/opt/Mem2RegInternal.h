// Internal state shared by mem2reg candidate analysis and SSA construction.
#ifndef SYSY_OPT_MEM2REGINTERNAL_H
#define SYSY_OPT_MEM2REGINTERNAL_H

#include <algorithm>
#include <vector>

#include "ir/Module.h"
#include "opt/Mem2Reg.h"

namespace sysy {
namespace flat {
namespace mem2reg_detail {

struct CandidatePlan {
  Function* function = nullptr;
  Instruction* allocaInst = nullptr;
  const Type* objectType = nullptr;
  std::vector<Instruction*> loads;
  std::vector<Instruction*> stores;
};

struct Reachability {
  std::vector<char> reachable;
};

inline bool containsInstruction(const std::vector<Instruction*>& values,
                                Instruction* needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

Reachability findReachable(const Function& function);
std::vector<char> computeNeedIn(const CandidatePlan& plan,
                                const Reachability& reach);
bool prepareCandidate(Function* function, Instruction* allocaInst,
                      CandidatePlan& plan, Mem2RegSkipReason& reason);

}  // namespace mem2reg_detail
}  // namespace flat
}  // namespace sysy

#endif  // SYSY_OPT_MEM2REGINTERNAL_H
