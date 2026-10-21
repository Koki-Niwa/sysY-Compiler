// B01 MIR core: deterministic dump, verifier positives and negatives.
#include <cstdio>
#include <string>
#include "mir/MIR.h"
using namespace sysy::mir;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
Instr copy(uint32_t dst, int64_t value) {
  Instr i; i.opcode = Opcode::Copy; i.def = Reg{dst, Type::I32, RegClass::GPR};
  i.operands.push_back(Operand::makeImm(value)); return i;
}
Instr add(uint32_t dst, uint32_t a, uint32_t b) {
  Instr i; i.opcode = Opcode::Add; i.def = Reg{dst, Type::I32, RegClass::GPR};
  i.operands.push_back(Operand::makeReg(Reg{a, Type::I32, RegClass::GPR}));
  i.operands.push_back(Operand::makeReg(Reg{b, Type::I32, RegClass::GPR}));
  return i;
}
Instr br(uint32_t target) {
  Instr i; i.opcode = Opcode::Br; i.operands.push_back(Operand::makeBlock(target)); return i;
}
Module module() {
  Module m; m.name = "t";
  Function f; f.name = "main"; f.frameSlots = 1;
  Block b0; b0.id = 0; b0.instructions = {copy(1, 3), br(1)};
  Block b1; b1.id = 1;
  Instr a = add(2, 1, 1);
  Instr ret; ret.opcode = Opcode::Ret;
  ret.operands.push_back(Operand::makeReg(Reg{2, Type::I32, RegClass::GPR}));
  b1.instructions = {a, ret};
  f.blocks = {b0, b1}; m.functions.push_back(f);
  return m;
}
}  // namespace
int main() {
  Module m = module();
  check(verify(m).empty(), "well-formed MIR verifies");
  std::string text = dump(m);
  check(text == dump(m), "MIR dump is deterministic");
  auto parsed = parse(text);
  check(parsed.has_value(), "MIR dump parses back");
  check(parsed && dump(*parsed) == text, "MIR dump->parse->dump is byte-identical");
  check(parsed && verify(*parsed).empty(), "parsed MIR verifies");
  std::string error;
  check(!parse("mir t\nfunc f frame=0\nB0:\n  nope r1:i32\n", &error).has_value() &&
            !error.empty(),
        "unknown opcode is rejected");
  check(text.find("B0:") != std::string::npos &&
            text.find("add r2:i32 r1:i32 r1:i32") != std::string::npos,
        "MIR dump has blocks and typed registers");
  Module bad = m;
  bad.functions[0].blocks[1].instructions[0].def = Reg{1, Type::I32, RegClass::GPR};
  check(!verify(bad).empty(), "duplicate definition is rejected");
  bad = m;
  bad.functions[0].blocks[0].instructions[0].operands.push_back(
      Operand::makeReg(Reg{9, Type::I32, RegClass::GPR}));
  check(!verify(bad).empty(), "undefined register use is rejected");
  bad = m;
  bad.functions[0].blocks[0].instructions[1] = br(9);
  check(!verify(bad).empty(), "out-of-range branch target is rejected");
  bad = m;
  bad.functions[0].blocks[1].instructions.pop_back();
  check(!verify(bad).empty(), "missing terminator is rejected");
  bad = m;
  bad.functions[0].blocks[1].instructions[0].operands[0] =
      Operand::makeReg(Reg{1, Type::I64, RegClass::GPR});
  check(!verify(bad).empty(), "register type mismatch is rejected");
  std::printf("MIR_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
