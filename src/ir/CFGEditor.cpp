#include "ir/CFGEditor.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include "ir/FlatSemantics.h"

namespace sysy { namespace flat {
namespace {
using Pair = std::pair<BasicBlock*, Value*>;
using Incoming = std::vector<Pair>;

Incoming incoming(const Instruction* phi) {
  Incoming result;
  for (size_t i = 0; i < phi->numOperands(); ++i)
    result.emplace_back(phi->succ(i), phi->operand(i));
  return result;
}
void writeIncoming(Instruction* phi, Incoming pairs) {
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
    return a.first->index() < b.first->index();
  });
  phi->clearOperands();
  phi->clearSuccs();
  for (const Pair& item : pairs) {
    phi->addOperand(item.second);
    phi->addSucc(item.first);
  }
}
bool sortedUnique(const std::vector<BasicBlock*>& blocks) {
  for (size_t i = 1; i < blocks.size(); ++i)
    if (blocks[i - 1] == nullptr || blocks[i] == nullptr ||
        blocks[i - 1]->index() >= blocks[i]->index()) return false;
  return blocks.empty() || blocks.front() != nullptr;
}
} // namespace

bool CFGEditor::readPhis(BasicBlock* block, std::vector<Instruction*>& phis) const {
  if (!mutator_.isLiveBlock(block) || !sortedUnique(block->preds())) return false;
  bool pastPhi = false;
  for (Instruction* inst : block->insts()) {
    if (inst->op() != Opcode::Phi) { pastPhi = true; continue; }
    if (pastPhi || !mutator_.isLive(inst) ||
        inst->numOperands() != block->numPreds() ||
        inst->numSuccs() != block->numPreds()) return false;
    for (size_t i = 0; i < block->numPreds(); ++i) {
      if (inst->succ(i) != block->pred(i) || inst->operand(i) == nullptr ||
          inst->operand(i)->type() != inst->type() ||
          !mutator_.valueAvailableInFunction(inst->operand(i), block->parent()))
        return false;
      const Value* v = inst->operand(i);
      if (std::count_if(v->uses().begin(), v->uses().end(),
          [=](const Use& use) {
            return use.user == inst && use.operandIndex == i;
          }) != 1) return false;
    }
    phis.push_back(inst);
  }
  return true;
}
bool CFGEditor::hasSuccessor(BasicBlock* from, BasicBlock* target,
                              size_t excludeIndex) const {
  Instruction* term = from->terminator();
  if (term == nullptr) return false;
  for (size_t i = 0; i < term->numSuccs(); ++i)
    if (i != excludeIndex && term->succ(i) == target) return true;
  return false;
}
void CFGEditor::updatePreds(BasicBlock* block, BasicBlock* remove, BasicBlock* add) {
  std::vector<BasicBlock*> list = block->preds();
  if (remove != nullptr)
    list.erase(std::remove(list.begin(), list.end(), remove), list.end());
  if (add != nullptr && std::find(list.begin(), list.end(), add) == list.end())
    list.push_back(add);
  std::sort(list.begin(), list.end(), [](const BasicBlock* a, const BasicBlock* b) {
    return a->index() < b->index();
  });
  block->clearPreds();
  for (BasicBlock* pred : list) block->addPred(pred);
}

MutationResult CFGEditor::rewritePhiValue(Instruction* phi, BasicBlock* pred,
                                          Value* value) {
  if (phi == nullptr || phi->op() != Opcode::Phi ||
      !mutator_.isLive(phi) || pred == nullptr) return MutationResult::Rejected;
  std::vector<Instruction*> phis;
  if (!readPhis(phi->parent(), phis)) return MutationResult::Rejected;
  const auto& preds = phi->parent()->preds();
  const auto it = std::find(preds.begin(), preds.end(), pred);
  if (it == preds.end() || value == nullptr || value->type() != phi->type())
    return MutationResult::Rejected;
  return mutator_.rewriteOperand(phi, static_cast<size_t>(it - preds.begin()), value);
}

