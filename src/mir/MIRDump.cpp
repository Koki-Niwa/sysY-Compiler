#include "mir/MIRInternal.h"
#include <sstream>
#include <string>
namespace sysy { namespace mir {
namespace {
const char* typeName(Type type) {
  switch (type) {
    case Type::I32: return "i32";
    case Type::I64: return "i64";
    case Type::F32: return "f32";
    case Type::Void: return "void";
  }
  return "?";
}
const char* classPrefix(RegClass cls) { return cls == RegClass::GPR ? "r" : "f"; }
std::string regName(const Reg& reg) {
  return std::string(classPrefix(reg.cls)) + std::to_string(reg.id) + ":" +
         typeName(reg.type);
}
std::string operandName(const Operand& operand) {
  switch (operand.kind) {
    case OperandKind::Reg: return regName(operand.reg);
    case OperandKind::Imm: return std::to_string(operand.imm);
    case OperandKind::Frame: return "frame" + std::to_string(operand.index);
    case OperandKind::Block: return "B" + std::to_string(operand.index);
  }
  return "?";
}
}

std::string dump(const Module& module) {
  std::ostringstream out;
  out << "mir " << module.name << "\n";
  for (const Function& function : module.functions) {
    out << "func " << function.name << " frame=" << function.frameSlots << "\n";
    for (const FrameObject& object : function.frameObjects)
      out << "frameobj " << object.id << " size=" << object.size
          << " align=" << object.align << " spill=" << (object.spill ? 1 : 0)
          << "\n";
    for (const Block& block : function.blocks) {
      out << "B" << block.id << ":\n";
      for (const Instr& inst : block.instructions) {
        const detail::OpcodeDescriptor* descriptor = detail::opcodeDescriptor(inst.opcode);
        out << "  " << (descriptor == nullptr ? "?" : descriptor->spelling);
        if (inst.def) out << " " << regName(*inst.def);
        for (const Operand& operand : inst.operands)
          out << " " << operandName(operand);
        if (inst.mem.valid) {
          out << " mem[";
          if (inst.mem.frameBase)
            out << "frame" << inst.mem.frame;
          else
            out << regName(inst.mem.base);
          out << "+" << inst.mem.offset << ":" << inst.mem.size;
          if (inst.mem.aliasClass != 0) out << ":alias=" << inst.mem.aliasClass;
          if (inst.mem.isVolatile) out << ":volatile";
          out << "]";
        }
        for (const RegMask& mask : inst.clobbers)
          out << " clobber=" << (mask.cls == RegClass::GPR ? "g" : "f")
              << ":0x" << std::hex << mask.bits << std::dec;
        out << "\n";
      }
    }
  }
  return out.str();
}
}}  // namespace sysy::mir
