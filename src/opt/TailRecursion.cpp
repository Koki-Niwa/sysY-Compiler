#include "opt/TailRecursion.h"
#include <unordered_map>
#include <vector>
#include "analysis/TailCallAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
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
    } else if (operand != nullptr && (operand->isConstant() || operand->isGlobalAddr())) {
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

TailRecursionEliminationPass::TailRecursionEliminationPass()
    : FunctionPass("tco") {}

PassResult TailRecursionEliminationPass::run(Function& function, PassContext& context) {
  PassResult result;
  AnalysisHandle<TailCallResult> analysis =
      context.analyses().get<TailCallAnalysis>(function);
  if (!analysis.isCurrent() || !analysis.get()->hasDirectSelfTail ||
      !analysis.get()->allDirectSelfTailsStackSafe)
    return result;
  if (function.blockCount() != 1) return result;   // S22b scope
  BasicBlock* entry = function.entry();
  Instruction* ret = entry->terminator();
  if (ret == nullptr || ret->op() != Opcode::Ret || ret->numOperands() != 1)
    return result;
  auto* call = static_cast<Instruction*>(ret->operand(0));
  if (call->op() != Opcode::Call || call->callee() != function.name()) return result;
  if (call->numOperands() != function.numParams()) return result;
  for (Instruction* inst : entry->insts())
    if (inst->op() == Opcode::Phi || inst->op() == Opcode::Alloca) return result;
  std::vector<Instruction*> originals;
  for (Instruction* inst : entry->insts())
    if (inst != ret) originals.push_back(inst);
  Module& module = context.analyses().module();
  BasicBlock* loop = module.createBlock(&function);
  // Placeholder terminator so tracked insertions can target the new block.
  Instruction* placeholder = createUnreachable({});
  module.ownInst(placeholder);
  loop->addInst(placeholder);
  std::unordered_map<const Value*, Value*> mapping;
  size_t insertAt = 0;
  std::vector<std::pair<Instruction*, Instruction*>> slots;
  for (size_t i = 0; i < function.numParams(); ++i) {
    const Type* type = function.param(i)->type();
    Instruction* slot = createAlloca(type, {});
    module.ownInst(slot);
    if (context.mutator().insertInstruction(entry, 0, slot) != MutationResult::Applied)
      return result;
    Instruction* store = createStore(type, function.param(i), slot, {});
    module.ownInst(store);
    if (context.mutator().insertInstruction(entry, 1, store) != MutationResult::Applied)
      return result;
    slots.emplace_back(slot, nullptr);
  }
  // Loads at the loop top; uses of parameters in the cloned body map to these.
  for (size_t i = 0; i < function.numParams(); ++i) {
    Instruction* load = createLoad(function.param(i)->type(), slots[i].first, {});
    if (context.mutator().insertInstruction(loop, insertAt, load) !=
        MutationResult::Applied)
      return result;
    ++insertAt;
    mapping[function.param(i)] = load;
    slots[i].second = load;
  }
  for (Instruction* inst : originals) {
    if (inst == call) continue;
    Instruction* copy = clone(module, context.mutator(), loop, insertAt, inst, mapping);
    if (copy == nullptr) return result;
    mapping[inst] = copy;
  }
  // Assign the next argument values to the slots and loop.
  for (size_t i = 0; i < call->numOperands(); ++i) {
    const Value* value = call->operand(i);
    const auto it = mapping.find(value);
    Value* mapped = it == mapping.end() ? const_cast<Value*>(value) : it->second;
    Instruction* store = createStore(function.param(i)->type(), mapped,
                                     slots[i].first, {});
    if (context.mutator().insertInstruction(loop, insertAt, store) !=
        MutationResult::Applied)
      return result;
    ++insertAt;
  }
  if (context.cfg().replaceTerminator(loop, createBr(loop, {})) !=
      MutationResult::Applied)
    return result;
  if (context.cfg().replaceTerminator(entry, createBr(loop, {})) !=
      MutationResult::Applied)
    return result;
  for (size_t i = originals.size(); i-- > 0;) {
    Instruction* inst = originals[i];
    if (inst == call) {
      if (context.mutator().eraseInstruction(inst) != MutationResult::Applied)
        return result;
      continue;
    }
    if (context.mutator().eraseInstruction(inst) != MutationResult::Applied)
      return result;
  }
  ++result.stats.counters["tco.transformed"];
  result.status = PassStatus::Applied;
  return result;
}
}}  // namespace sysy::flat