MutationResult CFGEditor::insertPhi(BasicBlock* block, Instruction* phi) {
  return insertPhiBatch({{block, phi}});
}
MutationResult CFGEditor::insertPhiBatch(
    const std::vector<std::pair<BasicBlock*, Instruction*>>& items) {
  if (items.empty()) return MutationResult::NoChange;
  Module& module = mutator_.module();
  std::unordered_map<Instruction*, Function*> planned;
  for (const auto& item : items) {
    if (!mutator_.isLiveBlock(item.first) || item.second == nullptr ||
        !planned.emplace(item.second, item.first->parent()).second)
      return MutationResult::Rejected;
  }
  std::vector<Function*> affected;
  for (const auto& item : items) {
    BasicBlock* block = item.first;
    Instruction* phi = item.second;
    std::vector<Instruction*> current;
    if (!readPhis(block, current) || block == block->parent()->entry() ||
        phi->op() != Opcode::Phi ||
        phi->parent() != nullptr ||
        phi->lifecycle() != InstructionLifecycle::Detached ||
        phi->numOperands() != block->numPreds() ||
        phi->numSuccs() != block->numPreds() ||
        phi->ownerModule() != nullptr ||
        module.ownsInstruction(phi)) return MutationResult::Rejected;
    std::string error;
    if (!validateFlatInstruction(*phi, error)) return MutationResult::Rejected;
    size_t selfIncoming = 0;
    for (size_t i = 0; i < phi->numOperands(); ++i) {
      Value* value = phi->operand(i);
      if (value == phi) ++selfIncoming;
      if (phi->succ(i) != block->pred(i) || value == nullptr ||
          value->type() != phi->type()) return MutationResult::Rejected;
      if (value->isInst() && planned.count(static_cast<Instruction*>(value))) {
        if (planned.at(static_cast<Instruction*>(value)) != block->parent())
          return MutationResult::Rejected;
      } else if (!mutator_.valueAvailableInFunction(value, block->parent())) {
        return MutationResult::Rejected;
      }
      if (std::count_if(value->uses().begin(), value->uses().end(),
          [=](const Use& use) { return use.user == phi && use.operandIndex == i; }) != 1)
        return MutationResult::Rejected;
    }
    if (selfIncoming == phi->numOperands()) return MutationResult::Rejected;
    affected.push_back(block->parent());
  }
  for (const auto& item : items) {
    BasicBlock* block = item.first;
    Instruction* phi = item.second;
    // All ownership and provenance checks completed for the entire batch.
    if (!module.ownInst(phi)) return MutationResult::Rejected;
    size_t index = 0;
    while (index < block->size() && block->at(index)->op() == Opcode::Phi) ++index;
    block->insertInst(index, phi);
  }
  mutator_.commit(MutationKind::DataFlow | MutationKind::CFG, affected);
  return MutationResult::Applied;
}

MutationResult CFGEditor::erasePhi(Instruction* phi) {
  if (phi == nullptr || phi->op() != Opcode::Phi ||
      !mutator_.isLive(phi) || phi->hasUses()) return MutationResult::Rejected;
  std::vector<Instruction*> phis;
  if (!readPhis(phi->parent(), phis)) return MutationResult::Rejected;
  Function* function = phi->parent()->parent();
  if (!mutator_.rawErase(phi)) return MutationResult::Rejected;
  mutator_.commit(MutationKind::DataFlow | MutationKind::CFG, {function});
  return MutationResult::Applied;
}

