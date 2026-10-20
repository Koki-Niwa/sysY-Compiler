#include "opt/LoopInterchange.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include "analysis/LoopAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool branch(BasicBlock* block, size_t successors) {
  const Instruction* term = block == nullptr ? nullptr : block->terminator();
  return term != nullptr && term->op() == Opcode::Br &&
         term->numSuccs() == successors &&
         term->numOperands() == (successors == 2 ? 1u : 0u);
}
bool predecessors(BasicBlock* block, std::vector<BasicBlock*> expected) {
  std::sort(expected.begin(), expected.end(), [](BasicBlock* a, BasicBlock* b) {
    return a->index() < b->index();
  });
  return block->preds() == expected;
}
bool noPhis(BasicBlock* block) {
  for (Instruction* inst : block->insts())
    if (inst->op() == Opcode::Phi) return false;
  return true;
}
bool constantI32(const Value* value, int64_t& out) {
  if (value == nullptr || !value->isConstant() || value->type() != typePool().i32())
    return false;
  out = static_cast<const Constant*>(value)->intVal();
  return true;
}
Value* incoming(Instruction* phi, BasicBlock* pred) {
  for (size_t n = 0; n < phi->numSuccs(); ++n)
    if (phi->succ(n) == pred) return phi->operand(n);
  return nullptr;
}
struct Induction {
  Instruction* phi = nullptr;
  Instruction* increment = nullptr;
  Value* initial = nullptr;
  Value* step = nullptr;
};
// A finite constant domain also proves rectangularity and rules out wraparound
// extending one of the loops. Empty domains are allowed: no body executes.
bool matchInduction(BasicBlock* header, BasicBlock* entry, BasicBlock* latch,
                    Induction& out) {
  if (header->size() != 3 || !branch(header, 2)) return false;
  Instruction* phi = header->at(0);
  Instruction* cmp = header->at(1);
  if (phi->op() != Opcode::Phi || phi->type() != typePool().i32() ||
      phi->numOperands() != 2 || phi->numSuccs() != 2 ||
      phi->succs() != header->preds() || cmp->op() != Opcode::ICmp ||
      cmp->ipred() != IPred::Slt || cmp->numOperands() != 2 ||
      cmp->operand(0) != phi || header->terminator()->operand(0) != cmp)
    return false;
  Value* init = incoming(phi, entry);
  Value* back = incoming(phi, latch);
  if (back == nullptr || !back->isInst()) return false;
  auto* increment = static_cast<Instruction*>(back);
  if (increment->parent() != latch || increment->op() != Opcode::Add ||
      increment->type() != typePool().i32() || increment->numOperands() != 2 ||
      increment->operand(0) != phi || increment->useCount() != 1 ||
      increment->uses()[0].user != phi)
    return false;
  Value* step = increment->operand(1);
  int64_t start = 0, bound = 0, stride = 0;
  if (!constantI32(init, start) || !constantI32(cmp->operand(1), bound) ||
      !constantI32(step, stride) || stride <= 0)
    return false;
  if (start < bound) {
    const int64_t iterations = (bound - start + stride - 1) / stride;
    if (start + iterations * stride > std::numeric_limits<int32_t>::max())
      return false;
  }
  out = {phi, increment, init, step};
  return true;
}
bool pureBodyInstruction(const Instruction& inst) {
  // Explicitly exclude trapping division, all writes/call-like operations and
  // allocations. Ordinary loads can be permuted because this entire region
  // contains no writes; every body value must stay inside the region.
  switch (inst.op()) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::Select:
    case Opcode::SIToFP: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
    case Opcode::BitCast: case Opcode::GEP: case Opcode::Load:
      return true;
    default: return false;
  }
}
struct Nest {
  BasicBlock *pre, *outer, *inner, *body, *latch, *exit;
  Induction o, i;
};
bool matchNest(Function& function, const LoopEntry& outer,
               const LoopEntry& inner, Nest& n) {
  if (outer.blocks.size() != 4 || inner.blocks.size() != 2) return false;
  n.outer = function.at(outer.header);
  n.inner = function.at(inner.header);
  if (!branch(n.outer, 2) || !branch(n.inner, 2)) return false;
  n.body = n.inner->terminator()->succ(0);
  n.latch = n.inner->terminator()->succ(1);
  n.exit = n.outer->terminator()->succ(1);
  if (n.outer->terminator()->succ(0) != n.inner ||
      !branch(n.body, 1) || !branch(n.latch, 1) || n.exit == nullptr ||
      n.body->terminator()->succ(0) != n.inner ||
      n.latch->terminator()->succ(0) != n.outer)
    return false;
  const std::vector<BasicBlock*> region{n.outer, n.inner, n.body, n.latch};
  for (size_t k = 0; k < region.size(); ++k) {
    if (region[k] == n.exit || region[k]->parent() != &function ||
        std::find(region.begin(), region.begin() + k, region[k]) != region.begin() + k ||
        std::find(outer.blocks.begin(), outer.blocks.end(), region[k]->index()) ==
            outer.blocks.end()) return false;
  }
  if (std::find(inner.blocks.begin(), inner.blocks.end(), n.body->index()) ==
          inner.blocks.end() || n.exit->parent() != &function || !noPhis(n.exit) ||
      n.outer->numPreds() != 2)
    return false;
  n.pre = n.outer->pred(0) == n.latch ? n.outer->pred(1) : n.outer->pred(0);
  if (std::find(region.begin(), region.end(), n.pre) != region.end() ||
      n.pre == n.exit || !branch(n.pre, 1) ||
      n.pre->terminator()->succ(0) != n.outer ||
      !predecessors(n.outer, {n.pre, n.latch}) ||
      !predecessors(n.inner, {n.outer, n.body}) ||
      !predecessors(n.body, {n.inner}) || !predecessors(n.latch, {n.inner}) ||
      !matchInduction(n.outer, n.pre, n.latch, n.o) ||
      !matchInduction(n.inner, n.outer, n.body, n.i) ||
      n.latch->size() != 2 || n.latch->at(0) != n.o.increment)
    return false;
  for (Instruction* inst : n.body->insts())
    if (inst != n.body->terminator() && !pureBodyInstruction(*inst)) return false;
  for (BasicBlock* block : region) {
    for (Instruction* inst : block->insts()) {
      for (const Use& use : inst->uses()) {
        if (use.user == nullptr ||
            std::find(region.begin(), region.end(), use.user->parent()) == region.end())
          return false;
      }
    }
  }
  return true;
}
PassResult commitNest(const Nest& n, PassContext& context) {
  PassResult result;
  auto failed = [&]() {
    result.status = PassStatus::Failed;
    result.diagnostic = "loop-interchange: prevalidated edit rejected";
    return result;
  };
  IRMutator& mutator = context.mutator();
  // The IV operands differ. Step operands may already be identical; only those
  // two edits are explicitly allowed to report NoChange. No analysis handles
  // are consulted after the first write. Unexpected failure aborts the pipeline
  // (ContractViolation after mutation), never a normal unsupported-shape decline.
  if (mutator.rewriteOperand(n.o.increment, 0, n.i.phi) != MutationResult::Applied)
    return failed();
  const MutationResult iStep = n.o.step == n.i.step ? MutationResult::NoChange
                                                  : MutationResult::Applied;
  if (mutator.rewriteOperand(n.o.increment, 1, n.i.step) != iStep) return failed();
  if (mutator.rewriteOperand(n.i.increment, 0, n.o.phi) != MutationResult::Applied)
    return failed();
  if (mutator.rewriteOperand(n.i.increment, 1, n.o.step) != iStep) return failed();
  CFGEditor& cfg = context.cfg();
  if (cfg.redirectEdge(n.pre, 0, n.inner, {n.i.initial}) != MutationResult::Applied)
    return failed();
  if (cfg.redirectEdge(n.inner, 0, n.outer, {n.o.initial}) != MutationResult::Applied)
    return failed();
  if (cfg.redirectEdge(n.outer, 0, n.body, {}) != MutationResult::Applied) return failed();
  if (cfg.redirectEdge(n.outer, 1, n.latch, {}) != MutationResult::Applied) return failed();
  if (cfg.redirectEdge(n.inner, 1, n.exit, {}) != MutationResult::Applied) return failed();
  if (cfg.redirectEdge(n.latch, 0, n.inner, {n.o.increment}) != MutationResult::Applied)
    return failed();
  if (cfg.redirectEdge(n.body, 0, n.outer, {n.i.increment}) != MutationResult::Applied)
    return failed();
  result.status = PassStatus::Applied;
  result.stats.counters["interchanged"] = 1;
  return result;
}
}  // namespace
LoopInterchangePass::LoopInterchangePass() : FunctionPass("loop-interchange") {}
PassResult LoopInterchangePass::run(Function& function, PassContext& context) {
  const auto loops = context.analyses().get<LoopInfoAnalysis>(function);
  if (!loops.isCurrent()) return {};
  for (const LoopEntry& inner : loops.get()->loops) {
    if (inner.parent < 0 || static_cast<size_t>(inner.parent) >= loops.get()->loops.size())
      continue;
    Nest nest{};
    if (matchNest(function, loops.get()->loops[static_cast<size_t>(inner.parent)], inner, nest))
      return commitNest(nest, context);
  }
  return {};
}
}}  // namespace sysy::flat
