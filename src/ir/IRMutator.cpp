#include "ir/IRMutator.h"
#include <algorithm>
#include <unordered_set>
#include "ir/FlatSemantics.h"

namespace sysy { namespace flat {
namespace {
struct Edge {
  Instruction* user;
  size_t index;
  bool operator==(const Edge& other) const {
    return user == other.user && index == other.index;
  }
};
struct EdgeHash {
  size_t operator()(const Edge& e) const {
    return std::hash<Instruction*>{}(e.user) ^ (std::hash<size_t>{}(e.index) << 1);
  }
};
bool hasEdge(const Value* v, Instruction* user, size_t index) {
  return std::count_if(v->uses().begin(), v->uses().end(),
                       [=](const Use& u) {
                         return u.user == user && u.operandIndex == index;
                       }) == 1;
}
}  // namespace

bool IRMutator::isLiveBlock(const BasicBlock* block) const {
  if (block == nullptr || block->parent() == nullptr ||
      block->parent()->parent() != &module_) return false;
  const auto& blocks = block->parent()->blocks();
  return std::count(blocks.begin(), blocks.end(), block) == 1;
}
bool IRMutator::isLive(const Instruction* inst) const {
  if (inst == nullptr || !module_.ownsInstruction(inst) ||
      !module_.isLiveInstruction(inst) ||
      inst->lifecycle() != InstructionLifecycle::Live ||
      !isLiveBlock(inst->parent())) return false;
  const auto& insts = inst->parent()->insts();
  return std::count(insts.begin(), insts.end(), inst) == 1;
}
bool IRMutator::valueAvailable(Value* value, bool forLive) const {
  if (value == nullptr) return !forLive;
  if (!value->isInst()) return true;
  Instruction* inst = static_cast<Instruction*>(value);
  if (inst->lifecycle() == InstructionLifecycle::Tombstone) return false;
  return !forLive || isLive(inst);
}
bool IRMutator::operandLegal(const Instruction* user, size_t index,
                             Value* value) const {
  if (user == nullptr || index >= user->numOperands()) return false;
  if (!valueAvailable(value, user->lifecycle() == InstructionLifecycle::Live))
    return false;
  if (value == nullptr) return true; // detached scratch only
  Value* previous = user->operand(index);
  if (previous != nullptr && previous->type() != value->type()) return false;
  if (user->op() == Opcode::Phi && value->type() != user->type()) return false;
  if (previous == nullptr && user->op() == Opcode::Load &&
      (value->type() == nullptr || !value->type()->isPtr())) return false;
  return value->type() != nullptr;
}
bool IRMutator::validUses(const Value* value) const {
  if (value == nullptr) return false;
  std::unordered_set<Edge, EdgeHash> seen;
  for (const Use& use : value->uses()) {
    Instruction* user = use.user;
    if (user == nullptr || user->lifecycle() == InstructionLifecycle::Tombstone ||
        use.operandIndex >= user->numOperands() ||
        user->operand(use.operandIndex) != value ||
        !seen.insert(Edge{user, use.operandIndex}).second) return false;
    if (user->lifecycle() == InstructionLifecycle::Live && !isLive(user))
      return false;
  }
  return true;
}
MutationKind IRMutator::kindsFor(Opcode op) {
  MutationKind kinds = MutationKind::DataFlow;
  switch (op) {
    case Opcode::Alloca: case Opcode::Load: case Opcode::Store:
    case Opcode::GEP: case Opcode::BitCast:
      kinds |= MutationKind::Memory; break;
    case Opcode::Call: case Opcode::LLVMMemCpy: case Opcode::LLVMMemSet:
      kinds |= MutationKind::Calls | MutationKind::Memory; break;
    default: break;
  }
  return kinds;
}
void IRMutator::commit(MutationKind kinds, const std::vector<Function*>& functions) {
  std::unordered_set<Function*> unique;
  for (Function* f : functions)
    if (f != nullptr && f->parent() == &module_) unique.insert(f);
  if (unique.empty()) return;
  module_.revision_.bump(kinds);
  for (Function* f : unique) {
    f->revision_.bump(kinds);
    summary_.affectedFunctions.insert(f);
  }
  ++summary_.operations;
  summary_.kinds |= kinds;
}
MutationResult IRMutator::rewriteOperand(Instruction* user, size_t index,
                                     Value* replacement) {
  if (user == nullptr || user->lifecycle() == InstructionLifecycle::Tombstone ||
      (user->lifecycle() == InstructionLifecycle::Live && !isLive(user)) ||
      !operandLegal(user, index, replacement)) return MutationResult::Rejected;
  if (user->operand(index) == replacement) return MutationResult::NoChange;
  if (Value* old = user->operand(index))
    if (!hasEdge(old, user, index)) return MutationResult::Rejected;
  user->setOperand(index, replacement);
  if (user->lifecycle() == InstructionLifecycle::Live)
    commit(kindsFor(user->op()), {user->parent()->parent()});
  return MutationResult::Applied;
}
MutationResult IRMutator::replaceUses(Value* oldValue, Value* replacement) {
  if (oldValue == nullptr || replacement == nullptr ||
      (oldValue->isInst() &&
       static_cast<Instruction*>(oldValue)->lifecycle() == InstructionLifecycle::Tombstone) ||
      oldValue->type() != replacement->type() ||
      !valueAvailable(replacement, false) ||
      !validUses(oldValue)) return MutationResult::Rejected;
  if (oldValue == replacement || oldValue->uses().empty())
    return MutationResult::NoChange;
  MutationKind kinds = MutationKind::None;
  std::vector<Function*> affected;
  const std::vector<Use> edges = oldValue->uses();
  for (const Use& use : edges) {
    if (!operandLegal(use.user, use.operandIndex, replacement))
      return MutationResult::Rejected;
    if (use.user->lifecycle() == InstructionLifecycle::Live) {
      affected.push_back(use.user->parent()->parent());
      kinds |= kindsFor(use.user->op());
    }
  }
  for (const Use& use : edges) use.user->setOperand(use.operandIndex, replacement);
  commit(kinds, affected);
  return MutationResult::Applied;
}
MutationResult IRMutator::insertInstruction(BasicBlock* block, size_t index,
                                            Instruction* inst) {
  if (!isLiveBlock(block) || inst == nullptr ||
      inst->lifecycle() != InstructionLifecycle::Detached || inst->parent() != nullptr ||
      inst->op() == Opcode::Phi || isTerminatorOpcode(inst->op()) ||
      index >= block->size() || block->terminator() == nullptr)
    return MutationResult::Rejected;
  for (size_t j = index; j < block->size(); ++j)
    if (block->at(j)->op() == Opcode::Phi) return MutationResult::Rejected;
  std::string error;
  if (!validateFlatInstruction(*inst, error)) return MutationResult::Rejected;
  for (size_t j = 0; j < inst->numOperands(); ++j)
    if (!valueAvailable(inst->operand(j), true) ||
        !hasEdge(inst->operand(j), inst, j)) return MutationResult::Rejected;
  if (inst->hasUses()) return MutationResult::Rejected;
  module_.ownInst(inst);
  block->insertInst(index, inst);
  commit(kindsFor(inst->op()), {block->parent()});
  return MutationResult::Applied;
}
bool IRMutator::rawErase(Instruction* inst) {
  if (!isLive(inst) || inst->hasUses() || inst->parent() == nullptr) return false;
  BasicBlock* block = inst->parent();
  if (!block->removeInst(inst)) return false;
  inst->clearOperands();
  inst->clearSuccs();
  inst->setLifecycle(InstructionLifecycle::Tombstone);
  return true;
}
MutationResult IRMutator::eraseInstruction(Instruction* inst) {
  if (!isLive(inst) || isTerminatorOpcode(inst->op()) ||
      inst->op() == Opcode::Phi || inst->hasUses()) return MutationResult::Rejected;
  Function* f = inst->parent()->parent();
  const MutationKind kinds = kindsFor(inst->op());
  if (!rawErase(inst)) return MutationResult::Rejected;
  commit(kinds, {f});
  return MutationResult::Applied;
}
MutationResult IRMutator::replaceAndErase(Instruction* oldInst,
                                          Value* replacement) {
  if (!isLive(oldInst) || oldInst == replacement ||
      isTerminatorOpcode(oldInst->op()) || oldInst->op() == Opcode::Phi ||
      replacement == nullptr || replacement->type() != oldInst->type() ||
      !valueAvailable(replacement, true) || !validUses(oldInst))
    return MutationResult::Rejected;
  MutationKind kinds = kindsFor(oldInst->op());
  std::vector<Function*> affected{oldInst->parent()->parent()};
  const std::vector<Use> edges = oldInst->uses();
  for (const Use& use : edges) {
    if (!operandLegal(use.user, use.operandIndex, replacement))
      return MutationResult::Rejected;
    if (use.user->lifecycle() == InstructionLifecycle::Live) {
      kinds |= kindsFor(use.user->op());
      affected.push_back(use.user->parent()->parent());
    }
  }
  for (const Use& use : edges) use.user->setOperand(use.operandIndex, replacement);
  if (!rawErase(oldInst)) return MutationResult::Rejected; // proved by preflight
  commit(kinds, affected);
  return MutationResult::Applied;
}
MutationResult IRMutator::discardDetached(const std::vector<Instruction*>& scratch) {
  std::unordered_set<Instruction*> set;
  for (Instruction* inst : scratch) {
    if (inst == nullptr || inst->lifecycle() != InstructionLifecycle::Detached ||
        inst->parent() != nullptr || module_.ownsInstruction(inst) ||
        !set.insert(inst).second) return MutationResult::Rejected;
  }
  for (Instruction* inst : scratch)
    for (const Use& use : inst->uses())
      if (!set.count(use.user)) return MutationResult::Rejected;
  for (Instruction* inst : scratch) {
    inst->clearOperands();
    inst->clearSuccs();
  }
  for (Instruction* inst : scratch) delete inst;
  return scratch.empty() ? MutationResult::NoChange : MutationResult::Applied;
}
}} // namespace sysy::flat
