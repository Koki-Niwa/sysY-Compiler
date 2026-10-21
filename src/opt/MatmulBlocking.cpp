#include "opt/MatmulBlocking.h"
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include "ir/Module.h"
#include "ir/CFGEditor.h"
#include "analysis/BlockingPlan.h"
#include "opt/FlatPassManager.h"
#include <set>
#include "ir/Function.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
struct MatmulNest {
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* reduction = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* reductionPre = nullptr;
  BasicBlock* reductionLatch = nullptr;
  BasicBlock* reductionExit = nullptr;
  BasicBlock* jLatch = nullptr;
  BasicBlock* outerLatch = nullptr;
  BasicBlock* exit = nullptr;
  Instruction* outerPhi = nullptr;
  Instruction* innerPhi = nullptr;
  Instruction* reductionPhi = nullptr;
  Instruction* accumulator = nullptr;
  Instruction* outerCmp = nullptr;
  Instruction* innerCmp = nullptr;
  Instruction* reductionCmp = nullptr;
  int64_t outerBound = 0;
  int64_t innerBound = 0;
  int64_t reductionBound = 0;
  const GlobalAddr* output = nullptr;
};
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
  if (phi->succ(1 - preIndex) != latch) return false;
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
  if (phi->succ(1 - preIndex) != latch) return false;
  int64_t zero = 0;
  if (!constantI32(phi->operand(preIndex), zero) || zero != 0 ||
      phi->operand(1 - preIndex) == nullptr ||
      !phi->operand(1 - preIndex)->isInst())
    return false;
  const auto* update = static_cast<const Instruction*>(phi->operand(1 - preIndex));
  return (update->parent() == body || update->parent() == latch) &&
         update->op() == Opcode::Add &&
         update->numOperands() == 2 &&
         (update->operand(0) == phi || update->operand(1) == phi);
}
}  // namespace
bool matchMatmul(const Function& function, MatmulNest& nest) {
  for (BasicBlock* outer : function.blocks()) {
    if (outer->size() != 3 || outer->at(0)->op() != Opcode::Phi ||
        outer->at(1)->op() != Opcode::ICmp || outer->at(2)->op() != Opcode::Br ||
        outer->at(2)->numSuccs() != 2)
      continue;
    Instruction* outerPhi = outer->at(0);
    BasicBlock* inner = skipTrampolines(outer->at(2)->succ(0));
    BasicBlock* exit = outer->at(2)->succ(1);
    if (inner == nullptr || inner->size() != 3 || inner->at(0)->op() != Opcode::Phi ||
        inner->at(1)->op() != Opcode::ICmp || inner->at(2)->op() != Opcode::Br ||
        inner->at(2)->numSuccs() != 2)
      continue;
    Instruction* innerPhi = inner->at(0);
    BasicBlock* reduction = skipTrampolines(inner->at(2)->succ(0));
    BasicBlock* innerExit = skipTrampolines(inner->at(2)->succ(1));
    if (reduction == nullptr || reduction->size() < 4 ||
        reduction->at(0)->op() != Opcode::Phi ||
        reduction->at(1)->op() != Opcode::Phi ||
        reduction->at(2)->op() != Opcode::ICmp ||
        reduction->at(3)->op() != Opcode::Br ||
        reduction->at(3)->numSuccs() != 2)
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
    nest.outer = outer;
    nest.inner = inner;
    nest.reduction = reduction;
    nest.reductionPre = reductionPre;
    nest.body = body;
    nest.reductionLatch = reductionLatch;
    nest.reductionExit = reductionExit;
    nest.jLatch = jLatch;
    nest.outerLatch = outerLatch;
    nest.exit = exit;
    nest.outerPhi = outerPhi;
    nest.innerPhi = innerPhi;
    nest.reductionPhi = reductionPhi;
    nest.accumulator = accumulator;
    nest.outerCmp = outer->at(1);
    nest.innerCmp = inner->at(1);
    nest.reductionCmp = reduction->at(2);
    nest.outerBound = outerBound;
    nest.innerBound = innerBound;
    nest.reductionBound = reductionBound;
    nest.output = output;
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

namespace {
Instruction* insertBeforeTerm(Module& module, IRMutator& mutator,
                              BasicBlock* block, Instruction* inst) {
  if (mutator.insertInstruction(block, block->size() - 1, inst) !=
      MutationResult::Applied) {
    mutator.discardDetached({inst});
    return nullptr;
  }
  (void)module;
  return inst;
}
MutationResult replaceTerm(CFGEditor& cfg, IRMutator& mutator, BasicBlock* block,
                           Instruction* term,
                           const std::vector<std::vector<Value*>>& values = {}) {
  const MutationResult result = cfg.replaceTerminator(block, term, values);
  if (result != MutationResult::Applied) mutator.discardDetached({term});
  return result;
}
}  // namespace

MatmulBlockingPass::MatmulBlockingPass() : FunctionPass("matmul-blocking") {}

PassResult MatmulBlockingPass::run(Function& function, PassContext& context) {
  PassResult result;
  MatmulNest nest;
  if (!matchMatmul(function, nest)) return result;
  BlockingContext planning;
  planning.outerTrip = static_cast<size_t>(nest.outerBound);
  planning.innerTrip = static_cast<size_t>(nest.innerBound);
  planning.outerStride = planning.innerTrip;
  planning.innerStride = 1;
  planning.elementBytes = 4;
  planning.dependenceFree = true;
  const BlockingPlan plan = planBlocking(planning);
  if (!plan.block) return result;
  // The conservative line-footprint planner may choose a full inner extent
  // (e.g. 32x64 for a 64x64 matmul); that is only 1-D blocking. Halve a full
  // dimension so both loop transforms are genuinely two-dimensional.
  size_t tileOuterSize = plan.tileOuter;
  size_t tileInnerSize = plan.tileInner;
  if (tileOuterSize >= planning.outerTrip && planning.outerTrip >= 16)
    tileOuterSize = planning.outerTrip / 2;
  if (tileInnerSize >= planning.innerTrip && planning.innerTrip >= 16)
    tileInnerSize = planning.innerTrip / 2;
  if (tileOuterSize < 8 || tileInnerSize < 8 ||
      tileOuterSize >= planning.outerTrip ||
      tileInnerSize >= planning.innerTrip)
    return result;
  const int64_t tileOuter = static_cast<int64_t>(tileOuterSize);
  const int64_t tileInner = static_cast<int64_t>(tileInnerSize);
  if (nest.outerBound > std::numeric_limits<int32_t>::max() - tileOuter ||
      nest.innerBound > std::numeric_limits<int32_t>::max() - tileInner)
    return result;
  Module& module = context.analyses().module();
  IRMutator& mutator = context.mutator();
  CFGEditor& cfg = context.cfg();
  auto fail = [] { PassResult p; p.status = PassStatus::Failed;
                   p.diagnostic = "matmul-blocking: rewrite rejected"; return p; }();
  const std::vector<std::string> names{"T1", "T2", "I", "J", "LJ", "LI",
                                       "LT2", "LT1"};
  std::unordered_map<std::string, BasicBlock*> byName;
  for (const std::string& name : names) {
    BasicBlock* block = module.createBlock(&function);
    Instruction* placeholder = createUnreachable({});
    module.ownInst(placeholder);
    block->addInst(placeholder);
    byName[name] = block;
  }
  BasicBlock* t1 = byName["T1"]; BasicBlock* t2 = byName["T2"];
  BasicBlock* i = byName["I"]; BasicBlock* j = byName["J"];
  BasicBlock* lj = byName["LJ"]; BasicBlock* li = byName["LI"];
  BasicBlock* lt2 = byName["LT2"]; BasicBlock* lt1 = byName["LT1"];
  Constant* zero = module.getIntConst(typePool().i32(), 0);
  if (cfg.redirectEdge(nest.pre, 0, t1, {}) != MutationResult::Applied)
    return fail;
  Instruction* i0 = createPhi(typePool().i32(), {zero}, {nest.pre}, {});
  if (cfg.insertPhi(t1, i0) != MutationResult::Applied) {
    mutator.discardDetached({i0});
    return fail;
  }
  Instruction* i0Plus = createBin(Opcode::Add, i0,
                                  module.getIntConst(typePool().i32(), tileOuter), {});
  if (insertBeforeTerm(module, mutator, t1, i0Plus) == nullptr) return fail;
  Instruction* i0PlusCmp = createICmp(IPred::Slt, i0Plus,
                                      module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0PlusCmp) == nullptr) return fail;
  Instruction* i0End = createSelect(i0PlusCmp, i0Plus,
                                    module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0End) == nullptr) return fail;
  Instruction* i0Cmp = createICmp(IPred::Slt, i0,
                                  module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0Cmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t1, createCondBr(i0Cmp, t2, nest.exit, {})) !=
      MutationResult::Applied) return fail;
  Instruction* j0 = createPhi(typePool().i32(), {zero}, {t1}, {});
  if (cfg.insertPhi(t2, j0) != MutationResult::Applied) {
    mutator.discardDetached({j0});
    return fail;
  }
  Instruction* j0Plus = createBin(Opcode::Add, j0,
                                  module.getIntConst(typePool().i32(), tileInner), {});
  if (insertBeforeTerm(module, mutator, t2, j0Plus) == nullptr) return fail;
  Instruction* j0PlusCmp = createICmp(IPred::Slt, j0Plus,
                                      module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0PlusCmp) == nullptr) return fail;
  Instruction* j0End = createSelect(j0PlusCmp, j0Plus,
                                    module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0End) == nullptr) return fail;
  Instruction* j0Cmp = createICmp(IPred::Slt, j0,
                                  module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0Cmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t2, createCondBr(j0Cmp, i, lt1, {})) !=
      MutationResult::Applied) return fail;
  Instruction* ip = createPhi(typePool().i32(), {i0}, {t2}, {});
  if (cfg.insertPhi(i, ip) != MutationResult::Applied) {
    mutator.discardDetached({ip});
    return fail;
  }
  Instruction* iCmp = createICmp(IPred::Slt, ip, i0End, {});
  if (insertBeforeTerm(module, mutator, i, iCmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, i, createCondBr(iCmp, j, lt2, {})) !=
      MutationResult::Applied) return fail;
  Instruction* jp = createPhi(typePool().i32(), {j0}, {i}, {});
  if (cfg.insertPhi(j, jp) != MutationResult::Applied) {
    mutator.discardDetached({jp});
    return fail;
  }
  Instruction* jCmp = createICmp(IPred::Slt, jp, j0End, {});
  if (insertBeforeTerm(module, mutator, j, jCmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, j, createCondBr(jCmp, nest.reductionPre, li, {})) !=
      MutationResult::Applied) return fail;
  // Old O/J blocks become unreachable. Replace their phi uses globally with a
  // dominating zero, then rewrite only the retained K loop/KX positions with
  // the new I/J values. Replacing dead-block uses with a live new phi would
  // violate dominance (V3) even though those blocks are unreachable.
  auto replaceRetained = [&](Instruction* oldPhi, Value* replacement) -> bool {
    std::vector<std::pair<Instruction*, size_t>> retained;
    for (BasicBlock* block : {nest.reduction, nest.body, nest.reductionLatch,
                              nest.reductionExit}) {
      for (Instruction* inst : block->insts())
        for (size_t operand = 0; operand < inst->numOperands(); ++operand)
          if (inst->operand(operand) == oldPhi)
            retained.push_back({inst, operand});
    }
    const MutationResult replaced = mutator.replaceUses(oldPhi, zero);
    if (replaced != MutationResult::Applied && replaced != MutationResult::NoChange)
      return false;
    if (cfg.erasePhi(oldPhi) != MutationResult::Applied) return false;
    for (const auto& [user, operand] : retained)
      user->setOperand(operand, replacement);
    return true;
  };
  if (!replaceRetained(nest.outerPhi, ip) ||
      !replaceRetained(nest.innerPhi, jp))
    return fail;
  if (cfg.replaceTerminator(nest.reductionExit, createBr(lj, {})) !=
      MutationResult::Applied)
    return fail;
  Instruction* jNext = createBin(Opcode::Add, jp,
                                 module.getIntConst(typePool().i32(), 1), {});
  if (insertBeforeTerm(module, mutator, lj, jNext) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lj, createBr(j, {}), {{jNext}}) !=
      MutationResult::Applied) return fail;
  Instruction* iNext = createBin(Opcode::Add, ip,
                                 module.getIntConst(typePool().i32(), 1), {});
  if (insertBeforeTerm(module, mutator, li, iNext) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, li, createBr(i, {}), {{iNext}}) !=
      MutationResult::Applied) return fail;
  Instruction* j0Next = createBin(Opcode::Add, j0,
                                  module.getIntConst(typePool().i32(), tileInner), {});
  if (insertBeforeTerm(module, mutator, lt2, j0Next) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lt2, createBr(t2, {}), {{j0Next}}) !=
      MutationResult::Applied) return fail;
  Instruction* i0Next = createBin(Opcode::Add, i0,
                                  module.getIntConst(typePool().i32(), tileOuter), {});
  if (insertBeforeTerm(module, mutator, lt1, i0Next) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lt1, createBr(t1, {}), {{i0Next}}) !=
      MutationResult::Applied) return fail;
  ++result.stats.counters["blocked-matmul"];
  result.stats.counters["tile-outer"] = static_cast<long long>(tileOuter);
  result.stats.counters["tile-inner"] = static_cast<long long>(tileInner);
  result.status = PassStatus::Applied;
  return result;
}
}}  // namespace sysy::flat
