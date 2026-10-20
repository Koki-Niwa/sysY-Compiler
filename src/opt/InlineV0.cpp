#include "opt/InlineV0.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/FunctionSummary.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool isArgumentOf(const Value* value, const Function* function) {
  if (value == nullptr || !value->isArgument()) return false;
  const auto* argument = static_cast<const Argument*>(value);
  return argument->owner() == function && argument->index() < function->numParams() &&
         function->param(argument->index()) == argument;
}
bool isModuleValue(const Value* value) {
  return value != nullptr &&
         (value->isConstant() || value->isGlobalAddr());
}
}  // namespace

InlineV0Pass::InlineV0Pass(const TargetCostModel* model, CostContext context,
                           InlineProbe probe, size_t probeBudget)
    : FunctionPass("inline-v0"), model_(model), costContext_(context),
      probe_(std::move(probe)), probeBudget_(probeBudget) {}

PassResult InlineV0Pass::run(Function& function, PassContext& context) {
  Module& module = context.analyses().module();
  IRMutator& mutator = context.mutator();
  PassResult result;
  size_t probesLeft = probeBudget_;
  const TargetCostModel* model = model_ != nullptr ? model_ : nullptr;
  ConservativeCostModel fallback;
  if (model == nullptr) model = &fallback;
  std::vector<Instruction*> calls;
  for (BasicBlock* block : function.blocks())
    for (Instruction* inst : block->insts())
      if (inst != nullptr && inst->op() == Opcode::Call) calls.push_back(inst);
  if (calls.empty()) return result;

  AnalysisHandle<BlockFrequencyInfo> frequencies =
      context.analyses().get<BlockFrequencyAnalysis>(function);
  AnalysisHandle<FunctionSummaryInfo> summaries =
      context.analyses().get<FunctionSummaryAnalysis>();
  if (!frequencies.isCurrent() || !summaries.isCurrent()) {
    result.status = PassStatus::Failed;
    result.diagnostic = "inline-v0 requires block frequencies and function summaries";
    return result;
  }
  // Snapshot analysis results before the first mutation: inlining changes the
  // caller's data flow, which invalidates the handles. The callee summaries and
  // block frequencies remain valid inputs for the rest of this run.
  const BlockFrequencyInfo frequencySnapshot = *frequencies.get();
  const FunctionSummaryInfo summarySnapshot = *summaries.get();
  const double entryFrequency = frequencySnapshot.frequency(function.entry()->index());
  if (entryFrequency <= 0.0) return result;

  for (Instruction* call : calls) {
    Function* callee = module.findFunction(call->callee());
    if (callee == nullptr || callee->isDeclaration() || callee == &function) {
      ++result.stats.counters["declined-recursive-or-external"];
      continue;
    }
    if (callee->blockCount() != 1) {
      ++result.stats.counters["declined-multi-block"];
      continue;
    }
    const FunctionSummary* summary = nullptr;
    for (size_t i = 0; i < summarySnapshot.perFunction.size(); ++i) {
      const FunctionSummary& candidate = summarySnapshot.perFunction[i];
      if (candidate.function == callee) { summary = &candidate; break; }
    }
    if (summary == nullptr || summary->recursive) {
      ++result.stats.counters["declined-recursive-or-external"];
      continue;
    }
    BasicBlock* entry = callee->entry();
    Instruction* terminator = entry != nullptr ? entry->terminator() : nullptr;
    if (terminator == nullptr || terminator->op() != Opcode::Ret) {
      ++result.stats.counters["declined-multi-block"];
      continue;
    }
    bool feasible = true;
    size_t bodySize = 0;
    std::unordered_map<const Value*, bool> defined;
    for (const Argument* argument : callee->params()) defined[argument] = true;
    for (Instruction* inst : entry->insts()) {
      if (inst == terminator) continue;
      if (inst->op() == Opcode::Phi || inst->op() == Opcode::Alloca ||
          inst->op() == Opcode::Unreachable) {
        feasible = false;
        break;
      }
      if (inst->op() == Opcode::Call && inst->callee() == callee->name()) {
        feasible = false;   // self recursion, even in one block
        break;
      }
      for (size_t i = 0; i < inst->numOperands(); ++i) {
        const Value* operand = inst->operand(i);
        if (isModuleValue(operand)) continue;
        if (operand != nullptr && operand->isArgument() &&
            isArgumentOf(operand, callee))
          continue;
        if (operand != nullptr && operand->isInst() &&
            defined.count(operand) != 0)
          continue;
        feasible = false;
        break;
      }
      if (!feasible) break;
      defined[inst] = true;
      ++bodySize;
    }
    if (!feasible) {
      ++result.stats.counters["declined-unsupported-body"];
      continue;
    }
    const double hotness =
        frequencySnapshot.frequency(call->parent()->index()) / entryFrequency;
    double bodyCost = 0.0;
    for (Instruction* inst : entry->insts())
      if (inst != terminator) bodyCost += model->instructionCost(*inst, costContext_);
    const double overhead = model->callOverhead(costContext_);
    InlineRecord record;
    record.call = call;
    record.frequency = hotness;
    record.instructions = bodySize;
    record.codeGrowth = static_cast<double>(bodySize);
    record.latencyDelta = overhead - bodyCost;
    if (hotness < 0.5) {
      record.reason = "cold";
      records_.push_back(record);
      ++result.stats.counters["declined-cold"];
      continue;
    }
    const bool overCap =
        bodySize > model->maxInlineInstructions(costContext_) ||
        bodyCost > overhead + 4.0;
    bool allow = !overCap;
    if (overCap && probe_ && probesLeft > 0 && bodySize <= 32) {
      InlineProbeRequest request;
      request.call = call;
      request.callee = callee;
      request.frequency = hotness;
      request.instructions = bodySize;
      const std::optional<InlineProbeResult> probeResult = probe_(request);
      --probesLeft;
      record.probeUsed = true;
      if (probeResult.has_value()) {
        record.latencyDelta = probeResult->latencyDelta;
        record.codeGrowth = probeResult->codeGrowth;
        allow = probeResult->profitable;
      }
    }
    if (!allow) {
      record.reason = record.probeUsed ? "probe declined" : "cost cap";
      records_.push_back(record);
      ++result.stats.counters["declined-cost"];
      continue;
    }
    if (call->type() != nullptr) {
      if (terminator->numOperands() == 0 ||
          terminator->operand(0) == nullptr ||
          terminator->operand(0)->type() != call->type()) {
        ++result.stats.counters["declined-unsupported-body"];
        continue;
      }
    } else if (terminator->numOperands() != 0) {
      ++result.stats.counters["declined-unsupported-body"];
      continue;
    }

    BasicBlock* block = call->parent();
    size_t insertAt = 0;
    while (insertAt < block->size() && block->at(insertAt) != call) ++insertAt;
    std::unordered_map<const Value*, Value*> mapping;
    for (size_t i = 0; i < callee->numParams(); ++i) {
      if (i >= call->numOperands()) { feasible = false; break; }
      mapping[callee->param(i)] = call->operand(i);
    }
    if (!feasible) {
      ++result.stats.counters["declined-unsupported-body"];
      continue;
    }
    bool failed = false;
    for (Instruction* inst : entry->insts()) {
      if (inst == terminator) continue;
      Instruction* clone = module.createInst(inst->op(), inst->type(), inst->loc());
      clone->setIPred(inst->ipred());
      clone->setFPred(inst->fpred());
      clone->setSrcElemType(inst->srcElemType());
      if (inst->op() == Opcode::Call) {
        clone->setCallee(inst->callee());
        clone->setTailCall(inst->isTailCall());
      }
      for (size_t i = 0; i < inst->numOperands(); ++i) {
        const Value* operand = inst->operand(i);
        if (isModuleValue(operand)) { clone->addOperand(const_cast<Value*>(operand)); continue; }
        const auto it = mapping.find(operand);
        if (it == mapping.end()) { failed = true; break; }
        clone->addOperand(it->second);
      }
      if (failed) break;
      if (mutator.insertInstruction(block, insertAt, clone) != MutationResult::Applied) {
        failed = true;
        break;
      }
      mapping[inst] = clone;
      ++insertAt;
    }
    if (failed) {
      result.status = PassStatus::Failed;
      result.diagnostic = "inline-v0 insertion rejected";
      return result;
    }
    Value* replacement = nullptr;
    if (terminator->numOperands() != 0) {
      const Value* returned = terminator->operand(0);
      if (isModuleValue(returned)) {
        replacement = const_cast<Value*>(returned);
      } else {
        const auto it = mapping.find(returned);
        replacement = it == mapping.end() ? nullptr : it->second;
      }
    }
    const MutationResult finish =
        replacement != nullptr
            ? mutator.replaceAndErase(call, replacement)
            : mutator.eraseInstruction(call);
    if (finish != MutationResult::Applied) {
      result.status = PassStatus::Failed;
      result.diagnostic = "inline-v0 call removal rejected";
      return result;
    }
    record.inlined = true;
    record.reason = record.probeUsed ? "probe approved" : "hot tiny";
    records_.push_back(record);
    ++result.stats.counters["inlined-calls"];
  }
  result.status = context.mutator().summary().changed() ? PassStatus::Applied
                                                        : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
