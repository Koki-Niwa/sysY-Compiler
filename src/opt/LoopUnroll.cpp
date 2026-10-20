#include "opt/LoopUnroll.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/LoopAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
struct Induction {
  Instruction* phi = nullptr;
  Instruction* compare = nullptr;
  Instruction* increment = nullptr;
  BasicBlock* preheader = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* exit = nullptr;
  int64_t start = 0;
  int64_t bound = 0;
  int64_t step = 0;
  size_t trip = 0;
};
bool constantI32(const Value* value, int64_t& out) {
  if (value == nullptr || !value->isConstant() || value->type() != typePool().i32())
    return false;
  out = static_cast<const Constant*>(value)->intVal();
  return true;
}
bool inLoop(const LoopEntry& loop, const BasicBlock* block) {
  if (block == nullptr) return false;
  for (size_t index : loop.blocks)
    if (index == block->index()) return true;
  return false;
}
bool matchUnroll(const Function& function, const LoopEntry& loop, Induction& out) {
  if (loop.blocks.size() != 2) return false;
  BasicBlock* header = function.at(loop.header);
  if (header == nullptr || header->size() != 3) return false;
  Instruction* phi = header->at(0);
  Instruction* cmp = header->at(1);
  Instruction* term = header->at(2);
  if (phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || phi->numSuccs() != 2 ||
      cmp->op() != Opcode::ICmp || cmp->ipred() != IPred::Slt ||
      cmp->operand(0) != phi || term->op() != Opcode::Br ||
      term->numSuccs() != 2 || term->operand(0) != cmp)
    return false;
  BasicBlock* body = term->succ(0);
  BasicBlock* exit = term->succ(1);
  if (!inLoop(loop, body) || exit == nullptr || exit->parent() != &function ||
      inLoop(loop, exit) || function.at(loop.blocks[0]) == nullptr ||
      function.at(loop.blocks[1]) == nullptr || body == header)
    return false;
  if (body->terminator() == nullptr || body->terminator()->op() != Opcode::Br ||
      body->terminator()->numSuccs() != 1 ||
      body->terminator()->succ(0) != header)
    return false;
  BasicBlock* preheader = nullptr;
  BasicBlock* latch = nullptr;
  for (size_t i = 0; i < phi->numSuccs(); ++i) {
    BasicBlock* pred = phi->succ(i);
    if (pred == nullptr) return false;
    if (inLoop(loop, pred)) {
      if (latch != nullptr) return false;
      latch = pred;
    } else {
      if (preheader != nullptr) return false;
      preheader = pred;
    }
  }
  if (preheader == nullptr || header->preds().size() != 2 ||
      std::find(header->preds().begin(), header->preds().end(), preheader) ==
          header->preds().end() ||
      std::find(header->preds().begin(), header->preds().end(), latch) ==
          header->preds().end())
    return false;
  Instruction* preTerm = preheader->terminator();
  if (preTerm == nullptr || preTerm->op() != Opcode::Br ||
      preTerm->numSuccs() != 1 || preTerm->succ(0) != header)
    return false;
  for (Instruction* inst : exit->insts())
    if (inst->op() == Opcode::Phi) return false;
  for (Instruction* inst : body->insts())
    if (inst->op() == Opcode::Phi || inst->op() == Opcode::Alloca) return false;
  // The clone re-executes every body definition; reject any live-out before
  // touching IR (exit-value reconstruction is not implemented).
  for (size_t index : loop.blocks)
    for (Instruction* inst : function.at(index)->insts())
      for (const Use& use : inst->uses())
        if (use.user == nullptr || !inLoop(loop, use.user->parent())) return false;
  int64_t start = 0, bound = 0;
  const size_t preIndex = phi->succ(0) == preheader ? 0 : 1;
  const size_t latchIndex = phi->succ(0) == latch ? 0 : 1;
  if (phi->succ(preIndex) != preheader || phi->succ(latchIndex) != latch ||
      !constantI32(phi->operand(preIndex), start) ||
      !constantI32(cmp->operand(1), bound))
    return false;
  const Value* back = phi->operand(latchIndex);
  if (back == nullptr || !back->isInst()) return false;
  const auto* increment = static_cast<const Instruction*>(back);
  if (increment->parent() != latch || increment->op() != Opcode::Add ||
      increment->numOperands() != 2 || increment->operand(0) != phi ||
      increment->useCount() != 1 || increment->uses()[0].user != phi)
    return false;
  int64_t step = 0;
  if (!constantI32(increment->operand(1), step) || step <= 0) return false;
  const int64_t iterations =
      start < bound ? (bound - start + step - 1) / step : 0;
  if (iterations < 2) return false;
  if (iterations > std::numeric_limits<int32_t>::max() ||
      start + iterations * step > std::numeric_limits<int32_t>::max())
    return false;
  const int64_t limit = bound - step;
  if (limit < std::numeric_limits<int32_t>::min()) return false;
  out = Induction{phi, cmp, const_cast<Instruction*>(increment), preheader, body,
                  exit, start, bound, step, static_cast<size_t>(iterations)};
  return true;
}
Instruction* clone(Module& module, IRMutator& mutator, BasicBlock* block,
                   size_t& index, const Instruction* inst,
                   const std::unordered_map<const Value*, Value*>& mapping) {
  Instruction* copy = module.createInst(inst->op(), inst->type(), inst->loc());
  copy->setIPred(inst->ipred());
  copy->setFPred(inst->fpred());
  copy->setSrcElemType(inst->srcElemType());
  if (inst->op() == Opcode::Call) {
    copy->setCallee(inst->callee());
    copy->setTailCall(inst->isTailCall());
  }
  for (size_t i = 0; i < inst->numOperands(); ++i) {
    const Value* operand = inst->operand(i);
    const auto it = mapping.find(operand);
    if (it != mapping.end()) {
      copy->addOperand(it->second);
    } else if (operand != nullptr &&
               (operand->isConstant() || operand->isGlobalAddr())) {
      copy->addOperand(const_cast<Value*>(operand));
    } else {
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
LoopUnrollPass::LoopUnrollPass(UnrollProfile profile)
    : FunctionPass("loop-unroll"), profile_(profile) {}
PassResult LoopUnrollPass::run(Function& function, PassContext& context) {
  PassResult result;
  const auto loops = context.analyses().get<LoopInfoAnalysis>(function);
  const auto frequencies =
      context.analyses().get<BlockFrequencyAnalysis>(function);
  if (!loops.isCurrent() || !frequencies.isCurrent() || function.entry() == nullptr)
    return result;
  const double entry = frequencies.get()->frequency(function.entry()->index());
  if (entry <= 0.0) return result;
  for (const LoopEntry& loop : loops.get()->loops) {
    Induction ind;
    if (!matchUnroll(function, loop, ind)) continue;
    size_t bodyInstructions = 0;
    for (Instruction* inst : ind.body->insts())
      if (inst != ind.body->terminator()) ++bodyInstructions;
    UnrollContext planContext;
    planContext.tripCount = ind.trip;
    planContext.bodyInstructions = bodyInstructions;
    planContext.liveValues = bodyInstructions;
    planContext.hotness = frequencies.get()->frequency(loop.header) / entry;
    const UnrollPlan plan = planUnroll(planContext, profile_);
    if (!plan.unroll || plan.factor < 2) continue;
    Module& module = context.analyses().module();
    IRMutator& mutator = context.mutator();
    CFGEditor& cfg = context.cfg();
    BasicBlock* second = module.createBlock(&function);
    Instruction* placeholder = createUnreachable({});
    module.ownInst(placeholder);
    second->addInst(placeholder);
    // i + step in the second body; the guard proves i+step < bound.
    Instruction* iPlus = createBin(Opcode::Add, ind.phi,
                                   module.getIntConst(typePool().i32(), ind.step), {});
    size_t insertAt = 0;
    if (mutator.insertInstruction(second, insertAt, iPlus) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: i+step rejected"; return p; }();
    ++insertAt;
    std::unordered_map<const Value*, Value*> mapping;
    mapping[ind.phi] = iPlus;
    for (Instruction* inst : ind.body->insts()) {
      if (inst == ind.body->terminator()) continue;
      Instruction* copy = clone(module, mutator, second, insertAt, inst, mapping);
      if (copy == nullptr)
        return [] { PassResult p; p.status = PassStatus::Failed;
                    p.diagnostic = "loop-unroll: body clone rejected"; return p; }();
      mapping[inst] = copy;
    }
    Instruction* iPlus2 = createBin(Opcode::Add, iPlus,
                                    module.getIntConst(typePool().i32(), ind.step), {});
    if (mutator.insertInstruction(second, insertAt, iPlus2) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: i+2step rejected"; return p; }();
    if (cfg.replaceTerminator(second, createBr(ind.phi->parent(), {}),
                              {{iPlus2}}) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: back edge rejected"; return p; }();
    if (cfg.redirectEdge(ind.body, 0, second, {}) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: body redirect rejected"; return p; }();
    // Tail: one final body execution when the pair loop leaves an odd remainder.
    // The even case enters TH with i >= bound and exits without executing TB.
    BasicBlock* tailHeader = module.createBlock(&function);
    BasicBlock* tailBody = module.createBlock(&function);
    Instruction* thPlaceholder = createUnreachable({});
    module.ownInst(thPlaceholder);
    tailHeader->addInst(thPlaceholder);
    Instruction* tbPlaceholder = createUnreachable({});
    module.ownInst(tbPlaceholder);
    tailBody->addInst(tbPlaceholder);
    if (cfg.redirectEdge(ind.phi->parent(), 1, tailHeader, {}) !=
        MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail header edge rejected"; return p; }();
    Instruction* tailPhi = createPhi(typePool().i32(), {ind.phi}, {ind.phi->parent()}, {});
    if (cfg.insertPhi(tailHeader, tailPhi) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail phi rejected"; return p; }();
    Instruction* tailCmp = createICmp(IPred::Slt, tailPhi,
                                      module.getIntConst(typePool().i32(), ind.bound), {});
    if (mutator.insertInstruction(tailHeader, tailHeader->size() - 1, tailCmp) !=
        MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail compare rejected"; return p; }();
    if (cfg.replaceTerminator(tailHeader, createCondBr(tailCmp, tailBody, ind.exit, {})) !=
        MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail guard rejected"; return p; }();
    size_t tailAt = 0;
    std::unordered_map<const Value*, Value*> tailMapping;
    tailMapping[ind.phi] = tailPhi;
    for (Instruction* inst : ind.body->insts()) {
      if (inst == ind.body->terminator()) continue;
      Instruction* copy = clone(module, mutator, tailBody, tailAt, inst, tailMapping);
      if (copy == nullptr)
        return [] { PassResult p; p.status = PassStatus::Failed;
                    p.diagnostic = "loop-unroll: tail body rejected"; return p; }();
      tailMapping[inst] = copy;
    }
    Instruction* tailNext = createBin(Opcode::Add, tailPhi,
                                      module.getIntConst(typePool().i32(), ind.step), {});
    if (mutator.insertInstruction(tailBody, tailAt, tailNext) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail increment rejected"; return p; }();
    if (cfg.replaceTerminator(tailBody, createBr(tailHeader, {}), {{tailNext}}) !=
        MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: tail back edge rejected"; return p; }();
    const int64_t limit = ind.bound - ind.step;
    if (mutator.rewriteOperand(ind.compare, 1,
                               module.getIntConst(typePool().i32(), limit)) !=
        MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-unroll: bound rewrite rejected"; return p; }();
    ++result.stats.counters["unrolled-loops"];
    result.stats.counters["unroll-factor"] = 2;
    result.status = PassStatus::Applied;
    return result;
  }
  return result;
}
}}  // namespace sysy::flat
