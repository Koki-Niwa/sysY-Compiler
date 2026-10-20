// S25a: BFI-driven unroll advice for canonical constant-trip loops.
#include <cstdio>
#include "analysis/UnrollAdvisor.h"
#include "ir/FlatVerifier.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
Instruction* add(Module& m, BasicBlock* b, Instruction* i) {
  m.ownInst(i); b->addInst(i); return i;
}
void testCanonical() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("loop", i32);
  BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi,
                                               m.getIntConst(i32, 10), {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* t1 = add(m, body, createBin(Opcode::Add, phi, m.getIntConst(i32, 5), {}));
  Instruction* t2 = add(m, body, createBin(Opcode::Add, t1, m.getIntConst(i32, 7), {}));
  Instruction* next = add(m, body, createBin(Opcode::Add, phi, m.getIntConst(i32, 1), {}));
  (void)t2;
  add(m, body, createBr(header, {}));
  phi->setOperand(1, next);
  add(m, exit, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "advisor fixture verifies");
  AnalysisManager manager(m);
  UnrollProfile profile;
  profile.target = UnrollTarget::AArch64;
  profile.scalarRegisters = 32;
  auto advice = adviseUnrolls(*f, manager, profile);
  check(advice.has_value() && advice->loops.size() == 1 &&
        advice->loops[0].tripCount == 10 && advice->loops[0].hotness >= 0.9 &&
        advice->loops[0].plan.unroll && advice->loops[0].plan.factor >= 2,
        "BFI and the loop shape feed a hot canonical unroll plan");
}
void testUnsupportedShape() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("dynamic", i32);
  BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
  Argument* n = m.createArgument(i32, {});
  f->addParam(n);
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi, n, {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* next = add(m, body, createBin(Opcode::Add, phi, m.getIntConst(i32, 1), {}));
  add(m, body, createBr(header, {}));
  phi->setOperand(1, next);
  add(m, exit, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  AnalysisManager manager(m);
  UnrollProfile profile;
  auto advice = adviseUnrolls(*f, manager, profile);
  check(advice.has_value() && advice->loops.empty(),
        "non-constant trip count is skipped rather than guessed");
}
void testStepThreeLiveout() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("step_three", i32);
  BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(
      i32, {m.getIntConst(i32, 0), nullptr}, {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(
      IPred::Slt, phi, m.getIntConst(i32, 10), {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* next = add(m, body, createBin(
      Opcode::Add, phi, m.getIntConst(i32, 3), {}));
  add(m, body, createBin(Opcode::Add, next, m.getIntConst(i32, 0), {}));
  add(m, body, createBr(header, {}));
  phi->setOperand(1, next);
  add(m, exit, createPhi(i32, {phi}, {header}, {}));
  add(m, exit, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "step-three liveout fixture verifies");
  AnalysisManager manager(m); UnrollProfile profile;
  auto advice = adviseUnrolls(*f, manager, profile);
  check(advice.has_value() && advice->loops.size() == 1 &&
        advice->loops[0].tripCount == 4,
        "step-three recurrence with an exit phi has the exact trip count");
}
void testRejectsNonCanonicalEdges() {
  const Type* i32 = typePool().i32();
  {
    Module m; Function* f = m.createFunction("swapped", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
    BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    Instruction* phi = add(m, header, createPhi(
        i32, {m.getIntConst(i32, 0), nullptr}, {pre, body}, {}));
    Instruction* cmp = add(m, header, createICmp(
        IPred::Slt, phi, m.getIntConst(i32, 10), {}));
    add(m, header, createCondBr(cmp, exit, body, {}));
    Instruction* next = add(m, body, createBin(
        Opcode::Add, phi, m.getIntConst(i32, 1), {}));
    add(m, body, createBr(header, {})); phi->setOperand(1, next);
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    m.rebuildCFG(); m.rebuildUseDef();
    check(verifyFlatModule(m).empty(), "swapped-edge fixture verifies");
    AnalysisManager manager(m); UnrollProfile profile;
    auto advice = adviseUnrolls(*f, manager, profile);
    check(advice.has_value() && advice->loops.empty(),
          "swapped branch polarity is not a canonical counted loop");
  }
  {
    Module m; Function* f = m.createFunction("early_exit", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
    BasicBlock* body = m.createBlock(f); BasicBlock* early = m.createBlock(f);
    BasicBlock* latch = m.createBlock(f);
    BasicBlock* exit = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    Instruction* phi = add(m, header, createPhi(
        i32, {m.getIntConst(i32, 0), nullptr}, {pre, latch}, {}));
    Instruction* cmp = add(m, header, createICmp(
        IPred::Slt, phi, m.getIntConst(i32, 10), {}));
    add(m, header, createCondBr(cmp, body, exit, {}));
    add(m, body, createCondBr(m.getIntConst(i32, 1), early, latch, {}));
    Instruction* next = add(m, latch, createBin(
        Opcode::Add, phi, m.getIntConst(i32, 1), {}));
    add(m, latch, createBr(header, {}));
    phi->setOperand(1, next);
    add(m, early, createBr(exit, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    m.rebuildCFG(); m.rebuildUseDef();
    check(verifyFlatModule(m).empty(), "early-exit fixture verifies");
    AnalysisManager manager(m); UnrollProfile profile;
    auto advice = adviseUnrolls(*f, manager, profile);
    check(advice.has_value() && advice->loops.empty(),
          "a body early exit prevents exact-trip advice");
  }
}
void testControllingCompareAndOverflow() {
  const Type* i32 = typePool().i32();
  {
    Module m; Function* f = m.createFunction("dynamic_unused", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
    BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
    Argument* n = m.createArgument(i32, {}); f->addParam(n);
    add(m, pre, createBr(header, {}));
    Instruction* phi = add(m, header, createPhi(
        i32, {m.getIntConst(i32, 0), nullptr}, {pre, body}, {}));
    Instruction* actual = add(m, header, createICmp(IPred::Slt, phi, n, {}));
    add(m, header, createICmp(IPred::Slt, phi, m.getIntConst(i32, 10), {}));
    add(m, header, createCondBr(actual, body, exit, {}));
    Instruction* next = add(m, body, createBin(
        Opcode::Add, phi, m.getIntConst(i32, 1), {}));
    add(m, body, createBr(header, {}));
    phi->setOperand(1, next);
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    m.rebuildCFG(); m.rebuildUseDef();
    check(verifyFlatModule(m).empty(), "dynamic controlling-compare fixture verifies");
    AnalysisManager manager(m); UnrollProfile profile;
    auto advice = adviseUnrolls(*f, manager, profile);
    check(advice.has_value() && advice->loops.empty(),
          "unused constant comparison cannot override the dynamic loop bound");
  }
  {
    Module m; Function* f = m.createFunction("overflow", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
    BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    Instruction* phi = add(m, header, createPhi(
        i32, {m.getIntConst(i32, 2147483643), nullptr}, {pre, body}, {}));
    Instruction* cmp = add(m, header, createICmp(
        IPred::Slt, phi, m.getIntConst(i32, 2147483647), {}));
    add(m, header, createCondBr(cmp, body, exit, {}));
    Instruction* next = add(m, body, createBin(
        Opcode::Add, phi, m.getIntConst(i32, 3), {}));
    add(m, body, createBr(header, {}));
    phi->setOperand(1, next);
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    m.rebuildCFG(); m.rebuildUseDef();
    check(verifyFlatModule(m).empty(), "overflowing recurrence fixture verifies");
    AnalysisManager manager(m); UnrollProfile profile;
    auto advice = adviseUnrolls(*f, manager, profile);
    check(advice.has_value() && advice->loops.empty(),
          "i32 induction overflow is excluded from constant-trip advice");
  }
}
}  // namespace
int main() {
  testCanonical();
  testUnsupportedShape();
  testStepThreeLiveout();
  testRejectsNonCanonicalEdges();
  testControllingCompareAndOverflow();
  std::printf("S25A_ADVISOR_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
