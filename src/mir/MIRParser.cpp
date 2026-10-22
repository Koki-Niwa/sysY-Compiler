#include "mir/MIRInternal.h"
#include <cstdint>
#include <sstream>
#include <string>

namespace sysy { namespace mir {
namespace {
using detail::opcodeDescriptor;

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

bool parseUnsigned(const std::string& token, uint64_t maximum, uint64_t& value) {
  if (token.empty()) return false;
  uint64_t result = 0;
  for (char ch : token) {
    if (ch < '0' || ch > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(ch - '0');
    if (digit > maximum) return false;
    if (result > (maximum - digit) / 10) return false;
    result = result * 10 + digit;
  }
  value = result;
  return true;
}

bool parseBits(const std::string& token, uint64_t& value) {
  if (token.empty()) return false;
  size_t begin = 0;
  bool hexadecimal = false;
  if (token.size() >= 2 && token[0] == '0' &&
      (token[1] == 'x' || token[1] == 'X')) {
    begin = 2;
    hexadecimal = true;
  }
  if (begin == token.size()) return false;
  uint64_t result = 0;
  for (; begin < token.size(); ++begin) {
    const char ch = token[begin];
    uint64_t digit = 0;
    if (ch >= '0' && ch <= '9') digit = static_cast<uint64_t>(ch - '0');
    else if (hexadecimal && ch >= 'a' && ch <= 'f')
      digit = static_cast<uint64_t>(ch - 'a' + 10);
    else if (hexadecimal && ch >= 'A' && ch <= 'F')
      digit = static_cast<uint64_t>(ch - 'A' + 10);
    else return false;
    const uint64_t base = hexadecimal ? 16 : 10;
    if (result > (UINT64_MAX - digit) / base) return false;
    result = result * base + digit;
  }
  value = result;
  return true;
}

bool parseSigned(const std::string& token, int64_t& value) {
  if (token.empty()) return false;
  size_t begin = 0;
  bool negative = false;
  if (token[0] == '-') {
    negative = true;
    begin = 1;
  } else if (token[0] == '+') {
    begin = 1;
  }
  if (begin == token.size()) return false;
  const uint64_t maximum = negative ? (uint64_t{1} << 63)
                                    : static_cast<uint64_t>(INT64_MAX);
  uint64_t magnitude = 0;
  if (!parseUnsigned(token.substr(begin), maximum, magnitude)) return false;
  if (negative) {
    if (magnitude == (uint64_t{1} << 63)) value = INT64_MIN;
    else value = -static_cast<int64_t>(magnitude);
  } else {
    value = static_cast<int64_t>(magnitude);
  }
  return true;
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
  if (colon == std::string::npos || colon < 2 ||
      token.find(':', colon + 1) != std::string::npos)
    return false;
  if (token[0] != 'r' && token[0] != 'f') return false;
  uint64_t id = 0;
  if (!parseUnsigned(token.substr(1, colon - 1), UINT32_MAX, id)) return false;
  Type type;
  if (!parseType(token.substr(colon + 1), type)) return false;
  reg = Reg{static_cast<uint32_t>(id), type,
            token[0] == 'r' ? RegClass::GPR : RegClass::FPR};
  return true;
}

bool parseMem(const std::string& token, MemOperand& mem) {
  if (!startsWith(token, "mem[") || token.size() < 6 || token.back() != ']') return false;
  const std::string body = token.substr(4, token.size() - 5);
  const size_t plus = body.find('+');
  if (plus == std::string::npos || plus == 0) return false;
  const size_t colon = body.find(':', plus + 1);
  if (colon == std::string::npos || colon == plus + 1) return false;
  if (body.find('+', plus + 1) != std::string::npos) return false;
  const std::string baseText = body.substr(0, plus);
  bool frameBase = false;
  uint64_t frame = 0;
  Reg base;
  if (startsWith(baseText, "frame")) {
    if (!parseUnsigned(baseText.substr(5), UINT32_MAX, frame)) return false;
    frameBase = true;
  } else if (!parseReg(baseText, base)) {
    return false;
  }
  int64_t offset = 0;
  if (!parseSigned(body.substr(plus + 1, colon - plus - 1), offset)) return false;
  const std::string rest = body.substr(colon + 1);
  const size_t next = rest.find(':');
  const std::string sizeText = rest.substr(0, next);
  uint64_t size = 0;
  if (!parseUnsigned(sizeText, UINT32_MAX, size)) return false;
  mem = MemOperand{};
  mem.valid = true;
  mem.base = base;
  mem.frameBase = frameBase;
  mem.frame = static_cast<uint32_t>(frame);
  mem.offset = offset;
  mem.size = static_cast<uint32_t>(size);
  if (next == std::string::npos) return true;
  const std::string tail = rest.substr(next + 1);
  size_t begin = 0;
  bool sawVolatile = false;
  bool sawAlias = false;
  while (begin <= tail.size()) {
    const size_t end = tail.find(':', begin);
    const std::string part = tail.substr(begin, end == std::string::npos
                                                  ? std::string::npos
                                                  : end - begin);
    if (part == "volatile") {
      if (sawVolatile) return false;
      sawVolatile = true;
      mem.isVolatile = true;
    } else if (startsWith(part, "alias=")) {
      if (sawAlias) return false;
      uint64_t alias = 0;
      if (!parseUnsigned(part.substr(6), UINT32_MAX, alias)) return false;
      sawAlias = true;
      mem.aliasClass = static_cast<uint32_t>(alias);
    } else {
      return false;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

bool parseField(const std::string& token, const char* prefix,
                uint64_t maximum, uint64_t& value) {
  if (!startsWith(token, prefix)) return false;
  return parseUnsigned(token.substr(std::string(prefix).size()), maximum, value);
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
      if (function == nullptr || block != nullptr) {
        if (error) *error = "frame object outside function header";
        return std::nullopt;
      }
      std::istringstream objectLine(line.substr(9));
      FrameObject object;
      std::string idToken, sizeToken, alignToken, spillToken, extra;
      uint64_t id = 0, size = 0, align = 0, spill = 0;
      if (!(objectLine >> idToken >> sizeToken >> alignToken >> spillToken) ||
          (objectLine >> extra) || !parseUnsigned(idToken, UINT32_MAX, id) ||
          !parseField(sizeToken, "size=", UINT32_MAX, size) ||
          !parseField(alignToken, "align=", UINT32_MAX, align) ||
          !parseField(spillToken, "spill=", 1, spill)) {
        if (error) *error = "malformed frame object";
        return std::nullopt;
      }
      object.id = static_cast<uint32_t>(id);
      object.size = static_cast<uint32_t>(size);
      object.align = static_cast<uint32_t>(align);
      object.spill = spill != 0;
      function->frameObjects.push_back(object);
      continue;
    }
    if (startsWith(line, "func ")) {
      std::istringstream header(line.substr(5));
      Function next;
      std::string frame, extra;
      uint64_t frameSlots = 0;
      if (!(header >> next.name >> frame) || (header >> extra) ||
          !parseField(frame, "frame=", UINT32_MAX, frameSlots)) {
        if (error) *error = "malformed func header";
        return std::nullopt;
      }
      next.frameSlots = static_cast<uint32_t>(frameSlots);
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
      uint64_t id = 0;
      if (!parseUnsigned(line.substr(1, line.size() - 2), UINT32_MAX, id)) {
        if (error) *error = "malformed block id";
        return std::nullopt;
      }
      Block next;
      next.id = static_cast<uint32_t>(id);
      function->blocks.push_back(std::move(next));
      block = &function->blocks.back();
      continue;
    }
    if (function == nullptr || block == nullptr) {
      if (error) *error = "instruction outside function/block";
      return std::nullopt;
    }
    std::istringstream tokens(line);
    std::string opcodeToken;
    if (!(tokens >> opcodeToken)) continue;
    const detail::OpcodeDescriptor* descriptor = opcodeDescriptor(opcodeToken);
    if (descriptor == nullptr) {
      if (error) *error = "unknown opcode: " + opcodeToken;
      return std::nullopt;
    }
    Instr inst;
    inst.opcode = descriptor->opcode;
    std::string token;
    if (descriptor->hasDef) {
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
        if (colon != 1 || (body[0] != 'g' && body[0] != 'f')) {
          if (error) *error = "malformed clobber";
          return std::nullopt;
        }
        RegMask mask;
        mask.cls = body[0] == 'g' ? RegClass::GPR : RegClass::FPR;
        if (!parseBits(body.substr(colon + 1), mask.bits)) {
          if (error) *error = "malformed clobber bits";
          return std::nullopt;
        }
        inst.clobbers.push_back(mask);
        continue;
      }
      if (startsWith(token, "mem[")) {
        if (inst.mem.valid || !parseMem(token, inst.mem)) {
          if (error) *error = "malformed memory operand";
          return std::nullopt;
        }
        continue;
      }
      if (token[0] == 'B' && token.size() > 1) {
        uint64_t id = 0;
        if (parseUnsigned(token.substr(1), UINT32_MAX, id)) {
          inst.operands.push_back(Operand::makeBlock(static_cast<uint32_t>(id)));
          continue;
        }
      }
      if (startsWith(token, "frame")) {
        uint64_t id = 0;
        if (parseUnsigned(token.substr(5), UINT32_MAX, id)) {
          inst.operands.push_back(Operand::makeFrame(static_cast<uint32_t>(id)));
          continue;
        }
      }
      if (token.find(':') != std::string::npos) {
        Reg reg;
        if (parseReg(token, reg)) {
          inst.operands.push_back(Operand::makeReg(reg));
          continue;
        }
      }
      int64_t value = 0;
      if (parseSigned(token, value)) {
        inst.operands.push_back(Operand::makeImm(value));
        continue;
      }
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