MutationResult CFGEditor::redirectEdge(BasicBlock* from, size_t index,
    BasicBlock* target, const std::vector<Value*>& newValues) {
  if (!mutator_.isLiveBlock(from) || !mutator_.isLiveBlock(target) ||
      from->parent() != target->parent()) return MutationResult::Rejected;
  Instruction* term = from->terminator();
  if (term == nullptr || !mutator_.isLive(term) ||
      term->op() != Opcode::Br || index >= term->numSuccs())
    return MutationResult::Rejected;
  BasicBlock* old = term->succ(index);
  if (!mutator_.isLiveBlock(old) || old->parent() != from->parent())
    return MutationResult::Rejected;
  if (old == target) return MutationResult::NoChange;
  const bool removeOld = !hasSuccessor(from, old, index);
  const bool addTarget = !hasSuccessor(from, target, index);
  // Before the edit the successor cache must describe the source exactly.
  if (std::find(old->preds().begin(), old->preds().end(), from) == old->preds().end())
    return MutationResult::Rejected;
  if ((std::find(target->preds().begin(), target->preds().end(), from) ==
       target->preds().end()) != addTarget) return MutationResult::Rejected;
  std::vector<Instruction*> oldPhis, newPhis;
  if (!readPhis(old, oldPhis) || !readPhis(target, newPhis))
    return MutationResult::Rejected;
  if ((addTarget && newValues.size() != newPhis.size()) ||
      (!addTarget && !newValues.empty()) ||
      (removeOld && old->numPreds() == 1 && !oldPhis.empty()))
    return MutationResult::Rejected;
  for (size_t i = 0; i < newValues.size(); ++i)
    if (newValues[i] == nullptr || newValues[i]->type() != newPhis[i]->type() ||
        !mutator_.valueAvailableInFunction(newValues[i], from->parent()))
      return MutationResult::Rejected;
  for (Instruction* phi : oldPhis) {
    if (removeOld && std::count(phi->succs().begin(), phi->succs().end(), from) != 1)
      return MutationResult::Rejected;
  }
  std::vector<Incoming> oldIncoming, newIncoming;
  for (Instruction* phi : oldPhis) {
    Incoming pairs = incoming(phi);
    if (removeOld)
      pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
          [=](const Pair& item) { return item.first == from; }), pairs.end());
    oldIncoming.push_back(std::move(pairs));
  }
  for (size_t i = 0; i < newPhis.size(); ++i) {
    Incoming pairs = incoming(newPhis[i]);
    if (addTarget) pairs.emplace_back(from, newValues[i]);
    newIncoming.push_back(std::move(pairs));
  }
  term->setSucc(index, target);
  if (removeOld) updatePreds(old, from, nullptr);
  if (addTarget) updatePreds(target, nullptr, from);
  for (size_t i = 0; i < oldPhis.size(); ++i) if (removeOld)
    writeIncoming(oldPhis[i], std::move(oldIncoming[i]));
  for (size_t i = 0; i < newPhis.size(); ++i) if (addTarget)
    writeIncoming(newPhis[i], std::move(newIncoming[i]));
  MutationKind kinds = MutationKind::CFG;
  if ((!oldPhis.empty() && removeOld) || (!newPhis.empty() && addTarget))
    kinds |= MutationKind::DataFlow;
  mutator_.commit(kinds, {from->parent()});
  return MutationResult::Applied;
}

