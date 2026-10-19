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
Instruction* bin(IRMutator& mutator, BasicBlock* block, size_t& index,
                 Opcode op, Value* left, Value* right) {
  return insert(mutator, block, index, createBin(op, left, right, {}));
}
// floor(a*b / 2^32) for unsigned 32-bit a and b, using i32 ops only.
Instruction* mulhi32(Module& module, IRMutator& mutator, BasicBlock* block,
                     size_t& index, Value* a, Value* b) {
  auto c = [&](int32_t v) { return module.getIntConst(typePool().i32(), v); };
  Instruction* a0 = bin(mutator, block, index, Opcode::And, a, c(0xffff));
  if (!a0) return nullptr;
  Instruction* a1 = bin(mutator, block, index, Opcode::LShr, a, c(16));
  if (!a1) return nullptr;
  Instruction* b0 = bin(mutator, block, index, Opcode::And, b, c(0xffff));
  if (!b0) return nullptr;
  Instruction* b1 = bin(mutator, block, index, Opcode::LShr, b, c(16));
  if (!b1) return nullptr;
  Instruction* p0 = bin(mutator, block, index, Opcode::Mul, a0, b0);
  if (!p0) return nullptr;
  Instruction* p1 = bin(mutator, block, index, Opcode::Mul, a0, b1);
  if (!p1) return nullptr;
  Instruction* p2 = bin(mutator, block, index, Opcode::Mul, a1, b0);
  if (!p2) return nullptr;
  Instruction* p3 = bin(mutator, block, index, Opcode::Mul, a1, b1);
  if (!p3) return nullptr;
  Instruction* t0 = bin(mutator, block, index, Opcode::LShr, p0, c(16));
  if (!t0) return nullptr;
  Instruction* t1 = bin(mutator, block, index, Opcode::And, p1, c(0xffff));
  if (!t1) return nullptr;
  Instruction* t2 = bin(mutator, block, index, Opcode::And, p2, c(0xffff));
  if (!t2) return nullptr;
  Instruction* mid = bin(mutator, block, index, Opcode::Add, t0, t1);
  if (!mid) return nullptr;
  mid = bin(mutator, block, index, Opcode::Add, mid, t2);
  if (!mid) return nullptr;
  Instruction* midHigh = bin(mutator, block, index, Opcode::LShr, mid, c(16));
  if (!midHigh) return nullptr;
  Instruction* p1High = bin(mutator, block, index, Opcode::LShr, p1, c(16));
  if (!p1High) return nullptr;
  Instruction* p2High = bin(mutator, block, index, Opcode::LShr, p2, c(16));
  if (!p2High) return nullptr;
  Instruction* high = bin(mutator, block, index, Opcode::Add, p3, p1High);
  if (!high) return nullptr;
  high = bin(mutator, block, index, Opcode::Add, high, p2High);
  if (!high) return nullptr;
  return bin(mutator, block, index, Opcode::Add, high, midHigh);
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
    const int64_t divisorValue = static_cast<const Constant*>(divisor)->intVal();
    size_t shift = 0;
    if (!powerOfTwo(divisorValue, shift)) {
      // Unsigned magic on |x| (abs fits in 31 bits), then sign restoration.
      const uint64_t magnitude = divisorValue < 0
                                     ? static_cast<uint64_t>(-(divisorValue + 1)) + 1
                                     : static_cast<uint64_t>(divisorValue);
      if (divisorValue == 0 || divisorValue == 1 || divisorValue == -1 ||
          magnitude >= (uint64_t{1} << 31))
        continue;
      const uint64_t magic = ((uint64_t{1} << 32) / magnitude) + 1;
      BasicBlock* block = inst->parent();
      size_t index = indexOf(block, inst);
      Module& module = context.analyses().module();
      const Type* i32 = inst->type();
      auto constant = [&](int32_t value) { return module.getIntConst(i32, value); };
      const Value* x = inst->operand(0);
      Instruction* mask = bin(context.mutator(), block, index,
                              Opcode::AShr, const_cast<Value*>(x), constant(31));
      if (mask == nullptr) continue;
      Instruction* xored = bin(context.mutator(), block, index,
                               Opcode::Xor, const_cast<Value*>(x), mask);
      if (xored == nullptr) continue;
      Instruction* absolute = bin(context.mutator(), block, index,
                                  Opcode::Sub, xored, mask);
      if (absolute == nullptr) continue;
      Instruction* high = mulhi32(module, context.mutator(), block, index,
                                  absolute, constant(static_cast<int32_t>(magic)));
      if (high == nullptr) continue;
      const int32_t magnitude32 = static_cast<int32_t>(magnitude);
      Instruction* product = bin(context.mutator(), block, index,
                                 Opcode::Mul, high, constant(magnitude32));
      if (product == nullptr) continue;
      Instruction* remainder = bin(context.mutator(), block, index,
                                   Opcode::Sub, absolute, product);
      if (remainder == nullptr) continue;
      Instruction* negative = insert(context.mutator(), block, index,
          createICmp(IPred::Slt, remainder, constant(0), inst->loc()));
      if (negative == nullptr) continue;
      Instruction* tooLarge = insert(context.mutator(), block, index,
          createICmp(IPred::Sge, remainder, constant(magnitude32), inst->loc()));
      if (tooLarge == nullptr) continue;
      Instruction* correction = bin(context.mutator(), block, index,
                                    Opcode::Sub, tooLarge, negative);
      if (correction == nullptr) continue;
      Instruction* quotientAbs = bin(context.mutator(), block, index,
                                     Opcode::Add, high, correction);
      if (quotientAbs == nullptr) continue;
      Instruction* signedQuotient = bin(context.mutator(), block, index,
                                        Opcode::Xor, quotientAbs, mask);
      if (signedQuotient == nullptr) continue;
      signedQuotient = bin(context.mutator(), block, index,
                           Opcode::Sub, signedQuotient, mask);
      if (signedQuotient == nullptr) continue;
      if (divisorValue < 0) {
        signedQuotient = bin(context.mutator(), block, index,
                             Opcode::Sub, constant(0), signedQuotient);
        if (signedQuotient == nullptr) continue;
      }
      Instruction* isMin = insert(context.mutator(), block, index,
          createICmp(IPred::Eq, const_cast<Value*>(x), constant(INT32_MIN), inst->loc()));
      if (isMin == nullptr) continue;
      const int32_t minQuotient = static_cast<int32_t>(
          static_cast<int64_t>(INT32_MIN) / static_cast<int64_t>(divisorValue));
      Instruction* selected = insert(context.mutator(), block, index,
          createSelect(isMin, constant(minQuotient), signedQuotient, inst->loc()));
      if (selected == nullptr) continue;
      if (context.mutator().replaceAndErase(inst, selected) == MutationResult::Applied) {
        ++result.stats.counters["sdiv-magic"];
        changed = true;
      }
      continue;
    }
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
