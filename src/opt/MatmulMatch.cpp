#include "opt/MatmulBlockingInternal.h"
#include <set>
#include "ir/Function.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool constantI32(const Value* value, int64_t& out) {
  if (value == nullptr || !value->isConstant() || value->type() != typePool().i32())
    return false;
  out = static_cast<const Constant*>(value)->intVal();
  return true;
}
bool matchInduction(Instruction* phi, Instruction* cmp, int64_t& bound,
                    BasicBlock* pre, BasicBlock* latch) {
  if (phi == nullptr || cmp == nullptr || pre == nullptr || latch == nullptr ||
      phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || cmp->op() != Opcode::ICmp ||
      cmp->ipred() != IPred::Slt || cmp->operand(0) != phi ||
      !constantI32(cmp->operand(1), bound) || bound <= 0)
    return false;
  const size_t preIndex = phi->succ(0) == pre ? 0 : 1;
  if (phi->succ(preIndex) != pre || phi->succ(1 - preIndex) != latch) return false;
  int64_t start = 0, step = 0;
  if (!constantI32(phi->operand(preIndex), start) || start != 0 ||
      phi->operand(1 - preIndex) == nullptr ||
      !phi->operand(1 - preIndex)->isInst())
    return false;
  const auto* inc = static_cast<const Instruction*>(phi->operand(1 - preIndex));
  return inc->parent() == latch && inc->op() == Opcode::Add &&
         inc->numOperands() == 2 && inc->operand(0) == phi &&
         constantI32(inc->operand(1), step) && step == 1;
}
BasicBlock* skipTrampolines(BasicBlock* block) {
  for (size_t steps = 0; block != nullptr && steps < 8; ++steps) {
    if (block->size() != 1) return block;
    Instruction* term = block->terminator();
    if (term == nullptr || term->op() != Opcode::Br || term->numSuccs() != 1 ||
        term->succ(0) == block)
      return block;
    block = term->succ(0);
  }
  return block;
}
BasicBlock* onlyPredOtherThan(const BasicBlock* block, const BasicBlock* other) {
  if (block == nullptr) return nullptr;
  BasicBlock* result = nullptr;
  for (BasicBlock* pred : block->preds()) {
    if (pred == other) continue;
    if (result != nullptr) return nullptr;
    result = pred;
  }
  return result;
}
bool indexMatches(const Value* value, const Instruction* expected) {
  while (value != nullptr && value->isInst()) {
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() != Opcode::SExt && inst->op() != Opcode::ZExt) break;
    value = inst->operand(0);
  }
  return value == expected;
}
const GlobalAddr* baseGlobal(const Value* value) {
  while (value != nullptr && value->isInst()) {
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() != Opcode::GEP || inst->numOperands() == 0) return nullptr;
    value = inst->operand(0);
  }
  return value != nullptr && value->isGlobalAddr()
             ? static_cast<const GlobalAddr*>(value) : nullptr;
}
bool canonicalStore(const Instruction* store, const Instruction* outer,
                    const Instruction* inner, size_t outerBound,
                    size_t innerBound) {
  if (store == nullptr || store->numOperands() != 2) return false;
  const Value* pointer = store->operand(1);
  if (pointer == nullptr || !pointer->isInst()) return false;
  const auto* innerGep = static_cast<const Instruction*>(pointer);
  if (innerGep->op() != Opcode::GEP || innerGep->numOperands() != 2 ||
      innerGep->srcElemType() != typePool().i32() ||
      !indexMatches(innerGep->operand(1), inner))
    return false;
  const Value* outerValue = innerGep->operand(0);
  if (outerValue == nullptr || !outerValue->isInst()) return false;
  const auto* outerGep = static_cast<const Instruction*>(outerValue);
  if (outerGep->op() != Opcode::GEP || outerGep->numOperands() != 2 ||
      outerGep->srcElemType() == nullptr ||
      outerGep->srcElemType()->kind != TypeKind::Array ||
      outerGep->srcElemType()->elem != typePool().i32() ||
      !indexMatches(outerGep->operand(1), outer))
    return false;
  const Value* base = outerGep->operand(0);
  if (base == nullptr || !base->isGlobalAddr()) return false;
  const auto* global = static_cast<const GlobalAddr*>(base);
  const Type* object = global->type() == nullptr ? nullptr : global->type()->elem;
  return object != nullptr && object->kind == TypeKind::Array &&
         object->elem == outerGep->srcElemType() &&
         static_cast<size_t>(object->len) >= outerBound &&
         static_cast<size_t>(outerGep->srcElemType()->len) >= innerBound;
}
bool matchAccumulator(Instruction* phi, BasicBlock* pre, BasicBlock* body,
                      BasicBlock* latch) {
  if (phi == nullptr || phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2) return false;
  const size_t preIndex = phi->succ(0) == pre ? 0 : 1;
  if (phi->succ(preIndex) != pre || phi->succ(1 - preIndex) != latch) return false;
  int64_t zero = 0;
  if (!constantI32(phi->operand(preIndex), zero) || zero != 0 ||
      phi->operand(1 - preIndex) == nullptr ||
      !phi->operand(1 - preIndex)->isInst())
    return false;
  const auto* update = static_cast<const Instruction*>(phi->operand(1 - preIndex));
  return (update->parent() == body) &&
         update->op() == Opcode::Add &&
         update->numOperands() == 2 &&
         (update->operand(0) == phi || update->operand(1) == phi);
}
// Check the exact predecessor set, including unreachable incoming edges.
bool predecessors(BasicBlock* block, std::initializer_list<BasicBlock*> expected) {
  return block != nullptr && block->preds().size() == expected.size() &&
         std::set<BasicBlock*>(block->preds().begin(), block->preds().end()) ==
             std::set<BasicBlock*>(expected);
}
bool jumpsTo(BasicBlock* block, BasicBlock* target) {
  const Instruction* term = block == nullptr ? nullptr : block->terminator();
  return term != nullptr && term->op() == Opcode::Br && term->numSuccs() == 1 &&
         term->succ(0) == target;
}
// Empty paths may be bypassed only when they have no other entry. Return the
// final predecessor so header phi edges can be checked against this same path.
BasicBlock* emptyPath(BasicBlock* from, BasicBlock* first, BasicBlock* target,
                      std::set<BasicBlock*>& region) {
  BasicBlock* previous = from;
  BasicBlock* current = first;
  for (size_t steps = 0; current != target; ++steps) {
    if (steps == 8 || current == nullptr || current->size() != 1 ||
        !predecessors(current, {previous}) || !region.insert(current).second ||
        current->terminator()->op() != Opcode::Br ||
        current->terminator()->numSuccs() != 1)
      return nullptr;
    previous = current;
    current = current->terminator()->succ(0);
  }
  return previous;
}
}  // namespace
bool matchMatmul(const Function& function, MatmulNest& nest) {
  for (BasicBlock* outer : function.blocks()) {
    if (outer->size() != 3 || outer->at(0)->op() != Opcode::Phi ||
        outer->at(1)->op() != Opcode::ICmp || outer->at(2)->op() != Opcode::Br ||
        outer->at(2)->numSuccs() != 2 ||
        outer->at(2)->operand(0) != outer->at(1))
      continue;
    Instruction* outerPhi = outer->at(0);
    BasicBlock* inner = skipTrampolines(outer->at(2)->succ(0));
    BasicBlock* exit = outer->at(2)->succ(1);
    if (inner == nullptr || inner->size() != 3 || inner->at(0)->op() != Opcode::Phi ||
        inner->at(1)->op() != Opcode::ICmp || inner->at(2)->op() != Opcode::Br ||
        inner->at(2)->numSuccs() != 2 ||
        inner->at(2)->operand(0) != inner->at(1))
      continue;
    Instruction* innerPhi = inner->at(0);
    BasicBlock* reduction = skipTrampolines(inner->at(2)->succ(0));
    BasicBlock* innerExit = skipTrampolines(inner->at(2)->succ(1));
    if (reduction == nullptr || reduction->size() != 4 ||
        reduction->at(0)->op() != Opcode::Phi ||
        reduction->at(1)->op() != Opcode::Phi ||
        reduction->at(2)->op() != Opcode::ICmp ||
        reduction->at(3)->op() != Opcode::Br ||
        reduction->at(3)->numSuccs() != 2 ||
        reduction->at(3)->operand(0) != reduction->at(2))
      continue;
    Instruction* reductionPhi = nullptr;
    Instruction* accumulator = nullptr;
    for (size_t i = 0; i < 2; ++i) {
      if (reduction->at(2)->operand(0) == reduction->at(i)) reductionPhi = reduction->at(i);
      else accumulator = reduction->at(i);
    }
    if (reductionPhi == nullptr || accumulator == nullptr) continue;
    BasicBlock* body = reduction->at(3)->succ(0);
    BasicBlock* reductionExit = reduction->at(3)->succ(1);
    if (body == nullptr || body->terminator() == nullptr ||
        body->terminator()->op() != Opcode::Br ||
        body->terminator()->numSuccs() != 1)
      continue;
    BasicBlock* reductionLatch = body->terminator()->succ(0);
    if (reductionLatch == nullptr || reductionLatch->terminator() == nullptr ||
        reductionLatch->terminator()->op() != Opcode::Br ||
        reductionLatch->terminator()->succ(0) != reduction ||
        reductionExit == nullptr || reductionExit->size() < 2 ||
        reductionExit->at(reductionExit->size() - 1)->op() != Opcode::Br ||
        reductionExit->at(reductionExit->size() - 1)->numSuccs() != 1)
      continue;
    const Instruction* exitStore = reductionExit->at(reductionExit->size() - 2);
    if (exitStore->op() != Opcode::Store || exitStore->operand(0) != accumulator)
      continue;
    bool exitPure = true;
    for (size_t i = 0; i + 2 < reductionExit->size(); ++i)
      if (reductionExit->at(i)->op() != Opcode::SExt &&
          reductionExit->at(i)->op() != Opcode::ZExt &&
          reductionExit->at(i)->op() != Opcode::GEP)
        exitPure = false;
    if (!exitPure) continue;
    BasicBlock* jLatch = skipTrampolines(reductionExit->at(reductionExit->size() - 1)->succ(0));
    if (jLatch == nullptr || jLatch->terminator() == nullptr ||
        jLatch->terminator()->op() != Opcode::Br ||
        jLatch->terminator()->succ(0) != inner)
      continue;
    BasicBlock* outerLatch = skipTrampolines(innerExit);
    if (outerLatch == nullptr || outerLatch->terminator() == nullptr ||
        outerLatch->terminator()->op() != Opcode::Br ||
        outerLatch->terminator()->succ(0) != outer)
      continue;
    BasicBlock* outerPre = onlyPredOtherThan(outer, outerLatch);
    BasicBlock* innerPre = onlyPredOtherThan(inner, jLatch);
    BasicBlock* reductionPre = onlyPredOtherThan(reduction, reductionLatch);
    if (outerPre == nullptr || innerPre == nullptr || reductionPre == nullptr)
      continue;
    int64_t outerBound = 0, innerBound = 0, reductionBound = 0;
    if (!matchInduction(outerPhi, outer->at(1), outerBound, outerPre, outerLatch))
      continue;
    if (!matchInduction(innerPhi, inner->at(1), innerBound, innerPre, jLatch))
      continue;
    if (!matchInduction(reductionPhi, reduction->at(2), reductionBound,
                        reductionPre, reductionLatch))
      continue;
    if (!matchAccumulator(accumulator, reductionPre, body, reductionLatch))
      continue;
    // All edits below rely on this closed CFG. In particular, the preheader
    // edge is successor zero, the exit needs no new phi values, and discarded
    // latches contain only the already-proven induction increment and branch.
    std::set<BasicBlock*> region{outer, inner, reduction, body, reductionLatch,
                                reductionExit, jLatch, outerLatch};
    if (region.size() != 8 || region.count(outerPre) || region.count(exit) ||
        outerPre == exit || !jumpsTo(outerPre, outer) ||
        outerLatch->size() != 2 || jLatch->size() != 2 ||
        reductionLatch->size() != 2 || !jumpsTo(outerLatch, outer) ||
        !jumpsTo(jLatch, inner) || !jumpsTo(reductionLatch, reduction) ||
        !predecessors(outer, {outerPre, outerLatch}) ||
        !predecessors(inner, {innerPre, jLatch}) ||
        !predecessors(reduction, {reductionPre, reductionLatch}) ||
        !predecessors(body, {reduction}) ||
        !predecessors(reductionLatch, {body}) ||
        !predecessors(reductionExit, {reduction}))
      continue;
    BasicBlock* innerPath = emptyPath(outer, outer->terminator()->succ(0), inner, region);
    BasicBlock* reductionPath = emptyPath(inner, inner->terminator()->succ(0), reduction, region);
    BasicBlock* jPath = emptyPath(reductionExit, reductionExit->terminator()->succ(0), jLatch, region);
    BasicBlock* outerPath = emptyPath(inner, inner->terminator()->succ(1), outerLatch, region);
    if (innerPath != innerPre || reductionPath != reductionPre ||
        jPath == nullptr || outerPath == nullptr ||
        !predecessors(jLatch, {jPath}) || !predecessors(outerLatch, {outerPath}) ||
        region.count(outerPre) || region.count(exit))
      continue;
    bool closed = true;
    for (Instruction* inst : exit->insts())
      if (inst->op() == Opcode::Phi) closed = false;
    const std::set<BasicBlock*> retained{reduction, body, reductionLatch, reductionExit};
    for (BasicBlock* block : retained) {
      for (Instruction* inst : block->insts()) {
        if (block == body && inst->op() == Opcode::Phi) closed = false;
        for (Value* operand : inst->operands()) {
          if (operand != nullptr && operand->isInst() && operand != outerPhi &&
              operand != innerPhi &&
              !retained.count(static_cast<Instruction*>(operand)->parent()))
            closed = false;
        }
      }
    }
    // The accumulator is local to K: exactly the recurrence and final store
    // may consume it. This excludes hidden feedback through address operands.
    const size_t accPre = accumulator->succ(0) == reductionPre ? 0 : 1;
    const Value* update = accumulator->operand(1 - accPre);
    for (const Use& use : accumulator->uses())
      if (use.user != update && use.user != exitStore) closed = false;
    if (!closed) continue;
    const Instruction* store = exitStore;
    if (!canonicalStore(store, outerPhi, innerPhi,
                        static_cast<size_t>(outerBound),
                        static_cast<size_t>(innerBound)))
      continue;
    const GlobalAddr* output = baseGlobal(store->operand(1));
    if (output == nullptr) continue;
    bool safe = true;
    for (BasicBlock* block : {outer, inner, reduction, body, reductionLatch,
                              reductionExit, jLatch, outerLatch}) {
      for (Instruction* inst : block->insts()) {
        if (inst->op() == Opcode::Store && inst != store) safe = false;
        if (inst->op() == Opcode::Call || inst->op() == Opcode::Alloca ||
            inst->op() == Opcode::LLVMMemCpy || inst->op() == Opcode::LLVMMemSet)
          safe = false;
        if (inst->op() == Opcode::Load) {
          const GlobalAddr* base = baseGlobal(inst->operand(0));
          if (base == nullptr || base == output) safe = false;
        }
      }
    }
    if (!safe) continue;
    bool liveOut = false;
    for (BasicBlock* block : {outer, inner, reduction, body, reductionLatch,
                              reductionExit, jLatch, outerLatch}) {
      for (Instruction* inst : block->insts())
        for (const Use& use : inst->uses()) {
          BasicBlock* user = use.user == nullptr ? nullptr : use.user->parent();
          if (user == nullptr || user == exit ||
              (user != outer && user != inner && user != reduction && user != body &&
               user != reductionLatch && user != reductionExit && user != jLatch &&
               user != outerLatch))
            liveOut = true;
        }
    }
    if (liveOut) continue;
    nest.pre = outerPre;
    nest.reduction = reduction;
    nest.body = body;
    nest.reductionLatch = reductionLatch;
    nest.reductionExit = reductionExit;
    nest.exit = exit;
    nest.outerPhi = outerPhi;
    nest.innerPhi = innerPhi;
    nest.outerBound = outerBound;
    nest.innerBound = innerBound;
    nest.reductionBound = reductionBound;
    return true;
  }
  return false;
}

std::optional<MatmulCandidate> matchMatmulNest(const Function& function) {
  MatmulNest nest;
  if (!matchMatmul(function, nest)) return std::nullopt;
  MatmulCandidate candidate;
  candidate.outerBound = static_cast<size_t>(nest.outerBound);
  candidate.innerBound = static_cast<size_t>(nest.innerBound);
  candidate.reductionBound = static_cast<size_t>(nest.reductionBound);
  return candidate;
}

}}  // namespace sysy::flat
