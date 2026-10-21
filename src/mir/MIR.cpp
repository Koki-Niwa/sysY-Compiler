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
    std::set<uint32_t> frameIds;
    for (size_t i = 0; i < function.frameObjects.size(); ++i) {
      const FrameObject& object = function.frameObjects[i];
      if (object.id != i)
        errors.push_back(function.name + ": frame object ids must be contiguous");
      if (!frameIds.insert(object.id).second)
        errors.push_back(function.name + ": duplicate frame object");
      if (object.size == 0)
        errors.push_back(function.name + ": zero-sized frame object");
      if (object.align == 0 || (object.align & (object.align - 1)) != 0)
        errors.push_back(function.name + ": frame alignment must be a power of two");
    }
    std::set<uint32_t> blockIds;
    for (size_t b = 0; b < function.blocks.size(); ++b) {
      const Block& block = function.blocks[b];
      if (block.id != b) {
        errors.push_back(function.name + ": block ids must be contiguous (B" +
                         std::to_string(b) + " expected)");
        continue;
      }
      if (!blockIds.insert(block.id).second)
        errors.push_back(function.name + ": duplicate block B" +
                         std::to_string(block.id));
      if (block.instructions.empty()) {
        errors.push_back(function.name + ": empty block B" +
                         std::to_string(block.id));
        continue;
      }
      bool terminatorSeen = false;
      for (const Instr& inst : block.instructions) {
        if (terminatorSeen)
          errors.push_back(function.name + ": instruction after terminator");
        if (isTerminator(inst.opcode)) terminatorSeen = true;
      }
      if (!terminatorSeen)
        errors.push_back(function.name + ": block B" + std::to_string(block.id) +
                         " has no terminator");
      const Instr& term = block.instructions.back();
      if (term.opcode == Opcode::Br &&
          (term.operands.size() != 1 ||
           term.operands[0].kind != OperandKind::Block))
        errors.push_back(function.name + ": malformed br");
      if (term.opcode == Opcode::CondBr &&
          (term.operands.size() != 3 ||
           term.operands[1].kind != OperandKind::Block ||
           term.operands[2].kind != OperandKind::Block))
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
    // Dominator sets over reachable blocks; unreachable blocks are rejected
    // because no definition can dominate their uses.
    const size_t count = function.blocks.size();
    std::vector<bool> reachable(count, false);
    std::vector<uint32_t> work{0};
    reachable[0] = true;
    while (!work.empty()) {
      const uint32_t b = work.back();
      work.pop_back();
      for (uint32_t target : succ[b])
        if (target < count && !reachable[target]) {
          reachable[target] = true;
          work.push_back(target);
        }
    }
    for (size_t b = 0; b < count; ++b)
      if (!reachable[b])
        errors.push_back(function.name + ": unreachable block B" + std::to_string(b));
    std::vector<std::set<uint32_t>> dominators(count);
    for (size_t b = 0; b < count; ++b)
      if (reachable[b]) {
        if (b == 0) dominators[b] = {0};
        else for (size_t i = 0; i < count; ++i) dominators[b].insert(i);
      }
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 1; b < count; ++b) {
        if (!reachable[b] || preds[b].empty()) continue;
        std::set<uint32_t> next;
        bool first = true;
        for (uint32_t pred : preds[b]) {
          if (!reachable[pred]) continue;
          if (first) { next = dominators[pred]; first = false; }
          else {
            std::set<uint32_t> intersection;
            for (uint32_t value : next)
              if (dominators[pred].count(value)) intersection.insert(value);
            next = std::move(intersection);
          }
        }
        next.insert(static_cast<uint32_t>(b));
        if (next != dominators[b]) {
          dominators[b] = std::move(next);
          changed = true;
        }
      }
    }
    auto dominates = [&](uint32_t definition, uint32_t use) {
      return reachable[use] && dominators[use].count(definition) != 0;
    };
    struct Def {
      Reg reg;
      uint32_t block = 0;
      size_t index = 0;
    };
    std::map<uint32_t, Def> defs;
    for (size_t b = 0; b < count; ++b) {
      for (size_t i = 0; i < function.blocks[b].instructions.size(); ++i) {
        const Instr& inst = function.blocks[b].instructions[i];
        if (!inst.def) continue;
        if (!hasDef(inst.opcode))
          errors.push_back(function.name + ": unexpected def on terminator");
        if (inst.def->type == Type::Void)
          errors.push_back(function.name + ": void def");
        const auto found = defs.find(inst.def->id);
        if (found != defs.end())
          errors.push_back(function.name + ": duplicate definition of r" +
                           std::to_string(inst.def->id));
        else
          defs[inst.def->id] = Def{*inst.def, static_cast<uint32_t>(b), i};
      }
    }
    auto checkUse = [&](const Reg& reg, uint32_t block, size_t index,
                        bool phiValue, uint32_t phiPred) {
      const auto found = defs.find(reg.id);
      if (found == defs.end()) {
        errors.push_back(function.name + ": use of undefined register r" +
                         std::to_string(reg.id));
        return;
      }
      if (!(found->second.reg == reg))
        errors.push_back(function.name + ": register type/class mismatch");
      if (phiValue) {
        if (!dominates(found->second.block, phiPred))
          errors.push_back(function.name + ": phi value does not dominate predecessor");
      } else if (!dominates(found->second.block, block) ||
                 (found->second.block == block && found->second.index >= index)) {
        errors.push_back(function.name + ": use does not follow a dominating definition");
      }
    };
    for (size_t b = 0; b < count; ++b) {
      const Block& block = function.blocks[b];
      for (size_t i = 0; i < block.instructions.size(); ++i) {
        const Instr& inst = block.instructions[i];
        if (inst.opcode == Opcode::Phi) {
          if (inst.operands.size() != preds[b].size() * 2) {
            errors.push_back(function.name + ": phi arity does not match predecessors");
          } else {
            for (size_t pair = 0; pair < preds[b].size(); ++pair) {
              const Operand& value = inst.operands[pair * 2];
              const Operand& edge = inst.operands[pair * 2 + 1];
              if (value.kind != OperandKind::Reg || edge.kind != OperandKind::Block ||
                  edge.index != preds[b][pair])
                errors.push_back(function.name + ": malformed phi edge pair");
              else
                checkUse(value.reg, static_cast<uint32_t>(b), i, true, edge.index);
            }
          }
          continue;
        }
        for (const Operand& operand : inst.operands) {
          if (operand.kind == OperandKind::Reg)
            checkUse(operand.reg, static_cast<uint32_t>(b), i, false, 0);
          if (operand.kind == OperandKind::Frame) {
            const uint32_t limit = function.frameObjects.empty()
                ? function.frameSlots
                : static_cast<uint32_t>(function.frameObjects.size());
            if (operand.index >= limit)
              errors.push_back(function.name + ": frame index out of range");
          }
        }
        if (inst.mem.valid) {
          if (inst.mem.size == 0) errors.push_back(function.name + ": zero memory size");
          if (inst.mem.base.id != 0)
            checkUse(inst.mem.base, static_cast<uint32_t>(b), i, false, 0);
        }
        for (const RegMask& mask : inst.clobbers)
          if (mask.cls != RegClass::GPR && mask.cls != RegClass::FPR)
            errors.push_back(function.name + ": invalid register mask class");
      }
    }
  }
  return errors;
}
}}  // namespace sysy::mir

