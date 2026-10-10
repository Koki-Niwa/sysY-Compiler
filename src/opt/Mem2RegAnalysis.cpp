// Candidate eligibility, reachability, and definite-initialization analysis.
#include "opt/Mem2RegInternal.h"

#include <deque>
#include <unordered_set>

namespace sysy {
namespace flat {
namespace mem2reg_detail {
namespace {

bool isCandidateObjectType(const Type* type) {
  return type != nullptr && (type->isInt() || type->isFloat());
}

bool isDirectUse(const Instruction* user, const Instruction* allocaInst,
                 size_t& occurrences) {
  occurrences = 0;
  if (user == nullptr || allocaInst == nullptr) return false;
  for (size_t i = 0; i < user->numOperands(); ++i) {
    if (user->operand(i) == allocaInst) ++occurrences;
  }
  return occurrences == 1;
}

bool analyzeUses(const CandidatePlan& seed, CandidatePlan& plan,
                 Mem2RegSkipReason& reason) {
  plan = seed;
  Instruction* allocaInst = plan.allocaInst;
  if (allocaInst == nullptr || plan.function == nullptr) return false;
  std::unordered_set<Instruction*> seen;
  for (const Use& use : allocaInst->uses()) {
    Instruction* user = use.user;
    if (user == nullptr || !seen.insert(user).second) continue;
    if (user->parent() == nullptr || user->parent()->parent() != plan.function) {
      reason = Mem2RegSkipReason::EscapedOrDerivedUse;
      return false;
    }
    size_t occurrences = 0;
    if (!isDirectUse(user, allocaInst, occurrences)) {
      reason = Mem2RegSkipReason::EscapedOrDerivedUse;
      return false;
    }
    if (user->op() == Opcode::Load) {
      const bool correctType = user->numOperands() == 1 && user->operand(0) == allocaInst &&
                               user->type() == plan.objectType &&
                               allocaInst->type() == typePool().ptrTo(plan.objectType);
      if (!correctType) {
        reason = Mem2RegSkipReason::WrongUseType;
        return false;
      }
      plan.loads.push_back(user);
      continue;
    }
    if (user->op() == Opcode::Store) {
      const bool correctType = user->numOperands() == 2 && user->operand(1) == allocaInst &&
                               user->operand(0) != nullptr &&
                               user->operand(0)->type() == plan.objectType &&
                               user->srcElemType() == plan.objectType &&
                               allocaInst->type() == typePool().ptrTo(plan.objectType);
      if (!correctType) {
        reason = Mem2RegSkipReason::WrongUseType;
        return false;
      }
      plan.stores.push_back(user);
      continue;
    }
    reason = Mem2RegSkipReason::EscapedOrDerivedUse;
    return false;
  }
  return true;
}

bool analyzeInitialization(const CandidatePlan& plan, const Reachability& reach) {
  const Function& function = *plan.function;
  const size_t count = function.blockCount();
  std::vector<char> in(count, 1);
  std::vector<char> out(count, 1);
  std::deque<size_t> work;
  std::vector<char> queued(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (reach.bits()[i]) {
      work.push_back(i);
      queued[i] = 1;
    } else {
      in[i] = 0;
      out[i] = 0;
    }
  }
  in[function.entry()->index()] = 0;
  while (!work.empty()) {
    const size_t index = work.front();
    work.pop_front();
    queued[index] = 0;
    BasicBlock* block = function.at(index);
    bool newIn = false;
    if (block != function.entry()) {
      bool hasReachablePred = false;
      newIn = true;
      for (size_t i = 0; i < block->numPreds(); ++i) {
        BasicBlock* pred = block->pred(i);
        if (pred == nullptr || pred->parent() != &function ||
            !reach.bits()[pred->index()]) continue;
        hasReachablePred = true;
        newIn = newIn && out[pred->index()];
      }
      if (!hasReachablePred) newIn = false;
    }
    bool newOut = newIn;
    for (Instruction* instruction : block->insts()) {
      if (containsInstruction(plan.stores, instruction)) newOut = true;
    }
    const bool changed = in[index] != static_cast<char>(newIn) ||
                         out[index] != static_cast<char>(newOut);
    in[index] = static_cast<char>(newIn);
    out[index] = static_cast<char>(newOut);
    if (!changed) continue;
    Instruction* terminator = block->terminator();
    if (terminator == nullptr) continue;
    for (size_t i = 0; i < terminator->numSuccs(); ++i) {
      BasicBlock* successor = terminator->succ(i);
      if (successor == nullptr || successor->parent() != &function ||
          !reach.bits()[successor->index()] || queued[successor->index()]) continue;
      queued[successor->index()] = 1;
      work.push_back(successor->index());
    }
  }
  for (size_t index = 0; index < count; ++index) {
    if (!reach.bits()[index]) continue;
    BasicBlock* block = function.at(index);
    bool initialized = index == function.entry()->index() ? false : in[index];
    for (Instruction* instruction : block->insts()) {
      if (containsInstruction(plan.loads, instruction) && !initialized) return false;
      if (containsInstruction(plan.stores, instruction)) initialized = true;
    }
  }
  return true;
}

}  // namespace

std::vector<char> computeNeedIn(const CandidatePlan& plan,
                                const Reachability& reach) {
  const Function& function = *plan.function;
  const size_t count = function.blockCount();
  std::vector<char> hasStore(count, 0);
  std::vector<char> localRead(count, 0);
  for (size_t index = 0; index < count; ++index) {
    if (!reach.bits()[index]) continue;
    bool sawStore = false;
    for (Instruction* instruction : function.at(index)->insts()) {
      if (containsInstruction(plan.loads, instruction) && !sawStore) localRead[index] = 1;
      if (containsInstruction(plan.stores, instruction)) {
        sawStore = true;
        hasStore[index] = 1;
      }
    }
  }
  std::vector<char> need = localRead;
  std::deque<size_t> work;
  for (size_t index = 0; index < count; ++index) {
    if (reach.bits()[index] && localRead[index]) work.push_back(index);
  }
  while (!work.empty()) {
    BasicBlock* block = function.at(work.front());
    work.pop_front();
    for (size_t i = 0; i < block->numPreds(); ++i) {
      BasicBlock* pred = block->pred(i);
      if (pred == nullptr || pred->parent() != &function ||
          !reach.bits()[pred->index()] || hasStore[pred->index()] ||
          need[pred->index()]) continue;
      need[pred->index()] = 1;
      work.push_back(pred->index());
    }
  }
  return need;
}

bool prepareCandidate(Function* function, Instruction* allocaInst,
                      const Reachability& reach, CandidatePlan& plan,
                      Mem2RegSkipReason& reason) {
  if (function == nullptr || allocaInst == nullptr || function->entry() == nullptr ||
      allocaInst->parent() != function->entry()) {
    reason = Mem2RegSkipReason::NotEntryAlloca;
    return false;
  }
  const Type* pointerType = allocaInst->type();
  const Type* objectType = pointerType != nullptr && pointerType->isPtr()
                               ? pointerType->elem : nullptr;
  if (!isCandidateObjectType(objectType) || allocaInst->srcElemType() != objectType) {
    reason = Mem2RegSkipReason::UnsupportedObjectType;
    return false;
  }
  CandidatePlan seed;
  seed.function = function;
  seed.allocaInst = allocaInst;
  seed.objectType = objectType;
  if (!analyzeUses(seed, plan, reason)) return false;
  for (Instruction* access : plan.loads) {
    if (!reach.bits()[access->parent()->index()]) {
      reason = Mem2RegSkipReason::UnreachableUse;
      return false;
    }
  }
  for (Instruction* access : plan.stores) {
    if (!reach.bits()[access->parent()->index()]) {
      reason = Mem2RegSkipReason::UnreachableUse;
      return false;
    }
  }
  if (!analyzeInitialization(plan, reach)) {
    reason = Mem2RegSkipReason::ReadBeforeDefinition;
    return false;
  }
  return true;
}

}  // namespace mem2reg_detail
}  // namespace flat
}  // namespace sysy
