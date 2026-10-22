#include "mir/MIRInternal.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sysy { namespace mir { namespace detail {
namespace {
bool operandMatches(const Operand& operand, OperandConstraint constraint) {
  switch (constraint) {
    case OperandConstraint::Register: return operand.kind == OperandKind::Reg;
    case OperandConstraint::Immediate: return operand.kind == OperandKind::Imm;
    case OperandConstraint::RegisterOrImmediate:
      return operand.kind == OperandKind::Reg || operand.kind == OperandKind::Imm;
    case OperandConstraint::Block: return operand.kind == OperandKind::Block;
  }
  return false;
}

bool memoryBoundsKnown(const Function& function, uint32_t frame) {
  return !function.frameObjects.empty() &&
         frame < function.frameObjects.size();
}

bool safeMemoryBounds(const MemOperand& mem, const FrameObject& object) {
  if (mem.offset < 0 || mem.size == 0) return false;
  const int64_t size = static_cast<int64_t>(mem.size);
  if (mem.offset > INT64_MAX - size) return false;
  return mem.offset + size <= static_cast<int64_t>(object.size);
}

bool sameRegisterType(const Reg& left, const Reg& right) {
  return left.type == right.type && left.cls == right.cls;
}
}  // namespace

std::vector<std::string> verifyFunction(const Function& function) {
  std::vector<std::string> errors;
  const std::string prefix = function.name.empty() ? "<anonymous>" : function.name;
  const auto report = [&](const std::string& message) {
    errors.push_back(prefix + ": " + message);
  };
  if (function.blocks.empty()) {
    report("no blocks");
    return errors;
  }
  std::set<uint32_t> frameIds;
  for (size_t index = 0; index < function.frameObjects.size(); ++index) {
    const FrameObject& object = function.frameObjects[index];
    if (object.id != index) report("frame object ids must be contiguous");
    if (!frameIds.insert(object.id).second) report("duplicate frame object");
    if (object.size == 0) report("zero-sized frame object");
    if (object.align == 0 || (object.align & (object.align - 1)) != 0)
      report("frame alignment must be a power of two");
  }

  bool shapeValid = true;
  std::set<uint32_t> blockIds;
  for (size_t index = 0; index < function.blocks.size(); ++index) {
    const Block& block = function.blocks[index];
    if (block.id != index) {
      report("block ids must be contiguous (B" + std::to_string(index) +
             " expected)");
      shapeValid = false;
    }
    if (!blockIds.insert(block.id).second)
      report("duplicate block B" + std::to_string(block.id));
    if (block.instructions.empty()) {
      report("empty block B" + std::to_string(block.id));
      shapeValid = false;
      continue;
    }
    bool terminatorSeen = false;
    bool nonPhiSeen = false;
    for (const Instr& inst : block.instructions) {
      const OpcodeDescriptor* descriptor = opcodeDescriptor(inst.opcode);
      if (descriptor == nullptr) {
        report("unknown opcode");
        shapeValid = false;
        continue;
      }
      if (terminatorSeen) {
        report("instruction after terminator");
        shapeValid = false;
      }
      if (descriptor->terminator) terminatorSeen = true;
      if (descriptor->phi) {
        if (nonPhiSeen) {
          report("phi instructions must form a leading parallel group");
          shapeValid = false;
        }
      } else {
        nonPhiSeen = true;
      }
      if (inst.def.has_value() != descriptor->hasDef) {
        report("opcode definition policy violated");
        shapeValid = false;
      }
      if (inst.def) {
        if (inst.def->id == 0) report("definitions must use nonzero registers");
        if (!isLegalRegister(*inst.def)) report("invalid definition register");
      }
      if (!descriptor->phi &&
          (inst.operands.size() < descriptor->minOperands ||
           inst.operands.size() > descriptor->maxOperands)) {
        report("operand count does not match opcode");
        shapeValid = false;
      }
      const size_t checked = descriptor->phi
          ? 0 : std::min(inst.operands.size(), descriptor->maxOperands);
      for (size_t operand = 0; operand < checked; ++operand) {
        if (!operandMatches(inst.operands[operand], descriptor->operands[operand])) {
          report("operand kind does not match opcode");
          shapeValid = false;
        }
      }
      if (inst.mem.valid != descriptor->requiresMemory) {
        report(descriptor->requiresMemory ? "missing memory operand"
                                           : "unexpected memory operand");
        shapeValid = false;
      }
      if (inst.mem.valid) {
        if (inst.mem.size == 0) report("zero memory size");
        if (inst.mem.frameBase) {
          const uint64_t limit = function.frameObjects.empty()
              ? function.frameSlots : function.frameObjects.size();
          if (inst.mem.frame >= limit) {
            report("memory frame index out of range");
          } else if (memoryBoundsKnown(function, inst.mem.frame) &&
                     !safeMemoryBounds(inst.mem,
                                       function.frameObjects[inst.mem.frame])) {
            report("memory access is outside its frame object");
          } else if (!memoryBoundsKnown(function, inst.mem.frame) &&
                     inst.mem.offset < 0) {
            // Legacy frameSlots carry no object sizes; retain their range
            // contract but reject offsets that cannot be bounded safely.
            report("legacy frame access has negative offset");
          }
        } else if (inst.mem.base.id != 0 &&
                   (!isLegalRegister(inst.mem.base) ||
                    inst.mem.base.cls != RegClass::GPR ||
                    !isIntegerType(inst.mem.base.type))) {
          report("memory base must be an integer GPR");
        }
      }
      if (descriptor->phi) {
        if (!inst.def || inst.def->id == 0) shapeValid = false;
        if (inst.mem.valid) shapeValid = false;
        if (inst.operands.size() % 2 != 0) {
          report("phi operands must be value/block pairs");
          shapeValid = false;
        }
        for (size_t pair = 0; pair + 1 < inst.operands.size(); pair += 2) {
          if (inst.operands[pair].kind != OperandKind::Reg ||
              inst.operands[pair + 1].kind != OperandKind::Block) {
            report("malformed phi value/block pair");
            shapeValid = false;
          }
        }
      }
      if (descriptor->types == TypeConstraint::DefAndOperands && inst.def) {
        for (const Operand& operand : inst.operands)
          if (operand.kind == OperandKind::Reg &&
              !sameRegisterType(operand.reg, *inst.def)) {
            report("register operand type/class differs from definition");
            shapeValid = false;
          }
      }
      if (descriptor->types == TypeConstraint::IntegerCondition &&
          !inst.operands.empty() && inst.operands[0].kind == OperandKind::Reg &&
          (!isIntegerType(inst.operands[0].reg.type) ||
           inst.operands[0].reg.cls != RegClass::GPR)) {
        report("branch condition must be an integer GPR");
        shapeValid = false;
      }
      if (descriptor->types == TypeConstraint::NonVoidRegister &&
          !inst.operands.empty() && inst.operands[0].kind == OperandKind::Reg &&
          !isLegalRegister(inst.operands[0].reg)) {
        report("return operand must be a typed register");
        shapeValid = false;
      }
      if ((descriptor->types == TypeConstraint::DefAndMemory) && inst.mem.valid) {
        const uint32_t memorySize = inst.def ? typeSize(inst.def->type)
                                             : (inst.operands.empty() ||
                                                inst.operands[0].kind != OperandKind::Reg
                                                    ? 0
                                                    : typeSize(inst.operands[0].reg.type));
        if (memorySize == 0 || memorySize != inst.mem.size) {
          report("memory size does not match register type");
          shapeValid = false;
        }
      }
      for (const RegMask& mask : inst.clobbers)
        if (mask.cls != RegClass::GPR && mask.cls != RegClass::FPR)
          report("invalid register mask class");
    }
    const OpcodeDescriptor* terminator = opcodeDescriptor(
        block.instructions.back().opcode);
    if (terminator == nullptr || !terminator->terminator) {
      report("block B" + std::to_string(block.id) + " has no terminator");
      shapeValid = false;
    }
  }
  if (!shapeValid) return errors;

  std::vector<std::vector<uint32_t>> successors(function.blocks.size());
  std::vector<std::vector<uint32_t>> predecessors(function.blocks.size());
  for (size_t block = 0; block < function.blocks.size(); ++block) {
    const Instr& terminator = function.blocks[block].instructions.back();
    if (terminator.opcode == Opcode::Br)
      successors[block].push_back(terminator.operands[0].index);
    else if (terminator.opcode == Opcode::CondBr) {
      successors[block].push_back(terminator.operands[1].index);
      successors[block].push_back(terminator.operands[2].index);
    }
    for (uint32_t target : successors[block]) {
      if (target >= function.blocks.size()) {
        report("branch target out of range");
        shapeValid = false;
      } else {
        predecessors[target].push_back(static_cast<uint32_t>(block));
      }
    }
  }
  if (!shapeValid) return errors;

  struct Definition {
    Reg reg;
    uint32_t block;
    size_t index;
  };
  std::map<uint32_t, Definition> definitions;
  for (size_t block = 0; block < function.blocks.size(); ++block) {
    for (size_t index = 0; index < function.blocks[block].instructions.size(); ++index) {
      const Instr& inst = function.blocks[block].instructions[index];
      if (!inst.def) continue;
      const auto found = definitions.find(inst.def->id);
      if (found != definitions.end())
        report("duplicate definition of r" + std::to_string(inst.def->id));
      else
        definitions.emplace(inst.def->id,
                            Definition{*inst.def, static_cast<uint32_t>(block), index});
    }
  }
  const size_t count = function.blocks.size();
  std::vector<bool> reachable(count, false);
  std::vector<uint32_t> work{0};
  reachable[0] = true;
  while (!work.empty()) {
    const uint32_t block = work.back();
    work.pop_back();
    for (uint32_t target : successors[block])
      if (!reachable[target]) {
        reachable[target] = true;
        work.push_back(target);
      }
  }
  for (size_t block = 0; block < count; ++block)
    if (!reachable[block]) report("unreachable block B" + std::to_string(block));
  std::vector<std::set<uint32_t>> dominators(count);
  for (size_t block = 0; block < count; ++block) {
    if (!reachable[block]) continue;
    if (block == 0) dominators[block] = {0};
    else for (size_t candidate = 0; candidate < count; ++candidate)
      dominators[block].insert(static_cast<uint32_t>(candidate));
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t block = 1; block < count; ++block) {
      if (!reachable[block]) continue;
      std::set<uint32_t> next;
      bool first = true;
      for (uint32_t predecessor : predecessors[block]) {
        if (!reachable[predecessor]) continue;
        if (first) { next = dominators[predecessor]; first = false; }
        else {
          std::set<uint32_t> intersection;
          for (uint32_t value : next)
            if (dominators[predecessor].count(value)) intersection.insert(value);
          next = std::move(intersection);
        }
      }
      next.insert(static_cast<uint32_t>(block));
      if (next != dominators[block]) {
        dominators[block] = std::move(next);
        changed = true;
      }
    }
  }
  const auto dominates = [&](uint32_t definition, uint32_t use) {
    return reachable[use] && dominators[use].count(definition) != 0;
  };
  const auto checkUse = [&](const Reg& reg, uint32_t block, size_t index,
                            bool phiValue, uint32_t phiPredecessor) {
    const auto found = definitions.find(reg.id);
    if (reg.id == 0 || found == definitions.end()) {
      report("use of undefined register r" + std::to_string(reg.id));
      return;
    }
    if (!(found->second.reg == reg)) report("register type/class mismatch");
    if (phiValue) {
      if (!dominates(found->second.block, phiPredecessor))
        report("phi value does not dominate predecessor");
    } else if (!dominates(found->second.block, block) ||
               (found->second.block == block && found->second.index >= index)) {
      report("use does not follow a dominating definition");
    }
  };
  for (size_t block = 0; block < count; ++block) {
    const Block& current = function.blocks[block];
    for (size_t index = 0; index < current.instructions.size(); ++index) {
      const Instr& inst = current.instructions[index];
      if (inst.opcode == Opcode::Phi) {
        if (predecessors[block].size() > SIZE_MAX / 2 ||
            inst.operands.size() != predecessors[block].size() * 2) {
          report("phi arity does not match predecessors");
          continue;
        }
        for (size_t pair = 0; pair < predecessors[block].size(); ++pair) {
          const Operand& value = inst.operands[pair * 2];
          const Operand& edge = inst.operands[pair * 2 + 1];
          if (edge.index != predecessors[block][pair]) {
            report("malformed phi edge pair");
            continue;
          }
          checkUse(value.reg, static_cast<uint32_t>(block), index, true, edge.index);
          if (!sameRegisterType(value.reg, *inst.def))
            report("phi input type differs from result");
        }
        continue;
      }
      for (const Operand& operand : inst.operands) {
        if (operand.kind == OperandKind::Reg) checkUse(
            operand.reg, static_cast<uint32_t>(block), index, false, 0);
        if (operand.kind == OperandKind::Frame) {
          const uint64_t limit = function.frameObjects.empty()
              ? function.frameSlots : function.frameObjects.size();
          if (operand.index >= limit) report("frame index out of range");
        }
      }
      if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base.id != 0)
        checkUse(inst.mem.base, static_cast<uint32_t>(block), index, false, 0);
    }
  }
  return errors;
}
}}}  // namespace sysy::mir::detail

namespace sysy { namespace mir {
std::vector<std::string> verify(const Module& module) {
  std::vector<std::string> errors;
  std::set<std::string> names;
  for (const Function& function : module.functions) {
    if (function.name.empty()) errors.push_back("empty function name");
    if (!names.insert(function.name).second)
      errors.push_back("duplicate function " + function.name);
    const std::vector<std::string> functionErrors = detail::verifyFunction(function);
    errors.insert(errors.end(), functionErrors.begin(), functionErrors.end());
  }
  return errors;
}
}}  // namespace sysy::mir
