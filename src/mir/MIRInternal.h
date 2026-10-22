#ifndef SYSY_MIR_MIR_INTERNAL_H
#define SYSY_MIR_MIR_INTERNAL_H

#include "mir/MIR.h"
#include <array>
#include <string>
#include <vector>

namespace sysy { namespace mir { namespace detail {

enum class OperandConstraint : uint8_t { Register, Immediate, RegisterOrImmediate, Block };
enum class TypeConstraint : uint8_t {
  None,
  DefAndOperands,
  DefAndMemory,
  IntegerCondition,
  NonVoidRegister
};

struct OpcodeDescriptor {
  Opcode opcode;
  const char* spelling;
  bool hasDef;
  bool terminator;
  bool phi;
  bool requiresMemory;
  size_t minOperands;
  size_t maxOperands;
  std::array<OperandConstraint, 3> operands;
  TypeConstraint types;
};

const OpcodeDescriptor* opcodeDescriptor(Opcode opcode);
const OpcodeDescriptor* opcodeDescriptor(const std::string& spelling);
bool isIntegerType(Type type);
bool isLegalRegister(const Reg& reg);
uint32_t typeSize(Type type);

// Used by spill rewriting to validate a Function before and after a scratch
// transformation without duplicating the verifier's structural rules.
std::vector<std::string> verifyFunction(const Function& function);

}}}  // namespace sysy::mir::detail

#endif
