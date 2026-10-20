#include "analysis/UnrollAdvisor.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/LoopAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool constantI32(const Value* value, int64_t& out) {
  if (value == nullptr || !value->isConstant() || value->type() != typePool().i32())
    return false;
  out = static_cast<const Constant*>(value)->intVal();
  return true;
}
bool inside(const LoopEntry& loop, const BasicBlock* block) {
  if (block == nullptr) return false;
  for (size_t index : loop.blocks)
    if (index == block->index()) return true;
  return false;
}
bool canonicalTrip(const Function& function, const LoopEntry& loop,
                   size_t& trip, size_t& bodyInstructions) {
  BasicBlock* header = function.at(loop.header);
  if (header == nullptr || header->size() != 3) return false;
  Instruction* phi = header->at(0);
  Instruction* cmp = header->at(1);
  Instruction* term = header->at(2);
  if (phi == nullptr || cmp == nullptr || term == nullptr ||
      phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || phi->numSuccs() != 2 ||
      cmp->op() != Opcode::ICmp || cmp->numOperands() != 2 ||
      cmp->ipred() != IPred::Slt ||
      cmp->operand(0) != phi || term->op() != Opcode::Br ||
      term->numOperands() != 1 || term->numSuccs() != 2 ||
      term->operand(0) != cmp)
    return false;
  if (!inside(loop, term->succ(0)) || term->succ(1) == nullptr ||
      inside(loop, term->succ(1)) || term->succ(1)->parent() != &function)
    return false;
  BasicBlock* exit = term->succ(1);
  for (size_t index : loop.blocks) {
    BasicBlock* block = function.at(index);
    if (block == nullptr) return false;
    Instruction* blockTerm = block->terminator();
    if (blockTerm == nullptr) return false;
    for (size_t successorIndex = 0;
         successorIndex < blockTerm->numSuccs(); ++successorIndex) {
      BasicBlock* successor = blockTerm->succ(successorIndex);
      if (successor == nullptr || successor->parent() != &function ||
          (!inside(loop, successor) &&
           !(block == header && successor == exit)))
        return false;
    }
  }

  BasicBlock* preheader = nullptr;
  BasicBlock* latch = nullptr;
  for (size_t i = 0; i < phi->numSuccs(); ++i) {
    BasicBlock* pred = phi->succ(i);
    if (pred == nullptr || pred->parent() != &function) return false;
    if (inside(loop, pred)) {
      if (latch != nullptr) return false;
      latch = pred;
    } else {
      if (preheader != nullptr) return false;
      preheader = pred;
    }
  }
  if (preheader == nullptr || latch == nullptr ||
      header->preds().size() != 2 ||
      std::find(header->preds().begin(), header->preds().end(), preheader) ==
          header->preds().end() ||
      std::find(header->preds().begin(), header->preds().end(), latch) ==
          header->preds().end())
    return false;
  Instruction* preTerm = preheader->terminator();
  if (preTerm == nullptr || preTerm->op() != Opcode::Br ||
      preTerm->numOperands() != 0 || preTerm->numSuccs() != 1 ||
      preTerm->succ(0) != header)
    return false;
  Instruction* latchTerm = latch->terminator();
  if (latchTerm == nullptr || latchTerm->op() != Opcode::Br ||
      latchTerm->numOperands() != 0 || latchTerm->numSuccs() != 1 ||
      latchTerm->succ(0) != header)
    return false;

  int64_t start = 0, bound = 0;
  if (!constantI32(cmp->operand(1), bound)) return false;
  const size_t preIndex = phi->succ(0) == preheader ? 0 : 1;
  const size_t latchIndex = phi->succ(0) == latch ? 0 : 1;
  if (phi->succ(preIndex) != preheader || phi->succ(latchIndex) != latch ||
      !constantI32(phi->operand(preIndex), start))
    return false;
  const Value* back = phi->operand(latchIndex);
  if (back == nullptr || !back->isInst()) return false;
  const auto* increment = static_cast<const Instruction*>(back);
  if (increment->parent() != latch || increment->op() != Opcode::Add ||
      increment->numOperands() != 2 || increment->operand(0) != phi)
    return false;
  int64_t step = 0;
  if (!constantI32(increment->operand(1), step) || step <= 0) return false;
  const int64_t iterations =
      start < bound ? (bound - start + step - 1) / step : 0;
  if (iterations > std::numeric_limits<int32_t>::max() ||
      start + iterations * step > std::numeric_limits<int32_t>::max())
    return false;
  trip = static_cast<size_t>(iterations);
  bodyInstructions = 0;
  for (size_t index : loop.blocks) {
    BasicBlock* block = function.at(index);
    if (block == nullptr) continue;
    for (Instruction* inst : block->insts()) {
      if (inst->op() == Opcode::Phi || isTerminatorOpcode(inst->op())) continue;
      ++bodyInstructions;
    }
  }
  return true;
}
}  // namespace
std::optional<UnrollAdvice> adviseUnrolls(const Function& function,
                                          AnalysisManager& manager,
                                          const UnrollProfile& profile) {
  AnalysisHandle<BlockFrequencyInfo> frequencies =
      manager.get<BlockFrequencyAnalysis>(function);
  AnalysisHandle<LoopInfoResult> loops = manager.get<LoopInfoAnalysis>(function);
  if (!frequencies.isCurrent() || !loops.isCurrent() || function.entry() == nullptr)
    return std::nullopt;
  const double entry = frequencies.get()->frequency(function.entry()->index());
  if (entry <= 0.0) return std::nullopt;
  UnrollAdvice advice;
  for (const LoopEntry& loop : loops.get()->loops) {
    size_t trip = 0, body = 0;
    if (!canonicalTrip(function, loop, trip, body)) continue;
    const double hotness =
        frequencies.get()->frequency(loop.header) / entry;
    UnrollContext context;
    context.tripCount = trip;
    context.bodyInstructions = body;
    context.liveValues = body;   // conservative upper bound
    context.hotness = hotness;
    UnrollLoopAdvice item;
    item.header = loop.header;
    item.tripCount = trip;
    item.bodyInstructions = body;
    item.hotness = hotness;
    item.plan = planUnroll(context, profile);
    advice.loops.push_back(std::move(item));
  }
  return advice;
}
}}  // namespace sysy::flat
