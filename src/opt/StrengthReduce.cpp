#include "opt/StrengthReduce.h"
#include <cstdint>
#include <vector>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool powerOfTwo(int64_t value, size_t& shift) {
  if (value <= 0 || value > (int64_t{1} << 30)) return false;
  if ((value & (value - 1)) != 0) return false;
  shift = 0;
  while ((int64_t{1} << shift) != value) ++shift;
  return shift > 0;
}
size_t indexOf(BasicBlock* block, Instruction* inst) {
  size_t index = 0;
  while (index < block->size() && block->at(index) != inst) ++index;
  return index;
}
Instruction* insert(IRMutator& mutator, BasicBlock* block, size_t& index,
                    Instruction* inst) {
  if (mutator.insertInstruction(block, index, inst) != MutationResult::Applied)
    return nullptr;
  ++index;
  return inst;
}
}  // namespace

StrengthReducePass::StrengthReducePass() : FunctionPass("strength-reduce") {}

PassResult StrengthReducePass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  std::vector<Instruction*> instructions;
  for (BasicBlock* block : function.blocks())
    for (Instruction* inst : block->insts())
      if (inst != nullptr) instructions.push_back(inst);
  for (Instruction* inst : instructions) {
    if (inst->op() == Opcode::Mul && inst->numOperands() == 2) {
      const Value* lhs = inst->operand(0);
      const Value* rhs = inst->operand(1);
      const Value* constant = nullptr;
      const Value* value = nullptr;
      if (lhs != nullptr && lhs->isConstant()) { constant = lhs; value = rhs; }
      else if (rhs != nullptr && rhs->isConstant()) { constant = rhs; value = lhs; }
      size_t shift = 0;
      if (constant != nullptr &&
          powerOfTwo(static_cast<const Constant*>(constant)->intVal(), shift)) {
        Instruction* shl = context.analyses().module().createInst(
            Opcode::Shl, inst->type(), inst->loc());
        shl->addOperand(const_cast<Value*>(value));
        shl->addOperand(context.analyses().module().getIntConst(
            inst->type(), static_cast<int64_t>(shift)));
        const size_t index = indexOf(inst->parent(), inst);
        if (context.mutator().insertInstruction(inst->parent(), index, shl) ==
                MutationResult::Applied &&
            context.mutator().replaceAndErase(inst, shl) == MutationResult::Applied) {
          ++result.stats.counters["mul-to-shl"];
          changed = true;
        }
        continue;
      }
    }
    if (inst->op() != Opcode::SDiv || inst->numOperands() != 2) continue;
    const Value* divisor = inst->operand(1);
    if (divisor == nullptr || !divisor->isConstant()) continue;
    size_t shift = 0;
    if (!powerOfTwo(static_cast<const Constant*>(divisor)->intVal(), shift)) continue;
    BasicBlock* block = inst->parent();
    size_t index = indexOf(block, inst);
    Module& module = context.analyses().module();
    const Type* type = inst->type();
    Instruction* sign = module.createInst(Opcode::AShr, type, inst->loc());
    sign->addOperand(const_cast<Value*>(inst->operand(0)));
    sign->addOperand(module.getIntConst(type, 31));
    if (insert(context.mutator(), block, index, sign) == nullptr) continue;
    Instruction* mask = module.createInst(Opcode::And, type, inst->loc());
    mask->addOperand(sign);
    mask->addOperand(module.getIntConst(type, static_cast<int64_t>(1u << shift) - 1));
    if (insert(context.mutator(), block, index, mask) == nullptr) continue;
    Instruction* biased = module.createInst(Opcode::Add, type, inst->loc());
    biased->addOperand(const_cast<Value*>(inst->operand(0)));
    biased->addOperand(mask);
    if (insert(context.mutator(), block, index, biased) == nullptr) continue;
    Instruction* shifted = module.createInst(Opcode::AShr, type, inst->loc());
    shifted->addOperand(biased);
    shifted->addOperand(module.getIntConst(type, static_cast<int64_t>(shift)));
    if (context.mutator().insertInstruction(block, index, shifted) !=
        MutationResult::Applied)
      continue;
    if (context.mutator().replaceAndErase(inst, shifted) != MutationResult::Applied)
      continue;
    ++result.stats.counters["sdiv-pow2"];
    changed = true;
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
