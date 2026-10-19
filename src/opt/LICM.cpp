#include "opt/LICM.h"
#include <algorithm>
#include <unordered_set>
#include <vector>
#include "analysis/DominatorAnalysis.h"
#include "analysis/LoopAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool hoistable(const Instruction& inst) {
  switch (inst.op()) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::Select:
    case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
    case Opcode::BitCast: case Opcode::GEP:
      return true;
    default:
      return false;
  }
}
}  // namespace

LICMPass::LICMPass() : FunctionPass("licm") {}

PassResult LICMPass::run(Function& function, PassContext& context) {
  PassResult result;
  AnalysisHandle<LoopInfoResult> loops = context.analyses().get<LoopInfoAnalysis>(function);
  AnalysisHandle<DominatorInfo> dominators =
      context.analyses().get<DominatorAnalysis>(function);
  if (!loops.isCurrent() || !dominators.isCurrent()) return result;
  std::vector<const LoopEntry*> ordered;
  for (const LoopEntry& loop : loops.get()->loops) ordered.push_back(&loop);
  std::sort(ordered.begin(), ordered.end(), [](const LoopEntry* a, const LoopEntry* b) {
    return a->blocks.size() < b->blocks.size();
  });
  bool changed = false;
  for (const LoopEntry* loop : ordered) {
    BasicBlock* header = function.at(loop->header);
    if (header == nullptr) continue;
    std::unordered_set<size_t> members(loop->blocks.begin(), loop->blocks.end());
    std::vector<BasicBlock*> outside;
    for (BasicBlock* pred : header->preds())
      if (pred != nullptr && members.count(pred->index()) == 0) outside.push_back(pred);
    if (outside.size() != 1) continue;
    BasicBlock* preheader = outside[0];
    Instruction* preTerm = preheader->terminator();
    if (preTerm == nullptr || preTerm->numSuccs() != 1 || preTerm->succ(0) != header)
      continue;
    size_t insertAt = preheader->size() - 1;
    for (size_t round = 0; round < 64; ++round) {
      bool roundChanged = false;
      std::vector<Instruction*> candidates;
      for (size_t blockIndex : loop->blocks) {
        BasicBlock* block = function.at(blockIndex);
        if (block == nullptr) continue;
        for (Instruction* inst : block->insts())
          if (inst != nullptr && inst->parent() == block && hoistable(*inst))
            candidates.push_back(inst);
      }
      for (Instruction* inst : candidates) {
        bool invariant = true;
        for (size_t i = 0; i < inst->numOperands(); ++i) {
          const Value* operand = inst->operand(i);
          if (operand != nullptr && operand->isInst()) {
            const auto* def = static_cast<const Instruction*>(operand);
            BasicBlock* defBlock = def->parent();
            if (defBlock == nullptr) continue;
            if (members.count(defBlock->index()) != 0) { invariant = false; break; }
            if (!dominators.get()->dominates(defBlock, preheader)) { invariant = false; break; }
          }
        }
        if (!invariant) continue;
        Instruction* clone = context.analyses().module().createInst(
            inst->op(), inst->type(), inst->loc());
        clone->setIPred(inst->ipred());
        clone->setFPred(inst->fpred());
        clone->setSrcElemType(inst->srcElemType());
        if (inst->op() == Opcode::Call) clone->setCallee(inst->callee());
        for (size_t i = 0; i < inst->numOperands(); ++i)
          clone->addOperand(const_cast<Value*>(inst->operand(i)));
        if (context.mutator().insertInstruction(preheader, insertAt, clone) !=
            MutationResult::Applied) {
          continue;
        }
        ++insertAt;
        if (context.mutator().replaceUses(inst, clone) != MutationResult::Applied) {
          break;
        }
        if (context.mutator().eraseInstruction(inst) != MutationResult::Applied) break;
        ++result.stats.counters["licm.hoisted"];
        roundChanged = changed = true;
      }
      if (!roundChanged) break;
    }
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
