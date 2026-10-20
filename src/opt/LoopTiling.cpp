#include "opt/LoopTiling.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/LoopAnalysis.h"
#include "analysis/TilePlan.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
struct Induction {
  Instruction* phi = nullptr;
  Instruction* compare = nullptr;
  Instruction* increment = nullptr;
  BasicBlock* preheader = nullptr;
  BasicBlock* latch = nullptr;
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
bool matchInduction(const Function& function, const LoopEntry& loop,
                    Induction& out) {
  BasicBlock* header = function.at(loop.header);
  if (header == nullptr || header->size() != 3) return false;
  Instruction* phi = header->at(0);
  Instruction* cmp = header->at(1);
  Instruction* term = header->at(2);
  if (phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || phi->numSuccs() != 2 || term->op() != Opcode::Br ||
      term->numSuccs() != 2 || cmp->op() != Opcode::ICmp ||
      cmp->ipred() != IPred::Slt || cmp->operand(0) != phi ||
      term->operand(0) != cmp)
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
  if (preheader == nullptr || latch == nullptr ||
      header->preds().size() != 2 ||
      std::find(header->preds().begin(), header->preds().end(), preheader) ==
          header->preds().end() ||
      std::find(header->preds().begin(), header->preds().end(), latch) ==
          header->preds().end())
    return false;
  Instruction* preTerm = preheader->terminator();
  if (preTerm == nullptr || preTerm->op() != Opcode::Br ||
      preTerm->numSuccs() != 1 || preTerm->succ(0) != header)
    return false;
  Instruction* latchTerm = latch->terminator();
  if (latchTerm == nullptr || latchTerm->op() != Opcode::Br ||
      latchTerm->numSuccs() != 1 || latchTerm->succ(0) != header)
    return false;
  int64_t start = 0, bound = 0;
  if (!constantI32(phi->operand(phi->succ(0) == preheader ? 0 : 1), start) ||
      !constantI32(cmp->operand(1), bound))
    return false;
  const Value* back = phi->operand(phi->succ(0) == latch ? 0 : 1);
  if (back == nullptr || !back->isInst()) return false;
  const auto* increment = static_cast<const Instruction*>(back);
  if (increment->parent() != latch || increment->op() != Opcode::Add ||
      increment->numOperands() != 2 || increment->operand(0) != phi ||
      increment->useCount() != 1 || increment->uses()[0].user != phi)
    return false;
  int64_t step = 0;
  if (!constantI32(increment->operand(1), step) || step <= 0) return false;
  BasicBlock* exit = term->succ(1);
  if (exit == nullptr || exit->parent() != &function || inLoop(loop, exit))
    return false;
  for (Instruction* inst : exit->insts())
    if (inst->op() == Opcode::Phi) return false;
  const int64_t iterations =
      start < bound ? (bound - start + step - 1) / step : 0;
  if (iterations > std::numeric_limits<int32_t>::max() ||
      start + iterations * step > std::numeric_limits<int32_t>::max())
    return false;
  const size_t trip = static_cast<size_t>(iterations);
  out = Induction{phi, cmp, const_cast<Instruction*>(increment), preheader, latch, exit, start, bound,
                  step, trip};
  return trip >= 2;
}
size_t scalarBytes(const Type* type) {
  if (type == nullptr) return 0;
  if (type->kind == TypeKind::I32 || type->kind == TypeKind::F32) return 4;
  if (type->kind == TypeKind::I64 || type->kind == TypeKind::Ptr) return 8;
  if (type->kind == TypeKind::Array && type->elem != nullptr)
    return static_cast<size_t>(type->len > 0 ? type->len : 1) *
           scalarBytes(type->elem);
  return 0;
}
size_t elementBytes(const LoopEntry& loop, const Function& function) {
  for (size_t index : loop.blocks) {
    BasicBlock* block = function.at(index);
    if (block == nullptr) continue;
    for (Instruction* inst : block->insts()) {
      const Type* type = nullptr;
      if (inst->op() == Opcode::Load) type = inst->type();
      else if (inst->op() == Opcode::Store && inst->numOperands() > 0)
        type = inst->operand(0)->type();
      const size_t bytes = scalarBytes(type);
      if (bytes > 0) return bytes;
    }
  }
  return 0;
}
}  // namespace
LoopTilingPass::LoopTilingPass() : FunctionPass("loop-tiling") {}
PassResult LoopTilingPass::run(Function& function, PassContext& context) {
  PassResult result;
  const auto loops = context.analyses().get<LoopInfoAnalysis>(function);
  const auto frequencies =
      context.analyses().get<BlockFrequencyAnalysis>(function);
  if (!loops.isCurrent() || !frequencies.isCurrent() || function.entry() == nullptr)
    return {};
  const double entry = frequencies.get()->frequency(function.entry()->index());
  if (entry <= 0.0) return {};
  for (const LoopEntry& loop : loops.get()->loops) {
    Induction ind;
    if (!matchInduction(function, loop, ind)) continue;
    const size_t bytes = elementBytes(loop, function);
    if (bytes == 0) continue;
    const double hotness = frequencies.get()->frequency(loop.header) / entry;
    MemoryModel model;
    const TilePlan plan =
        planTiling(ind.trip, 2, bytes, model, hotness);
    if (!plan.tile || plan.tileSize < 2) continue;
    const int64_t tile = static_cast<int64_t>(plan.tileSize);
    CFGEditor& cfg = context.cfg();
    IRMutator& mutator = context.mutator();
    const CFGEditor::SplitResult pre = cfg.splitEdge(ind.preheader, 0);
    if (pre.result != MutationResult::Applied || pre.block == nullptr)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: preheader split failed"; return p; }();
    BasicBlock* tileHeader = pre.block;
    Instruction* tilePhi = createPhi(
        typePool().i32(), {mutator.module().getIntConst(typePool().i32(), ind.start)},
        {ind.preheader}, {});
    if (cfg.insertPhi(tileHeader, tilePhi) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile phi insertion failed"; return p; }();
    if (cfg.rewritePhiValue(ind.phi, tileHeader, tilePhi) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: header phi rewrite failed"; return p; }();
    auto insertBeforeTerminator = [&](BasicBlock* block, Instruction* inst) {
      return mutator.insertInstruction(block, block->size() - 1, inst);
    };
    Instruction* tilePlus = createBin(Opcode::Add, tilePhi,
                                      mutator.module().getIntConst(typePool().i32(), tile), {});
    if (insertBeforeTerminator(tileHeader, tilePlus) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile add rejected"; return p; }();
    Instruction* remaining = createBin(
        Opcode::Sub, mutator.module().getIntConst(typePool().i32(), ind.bound),
        tilePhi, {});
    if (insertBeforeTerminator(tileHeader, remaining) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: remaining subtraction rejected"; return p; }();
    Instruction* endCmp = createICmp(
        IPred::Sge, remaining,
        mutator.module().getIntConst(typePool().i32(), tile), {});
    if (insertBeforeTerminator(tileHeader, endCmp) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: end compare rejected"; return p; }();
    Instruction* tileEnd = createSelect(
        endCmp, tilePlus,
        mutator.module().getIntConst(typePool().i32(), ind.bound), {});
    if (insertBeforeTerminator(tileHeader, tileEnd) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile end rejected"; return p; }();
    Instruction* tileCmp = createICmp(IPred::Slt, tilePhi,
                                      mutator.module().getIntConst(typePool().i32(), ind.bound), {});
    if (insertBeforeTerminator(tileHeader, tileCmp) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile compare rejected"; return p; }();
    if (mutator.rewriteOperand(ind.compare, 1, tileEnd) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: header bound rewrite failed"; return p; }();
    Instruction* tileBranch = createCondBr(tileCmp, ind.phi->parent(), ind.exit,
                                           tileHeader->terminator()->loc());
    if (cfg.replaceTerminator(tileHeader, tileBranch) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile guard rejected"; return p; }();
    const CFGEditor::SplitResult exitSplit = cfg.splitEdge(ind.phi->parent(), 1);
    if (exitSplit.result != MutationResult::Applied || exitSplit.block == nullptr)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: exit split failed"; return p; }();
    BasicBlock* tileLatch = exitSplit.block;
    // The next tile starts at min(bi + T, bound). Reusing tileEnd avoids an
    // i32 overflow in bi + T on the final tile and exits when it equals bound.
    if (cfg.replaceTerminator(tileLatch, createBr(tileHeader, {}),
                              {{tileEnd}}) != MutationResult::Applied)
      return [] { PassResult p; p.status = PassStatus::Failed;
                  p.diagnostic = "loop-tiling: tile back edge rejected"; return p; }();
    ++result.stats.counters["tiled-loops"];
    result.stats.counters["tile-size"] += static_cast<long long>(tile);
    result.status = PassStatus::Applied;
    return result;
  }
  return {};
}
}}  // namespace sysy::flat
