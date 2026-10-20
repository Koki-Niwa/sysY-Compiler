#include "analysis/TailCallAnalysis.h"
#include "analysis/CallGraph.h"
#include "analysis/FunctionSummary.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool valueUsesAlloca(const Value* value, size_t depth = 0) {
  if (value == nullptr || depth > 16) return false;
  if (value->isInst()) {
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() == Opcode::Alloca) return true;
    for (size_t i = 0; i < inst->numOperands(); ++i)
      if (valueUsesAlloca(inst->operand(i), depth + 1)) return true;
  }
  return false;
}
bool isTailPosition(const Instruction* call) {
  if (call == nullptr || call->parent() == nullptr) return false;
  const Instruction* ret = call->parent()->terminator();
  return ret != nullptr && ret->op() == Opcode::Ret && ret->numOperands() == 1 &&
         ret->operand(0) == call;
}
}  // namespace
std::optional<TailCallResult> TailCallAnalysis::run(
    const Function& function, AnalysisManager& manager) {
  AnalysisHandle<FunctionSummaryInfo> summaries =
      manager.get<FunctionSummaryAnalysis>();
  AnalysisHandle<CallGraphInfo> graph = manager.get<CallGraphAnalysis>();
  if (!summaries.isCurrent() || !graph.isCurrent()) return std::nullopt;
  TailCallResult result;
  for (BasicBlock* block : function.blocks()) {
    for (Instruction* inst : block->insts()) {
      if (inst == nullptr || inst->op() != Opcode::Call) continue;
      TailCallInfo info;
      info.call = inst;
      if (!isTailPosition(inst)) {
        info.kind = TailCallKind::NotTail;
        info.reason = "not returned directly";
        result.calls.push_back(info);
        continue;
      }
      const Function* callee = graph.get()->module->findFunction(inst->callee());
      if (callee == &function) {
        info.kind = TailCallKind::DirectSelfTail;
        info.reason = "direct self tail call";
        for (size_t i = 0; i < inst->numOperands(); ++i)
          if (valueUsesAlloca(inst->operand(i))) info.hasStackObject = true;
        info.stackSafe = !info.hasStackObject;
        if (!info.stackSafe) info.reason = "argument may capture a stack object";
        result.hasDirectSelfTail = true;
        result.allDirectSelfTailsStackSafe =
            result.allDirectSelfTailsStackSafe && info.stackSafe;
      } else if (callee != nullptr) {
        const FunctionSummary* summary = nullptr;
        for (const FunctionSummary& candidate : summaries.get()->perFunction)
          if (candidate.function == callee) summary = &candidate;
        info.kind = summary != nullptr && summary->recursive
                        ? TailCallKind::MutualTail
                        : TailCallKind::GeneralTail;
        info.reason = info.kind == TailCallKind::MutualTail
                          ? "mutual recursion; backend decides"
                          : "callee is not this function; backend decides";
        result.calls.push_back(info);
        continue;
      } else {
        info.kind = TailCallKind::GeneralTail;
        info.reason = "unknown/external callee; backend decides";
        result.calls.push_back(info);
        continue;
      }
      result.calls.push_back(info);
    }
  }
  return result;
}
}}  // namespace sysy::flat