namespace sysy { namespace mir {
namespace {
bool startsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}
std::string trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}
bool parseType(const std::string& name, Type& type) {
  if (name == "i32") { type = Type::I32; return true; }
  if (name == "i64") { type = Type::I64; return true; }
  if (name == "f32") { type = Type::F32; return true; }
  if (name == "void") { type = Type::Void; return true; }
  return false;
}
bool parseReg(const std::string& token, Reg& reg) {
  const size_t colon = token.find(':');
  if (colon == std::string::npos || colon < 2) return false;
  const char prefix = token[0];
  if (prefix != 'r' && prefix != 'f') return false;
  size_t end = 1;
  uint64_t id = 0;
  while (end < colon && token[end] >= '0' && token[end] <= '9') {
    id = id * 10 + static_cast<uint64_t>(token[end] - '0');
    if (id > UINT32_MAX) return false;
    ++end;
  }
  if (end != colon) return false;
  Type type;
  if (!parseType(token.substr(colon + 1), type)) return false;
  reg = Reg{static_cast<uint32_t>(id), type,
            prefix == 'r' ? RegClass::GPR : RegClass::FPR};
  return true;
}
bool parseOpcode(const std::string& token, Opcode& opcode) {
  static const std::pair<const char*, Opcode> table[] = {
      {"add", Opcode::Add}, {"sub", Opcode::Sub}, {"mul", Opcode::Mul},
      {"div", Opcode::Div}, {"copy", Opcode::Copy}, {"load", Opcode::Load},
      {"store", Opcode::Store}, {"br", Opcode::Br}, {"condbr", Opcode::CondBr},
      {"ret", Opcode::Ret}, {"phi", Opcode::Phi}};
  for (const auto& entry : table)
    if (token == entry.first) { opcode = entry.second; return true; }
  return false;
}
bool parseMem(const std::string& token, MemOperand& mem) {
  if (!startsWith(token, "mem[") || token.back() != ']') return false;
  const std::string body = token.substr(4, token.size() - 5);
  const size_t plus = body.find('+');
  const size_t colon = body.rfind(':');
  if (plus == std::string::npos || colon == std::string::npos || plus > colon)
    return false;
  Reg base;
  if (!parseReg(body.substr(0, plus), base)) return false;
  int64_t offset = 0;
  if (!body.substr(plus + 1, colon - plus - 1).empty()) {
    try {
      offset = std::stoll(body.substr(plus + 1, colon - plus - 1));
    } catch (...) { return false; }
  }
  const std::string size = body.substr(colon + 1);
  if (size.empty()) return false;
  for (char ch : size) if (ch < '0' || ch > '9') return false;
  mem.valid = true;
  mem.base = base;
  mem.offset = offset;
  mem.size = static_cast<uint32_t>(std::stoul(size));
  return true;
}
}  // namespace

