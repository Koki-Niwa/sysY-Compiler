#include "opt/LoopInterchange.h"
#include <unordered_set>
#include <vector>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/InterchangePlan.h"
#include "analysis/LoopAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
std::vector<Instruction*> phis(BasicBlock* block) {
  std::vector<Instruction*> out;
  for (Instruction* inst : block->insts())
    if (inst->op() == Opcode::Phi) out.push_back(inst);
  return out;
}
Value* incomingFor(Instruction* phi, BasicBlock* pred) {
  for (size_t i = 0; i < phi->numSuccs(); ++i)
    if (phi->succ(i) == pred) return phi->operand(i);
  return nullptr;
}
bool valuesFor(const std::vector<Instruction*>& list, BasicBlock* pred,
               std::vector<Value*>& out) {
  out.clear();
  for (Instruction* phi : list) {
    Value* value = incomingFor(phi, pred);
    if (value == nullptr) return false;
    out.push_back(value);
  }
  return true;
}
}  // namespace

LoopInterchangePass::LoopInterchangePass() : FunctionPass("loop-interchange") {}

PassResult LoopInterchangePass::run(Function& function, PassContext& context) {
  PassResult result;
  AnalysisHandle<LoopInfoResult> loops = context.analyses().get<LoopInfoAnalysis>(function);
  AnalysisHandle<BlockFrequencyInfo> frequencies =
      context.analyses().get<BlockFrequencyAnalysis>(function);
  if (!loops.isCurrent() || !frequencies.isCurrent() || function.entry() == nullptr)
    return result;
  const double entry = frequencies.get()->frequency(function.entry()->index());
  if (entry <= 0.0) return result;
  for (size_t innerIndex = 0; innerIndex < loops.get()->loops.size(); ++innerIndex) {
    const LoopEntry& inner = loops.get()->loops[innerIndex];
    if (inner.parent < 0) continue;
    const LoopEntry& outer = loops.get()->loops[static_cast<size_t>(inner.parent)];
    BasicBlock* o = function.at(outer.header);
    BasicBlock* i = function.at(inner.header);
    if (o == nullptr || i == nullptr || outer.blocks.size() != 4 ||
        inner.blocks.size() != 2)
      continue;
    // Canonical shape.
    Instruction* oTerm = o->terminator();
    Instruction* iTerm = i->terminator();
    if (oTerm == nullptr || iTerm == nullptr || oTerm->numSuccs() != 2 ||
        iTerm->numSuccs() != 2)
      continue;
    BasicBlock* body = iTerm->succ(0);
    BasicBlock* innerExit = iTerm->succ(1);
    if (oTerm->succ(0) != i || body == nullptr || innerExit == nullptr ||
        body->terminator() == nullptr || body->terminator()->numSuccs() != 1 ||
        body->terminator()->succ(0) != i ||
        innerExit->terminator() == nullptr ||
        innerExit->terminator()->numSuccs() != 1 ||
        innerExit->terminator()->succ(0) != o)
      continue;
    BasicBlock* finalExit = oTerm->succ(1);
    if (finalExit == nullptr || finalExit == body || finalExit == innerExit) continue;
    if (!phis(body).empty() || !phis(innerExit).empty()) continue;
    // Load-only body: reordering is dependence-free.
    bool memoryWrite = false;
    for (Instruction* inst : body->insts())
      if (inst->op() == Opcode::Store || inst->op() == Opcode::Call ||
          inst->op() == Opcode::Alloca)
        memoryWrite = true;
    if (memoryWrite) continue;
    std::vector<BasicBlock*> outside;
    for (BasicBlock* pred : o->preds())
      if (pred != nullptr && pred != innerExit) outside.push_back(pred);
    if (outside.size() != 1) continue;
    BasicBlock* preheader = outside[0];
    if (preheader->terminator() == nullptr ||
        preheader->terminator()->numSuccs() != 1 ||
        preheader->terminator()->succ(0) != o)
      continue;
    const double hotness = frequencies.get()->frequency(o->index()) / entry;
    const InterchangePlan plan = planInterchange(
        {}, true, hotness, 0, 1, {1, 1}, 1, MemoryModel{});
    if (!plan.legal) continue;
    std::vector<Instruction*> oPhis = phis(o);
    std::vector<Instruction*> iPhis = phis(i);
    std::vector<Value*> initI, initO;
    Value* oldIBack = incomingFor(iPhis[0], body);
    Value* oldOBack = incomingFor(oPhis[0], innerExit);
    if (iPhis.size() != 1 || oPhis.size() != 1 || oldIBack == nullptr ||
        oldOBack == nullptr || !valuesFor(iPhis, o, initI) ||
        !valuesFor(oPhis, preheader, initO))
      continue;
    // New outer latch (innerExit) increments the old outer IV; new inner
    // latch (body) increments the old inner IV.
    const Type* type = iPhis[0]->type();
    Instruction* newIBack = createBin(Opcode::Add, iPhis[0],
                                      context.analyses().module().getIntConst(type, 1), {});
    if (context.mutator().insertInstruction(innerExit, innerExit->size() - 1,
                                            newIBack) != MutationResult::Applied)
      continue;
    Instruction* newOBack = createBin(Opcode::Add, oPhis[0],
                                      context.analyses().module().getIntConst(type, 1), {});
    if (context.mutator().insertInstruction(body, body->size() - 1, newOBack) !=
        MutationResult::Applied)
      continue;
    std::vector<Value*> backI{newIBack};
    std::vector<Value*> backO{newOBack};
    CFGEditor& cfg = context.cfg();
    if (cfg.redirectEdge(preheader, 0, i, initI) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(i, 0, o, initO) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(o, 0, body, {}) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(o, 1, innerExit, {}) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(i, 1, finalExit, {}) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(innerExit, 0, i, backI) != MutationResult::Applied) continue;
    if (cfg.redirectEdge(body, 0, o, backO) != MutationResult::Applied) continue;
    if (!static_cast<Instruction*>(oldIBack)->hasUses())
      context.mutator().eraseInstruction(static_cast<Instruction*>(oldIBack));
    if (!static_cast<Instruction*>(oldOBack)->hasUses())
      context.mutator().eraseInstruction(static_cast<Instruction*>(oldOBack));
    ++result.stats.counters["interchanged"];
    result.stats.counters["line-traffic-before"] +=
        static_cast<long long>(plan.trafficBefore);
    result.stats.counters["line-traffic-after"] +=
        static_cast<long long>(plan.trafficAfter);
    result.status = PassStatus::Applied;
    return result;
  }
  return result;
}
}}  // namespace sysy::flat
