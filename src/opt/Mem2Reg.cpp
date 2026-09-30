// opt/Mem2Reg.cpp -- explicit-worklist Braun-style scalar promotion.
#include "opt/Mem2Reg.h"
#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/Mem2RegInternal.h"
namespace sysy {
namespace flat {
namespace {
using mem2reg_detail::CandidatePlan;
using mem2reg_detail::Reachability;
using mem2reg_detail::computeNeedIn;
using mem2reg_detail::containsInstruction;
using mem2reg_detail::findReachable;
using mem2reg_detail::prepareCandidate;

void enqueueSuccessors(const Function& function, BasicBlock* block,
                       const Reachability& reach, std::deque<size_t>& work,
                       std::vector<char>& queued) {
  Instruction* terminator = block->terminator();
  if (terminator == nullptr) return;
  for (size_t i = 0; i < terminator->numSuccs(); ++i) {
    BasicBlock* successor = terminator->succ(i);
    if (successor == nullptr || successor->parent() != &function ||
        !reach.reachable[successor->index()] || queued[successor->index()]) continue;
    queued[successor->index()] = 1;
    work.push_back(successor->index());
  }
}

Value* zeroValue(Module& module, const Type* type) {
  return type->isFloat() ? static_cast<Value*>(module.getFloatConst(0u))
                         : static_cast<Value*>(module.getIntConst(type, 0));
}
Value* canonicalValue(Value* value,
                      const std::unordered_map<Instruction*, Value*>& folded) {
  std::unordered_set<Instruction*> seen;
  while (value != nullptr && value->isInst()) {
    Instruction* instruction = static_cast<Instruction*>(value);
    const auto it = folded.find(instruction);
    if (it == folded.end() || it->second == value || !seen.insert(instruction).second) break;
    value = it->second;
  }
  return value;
}
struct SSAPlan {
  bool complete = false;
  std::vector<Instruction*> generatedPhis;
  std::vector<BasicBlock*> phiBlocks;
  std::unordered_map<Instruction*, Value*> foldedPhis;
  std::unordered_map<Instruction*, Value*> loadValues;
};
SSAPlan constructSSA(Module& module, const CandidatePlan& plan,
                     const Reachability& reach, const std::vector<char>& needIn) {
  const Function& function = *plan.function;
  const size_t count = function.blockCount();
  std::vector<Value*> blockIn(count, nullptr);
  std::vector<Value*> blockOut(count, nullptr);
  std::vector<Instruction*> phis(count, nullptr);
  std::vector<std::vector<BasicBlock*>> orderedPreds(count);
  std::deque<size_t> work;
  std::vector<char> queued(count, 0);
  for (size_t i = 0; i < count; ++i) {
    BasicBlock* block = function.at(i);
    orderedPreds[i] = block->preds();
    std::sort(orderedPreds[i].begin(), orderedPreds[i].end(),
              [](const BasicBlock* left, const BasicBlock* right) {
                return left->index() < right->index();
              });
    if (reach.reachable[i]) {
      work.push_back(i);
      queued[i] = 1;
    }
  }
  for (size_t index = 0; index < count; ++index) {
    BasicBlock* block = function.at(index);
    if (!reach.reachable[index] || !needIn[index] || block == function.entry()) continue;
    size_t reachablePreds = 0;
    for (BasicBlock* pred : orderedPreds[index]) {
      if (pred != nullptr && pred->parent() == &function && reach.reachable[pred->index()]) {
        ++reachablePreds;
      }
    }
    if (reachablePreds < 2) continue;
    std::vector<Value*> values(orderedPreds[index].size(), nullptr);
    Instruction* phi = createPhi(plan.objectType, values, orderedPreds[index],
                                  plan.allocaInst->loc());
    phis[index] = phi;
    blockIn[index] = phi;
  }
  std::unordered_map<Instruction*, Value*> resolvedLoads;
  auto resolveStoreValue = [&](Value* value) -> Value* {
    if (value == nullptr || !value->isInst()) return value;
    Instruction* instruction = static_cast<Instruction*>(value);
    if (!containsInstruction(plan.loads, instruction)) return value;
    const auto it = resolvedLoads.find(instruction);
    return it == resolvedLoads.end() ? nullptr : it->second;
  };
  while (!work.empty()) {
    const size_t index = work.front();
    work.pop_front();
    queued[index] = 0;
    BasicBlock* block = function.at(index);
    Value* newIn = nullptr;
    bool inputsReady = true;
    if (needIn[index] && block != function.entry()) {
      if (phis[index] != nullptr) {
        newIn = phis[index];
        for (size_t i = 0; i < orderedPreds[index].size(); ++i) {
          BasicBlock* pred = orderedPreds[index][i];
          Value* incoming = nullptr;
          if (pred != nullptr && pred->parent() == &function &&
              reach.reachable[pred->index()]) {
            incoming = blockOut[pred->index()];
          }
          phis[index]->setOperand(i, incoming);
        }
      } else {
        bool hasReachablePred = false;
        Value* first = nullptr;
        bool differs = false;
        for (size_t i = 0; i < block->numPreds(); ++i) {
          BasicBlock* pred = block->pred(i);
          if (pred == nullptr || pred->parent() != &function ||
              !reach.reachable[pred->index()]) continue;
          hasReachablePred = true;
          Value* incoming = blockOut[pred->index()];
          if (incoming == nullptr) {
            inputsReady = false;
            continue;
          }
          if (first == nullptr) first = incoming;
          else if (first != incoming) differs = true;
        }
        if (!hasReachablePred) inputsReady = false;
        if (inputsReady && first != nullptr) newIn = differs ? phis[index] : first;
      }
    }
    Value* newOut = nullptr;
    Value* current = newIn;
    bool hasStore = false;
    for (Instruction* instruction : block->insts()) {
      if (containsInstruction(plan.loads, instruction)) {
        if (current != nullptr) resolvedLoads[instruction] = current;
      }
      if (containsInstruction(plan.stores, instruction)) {
        hasStore = true;
        current = resolveStoreValue(instruction->operand(0));
      }
    }
    if (hasStore) {
      newOut = current;
    } else if (!needIn[index] || inputsReady || phis[index] != nullptr) {
      newOut = current;
    }
    const bool inChanged = blockIn[index] != newIn &&
                           (inputsReady || !needIn[index] || phis[index] != nullptr);
    const bool outChanged = blockOut[index] != newOut;
    if (inChanged) blockIn[index] = newIn;
    if (outChanged) {
      blockOut[index] = newOut;
      enqueueSuccessors(function, block, reach, work, queued);
    }
  }
  SSAPlan result;
  for (size_t index = 0; index < phis.size(); ++index) {
    if (phis[index] != nullptr) {
      result.generatedPhis.push_back(phis[index]);
      result.phiBlocks.push_back(function.at(index));
    }
  }
  for (Instruction* load : plan.loads) {
    const auto it = resolvedLoads.find(load);
    if (it == resolvedLoads.end() || it->second == nullptr) return result;
  }
  for (size_t i = 0; i < result.generatedPhis.size(); ++i) {
    Instruction* phi = result.generatedPhis[i];
    for (size_t j = 0; j < phi->numSuccs(); ++j) {
      BasicBlock* pred = phi->succ(j);
      if (pred != nullptr && pred->parent() == &function &&
          reach.reachable[pred->index()] && phi->operand(j) == nullptr) return result;
    }
  }
  // Delay constant interning until every reachable definition is resolved.
  // A declined candidate must not leave even an otherwise-unused module value.
  Value* zero = nullptr;
  auto getZero = [&]() -> Value* {
    if (zero == nullptr) zero = zeroValue(module, plan.objectType);
    return zero;
  };
  for (Instruction* phi : result.generatedPhis) {
    for (size_t i = 0; i < phi->numSuccs(); ++i) {
      BasicBlock* pred = phi->succ(i);
      if (phi->operand(i) == nullptr &&
          (pred == nullptr || pred->parent() != &function ||
           !reach.reachable[pred->index()])) {
        phi->setOperand(i, getZero());
      }
    }
  }
  result.complete = true;
  for (size_t pass = 0; pass <= result.generatedPhis.size(); ++pass) {
    bool changed = false;
    for (Instruction* phi : result.generatedPhis) {
      if (result.foldedPhis.find(phi) != result.foldedPhis.end()) continue;
      Value* first = nullptr;
      bool differs = false;
      for (size_t i = 0; i < phi->numOperands(); ++i) {
        Value* value = canonicalValue(phi->operand(i), result.foldedPhis);
        if (value == phi) continue;
        if (first == nullptr) {
          first = value;
        } else if (first != value) {
          differs = true;
          break;
        }
      }
      if (differs) continue;
      result.foldedPhis[phi] = first != nullptr ? first : getZero();
      changed = true;
    }
    if (!changed) break;
  }
  for (Instruction* phi : result.generatedPhis) {
    for (size_t i = 0; i < phi->numOperands(); ++i) {
      Value* replacement = canonicalValue(phi->operand(i), result.foldedPhis);
      if (replacement != nullptr) phi->setOperand(i, replacement);
    }
  }
  for (Instruction* instruction : plan.loads) {
    const auto it = resolvedLoads.find(instruction);
    if (it != resolvedLoads.end()) {
      result.loadValues[instruction] = canonicalValue(it->second, result.foldedPhis);
    }
  }
  return result;
}
void discardSSA(SSAPlan& ssa) {
  for (Instruction* phi : ssa.generatedPhis) {
    if (phi->parent() != nullptr) phi->parent()->removeInst(phi);
    phi->clearOperands();
    phi->clearSuccs();
    delete phi;
  }
  ssa.generatedPhis.clear();
  ssa.phiBlocks.clear();
}
void materializeSSA(Module& module, SSAPlan& ssa) {
  for (size_t i = 0; i < ssa.generatedPhis.size(); ++i) {
    Instruction* phi = ssa.generatedPhis[i];
    BasicBlock* block = ssa.phiBlocks[i];
    module.ownInst(phi);
    size_t position = 0;
    while (position < block->size() && block->at(position)->op() == Opcode::Phi) ++position;
    block->insertInst(position, phi);
  }
}
void replaceUses(Module& module, const SSAPlan& ssa) {
  for (Function* function : module.functions()) {
    if (function == nullptr || function->isDeclaration()) continue;
    for (BasicBlock* block : function->blocks()) {
      for (Instruction* instruction : block->insts()) {
        for (size_t i = 0; i < instruction->numOperands(); ++i) {
          Value* operand = instruction->operand(i);
          if (operand != nullptr && operand->isInst()) {
            Instruction* asInstruction = static_cast<Instruction*>(operand);
            const auto loadIt = ssa.loadValues.find(asInstruction);
            if (loadIt != ssa.loadValues.end()) {
              instruction->setOperand(i, loadIt->second);
              continue;
            }
            const auto phiIt = ssa.foldedPhis.find(asInstruction);
            if (phiIt != ssa.foldedPhis.end()) {
              Value* replacement = canonicalValue(phiIt->second, ssa.foldedPhis);
              if (replacement != nullptr) instruction->setOperand(i, replacement);
            }
          }
        }
      }
    }
  }
}
void removePromotedInstructions(const CandidatePlan& plan, const SSAPlan& ssa,
                                Mem2RegResult& result) {
  for (Instruction* load : plan.loads) {
    if (load->parent() != nullptr) load->parent()->removeInst(load);
    load->clearOperands();
    ++result.removedLoads;
  }
  for (Instruction* store : plan.stores) {
    if (store->parent() != nullptr) store->parent()->removeInst(store);
    store->clearOperands();
    ++result.removedStores;
  }
  if (plan.allocaInst->parent() != nullptr) plan.allocaInst->parent()->removeInst(plan.allocaInst);
  plan.allocaInst->clearOperands();
  for (const auto& item : ssa.foldedPhis) {
    Instruction* phi = item.first;
    if (phi->parent() != nullptr) phi->parent()->removeInst(phi);
    phi->clearOperands();
    phi->clearSuccs();
    ++result.eliminatedPhis;
  }
  result.generatedPhis += ssa.generatedPhis.size();
}
}  // namespace
const char* mem2RegSkipReasonName(Mem2RegSkipReason reason) {
  switch (reason) {
    case Mem2RegSkipReason::NotEntryAlloca: return "not-entry-alloca";
    case Mem2RegSkipReason::UnsupportedObjectType: return "unsupported-object-type";
    case Mem2RegSkipReason::EscapedOrDerivedUse: return "escaped-or-derived-use";
    case Mem2RegSkipReason::WrongUseType: return "wrong-use-type";
    case Mem2RegSkipReason::UnreachableUse: return "unreachable-use";
    case Mem2RegSkipReason::ReadBeforeDefinition: return "read-before-definition";
    case Mem2RegSkipReason::UnresolvedDefinition: return "unresolved-definition";
    case Mem2RegSkipReason::Count: break;
  }
  return "?";
}
Mem2RegResult runMem2Reg(Module& module, const Mem2RegOptions& options) {
  Mem2RegResult result;
  if (options.captureDumps) result.beforeDump = dumpModule(module);

  const std::vector<Violation> violations = verifyFlatModule(module);
  if (!violations.empty()) {
    result.status = Mem2RegStatus::InvalidInput;
    result.diagnostic = formatViolations(violations);
    result.inputValid = false;
    result.success = false;
    if (options.captureDumps) result.afterDump = result.beforeDump;
    return result;
  }
  result.inputValid = true;
  result.success = true;
  if (!options.enabled) {
    result.status = Mem2RegStatus::Disabled;
    if (options.captureDumps) result.afterDump = result.beforeDump;
    return result;
  }

  std::vector<std::pair<Function*, Instruction*>> allocas;
  for (Function* function : module.functions()) {
    if (function == nullptr || function->isDeclaration()) continue;
    for (BasicBlock* block : function->blocks()) {
      for (Instruction* instruction : block->insts()) {
        if (instruction->op() == Opcode::Alloca) allocas.emplace_back(function, instruction);
      }
    }
  }

  for (const auto& item : allocas) {
    Function* function = item.first;
    Instruction* allocaInst = item.second;
    CandidatePlan plan;
    Mem2RegSkipReason reason = Mem2RegSkipReason::NotEntryAlloca;
    if (allocaInst->parent() != nullptr &&
        prepareCandidate(function, allocaInst, plan, reason)) {
      const Reachability reach = findReachable(*function);
      const std::vector<char> needIn = computeNeedIn(plan, reach);
      SSAPlan ssa = constructSSA(module, plan, reach, needIn);
      if (!ssa.complete) {
        discardSSA(ssa);
        result.candidates.push_back(
            Mem2RegCandidate{function->name(), allocaInst->loc(), false,
                             Mem2RegSkipReason::UnresolvedDefinition});
        module.rebuildUseDef();
        continue;
      }
      materializeSSA(module, ssa);
      replaceUses(module, ssa);
      removePromotedInstructions(plan, ssa, result);
      ++result.promotedAllocas;
      result.candidates.push_back(
          Mem2RegCandidate{function->name(), allocaInst->loc(), true,
                           Mem2RegSkipReason::Count});
      module.rebuildUseDef();
      continue;
    }
    result.candidates.push_back(
        Mem2RegCandidate{function->name(), allocaInst->loc(), false, reason});
  }

  module.rebuildCFG();
  module.rebuildUseDef();
  result.changed = result.promotedAllocas != 0;
  result.status = result.changed ? Mem2RegStatus::Changed : Mem2RegStatus::NoChange;
  if (options.captureDumps) result.afterDump = dumpModule(module);
  return result;
}

}  // namespace flat
}  // namespace sysy
