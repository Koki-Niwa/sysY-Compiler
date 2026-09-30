#include "ir/FlatSemantics.h"

#include <cstddef>

namespace sysy {
namespace flat {
namespace {

bool same(const Type* a, const Type* b) { return a != nullptr && a == b; }
bool voidResult(const Instruction& i) {
  return i.type() == nullptr || i.type()->isVoid();
}
bool operandTypes(const Instruction& i, const Type* type) {
  return i.numOperands() == 2 && i.operand(0) != nullptr && i.operand(1) != nullptr &&
         same(i.operand(0)->type(), type) && same(i.operand(1)->type(), type) &&
         same(i.type(), type);
}

bool fail(const Instruction& i, const char* why, std::string& error) {
  error = std::string(opcodeName(i.op())) + ": " + why;
  return false;
}

}  // namespace

bool validateFlatInstruction(const Instruction& i, std::string& error) {
  error.clear();
  const Type* i32 = typePool().i32();
  const Type* i64 = typePool().i64();
  const Type* f32 = typePool().f32();
  const Type* result = i.type();

  // Every operand must be present before an opcode-specific rule dereferences
  // it.  The verifier also reports the value ownership violation separately.
  for (Value* operand : i.operands()) {
    if (operand == nullptr || operand->type() == nullptr) {
      return fail(i, "contains a null or untyped operand", error);
    }
  }
  if (i.op() != Opcode::Phi && !isTerminatorOpcode(i.op()) && i.numSuccs() != 0) {
    return fail(i, "non-terminator instruction cannot carry CFG successors", error);
  }

  switch (i.op()) {
    case Opcode::Nop:
      return fail(i, "parameter placeholder is not an instruction", error);
    case Opcode::ConstantInt:
      return (result == i32 || result == i64) && i.numOperands() == 0
                 ? true
                 : fail(i, "integer constant must have i32 or i64 type", error);
    case Opcode::ConstantFP:
      return result == f32 && i.numOperands() == 0
                 ? true
                 : fail(i, "floating constant must have f32 type", error);
    case Opcode::Ret:
      return voidResult(i) && i.numOperands() <= 1 && i.numSuccs() == 0
                 ? true
                 : fail(i, "return must have void result and at most one operand", error);
    case Opcode::Br:
      if (!voidResult(i)) return fail(i, "branch cannot produce a result", error);
      if (i.numSuccs() == 1 && i.numOperands() == 0) return true;
      if (i.numSuccs() == 2 && i.numOperands() == 1 && i.operand(0)->type() == i32) {
        return true;
      }
      return fail(i, "branch needs one target or two targets with an i32 condition", error);
    case Opcode::Unreachable:
      return voidResult(i) && i.numOperands() == 0 && i.numSuccs() == 0
                 ? true
                 : fail(i, "unreachable takes no operands or successors", error);
    case Opcode::Alloca:
      return i.numOperands() == 0 && i.srcElemType() != nullptr &&
                     !i.srcElemType()->isVoid() && result != nullptr && result->isPtr() &&
                     result->elem == i.srcElemType()
                 ? true
                 : fail(i, "alloca result and object type do not match", error);
    case Opcode::Load:
      return i.numOperands() == 1 && result != nullptr && !result->isVoid() &&
                     i.operand(0)->type()->isPtr() &&
                     i.operand(0)->type()->elem == result
                 ? true
                 : fail(i, "load type does not match typed pointer element", error);
    case Opcode::Store:
      return voidResult(i) && i.numOperands() == 2 && i.operand(1)->type()->isPtr() &&
                     i.operand(0)->type() == i.operand(1)->type()->elem
                 ? true
                 : fail(i, "store value does not match pointer element type", error);
    case Opcode::GEP:
      return i.numOperands() == 2 && i.srcElemType() != nullptr &&
                     !i.srcElemType()->isVoid() && i.operand(0)->type()->isPtr() &&
                     i.operand(1)->type() == i64 && result != nullptr && result->isPtr() &&
                     result->elem == i.srcElemType()
                 ? true
                 : fail(i, "getelementptr address or index type is invalid", error);
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::SDiv:
    case Opcode::SRem: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
      return operandTypes(i, i32) ? true : fail(i, "integer operation requires i32 operands and result", error);
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
      return operandTypes(i, f32) ? true : fail(i, "floating operation requires f32 operands and result", error);
    case Opcode::FNeg:
      return i.numOperands() == 1 && result == f32 && i.operand(0)->type() == f32
                 ? true
                 : fail(i, "floating negation requires an f32 operand and result", error);
    case Opcode::ICmp:
      return i.numOperands() == 2 && result == i32 && i.operand(0)->type() == i32 &&
                     i.operand(1)->type() == i32
                 ? true
                 : fail(i, "integer comparison requires i32 operands and result", error);
    case Opcode::FCmp:
      return i.numOperands() == 2 && result == i32 && i.operand(0)->type() == f32 &&
                     i.operand(1)->type() == f32
                 ? true
                 : fail(i, "floating comparison requires f32 operands and i32 result", error);
    case Opcode::SIToFP:
      return i.numOperands() == 1 && i.operand(0)->type() == i32 && result == f32
                 ? true
                 : fail(i, "sitofp requires i32 to f32", error);
    case Opcode::FPToSI:
      return i.numOperands() == 1 && i.operand(0)->type() == f32 && result == i32
                 ? true
                 : fail(i, "fptosi requires f32 to i32", error);
    case Opcode::FPExt:
      // Flat IR keeps the source type as f32 while the LLVM emitter widens it
      // only at the putf ABI boundary.
      return i.numOperands() == 1 && i.operand(0)->type() == f32 && result == f32
                 ? true
                 : fail(i, "fpext requires the flat f32 representation", error);
    case Opcode::SExt: case Opcode::ZExt:
      return i.numOperands() == 1 && i.operand(0)->type() == i32 && result == i64
                 ? true
                 : fail(i, "integer extension requires i32 to i64", error);
    case Opcode::Trunc:
      return i.numOperands() == 1 && i.operand(0)->type() == i64 && result == i32
                 ? true
                 : fail(i, "trunc requires i64 to i32", error);
    case Opcode::BitCast:
      return i.numOperands() == 1 && i.operand(0)->type()->isPtr() && result != nullptr &&
                     result->isPtr()
                 ? true
                 : fail(i, "bitcast supports pointer to pointer only", error);
    case Opcode::Call:
      // Callee names, signatures known in this module, and putf/intrinsic ABI
      // details belong to the emitter or module verifier.  The local contract
      // is that every argument is a typed value and the result is a value or
      // the explicit void representation.
      return true;
    case Opcode::LLVMMemCpy: case Opcode::LLVMMemSet:
      return voidResult(i) ? true : fail(i, "LLVM memory intrinsics must return void", error);
    case Opcode::Phi:
      if (result == nullptr || result->isVoid() || i.numOperands() == 0 ||
          i.numOperands() != i.numSuccs()) {
        return fail(i, "phi operand and predecessor counts must match", error);
      }
      for (Value* operand : i.operands()) {
        if (operand->type() != result) return fail(i, "phi incoming type does not match result", error);
      }
      return true;
    case Opcode::Select:
      return i.numOperands() == 3 && i.operand(0)->type() == i32 && result != nullptr &&
                     !result->isVoid() && i.operand(1)->type() == result &&
                     i.operand(2)->type() == result
                 ? true
                 : fail(i, "select requires i32 condition and matching value types", error);
    case Opcode::kCount:
      return fail(i, "opcode is outside the flat instruction set", error);
  }
  return fail(i, "opcode is outside the flat instruction set", error);
}

}  // namespace flat
}  // namespace sysy
