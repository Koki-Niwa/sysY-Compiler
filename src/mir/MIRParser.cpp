#include "mir/MIR.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
namespace sysy { namespace mir {
namespace {
bool hasDef(Opcode opcode) {
  return opcode != Opcode::Store && opcode != Opcode::Br &&
         opcode != Opcode::CondBr && opcode != Opcode::Ret;
}
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
  const size_t colon = body.find(':', plus + 1);
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
  const std::string rest = body.substr(colon + 1);
  const size_t next = rest.find(':');
  const std::string size = rest.substr(0, next);
  if (size.empty()) return false;
  for (char ch : size) if (ch < '0' || ch > '9') return false;
  mem.valid = true;
  mem.base = base;
  mem.offset = offset;
  mem.size = static_cast<uint32_t>(std::stoul(size));
  if (next != std::string::npos) {
    std::string tail = rest.substr(next + 1);
    size_t begin = 0;
    while (begin <= tail.size()) {
      const size_t end = tail.find(':', begin);
      const std::string part = tail.substr(begin, end == std::string::npos
                                                    ? std::string::npos
                                                    : end - begin);
      if (part == "volatile") mem.isVolatile = true;
      else if (startsWith(part, "alias=")) {
        try { mem.aliasClass = static_cast<uint32_t>(std::stoul(part.substr(6))); }
        catch (...) { return false; }
      } else return false;
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  }
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
    if (startsWith(line, "frameobj ")) {
      if (function == nullptr) {
        if (error) *error = "frame object before function";
        return std::nullopt;
      }
      std::istringstream objectLine(line.substr(9));
      FrameObject object;
      std::string idToken, sizeToken, alignToken, spillToken;
      if (!(objectLine >> idToken >> sizeToken >> alignToken >> spillToken) ||
          !startsWith(sizeToken, "size=") || !startsWith(alignToken, "align=") ||
          !startsWith(spillToken, "spill=")) {
        if (error) *error = "malformed frame object";
        return std::nullopt;
      }
      try {
        object.id = static_cast<uint32_t>(std::stoul(idToken));
        object.size = static_cast<uint32_t>(std::stoul(sizeToken.substr(5)));
        object.align = static_cast<uint32_t>(std::stoul(alignToken.substr(6)));
        object.spill = std::stoul(spillToken.substr(6)) != 0;
      } catch (...) {
        if (error) *error = "malformed frame object field";
        return std::nullopt;
      }
      function->frameObjects.push_back(object);
      continue;
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
      if (startsWith(token, "clobber=")) {
        const std::string body = token.substr(8);
        const size_t colon = body.find(':');
        if (colon == std::string::npos || (body.substr(0, colon) != "g" &&
                                            body.substr(0, colon) != "f")) {
          if (error) *error = "malformed clobber";
          return std::nullopt;
        }
        RegMask mask;
        mask.cls = body.substr(0, colon) == "g" ? RegClass::GPR : RegClass::FPR;
        try {
          mask.bits = std::stoull(body.substr(colon + 1), nullptr, 0);
        } catch (...) {
          if (error) *error = "malformed clobber bits";
          return std::nullopt;
        }
        inst.clobbers.push_back(mask);
        continue;
      }
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

