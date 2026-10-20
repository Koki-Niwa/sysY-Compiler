#include "opt/LoopRotate.h"
#include <unordered_set>
#include <vector>
#include "analysis/DominatorAnalysis.h"
#include "analysis/LoopAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool pureCondition(const Instruction& inst) {
  switch (inst.op()) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::Select:
    case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
    case Opcode::BitCast: case Opcode::GEP: case Opcode::Load:
      return true;
    default:
      return false;
  }
}
}  // namespace

LoopRotatePass::LoopRotatePass() : FunctionPass("loop-rotate") {}

PassResult LoopRotatePass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (size_t round = 0; round < 8; ++round) {
    AnalysisHandle<LoopInfoResult> loops =
        context.analyses().get<LoopInfoAnalysis>(function);
    AnalysisHandle<DominatorInfo> dominators =
        context.analyses().get<DominatorAnalysis>(function);
    if (!loops.isCurrent() || !dominators.isCurrent()) break;
    bool rotated = false;
    for (const LoopEntry& loop : loops.get()->loops) {
      BasicBlock* header = function.at(loop.header);
      if (header == nullptr || header->terminator() == nullptr ||
          header->terminator()->op() != Opcode::Br ||
          header->terminator()->numSuccs() != 2)
        continue;
      bool hasPhi = false;
      for (Instruction* inst : header->insts())
        if (inst->op() == Opcode::Phi) hasPhi = true;
      if (hasPhi) continue;   // loop-carried phi rotation is S21b follow-up
      std::unordered_set<size_t> members(loop.blocks.begin(), loop.blocks.end());
      std::vector<BasicBlock*> outside;
      for (BasicBlock* pred : header->preds())
        if (pred != nullptr && members.count(pred->index()) == 0) outside.push_back(pred);
      if (outside.size() != 1) continue;
      BasicBlock* preheader = outside[0];
      Instruction* preTerm = preheader->terminator();
      if (preTerm == nullptr || preTerm->numSuccs() != 1 || preTerm->succ(0) != header)
        continue;
      BasicBlock* latch = nullptr;
      size_t backEdges = 0;
      for (size_t index : loop.blocks) {
        BasicBlock* block = function.at(index);
        if (block == nullptr || block == header) continue;
        Instruction* term = block->terminator();
        if (term != nullptr && term->op() == Opcode::Br && term->numSuccs() == 1 &&
            term->succ(0) == header) {
          latch = block;
          ++backEdges;
        }
      }
      if (backEdges != 1 || latch == nullptr) continue;
      BasicBlock* body = header->terminator()->succ(0);
      BasicBlock* exit = header->terminator()->succ(1);
      if (body == nullptr || exit == nullptr || body == exit) continue;
      bool phiTarget = false;
      for (BasicBlock* target : {body, exit})
        for (Instruction* inst : target->insts())
          if (inst->op() == Opcode::Phi) phiTarget = true;
      if (phiTarget) continue;
      const Value* condition = header->terminator()->operand(0);
      if (condition == nullptr) continue;
      Value* cloneValue = const_cast<Value*>(condition);
      if (condition->isInst()) {
        const auto* conditionInst = static_cast<const Instruction*>(condition);
        if (!pureCondition(*conditionInst)) continue;
        bool operandsOk = true;
        for (size_t i = 0; i < conditionInst->numOperands(); ++i) {
          const Value* operand = conditionInst->operand(i);
          if (operand != nullptr && operand->isInst()) {
            const auto* def = static_cast<const Instruction*>(operand);
            if (def->parent() != nullptr &&
                (members.count(def->parent()->index()) != 0 ||
                 !dominators.get()->dominates(def->parent(), latch))) {
              operandsOk = false;
              break;
            }
          }
        }
        if (!operandsOk) continue;
        Instruction* clone = context.analyses().module().createInst(
            conditionInst->op(), conditionInst->type(), conditionInst->loc());
        clone->setIPred(conditionInst->ipred());
        clone->setFPred(conditionInst->fpred());
        clone->setSrcElemType(conditionInst->srcElemType());
        for (size_t i = 0; i < conditionInst->numOperands(); ++i)
          clone->addOperand(const_cast<Value*>(conditionInst->operand(i)));
        const size_t insertAt = latch->size() - 1;
        if (context.mutator().insertInstruction(latch, insertAt, clone) !=
            MutationResult::Applied) {
          context.mutator().discardDetached({clone});
          continue;
        }
        cloneValue = clone;
      }
      Instruction* branch = createCondBr(cloneValue, body, exit, latch->terminator()->loc());
      if (context.cfg().replaceTerminator(latch, branch) != MutationResult::Applied)
        continue;
      ++result.stats.counters["rotated"];
      changed = true;
      rotated = true;
      break;
    }
    if (!rotated) break;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