std::optional<Module> parse(const std::string& text, std::string* error) {
  Module module;
  std::istringstream input(text);
  std::string line;
  Function* function = nullptr;
  Block* block = nullptr;
  bool headerSeen = false;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty()) continue;
    if (startsWith(line, "mir ")) {
      if (headerSeen || function != nullptr) {
        if (error) *error = "duplicate/misplaced mir header";
        return std::nullopt;
      }
      module.name = line.substr(4);
      headerSeen = true;
      continue;
    }
    if (!headerSeen) {
      if (error) *error = "missing mir header";
      return std::nullopt;
    }
    if (startsWith(line, "func ")) {
      std::istringstream header(line.substr(5));
      Function next;
      std::string frame;
      if (!(header >> next.name >> frame) || !startsWith(frame, "frame=")) {
        if (error) *error = "malformed func header";
        return std::nullopt;
      }
      try {
        next.frameSlots = static_cast<uint32_t>(std::stoul(frame.substr(6)));
      } catch (...) {
        if (error) *error = "malformed frame count";
        return std::nullopt;
      }
      module.functions.push_back(std::move(next));
      function = &module.functions.back();
      block = nullptr;
      continue;
    }
    if (line.back() == ':' && line.size() > 2 && line[0] == 'B') {
      if (function == nullptr) {
        if (error) *error = "block before function";
        return std::nullopt;
      }
      Block next;
      try {
        next.id = static_cast<uint32_t>(std::stoul(line.substr(1, line.size() - 2)));
      } catch (...) {
        if (error) *error = "malformed block id";
        return std::nullopt;
      }
      function->blocks.push_back(std::move(next));
      block = &function->blocks.back();
      continue;
    }
    if (function == nullptr || block == nullptr) {
      if (error) *error = "instruction outside function/block";
      return std::nullopt;
    }
    std::istringstream tokens(line);
    std::string opcodeNameToken;
    if (!(tokens >> opcodeNameToken)) continue;
    Instr inst;
    if (!parseOpcode(opcodeNameToken, inst.opcode)) {
      if (error) *error = "unknown opcode: " + opcodeNameToken;
      return std::nullopt;
    }
    std::string token;
    if (hasDef(inst.opcode)) {
      if (!(tokens >> token)) {
        if (error) *error = "missing instruction def";
        return std::nullopt;
      }
      Reg def;
      if (!parseReg(token, def)) {
        if (error) *error = "malformed def: " + token;
        return std::nullopt;
      }
      inst.def = def;
    }
    while (tokens >> token) {
      if (startsWith(token, "mem[")) {
        if (!parseMem(token, inst.mem)) {
          if (error) *error = "malformed memory operand";
          return std::nullopt;
        }
        continue;
      }
      if (token[0] == 'B' && token.size() > 1) {
        try {
          inst.operands.push_back(
              Operand::makeBlock(static_cast<uint32_t>(std::stoul(token.substr(1)))));
          continue;
        } catch (...) { /* fall through to error */ }
      }
      if (startsWith(token, "frame")) {
        try {
          inst.operands.push_back(
              Operand::makeFrame(static_cast<uint32_t>(std::stoul(token.substr(5)))));
          continue;
        } catch (...) { /* fall through */ }
      }
      if (token.find(':') != std::string::npos) {
        Reg reg;
        if (parseReg(token, reg)) {
          inst.operands.push_back(Operand::makeReg(reg));
          continue;
        }
      }
      try {
        size_t used = 0;
        const int64_t value = std::stoll(token, &used);
        if (used == token.size()) {
          inst.operands.push_back(Operand::makeImm(value));
          continue;
        }
      } catch (...) { /* fall through */ }
      if (error) *error = "malformed operand: " + token;
      return std::nullopt;
    }
    block->instructions.push_back(std::move(inst));
  }
  if (!headerSeen) {
    if (error) *error = "empty input";
    return std::nullopt;
  }
  return module;
}

}}  // namespace sysy::mir
