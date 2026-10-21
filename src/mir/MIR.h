// B01: minimal target-independent MIR core with explicit virtual registers,
// frame indices, memory operands and block successors. No code generation.
#ifndef SYSY_MIR_MIR_H
#define SYSY_MIR_MIR_H
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace sysy { namespace mir {
enum class Type : uint8_t { I32, I64, F32, Void };
enum class RegClass : uint8_t { GPR, FPR };
struct Reg {
  uint32_t id = 0;
  Type type = Type::I32;
  RegClass cls = RegClass::GPR;
  friend bool operator==(const Reg& a, const Reg& b) {
    return a.id == b.id && a.type == b.type && a.cls == b.cls;
  }
};
enum class OperandKind : uint8_t { Reg, Imm, Frame, Block };
struct Operand {
  OperandKind kind = OperandKind::Imm;
  Reg reg;
  int64_t imm = 0;
  uint32_t index = 0;  // Frame or Block
  static Operand makeReg(Reg r);
  static Operand makeImm(int64_t value);
  static Operand makeFrame(uint32_t index);
  static Operand makeBlock(uint32_t index);
};
struct MemOperand {
  bool valid = false;
  Reg base;
  int64_t offset = 0;
  uint32_t size = 0;
};
enum class Opcode : uint8_t {
  Add, Sub, Mul, Div, Copy, Load, Store, Br, CondBr, Ret, Phi
};
struct Instr {
  Opcode opcode = Opcode::Copy;
  std::optional<Reg> def;
  std::vector<Operand> operands;
  MemOperand mem;
};
struct Block {
  uint32_t id = 0;
  std::vector<Instr> instructions;
};
struct Function {
  std::string name;
  std::vector<Block> blocks;
  uint32_t frameSlots = 0;
};
struct Module {
  std::string name;
  std::vector<Function> functions;
};
// Deterministic, human-readable dump. Round-trip parsing is a later B01 slice.
std::string dump(const Module& module);
// Parse the deterministic dump format above. Returns nullopt and a diagnostic
// on malformed text; the verifier is not run implicitly.
std::optional<Module> parse(const std::string& text, std::string* error = nullptr);
// Return one message per violating instruction/block; empty means valid.
std::vector<std::string> verify(const Module& module);
}}  // namespace sysy::mir
#endif
