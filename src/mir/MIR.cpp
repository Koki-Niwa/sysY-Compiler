#include "mir/MIR.h"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
namespace sysy { namespace mir {
Operand Operand::makeReg(Reg r) {
  Operand o; o.kind = OperandKind::Reg; o.reg = r; return o;
}
Operand Operand::makeImm(int64_t value) {
  Operand o; o.kind = OperandKind::Imm; o.imm = value; return o;
}
Operand Operand::makeFrame(uint32_t index) {
  Operand o; o.kind = OperandKind::Frame; o.index = index; return o;
}
Operand Operand::makeBlock(uint32_t index) {
  Operand o; o.kind = OperandKind::Block; o.index = index; return o;
}
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
const char* opcodeName(Opcode opcode) {
  switch (opcode) {
    case Opcode::Add: return "add";
    case Opcode::Sub: return "sub";
    case Opcode::Mul: return "mul";
    case Opcode::Div: return "div";
    case Opcode::Copy: return "copy";
    case Opcode::Load: return "load";
    case Opcode::Store: return "store";
    case Opcode::Br: return "br";
    case Opcode::CondBr: return "condbr";
    case Opcode::Ret: return "ret";
    case Opcode::Phi: return "phi";
  }
  return "?";
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
bool isTerminator(Opcode opcode) {
  return opcode == Opcode::Br || opcode == Opcode::CondBr ||
         opcode == Opcode::Ret;
}
bool hasDef(Opcode opcode) {
  return opcode != Opcode::Store && opcode != Opcode::Br &&
         opcode != Opcode::CondBr && opcode != Opcode::Ret;
}
std::vector<std::vector<uint32_t>> successors(const Function& function) {
  std::vector<std::vector<uint32_t>> result(function.blocks.size());
  for (size_t b = 0; b < function.blocks.size(); ++b) {
    const Block& block = function.blocks[b];
    if (block.instructions.empty()) continue;
    const Instr& term = block.instructions.back();
    if (term.opcode == Opcode::Br && term.operands.size() == 1 &&
        term.operands[0].kind == OperandKind::Block)
      result[b].push_back(term.operands[0].index);
    if (term.opcode == Opcode::CondBr && term.operands.size() == 3 &&
        term.operands[1].kind == OperandKind::Block &&
        term.operands[2].kind == OperandKind::Block) {
      result[b].push_back(term.operands[1].index);
      result[b].push_back(term.operands[2].index);
    }
  }
  return result;
}
}  // namespace
std::string dump(const Module& module) {
  std::ostringstream out;
  out << "mir " << module.name << "\n";
  for (const Function& function : module.functions) {
    out << "func " << function.name << " frame=" << function.frameSlots << "\n";
    for (const Block& block : function.blocks) {
      out << "B" << block.id << ":\n";
      for (const Instr& inst : block.instructions) {
        out << "  " << opcodeName(inst.opcode);
        if (inst.def) out << " " << regName(*inst.def);
        for (const Operand& operand : inst.operands)
          out << " " << operandName(operand);
        if (inst.mem.valid)
          out << " mem[" << regName(inst.mem.base) << "+" << inst.mem.offset
              << ":" << inst.mem.size << "]";
        out << "\n";
      }
    }
  }
  return out.str();
}
std::vector<std::string> verify(const Module& module) {
  std::vector<std::string> errors;
  std::set<std::string> functionNames;
  for (const Function& function : module.functions) {
    if (function.name.empty()) errors.push_back("empty function name");
    if (!functionNames.insert(function.name).second)
      errors.push_back("duplicate function " + function.name);
    if (function.blocks.empty()) {
      errors.push_back(function.name + ": no blocks");
      continue;
    }
    std::set<uint32_t> blockIds;
    for (size_t b = 0; b < function.blocks.size(); ++b) {
      const Block& block = function.blocks[b];
      if (b == 0 && block.id != 0) errors.push_back(function.name + ": entry is not B0");
      if (block.id != b) {
        errors.push_back(function.name + ": block ids must be contiguous (B" +
                         std::to_string(b) + " expected)");
        continue;
      }
      if (!blockIds.insert(block.id).second)
        errors.push_back(function.name + ": duplicate block B" + std::to_string(block.id));
      if (block.instructions.empty()) {
        errors.push_back(function.name + ": empty block B" + std::to_string(block.id));
        continue;
      }
      bool terminatorSeen = false;
      for (const Instr& inst : block.instructions) {
        if (terminatorSeen)
          errors.push_back(function.name + ": instruction after terminator");
        if (isTerminator(inst.opcode)) terminatorSeen = true;
        if (inst.opcode == Opcode::Phi && terminatorSeen)
          errors.push_back(function.name + ": phi after terminator");
      }
      if (!terminatorSeen)
        errors.push_back(function.name + ": block B" + std::to_string(block.id) +
                         " has no terminator");
      if (block.instructions.back().opcode == Opcode::Br &&
          (block.instructions.back().operands.size() != 1 ||
           block.instructions.back().operands[0].kind != OperandKind::Block))
        errors.push_back(function.name + ": malformed br");
      if (block.instructions.back().opcode == Opcode::CondBr &&
          (block.instructions.back().operands.size() != 3 ||
           block.instructions.back().operands[1].kind != OperandKind::Block ||
           block.instructions.back().operands[2].kind != OperandKind::Block))
        errors.push_back(function.name + ": malformed condbr");
    }
    const auto succ = successors(function);
    std::vector<std::vector<uint32_t>> preds(function.blocks.size());
    for (size_t b = 0; b < succ.size(); ++b) {
      for (uint32_t target : succ[b]) {
        if (target >= function.blocks.size())
          errors.push_back(function.name + ": branch target out of range");
        else
          preds[target].push_back(static_cast<uint32_t>(b));
      }
    }
    std::map<uint32_t, Reg> defs;
    for (const Block& block : function.blocks) {
      for (const Instr& inst : block.instructions) {
        if (inst.def) {
          if (!hasDef(inst.opcode))
            errors.push_back(function.name + ": unexpected def on terminator");
          if (inst.def->type == Type::Void)
            errors.push_back(function.name + ": void def");
          const auto it = defs.find(inst.def->id);
          if (it != defs.end() && !(it->second == *inst.def))
            errors.push_back(function.name + ": register id reused with different type/class");
          else
            defs[inst.def->id] = *inst.def;
        }
        for (const Operand& operand : inst.operands) {
          if (operand.kind == OperandKind::Reg && operand.reg.id != 0) {
            const auto it = defs.find(operand.reg.id);
            if (it == defs.end())
              errors.push_back(function.name + ": use of undefined register");
            else if (!(it->second == operand.reg))
              errors.push_back(function.name + ": register type/class mismatch");
          }
          if (operand.kind == OperandKind::Frame && operand.index >= function.frameSlots)
            errors.push_back(function.name + ": frame index out of range");
        }
        if (inst.mem.valid) {
          if (inst.mem.size == 0) errors.push_back(function.name + ": zero memory size");
          if (inst.mem.base.id != 0 && defs.find(inst.mem.base.id) == defs.end())
            errors.push_back(function.name + ": undefined memory base");
        }
        if (inst.opcode == Opcode::Phi) {
          if (inst.operands.size() != preds[block.id].size() * 2)
            errors.push_back(function.name + ": phi arity does not match predecessors");
        }
      }
    }
  }
  return errors;
}
}}  // namespace sysy::mir
