#include "opt/MatmulBlockingInternal.h"
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include "ir/Module.h"
#include "ir/CFGEditor.h"
#include "analysis/BlockingPlan.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
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
  if (replaceTerm(cfg, mutator, j, createCondBr(jCmp, nest.reduction, li, {}),
                  {{zero, zero}, {}}) !=
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
      if (mutator.rewriteOperand(user, operand, replacement) != MutationResult::Applied)
        return false;
    return true;
  };
  if (!replaceRetained(nest.outerPhi, ip) ||
      !replaceRetained(nest.innerPhi, jp))
    return fail;
  if (replaceTerm(cfg, mutator, nest.reductionExit, createBr(lj, {})) !=
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
