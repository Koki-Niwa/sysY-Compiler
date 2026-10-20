#include "analysis/UnrollAdvisor.h"
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
  if (header == nullptr) return false;
  Instruction* phi = nullptr;
  Instruction* cmp = nullptr;
  for (Instruction* inst : header->insts()) {
    if (inst->op() == Opcode::Phi && phi == nullptr) phi = inst;
    if (inst->op() == Opcode::ICmp) cmp = inst;
  }
  if (phi == nullptr || cmp == nullptr || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || phi->numSuccs() != 2 ||
      cmp->ipred() != IPred::Slt || cmp->operand(0) != phi)
    return false;
  int64_t start = 0, bound = 0;
  if (!constantI32(cmp->operand(1), bound)) return false;
  const Value* back = nullptr;
  for (size_t i = 0; i < phi->numSuccs(); ++i) {
    if (inside(loop, phi->succ(i))) back = phi->operand(i);
    else {
      if (!constantI32(phi->operand(i), start)) return false;
    }
  }
  if (back == nullptr || !back->isInst()) return false;
  const auto* increment = static_cast<const Instruction*>(back);
  if (increment->op() != Opcode::Add || increment->numOperands() != 2 ||
      increment->operand(0) != phi) return false;
  int64_t step = 0;
  if (!constantI32(increment->operand(1), step) || step <= 0) return false;
  trip = start < bound ? static_cast<size_t>((bound - start + step - 1) / step) : 0;
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