MutationResult CFGEditor::replaceTerminator(BasicBlock* block,
                                               Instruction* newTerminator) {
  if (block == nullptr || newTerminator == nullptr ||
      !mutator_.isLiveBlock(block) || newTerminator->parent() != nullptr ||
      newTerminator->lifecycle() != InstructionLifecycle::Detached ||
      !isTerminatorOpcode(newTerminator->op()))
    return MutationResult::Rejected;
  Instruction* old = block->terminator();
  Function* function = block->parent();
  if (old == nullptr || function == nullptr) return MutationResult::Rejected;
  auto hasEdge = [](const Value* value, const Instruction* user, size_t index) {
    if (value == nullptr) return false;
    for (const Use& use : value->uses())
      if (use.user == user && use.operandIndex == index) return true;
    return false;
  };
  for (size_t i = 0; i < newTerminator->numOperands(); ++i)
    if (!mutator_.valueAvailableInFunction(newTerminator->operand(i), function) ||
        !hasEdge(newTerminator->operand(i), newTerminator, i))
      return MutationResult::Rejected;
  // Existing targets may lose an edge; a new target is allowed only when it
  // has no phi (the caller is responsible for any phi rewrite elsewhere).
  for (size_t s = 0; s < newTerminator->numSuccs(); ++s) {
    BasicBlock* target = newTerminator->succ(s);
    if (target == nullptr || target->parent() != function) return MutationResult::Rejected;
    if (hasSuccessor(block, target, newTerminator->numSuccs())) continue;
    for (Instruction* inst : target->insts())
      if (inst != nullptr && inst->op() == Opcode::Phi)
        return MutationResult::Rejected;
  }
  for (size_t s = 0; s < old->numSuccs(); ++s) {
    BasicBlock* target = old->succ(s);
    if (target == nullptr) continue;
    size_t before = 0, after = 0;
    for (size_t k = 0; k < old->numSuccs(); ++k)
      if (old->succ(k) == target) ++before;
    for (size_t k = 0; k < newTerminator->numSuccs(); ++k)
      if (newTerminator->succ(k) == target) ++after;
    if (after > before) return MutationResult::Rejected;   // would add an edge
    if (after > 0) continue;                              // predecessor remains
    for (Instruction* inst : target->insts())
      if (inst != nullptr && inst->op() == Opcode::Phi)
        return MutationResult::Rejected;
    updatePreds(target, block, nullptr);
  }
  for (size_t s = 0; s < newTerminator->numSuccs(); ++s) {
    BasicBlock* target = newTerminator->succ(s);
    if (target == nullptr) continue;
    if (!hasSuccessor(block, target, newTerminator->numSuccs()))
      updatePreds(target, nullptr, block);
  }
  if (!mutator_.module().ownInst(newTerminator)) return MutationResult::Rejected;
  if (!mutator_.rawErase(old)) return MutationResult::Rejected;
  block->addInst(newTerminator);
  mutator_.commit(MutationKind::CFG, {function});
  return MutationResult::Applied;
}

CFGEditor::SplitResult CFGEditor::splitEdge(BasicBlock* from, size_t index) {
  if (!mutator_.isLiveBlock(from)) return {MutationResult::Rejected, nullptr};
  Instruction* term = from->terminator();
  if (term == nullptr || !mutator_.isLive(term) ||
      term->op() != Opcode::Br || index >= term->numSuccs())
    return {MutationResult::Rejected, nullptr};
  BasicBlock* target = term->succ(index);
  if (!mutator_.isLiveBlock(target) || target->parent() != from->parent())
    return {MutationResult::Rejected, nullptr};
  if (std::count(target->preds().begin(), target->preds().end(), from) != 1)
    return {MutationResult::Rejected, nullptr};
  std::vector<Instruction*> phis;
  if (!readPhis(target, phis)) return {MutationResult::Rejected, nullptr};
  const bool keepOld = hasSuccessor(from, target, index);
  std::vector<Incoming> rewrite;
  for (Instruction* phi : phis) {
    Incoming pairs = incoming(phi);
    auto it = std::find_if(pairs.begin(), pairs.end(),
        [=](const Pair& p) { return p.first == from; });
    if (it == pairs.end()) return {MutationResult::Rejected, nullptr};
    rewrite.push_back(std::move(pairs));
  }
  Module& module = mutator_.module();
  BasicBlock* mid = module.createBlock(from->parent());
  if (mid == nullptr) return {MutationResult::Rejected, nullptr};
  Instruction* branch = createBr(target, term->loc());
  module.ownInst(branch);
  mid->addInst(branch);
  mid->addPred(from);
  term->setSucc(index, mid);
  updatePreds(target, keepOld ? nullptr : from, mid);
  for (size_t i = 0; i < phis.size(); ++i) {
    Incoming& pairs = rewrite[i];
    auto it = std::find_if(pairs.begin(), pairs.end(),
        [=](const Pair& p) { return p.first == from; });
    if (keepOld) pairs.emplace_back(mid, it->second);
    else it->first = mid;
    writeIncoming(phis[i], std::move(pairs));
  }
  mutator_.commit(phis.empty() ? MutationKind::CFG
                : MutationKind::CFG | MutationKind::DataFlow, {from->parent()});
  return {MutationResult::Applied, mid};
}
}} // namespace sysy::flat
