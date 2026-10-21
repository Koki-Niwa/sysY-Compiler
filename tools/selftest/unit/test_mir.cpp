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
Instr condbr(uint32_t cond, uint32_t yes, uint32_t no) {
  Instr i; i.opcode = Opcode::CondBr;
  i.operands.push_back(Operand::makeReg(Reg{cond, Type::I32, RegClass::GPR}));
  i.operands.push_back(Operand::makeBlock(yes));
  i.operands.push_back(Operand::makeBlock(no));
  return i;
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
  bad.functions[0].blocks[0].instructions.insert(
      bad.functions[0].blocks[0].instructions.begin() + 1, add(3, 2, 2));
  check(!verify(bad).empty(), "use before its definition is rejected");
  bad = m;
  bad.functions[0].blocks[1].instructions.pop_back();
  check(!verify(bad).empty(), "missing terminator is rejected");
  bad = m;
  bad.functions[0].blocks[1].instructions[0].operands[0] =
      Operand::makeReg(Reg{1, Type::I64, RegClass::GPR});
  check(!verify(bad).empty(), "register type mismatch is rejected");
  bad = m;
  bad.functions[0].frameObjects = {{0, 8, 16, true}};
  check(verify(bad).empty(), "aligned frame object and spill flag are accepted");
  bad.functions[0].frameObjects[0].align = 3;
  check(!verify(bad).empty(), "non-power-of-two frame alignment is rejected");
  bad = m;
  bad.functions[0].frameObjects = {{0, 8, 8, false}, {0, 4, 4, false}};
  check(!verify(bad).empty(), "duplicate frame object id is rejected");
  bad = m;
  bad.functions[0].blocks[0].instructions[0].clobbers.push_back(
      RegMask{RegClass::FPR, 0x3});
  bad.functions[0].blocks[0].instructions[0].mem =
      MemOperand{true, Reg{0, Type::I32, RegClass::GPR}, 0, 4, 2, true};
  check(verify(bad).empty(), "RegMask and volatile/alias memory facts are accepted");
  Liveness live = analyzeLiveness(m.functions[0]);
  check(live.liveIn.size() == 2 && live.liveOut.size() == 2,
        "liveness has one entry per block");
  check(live.liveIn[0].empty() && live.liveOut[0].count(1),
        "definition is live-out of its block and not live-in");
  check(live.liveIn[1].count(1) && live.liveOut[1].empty(),
        "entry use is live-in but return leaves nothing live-out");
  Module im;
  im.name = "i";
  Function iff;
  iff.name = "f";
  Block ib;
  ib.id = 0;
  ib.instructions = {copy(1, 1), copy(2, 2), br(1)};
  Block ib2;
  ib2.id = 1;
  Instr ret3;
  ret3.opcode = Opcode::Ret;
  ret3.operands.push_back(Operand::makeReg(Reg{3, Type::I32, RegClass::GPR}));
  ib2.instructions = {add(3, 1, 2), ret3};
  iff.blocks = {ib, ib2};
  im.functions.push_back(iff);
  Liveness ilive = analyzeLiveness(im.functions[0]);
  InterferenceGraph graph = buildInterference(im.functions[0], ilive);
  check(graph.neighbors.size() > 3 && graph.neighbors[1].count(2) &&
            graph.neighbors[2].count(1),
        "simultaneously live virtual registers interfere");
  check(!graph.neighbors[3].count(1) && !graph.neighbors[3].count(2),
        "defined result does not interfere with its own inputs");
  Module diamond;
  diamond.name = "d";
  Function df;
  df.name = "f";
  Block d0; d0.id = 0;
  d0.instructions = {copy(1, 1), copy(2, 2), copy(4, 0), condbr(4, 1, 2)};
  Block d1; d1.id = 1; d1.instructions = {br(3)};
  Block d2; d2.id = 2; d2.instructions = {br(3)};
  Block d3; d3.id = 3;
  Instr phi;
  phi.opcode = Opcode::Phi;
  phi.def = Reg{3, Type::I32, RegClass::GPR};
  phi.operands = {Operand::makeReg(Reg{1, Type::I32, RegClass::GPR}),
                  Operand::makeBlock(1),
                  Operand::makeReg(Reg{2, Type::I32, RegClass::GPR}),
                  Operand::makeBlock(2)};
  Instr dret; dret.opcode = Opcode::Ret;
  dret.operands.push_back(Operand::makeReg(Reg{3, Type::I32, RegClass::GPR}));
  d3.instructions = {phi, dret};
  df.blocks = {d0, d1, d2, d3};
  diamond.functions.push_back(df);
  Liveness dlive = analyzeLiveness(diamond.functions[0]);
  check(dlive.liveIn[3].empty(),
        "phi defs are local, so no phi operand appears in block live-in");
  check(dlive.edgeLive[{1, 3}] == std::set<uint32_t>{1} &&
            dlive.edgeLive[{2, 3}] == std::set<uint32_t>{2},
        "phi operands are live only on their predecessor edge");
  Allocation alloc = greedyColor(graph, 2);
  check(alloc.color[1] >= 0 && alloc.color[2] >= 0 &&
            alloc.color[1] != alloc.color[2] && !alloc.spilled[1] &&
            !alloc.spilled[2],
        "two-color allocation separates interfering registers");
  Allocation one = greedyColor(graph, 1);
  check((one.spilled[1] != one.spilled[2]) &&
            (one.spilled[1] || one.spilled[2]),
        "one-color allocation spills exactly one interfering register");
  InterferenceGraph independent = buildInterference(
      m.functions[0], analyzeLiveness(m.functions[0]));
  Allocation shared = greedyColor(independent, 1);
  check(!shared.spilled[1] && !shared.spilled[2],
        "non-interfering registers may share one color");
  SpillPlan spillPlan = planSpills(one, 8, 8);
  const size_t spilledId = one.spilled[1] ? 1 : 2;
  check(spillPlan.slot[spilledId] >= 0 &&
            spillPlan.objects.size() == 1 &&
            spillPlan.objects[0].spill && spillPlan.objects[0].size == 8 &&
            spillPlan.objects[0].align == 8,
        "spilled virtual register gets an aligned frame slot");
  SpillPlan noSpills = planSpills(alloc);
  check(noSpills.objects.empty(), "non-spilling allocation needs no spill slot");
  Module spilledModule;
  spilledModule.name = "spilled";
  Function spilled = im.functions[0];
  size_t spillErrors = 0;
  check(rewriteSpills(spilled, spillPlan, &spillErrors) && spillErrors == 0,
        "spill rewrite succeeds for a non-phi function");
  spilledModule.functions.push_back(spilled);
  check(verify(spilledModule).empty() && !spilled.frameObjects.empty(),
        "rewritten spill function verifies with frame objects");
  size_t loads = 0, stores = 0;
  for (const Block& block : spilled.blocks)
    for (const Instr& inst : block.instructions) {
      if (inst.opcode == Opcode::Load) ++loads;
      if (inst.opcode == Opcode::Store) ++stores;
    }
  check(loads > 0 && stores > 0, "spill rewrite emits frame loads and stores");
  const std::string spilledText = dump(spilledModule);
  auto spilledParsed = parse(spilledText);
  check(spilledParsed && dump(*spilledParsed) == spilledText &&
            verify(*spilledParsed).empty(),
        "frame-based spill memory round-trips through dump/parse");
  Module rich;
  rich.name = "rich";
  Function rf;
  rf.name = "f";
  rf.frameSlots = 1;
  rf.frameObjects = {{0, 8, 8, true}};
  Block rb0;
  rb0.id = 0;
  Instr load;
  load.opcode = Opcode::Load;
  load.def = Reg{1, Type::I32, RegClass::GPR};
  load.mem = MemOperand{true, Reg{0, Type::I32, RegClass::GPR}, 0, 4, 2, true};
  load.clobbers = {RegMask{RegClass::GPR, 0x3}};
  rb0.instructions = {load, br(1)};
  Block rb1;
  rb1.id = 1;
  Instr rret;
  rret.opcode = Opcode::Ret;
  rret.operands.push_back(Operand::makeReg(Reg{1, Type::I32, RegClass::GPR}));
  rb1.instructions = {rret};
  rf.blocks = {rb0, rb1};
  rich.functions.push_back(rf);
  const std::string richText = dump(rich);
  auto richParsed = parse(richText);
  check(richParsed.has_value(), "MIR frame/mask/memory facts parse back");
  check(richParsed && dump(*richParsed) == richText,
        "MIR frame/mask/memory dump is byte-identical");
  check(richParsed && verify(*richParsed).empty(),
        "MIR frame/mask/memory parsed module verifies");
  std::printf("MIR_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
