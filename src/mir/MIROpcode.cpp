#include "mir/MIRInternal.h"

namespace sysy { namespace mir { namespace detail {
namespace {
using OC = OperandConstraint;
using TC = TypeConstraint;
const OpcodeDescriptor kDescriptors[] = {
    {Opcode::Add, "add", true, false, false, false, 2, 2,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndOperands},
    {Opcode::Sub, "sub", true, false, false, false, 2, 2,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndOperands},
    {Opcode::Mul, "mul", true, false, false, false, 2, 2,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndOperands},
    {Opcode::Div, "div", true, false, false, false, 2, 2,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndOperands},
    {Opcode::Copy, "copy", true, false, false, false, 1, 1,
     {OC::RegisterOrImmediate, OC::Register, OC::Register}, TC::DefAndOperands},
    {Opcode::Load, "load", true, false, false, true, 0, 0,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndMemory},
    {Opcode::Store, "store", false, false, false, true, 1, 1,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndMemory},
    {Opcode::Br, "br", false, true, false, false, 1, 1,
     {OC::Block, OC::Register, OC::Register}, TC::None},
    {Opcode::CondBr, "condbr", false, true, false, false, 3, 3,
     {OC::Register, OC::Block, OC::Block}, TC::IntegerCondition},
    {Opcode::Ret, "ret", false, true, false, false, 0, 1,
     {OC::Register, OC::Register, OC::Register}, TC::NonVoidRegister},
    {Opcode::Phi, "phi", true, false, true, false, 0, 0,
     {OC::Register, OC::Register, OC::Register}, TC::DefAndOperands},
};
}  // namespace

const OpcodeDescriptor* opcodeDescriptor(Opcode opcode) {
  for (const OpcodeDescriptor& descriptor : kDescriptors)
    if (descriptor.opcode == opcode) return &descriptor;
  return nullptr;
}

const OpcodeDescriptor* opcodeDescriptor(const std::string& spelling) {
  for (const OpcodeDescriptor& descriptor : kDescriptors)
    if (spelling == descriptor.spelling) return &descriptor;
  return nullptr;
}

bool isIntegerType(Type type) { return type == Type::I32 || type == Type::I64; }

bool isLegalRegister(const Reg& reg) {
  if (reg.type == Type::Void) return false;
  if (isIntegerType(reg.type)) return reg.cls == RegClass::GPR;
  return reg.type == Type::F32 && reg.cls == RegClass::FPR;
}

uint32_t typeSize(Type type) {
  switch (type) {
    case Type::I32: return 4;
    case Type::I64: return 8;
    case Type::F32: return 4;
    case Type::Void: return 0;
  }
  return 0;
}

}}}  // namespace sysy::mir::detail
