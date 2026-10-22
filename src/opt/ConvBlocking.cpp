#include "opt/ConvBlocking.h"
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "analysis/BlockingPlan.h"
#include "ir/CFGEditor.h"
#include "ir/Function.h"
#include "ir/Instruction.h"
#include "ir/Module.h"
namespace sysy { namespace flat {
namespace {
struct ConvNest {
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* ki = nullptr;
  BasicBlock* kj = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* kjLatch = nullptr;
  BasicBlock* kiLatch = nullptr;
  BasicBlock* kx = nullptr;
  BasicBlock* jLatch = nullptr;
  BasicBlock* outerLatch = nullptr;
  BasicBlock* exit = nullptr;
  Instruction* outerPhi = nullptr;
  Instruction* innerPhi = nullptr;
  Instruction* kiPhi = nullptr;
  Instruction* accKi = nullptr;
  Instruction* kjPhi = nullptr;
  Instruction* accKj = nullptr;
  int64_t outerBound = 0;
  int64_t innerBound = 0;
};
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
bool singleBr(BasicBlock* block) {
  return block != nullptr && block->size() == 1 &&
         block->terminator() != nullptr &&
         block->terminator()->op() == Opcode::Br &&
         block->terminator()->numSuccs() == 1;
}
bool jumpsTo(BasicBlock* block, BasicBlock* target) {
  const Instruction* term = block == nullptr ? nullptr : block->terminator();
  return term != nullptr && term->op() == Opcode::Br &&
         term->numSuccs() == 1 && term->succ(0) == target;
}
bool predecessors(BasicBlock* block, std::initializer_list<BasicBlock*> expected) {
  return block != nullptr && block->preds().size() == expected.size() &&
         std::set<BasicBlock*>(block->preds().begin(), block->preds().end()) ==
             std::set<BasicBlock*>(expected);
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
bool matchConv(const Function& function, ConvNest& nest) {
  for (BasicBlock* outer : function.blocks()) {
    if (outer->size() != 3 || outer->at(0)->op() != Opcode::Phi ||
        outer->at(1)->op() != Opcode::ICmp || outer->at(2)->op() != Opcode::Br ||
        outer->at(2)->numSuccs() != 2 ||
        outer->at(2)->operand(0) != outer->at(1))
      continue;
    Instruction* outerPhi = outer->at(0);
    BasicBlock* inner = skipTrampolines(outer->at(2)->succ(0));
    BasicBlock* exit = outer->at(2)->succ(1);
    if (inner == nullptr || inner->size() != 3 ||
        inner->at(0)->op() != Opcode::Phi ||
        inner->at(1)->op() != Opcode::ICmp || inner->at(2)->op() != Opcode::Br ||
        inner->at(2)->numSuccs() != 2 ||
        inner->at(2)->operand(0) != inner->at(1))
      continue;
    Instruction* innerPhi = inner->at(0);
    BasicBlock* ki = skipTrampolines(inner->at(2)->succ(0));
    BasicBlock* outerLatch = skipTrampolines(inner->at(2)->succ(1));
    if (ki == nullptr || ki->size() != 4 || ki->at(0)->op() != Opcode::Phi ||
        ki->at(1)->op() != Opcode::Phi || ki->at(2)->op() != Opcode::ICmp ||
        ki->at(3)->op() != Opcode::Br || ki->at(3)->numSuccs() != 2 ||
        ki->at(3)->operand(0) != ki->at(2))
      continue;
    Instruction* kiPhi = nullptr;
    Instruction* accKi = nullptr;
    for (size_t index = 0; index < 2; ++index) {
      if (ki->at(2)->operand(0) == ki->at(index)) kiPhi = ki->at(index);
      else accKi = ki->at(index);
    }
    if (kiPhi == nullptr || accKi == nullptr) continue;
    BasicBlock* kj = skipTrampolines(ki->at(3)->succ(0));
    BasicBlock* kx = skipTrampolines(ki->at(3)->succ(1));
    if (kj == nullptr || kj->size() != 4 || kj->at(0)->op() != Opcode::Phi ||
        kj->at(1)->op() != Opcode::Phi || kj->at(2)->op() != Opcode::ICmp ||
        kj->at(3)->op() != Opcode::Br || kj->at(3)->numSuccs() != 2 ||
        kj->at(3)->operand(0) != kj->at(2))
      continue;
    Instruction* kjPhi = nullptr;
    Instruction* accKj = nullptr;
    for (size_t index = 0; index < 2; ++index) {
      if (kj->at(2)->operand(0) == kj->at(index)) kjPhi = kj->at(index);
      else accKj = kj->at(index);
    }
    if (kjPhi == nullptr || accKj == nullptr) continue;
    BasicBlock* body = kj->at(3)->succ(0);
    BasicBlock* kiLatch = skipTrampolines(kj->at(3)->succ(1));
    if (body == nullptr || body->terminator() == nullptr ||
        body->terminator()->op() != Opcode::Br ||
        body->terminator()->numSuccs() != 1)
      continue;
    BasicBlock* kjLatch = body->terminator()->succ(0);
    if (kjLatch == nullptr || kjLatch->size() != 2 ||
        kjLatch->terminator() == nullptr ||
        kjLatch->terminator()->op() != Opcode::Br ||
        kjLatch->terminator()->succ(0) != kj ||
        kiLatch == nullptr || kiLatch->size() != 2 ||
        kiLatch->terminator() == nullptr ||
        kiLatch->terminator()->op() != Opcode::Br ||
        kiLatch->terminator()->succ(0) != ki ||
        kx == nullptr || kx->size() < 2 ||
        kx->at(kx->size() - 1)->op() != Opcode::Br ||
        kx->at(kx->size() - 1)->numSuccs() != 1)
      continue;
    const Instruction* store = kx->at(kx->size() - 2);
    if (store->op() != Opcode::Store || store->operand(0) != accKi) continue;
    bool kxPure = true;
    for (size_t index = 0; index + 2 < kx->size(); ++index)
      if (kx->at(index)->op() != Opcode::SExt &&
          kx->at(index)->op() != Opcode::ZExt &&
          kx->at(index)->op() != Opcode::GEP)
        kxPure = false;
    if (!kxPure) continue;
    BasicBlock* jLatch = skipTrampolines(kx->at(kx->size() - 1)->succ(0));
    if (jLatch == nullptr || jLatch->size() != 2 ||
        jLatch->terminator() == nullptr ||
        jLatch->terminator()->op() != Opcode::Br ||
        jLatch->terminator()->succ(0) != inner ||
        outerLatch == nullptr || outerLatch->size() != 2 ||
        outerLatch->terminator() == nullptr ||
        outerLatch->terminator()->op() != Opcode::Br ||
        outerLatch->terminator()->succ(0) != outer)
      continue;
    BasicBlock* outerPre = onlyPredOtherThan(outer, outerLatch);
    BasicBlock* innerPre = onlyPredOtherThan(inner, jLatch);
    BasicBlock* kiPre = onlyPredOtherThan(ki, kiLatch);
    BasicBlock* kjPre = onlyPredOtherThan(kj, kjLatch);
    if (outerPre == nullptr || innerPre == nullptr || kiPre == nullptr ||
        kjPre == nullptr)
      continue;
    // The new J edge enters KI directly and supplies zero seeds, so the old KI
    // preheader must be a pure trampoline. Other preheaders remain live but the
    // rewrite still requires the exact predecessor/latch roles below.
    if (!singleBr(kiPre) || jLatch->size() != 2 || kjLatch->size() != 2 ||
        !jumpsTo(outerPre, outer) ||
        !predecessors(outer, {outerPre, outerLatch}) ||
        !predecessors(inner, {innerPre, jLatch}) ||
        !predecessors(ki, {kiPre, kiLatch}) ||
        !predecessors(kj, {kjPre, kjLatch}) ||
        !predecessors(body, {kj}) ||
        !predecessors(kjLatch, {body}) ||
        !predecessors(kx, {ki}) ||
        !predecessors(jLatch, {kx}))
      continue;
    bool exitHasPhi = false;
    for (Instruction* inst : exit->insts())
      if (inst->op() == Opcode::Phi) exitHasPhi = true;
    if (exitHasPhi) continue;
    // Retained instructions may only refer to retained definitions or the two
    // outer/inner induction phis that are rewritten.
    std::set<BasicBlock*> retained{ki, kj, body, kjLatch, kiLatch, kx};
    bool closed = true;
    for (BasicBlock* block : retained) {
      for (Instruction* inst : block->insts())
        for (Value* operand : inst->operands())
          if (operand != nullptr && operand->isInst() && operand != outerPhi &&
              operand != innerPhi &&
              !retained.count(static_cast<Instruction*>(operand)->parent()))
            closed = false;
    }
    if (!closed) continue;
    int64_t outerBound = 0, innerBound = 0, kiBound = 0, kjBound = 0;
    if (!matchInduction(outerPhi, outer->at(1), outerBound, outerPre, outerLatch) ||
        !matchInduction(innerPhi, inner->at(1), innerBound, innerPre, jLatch) ||
        !matchInduction(kiPhi, ki->at(2), kiBound, kiPre, kiLatch) ||
        !matchInduction(kjPhi, kj->at(2), kjBound, kjPre, kjLatch))
      continue;
    // KI's accumulator is initialized in KI and receives KJ's final phi; KJ's
    // accumulator is initialized from KI's and updated by an add in the body.
    const size_t kiPreIndex = accKi->succ(0) == kiPre ? 0 : 1;
    const size_t kjPreIndex = accKj->succ(0) == kjPre ? 0 : 1;
    int64_t zero = 0;
    if (!constantI32(accKi->operand(kiPreIndex), zero) || zero != 0 ||
        accKi->operand(1 - kiPreIndex) != accKj ||
        accKj->operand(kjPreIndex) != accKi ||
        accKj->operand(1 - kjPreIndex) == nullptr ||
        !accKj->operand(1 - kjPreIndex)->isInst())
      continue;
    const auto* update = static_cast<const Instruction*>(
        accKj->operand(1 - kjPreIndex));
    if (update->parent() != body || update->op() != Opcode::Add ||
        update->numOperands() != 2 ||
        (update->operand(0) != accKj && update->operand(1) != accKj))
      continue;
    bool accClosed = true;
    for (const Use& use : accKi->uses())
      if (use.user != accKj && use.user != store) accClosed = false;
    for (const Use& use : accKj->uses())
      if (use.user != accKi && use.user != update) accClosed = false;
    if (!accClosed) continue;
    if (!canonicalStore(store, outerPhi, innerPhi,
                        static_cast<size_t>(outerBound),
                        static_cast<size_t>(innerBound)))
      continue;
    const GlobalAddr* output = baseGlobal(store->operand(1));
    if (output == nullptr) continue;
    std::set<BasicBlock*> region{outer, inner, ki, kj, body, kjLatch, kiLatch,
                                 kx, jLatch, outerLatch};
    bool safe = true;
    for (BasicBlock* block : region) {
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
    for (BasicBlock* block : region) {
      for (Instruction* inst : block->insts())
        for (const Use& use : inst->uses()) {
          BasicBlock* user = use.user == nullptr ? nullptr : use.user->parent();
          if (user == nullptr || user == exit || !region.count(user)) safe = false;
        }
    }
    if (!safe) continue;
    nest.pre = outerPre;
    nest.outer = outer;
    nest.inner = inner;
    nest.ki = ki;
    nest.kj = kj;
    nest.body = body;
    nest.kjLatch = kjLatch;
    nest.kiLatch = kiLatch;
    nest.kx = kx;
    nest.jLatch = jLatch;
    nest.outerLatch = outerLatch;
    nest.exit = exit;
    nest.outerPhi = outerPhi;
    nest.innerPhi = innerPhi;
    nest.kiPhi = kiPhi;
    nest.accKi = accKi;
    nest.kjPhi = kjPhi;
    nest.accKj = accKj;
    nest.outerBound = outerBound;
    nest.innerBound = innerBound;
    return true;
  }
  return false;
}
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

ConvBlockingPass::ConvBlockingPass() : FunctionPass("conv-blocking") {}

PassResult ConvBlockingPass::run(Function& function, PassContext& context) {
  PassResult result;
  ConvNest nest;
  if (!matchConv(function, nest)) return result;
  BlockingContext planning;
  planning.outerTrip = static_cast<size_t>(nest.outerBound);
  planning.innerTrip = static_cast<size_t>(nest.innerBound);
  planning.outerStride = planning.innerTrip;
  planning.innerStride = 1;
  planning.elementBytes = 4;
  planning.dependenceFree = true;
  const BlockingPlan plan = planBlocking(planning);
  if (!plan.block) return result;
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
                   p.diagnostic = "conv-blocking: rewrite rejected"; return p; }();
  const std::vector<std::string> names{"T1", "T2", "I", "J", "LJ", "LI",
                                       "LT2", "LT1"};
  std::unordered_map<std::string, BasicBlock*> byName;
  for (const std::string& name : names) {
    BasicBlock* block = cfg.createBlock(&function);
    if (block == nullptr) return fail;
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
  Instruction* i0Cmp = createICmp(IPred::Slt, i0Plus,
                                  module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0Cmp) == nullptr) return fail;
  Instruction* i0End = createSelect(i0Cmp, i0Plus,
                                    module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0End) == nullptr) return fail;
  Instruction* i0Lt = createICmp(IPred::Slt, i0,
                                 module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0Lt) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t1, createCondBr(i0Lt, t2, nest.exit, {})) !=
      MutationResult::Applied) return fail;
  Instruction* j0 = createPhi(typePool().i32(), {zero}, {t1}, {});
  if (cfg.insertPhi(t2, j0) != MutationResult::Applied) {
    mutator.discardDetached({j0});
    return fail;
  }
  Instruction* j0Plus = createBin(Opcode::Add, j0,
                                  module.getIntConst(typePool().i32(), tileInner), {});
  if (insertBeforeTerm(module, mutator, t2, j0Plus) == nullptr) return fail;
  Instruction* j0Cmp = createICmp(IPred::Slt, j0Plus,
                                  module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0Cmp) == nullptr) return fail;
  Instruction* j0End = createSelect(j0Cmp, j0Plus,
                                    module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0End) == nullptr) return fail;
  Instruction* j0Lt = createICmp(IPred::Slt, j0,
                                 module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0Lt) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t2, createCondBr(j0Lt, i, lt1, {})) !=
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
  if (replaceTerm(cfg, mutator, j, createCondBr(jCmp, nest.ki, li, {}),
                  {{zero, zero}, {}}) != MutationResult::Applied)
    return fail;
  auto replaceRetained = [&](Instruction* oldPhi, Value* replacement) -> bool {
    std::vector<std::pair<Instruction*, size_t>> retained;
    for (BasicBlock* block : {nest.ki, nest.kj, nest.body, nest.kjLatch,
                              nest.kiLatch, nest.kx}) {
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
      if (mutator.rewriteOperand(user, operand, replacement) != MutationResult::Applied)
        return false;
    return true;
  };
  if (!replaceRetained(nest.outerPhi, ip) ||
      !replaceRetained(nest.innerPhi, jp))
    return fail;
  if (replaceTerm(cfg, mutator, nest.kx, createBr(lj, {})) !=
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
  ++result.stats.counters["blocked-conv"];
  result.stats.counters["tile-outer"] = static_cast<long long>(tileOuter);
  result.stats.counters["tile-inner"] = static_cast<long long>(tileInner);
  result.status = PassStatus::Applied;
  return result;
}
}}  // namespace sysy::flat
