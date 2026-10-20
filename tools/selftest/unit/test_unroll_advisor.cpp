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
}  // namespace
int main() {
  testCanonical();
  testUnsupportedShape();
  std::printf("S25A_ADVISOR_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
