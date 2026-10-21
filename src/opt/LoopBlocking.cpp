#include "opt/LoopBlocking.h"
#include <cstdint>
#include <limits>
#include <unordered_map>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/BlockingPlan.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
struct ReductionNest {
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* innerLatch = nullptr;
  BasicBlock* outerLatch = nullptr;
  BasicBlock* exit = nullptr;
  Instruction* outerPhi = nullptr;
  Instruction* innerPhi = nullptr;
  Instruction* outerCmp = nullptr;
  Instruction* innerCmp = nullptr;
  int64_t outerBound = 0;
  int64_t innerBound = 0;
  GlobalAddr* global = nullptr;
  bool map = false;
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
      phi->numOperands() != 2 || phi->numSuccs() != 2 ||
      cmp->op() != Opcode::ICmp || cmp->ipred() != IPred::Slt ||
      cmp->operand(0) != phi ||
      !((phi->succ(0) == pre && phi->succ(1) == latch) ||
        (phi->succ(0) == latch && phi->succ(1) == pre)) ||
      !constantI32(cmp->operand(1), bound) || bound <= 0)
    return false;
  const size_t preIndex = phi->succ(0) == pre ? 0 : 1;
  const size_t latchIndex = 1 - preIndex;
  int64_t start = 0, step = 0;
  if (!constantI32(phi->operand(preIndex), start) || start != 0 ||
      phi->operand(latchIndex) == nullptr || !phi->operand(latchIndex)->isInst())
    return false;
  const auto* inc = static_cast<const Instruction*>(phi->operand(latchIndex));
  if (inc->parent() != latch || inc->op() != Opcode::Add ||
      inc->numOperands() != 2 || inc->operand(0) != phi ||
      !constantI32(inc->operand(1), step) || step != 1)
    return false;
  return true;
}
const GlobalAddr* baseGlobal(const Value* value) {
  while (value != nullptr && value->isInst()) {
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() != Opcode::GEP || inst->numOperands() < 1) return nullptr;
    value = inst->operand(0);
  }
  return value != nullptr && value->isGlobalAddr()
             ? static_cast<const GlobalAddr*>(value)
             : nullptr;
}
BasicBlock* skipTrampolines(BasicBlock* block) {
  for (size_t steps = 0; block != nullptr && steps < 8; ++steps) {
    if (block->size() != 1) return block;
    Instruction* term = block->terminator();
    if (term == nullptr || term->op() != Opcode::Br || term->numSuccs() != 1)
      return block;
    BasicBlock* next = term->succ(0);
    if (next == nullptr || next == block) return block;
    block = next;
  }
  return block;
}
bool indexMatches(const Value* value, const Instruction* expected) {
  while (value != nullptr && value->isInst()) {
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() != Opcode::SExt && inst->op() != Opcode::ZExt) break;
    value = inst->operand(0);
  }
  return value == expected;
}
bool canonicalStoreAddress(const Instruction* store, const Instruction* outer,
                           const Instruction* inner, size_t outerBound,
                           size_t innerBound) {
  if (store == nullptr || store->numOperands() != 2) return false;
  const Value* pointer = store->operand(1);
  if (pointer == nullptr || !pointer->isInst()) return false;
  const auto* innerGep = static_cast<const Instruction*>(pointer);
  if (innerGep->op() != Opcode::GEP || innerGep->numOperands() != 2 ||
      innerGep->srcElemType() != typePool().i32())
    return false;
  if (!indexMatches(innerGep->operand(1), inner)) return false;
  const Value* outerValue = innerGep->operand(0);
  if (outerValue == nullptr || !outerValue->isInst()) return false;
  const auto* outerGep = static_cast<const Instruction*>(outerValue);
  if (outerGep->op() != Opcode::GEP || outerGep->numOperands() != 2 ||
      outerGep->srcElemType() == nullptr ||
      outerGep->srcElemType()->kind != TypeKind::Array ||
      outerGep->srcElemType()->elem != typePool().i32())
    return false;
  if (!indexMatches(outerGep->operand(1), outer)) return false;
  const Value* base = outerGep->operand(0);
  if (base == nullptr || !base->isGlobalAddr()) return false;
  const auto* global = static_cast<const GlobalAddr*>(base);
  const Type* object = global->type() == nullptr ? nullptr : global->type()->elem;
  if (object == nullptr || object->kind != TypeKind::Array ||
      object->elem != outerGep->srcElemType() ||
      static_cast<size_t>(object->len) < outerBound)
    return false;
  return static_cast<size_t>(outerGep->srcElemType()->len) >= innerBound;
}
bool matchNest(const Function& function, ReductionNest& nest) {
  for (BasicBlock* outer : function.blocks()) {
    if (outer->size() != 3 || outer->at(0)->op() != Opcode::Phi ||
        outer->at(1)->op() != Opcode::ICmp || outer->at(2)->op() != Opcode::Br ||
        outer->at(2)->numSuccs() != 2)
      continue;
    Instruction* outerPhi = outer->at(0);
    BasicBlock* innerEntry = outer->at(2)->succ(0);
    BasicBlock* inner = skipTrampolines(innerEntry);
    BasicBlock* exit = outer->at(2)->succ(1);
    BasicBlock* innerLatch = nullptr;
    if (inner == nullptr || inner->parent() != &function || inner->size() != 3 ||
        inner->at(0)->op() != Opcode::Phi || inner->at(2)->op() != Opcode::Br ||
        inner->at(2)->numSuccs() != 2)
      continue;
    BasicBlock* body = inner->at(2)->succ(0);
    BasicBlock* outerLatch = skipTrampolines(inner->at(2)->succ(1));
    if (body == nullptr || outerLatch == nullptr ||
        body->parent() != &function || body->size() < 3 ||
        body->terminator() == nullptr || body->terminator()->op() != Opcode::Br ||
        body->terminator()->numSuccs() != 1 ||
        outerLatch->terminator() == nullptr ||
        outerLatch->terminator()->op() != Opcode::Br ||
        outerLatch->terminator()->numSuccs() != 1 ||
        outerLatch->terminator()->succ(0) != outer)
      continue;
    innerLatch = body->terminator()->succ(0);
    if (innerLatch == nullptr || innerLatch->terminator() == nullptr ||
        innerLatch->terminator()->op() != Opcode::Br ||
        innerLatch->terminator()->numSuccs() != 1 ||
        innerLatch->terminator()->succ(0) != inner ||
        inner->at(2)->operand(0) != inner->at(1) ||
        outer->at(2)->operand(0) != outer->at(1))
      continue;
    BasicBlock* pre = nullptr;
    for (BasicBlock* pred : outer->preds())
      if (pred != outerLatch) {
        if (pre != nullptr) { pre = nullptr; break; }
        pre = pred;
      }
    if (pre == nullptr || outer->preds().size() != 2 ||
        inner->preds().size() != 2 || body->preds().size() != 1 ||
        innerLatch->preds().size() != 1 || outerLatch->preds().size() != 1 ||
        innerLatch->size() != 2 || outerLatch->size() != 2 ||
        pre->terminator() == nullptr || pre->terminator()->op() != Opcode::Br ||
        pre->terminator()->numSuccs() != 1 ||
        pre->terminator()->succ(0) != outer)
      continue;
    for (Instruction* inst : exit->insts())
      if (inst->op() == Opcode::Phi) {
        pre = nullptr;
        break;
      }
    if (pre == nullptr) continue;
    Instruction* innerPhi = inner->at(0);
    BasicBlock* innerPre = innerPhi->succ(0) == innerLatch ? innerPhi->succ(1)
                                                          : innerPhi->succ(0);
    if (!matchInduction(outerPhi, outer->at(1), nest.outerBound, pre, outerLatch) ||
        !matchInduction(innerPhi, inner->at(1), nest.innerBound, innerPre,
                        innerLatch))
      continue;
    std::vector<Instruction*> loads;
    Instruction* store = nullptr;
    for (Instruction* inst : body->insts()) {
      if (inst->op() == Opcode::Load) {
        loads.push_back(inst);
      } else if (inst->op() == Opcode::Store) {
        if (store != nullptr) { loads.clear(); break; }
        store = inst;
      } else if (inst->op() == Opcode::Call || inst->op() == Opcode::Alloca ||
                 inst->op() == Opcode::Phi) {
        loads.clear();
        break;
      }
    }
    if (loads.empty() || store == nullptr || store->numOperands() != 2)
      continue;
    const GlobalAddr* storeBase = baseGlobal(store->operand(1));
    if (storeBase == nullptr) continue;
    size_t sameAsStore = 0;
    bool basesValid = true;
    for (Instruction* load : loads) {
      if (load->numOperands() != 1) { basesValid = false; break; }
      const GlobalAddr* base = baseGlobal(load->operand(0));
      if (base == nullptr) { basesValid = false; break; }
      if (base == storeBase) ++sameAsStore;
    }
    if (!basesValid) continue;
    bool map = sameAsStore == 0;
    if (!map) {
      if (sameAsStore != 1 || loads.size() != 1) continue;
      Instruction* load = loads.front();
      // Associativity is not enough for shifted accesses such as
      // a[i][j+1] = a[i+1][j] + 1; the accumulator address must be identical.
      if (load->operand(0) != store->operand(1)) continue;
      if (!store->operand(0)->isInst()) continue;
      Instruction* add = const_cast<Instruction*>(
          static_cast<const Instruction*>(store->operand(0)));
      if (add->op() != Opcode::Add || add->numOperands() != 2 ||
          (add->operand(0) != load && add->operand(1) != load) ||
          load->useCount() != 1 || load->uses()[0].user != add)
        continue;
    } else {
      if (!canonicalStoreAddress(store, outerPhi, inner->at(0),
                                 static_cast<size_t>(nest.outerBound),
                                 static_cast<size_t>(nest.innerBound)))
        continue;
    }
    bool pure = true;
    for (Instruction* inst : body->insts()) {
      if (inst->op() == Opcode::Load || inst->op() == Opcode::Store ||
          inst == body->terminator()) continue;
      if (inst->op() == Opcode::Call || inst->op() == Opcode::Alloca ||
          inst->op() == Opcode::Phi || inst->op() == Opcode::LLVMMemCpy ||
          inst->op() == Opcode::LLVMMemSet) { pure = false; break; }
    }
    if (!pure) continue;
    std::vector<Instruction*> bodyInsts;
    for (Instruction* inst : body->insts())
      if (inst != body->terminator()) bodyInsts.push_back(inst);
    bool cloneable = true;
    for (Instruction* inst : bodyInsts)
      for (size_t i = 0; i < inst->numOperands(); ++i) {
        const Value* operand = inst->operand(i);
        if (operand == outerPhi || operand == inner->at(0) || operand->isConstant() ||
            operand->isGlobalAddr() || operand->isArgument())
          continue;
        if (std::find(bodyInsts.begin(), bodyInsts.end(), operand) == bodyInsts.end()) {
          cloneable = false;
          break;
        }
      }
    if (!cloneable) continue;
    // The original nest becomes unreachable; no loop-defined value may be
    // consumed outside it, or the new clone would break SSA uses.
    bool liveOut = false;
    for (BasicBlock* block : {outer, inner, body, innerLatch, outerLatch})
      for (Instruction* inst : block->insts())
        for (const Use& use : inst->uses()) {
          BasicBlock* userBlock = use.user == nullptr ? nullptr : use.user->parent();
          if (userBlock == nullptr || (userBlock != outer && userBlock != inner &&
                                       userBlock != body && userBlock != innerLatch &&
                                       userBlock != outerLatch)) {
            liveOut = true;
            break;
          }
        }
    if (liveOut) continue;
    ReductionNest candidate;
    candidate.pre = pre; candidate.outer = outer; candidate.inner = inner;
    candidate.body = body; candidate.innerLatch = innerLatch;
    candidate.outerLatch = outerLatch; candidate.exit = exit;
    candidate.outerPhi = outerPhi; candidate.innerPhi = inner->at(0);
    candidate.outerCmp = outer->at(1); candidate.innerCmp = inner->at(1);
    candidate.outerBound = nest.outerBound; candidate.innerBound = nest.innerBound;
    candidate.global = const_cast<GlobalAddr*>(storeBase);
    candidate.map = map;
    nest = candidate;
    return true;
  }
  return false;
}
Instruction* insertBeforeTerm(Module& module, IRMutator& mutator, BasicBlock* block,
                              Instruction* inst) {
  if (mutator.insertInstruction(block, block->size() - 1, inst) != MutationResult::Applied) {
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
Instruction* clone(Module& module, IRMutator& mutator, BasicBlock* block,
                   size_t& index, const Instruction* inst,
                   const std::unordered_map<const Value*, Value*>& mapping) {
  Instruction* copy = module.createInst(inst->op(), inst->type(), inst->loc());
  copy->setIPred(inst->ipred());
  copy->setFPred(inst->fpred());
  copy->setSrcElemType(inst->srcElemType());
  for (size_t i = 0; i < inst->numOperands(); ++i) {
    const Value* operand = inst->operand(i);
    const auto it = mapping.find(operand);
    if (it != mapping.end()) copy->addOperand(it->second);
    else if (operand != nullptr && (operand->isConstant() || operand->isGlobalAddr() ||
                                    operand->isArgument()))
      copy->addOperand(const_cast<Value*>(operand));
    else {
      mutator.discardDetached({copy});
      return nullptr;
    }
  }
  if (mutator.insertInstruction(block, index, copy) != MutationResult::Applied) {
    mutator.discardDetached({copy});
    return nullptr;
  }
  ++index;
  return copy;
}
}  // namespace
LoopBlockingPass::LoopBlockingPass() : FunctionPass("loop-blocking") {}
PassResult LoopBlockingPass::run(Function& function, PassContext& context) {
  PassResult result;
  ReductionNest nest;
  if (!matchNest(function, nest)) return result;
  const auto frequencies = context.analyses().get<BlockFrequencyAnalysis>(function);
  if (!frequencies.isCurrent() || function.entry() == nullptr) return result;
  const double entry = frequencies.get()->frequency(function.entry()->index());
  if (entry <= 0.0) return result;
  BlockingContext planning;
  planning.outerTrip = static_cast<size_t>(nest.outerBound);
  planning.innerTrip = static_cast<size_t>(nest.innerBound);
  planning.outerStride = planning.innerTrip;
  planning.innerStride = 1;
  planning.elementBytes = 4;
  planning.hotness = frequencies.get()->frequency(nest.outer->index()) / entry;
  planning.dependenceFree = true;  // integer-add reduction is associative
  const BlockingPlan plan = planBlocking(planning);
  if (!plan.block || plan.tileOuter < 8 || plan.tileInner < 8) return result;
  // A single full-extent tile degenerates to the original/wrong order. Require
  // both dimensions to be partial so this transform really produces a 2D
  // blocked permutation.
  if (plan.tileOuter >= planning.outerTrip || plan.tileInner >= planning.innerTrip)
    return result;
  const int64_t tileOuter = static_cast<int64_t>(plan.tileOuter);
  const int64_t tileInner = static_cast<int64_t>(plan.tileInner);
  if (nest.outerBound > std::numeric_limits<int32_t>::max() - tileOuter ||
      nest.innerBound > std::numeric_limits<int32_t>::max() - tileInner)
    return result;
  Module& module = context.analyses().module();
  IRMutator& mutator = context.mutator();
  CFGEditor& cfg = context.cfg();
  const std::vector<std::string> names{"T1", "T2", "I", "J", "NB", "LJ", "LI", "LT2", "LT1"};
  std::unordered_map<std::string, BasicBlock*> blocksByName;
  for (const std::string& name : names) {
    BasicBlock* block = module.createBlock(&function);
    Instruction* placeholder = createUnreachable({});
    module.ownInst(placeholder);
    block->addInst(placeholder);
    blocksByName[name] = block;
  }
  BasicBlock* t1 = blocksByName["T1"]; BasicBlock* t2 = blocksByName["T2"];
  BasicBlock* i = blocksByName["I"]; BasicBlock* j = blocksByName["J"];
  BasicBlock* nb = blocksByName["NB"]; BasicBlock* lj = blocksByName["LJ"];
  BasicBlock* li = blocksByName["LI"]; BasicBlock* lt2 = blocksByName["LT2"];
  BasicBlock* lt1 = blocksByName["LT1"];
  auto fail = [] { PassResult p; p.status = PassStatus::Failed;
                   p.diagnostic = "loop-blocking: rewrite rejected"; return p; }();
  if (cfg.redirectEdge(nest.pre, 0, t1, {}) != MutationResult::Applied) return fail;
  Instruction* i0 = createPhi(typePool().i32(), {module.getIntConst(typePool().i32(), 0)},
                              {nest.pre}, {});
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
  Instruction* iEnd = createSelect(i0PlusCmp, i0Plus,
                                   module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, iEnd) == nullptr) return fail;
  Instruction* i0Cmp = createICmp(IPred::Slt, i0,
                                  module.getIntConst(typePool().i32(), nest.outerBound), {});
  if (insertBeforeTerm(module, mutator, t1, i0Cmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t1, createCondBr(i0Cmp, t2, nest.exit, {})) !=
      MutationResult::Applied) return fail;
  Instruction* j0 = createPhi(typePool().i32(), {module.getIntConst(typePool().i32(), 0)},
                              {t1}, {});
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
  Instruction* jEnd = createSelect(j0PlusCmp, j0Plus,
                                   module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, jEnd) == nullptr) return fail;
  Instruction* j0Cmp = createICmp(IPred::Slt, j0,
                                  module.getIntConst(typePool().i32(), nest.innerBound), {});
  if (insertBeforeTerm(module, mutator, t2, j0Cmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, t2, createCondBr(j0Cmp, i, lt1, {})) !=
      MutationResult::Applied) return fail;
  Instruction* ii = createPhi(typePool().i32(), {i0}, {t2}, {});
  if (cfg.insertPhi(i, ii) != MutationResult::Applied) {
    mutator.discardDetached({ii});
    return fail;
  }
  Instruction* iCmp = createICmp(IPred::Slt, ii, iEnd, {});
  if (insertBeforeTerm(module, mutator, i, iCmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, i, createCondBr(iCmp, j, lt2, {})) !=
      MutationResult::Applied) return fail;
  Instruction* jj = createPhi(typePool().i32(), {j0}, {i}, {});
  if (cfg.insertPhi(j, jj) != MutationResult::Applied) {
    mutator.discardDetached({jj});
    return fail;
  }
  Instruction* jCmp = createICmp(IPred::Slt, jj, jEnd, {});
  if (insertBeforeTerm(module, mutator, j, jCmp) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, j, createCondBr(jCmp, nb, li, {})) !=
      MutationResult::Applied) return fail;
  std::unordered_map<const Value*, Value*> mapping;
  mapping[nest.outerPhi] = ii;
  mapping[nest.innerPhi] = jj;
  size_t index = 0;
  for (Instruction* inst : nest.body->insts()) {
    if (inst == nest.body->terminator()) continue;
    Instruction* copy = clone(module, mutator, nb, index, inst, mapping);
    if (copy == nullptr) return fail;
    mapping[inst] = copy;
  }
  if (replaceTerm(cfg, mutator, nb, createBr(lj, {})) != MutationResult::Applied) return fail;
  Instruction* jNext = createBin(Opcode::Add, jj,
                                 module.getIntConst(typePool().i32(), 1), {});
  if (insertBeforeTerm(module, mutator, lj, jNext) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lj, createBr(j, {}), {{jNext}}) != MutationResult::Applied)
    return fail;
  Instruction* iNext = createBin(Opcode::Add, ii,
                                 module.getIntConst(typePool().i32(), 1), {});
  if (insertBeforeTerm(module, mutator, li, iNext) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, li, createBr(i, {}), {{iNext}}) != MutationResult::Applied)
    return fail;
  Instruction* j0Next = createBin(Opcode::Add, j0,
                                  module.getIntConst(typePool().i32(), tileInner), {});
  if (insertBeforeTerm(module, mutator, lt2, j0Next) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lt2, createBr(t2, {}), {{j0Next}}) != MutationResult::Applied)
    return fail;
  Instruction* i0Next = createBin(Opcode::Add, i0,
                                  module.getIntConst(typePool().i32(), tileOuter), {});
  if (insertBeforeTerm(module, mutator, lt1, i0Next) == nullptr) return fail;
  if (replaceTerm(cfg, mutator, lt1, createBr(t1, {}), {{i0Next}}) != MutationResult::Applied)
    return fail;
  // The original nest is now unreachable. Leaving its phis behind would make
  // the verifier reject unreachable phi definitions; replace their uses with a
  // neutral constant and erase them. The old blocks remain valid dead code.
  Constant* zero = module.getIntConst(typePool().i32(), 0);
  if (mutator.replaceUses(nest.outerPhi, zero) != MutationResult::Applied ||
      cfg.erasePhi(nest.outerPhi) != MutationResult::Applied ||
      mutator.replaceUses(nest.innerPhi, zero) != MutationResult::Applied ||
      cfg.erasePhi(nest.innerPhi) != MutationResult::Applied)
    return fail;
  if (nest.map) ++result.stats.counters["blocked-maps"];
  else ++result.stats.counters["blocked-reductions"];
  result.stats.counters["tile-outer"] = static_cast<long long>(tileOuter);
  result.stats.counters["tile-inner"] = static_cast<long long>(tileInner);
  result.status = PassStatus::Applied;
  return result;
}
}}  // namespace sysy::flat
