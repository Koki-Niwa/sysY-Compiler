// S21b: guarded do-while rotation for phi-free loops and its negative cases.
#include <cstdio>
#include <memory>
#include <vector>
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopRotate.h"
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
struct RotateFixture {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* header = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* exit = nullptr;
  BasicBlock* latch = nullptr;
  GlobalAddr* global = nullptr;
  RotateFixture() {
    GlobalVariable* gv = m.createGlobal("g", i32);
    global = m.globalAddr(gv);
    f = m.createFunction("loop", i32);
    pre = m.createBlock(f); header = m.createBlock(f);
    body = m.createBlock(f); exit = m.createBlock(f); latch = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    Instruction* cond = add(m, header, createLoad(i32, global, {}));
    add(m, header, createCondBr(cond, body, exit, {}));
    Instruction* value = add(m, body, createLoad(i32, global, {}));
    Instruction* next = add(m, body, createBin(Opcode::Add, value,
                                               m.getIntConst(i32, 1), {}));
    add(m, body, createStore(i32, next, global, {}));
    add(m, body, createBr(latch, {}));
    add(m, latch, createBr(header, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testRotate() {
  RotateFixture f;
  check(verifyFlatModule(f.m).empty(), "rotate fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopRotatePass>());
  const auto r = pm.run(f.m);
  Instruction* latchTerm = f.latch->terminator();
  Instruction* headerTerm = f.header->terminator();
  check(r.ok() && r.changed && latchTerm->op() == Opcode::Br &&
        latchTerm->numSuccs() == 2 && latchTerm->succ(0) == f.body &&
        latchTerm->succ(1) == f.exit && headerTerm->numSuccs() == 2 &&
        verifyFlatModule(f.m).empty(),
        "guarded do-while rotation preserves header guard and latch exit");
  check(f.header->numPreds() == 1 && f.header->pred(0) == f.pre,
        "header no longer has the back edge after rotation");
}
void testNegativePhi() {
  RotateFixture f;
  // Add a header phi: this loop is not rotated in S21b.
  Value* one = f.m.getIntConst(f.i32, 1);
  Instruction* phi = createPhi(f.i32, {one, one}, {f.pre, f.latch}, {});
  f.m.ownInst(phi); f.header->insertInst(0, phi);
  f.m.rebuildCFG(); f.m.rebuildUseDef();
  check(verifyFlatModule(f.m).empty(), "phi negative fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopRotatePass>());
  const auto r = pm.run(f.m);
  check(r.ok() && !r.changed && f.latch->terminator()->numSuccs() == 1,
        "loop with a header phi is left untouched");
}
void testHeaderSideEffect() {
  RotateFixture f;
  // A store in the header must keep executing every iteration; rotation would
  // bypass it, so the pass must decline.
  Instruction* store = createStore(f.i32, f.m.getIntConst(f.i32, 1), f.global, {});
  f.m.ownInst(store);
  f.header->insertInst(0, store);
  f.m.rebuildCFG(); f.m.rebuildUseDef();
  check(verifyFlatModule(f.m).empty(), "header-store fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopRotatePass>());
  const auto r = pm.run(f.m);
  check(r.ok() && !r.changed && f.latch->terminator()->numSuccs() == 1,
        "header side effects block rotation");
}
}  // namespace
int main() {
  testRotate();
  testNegativePhi();
  testHeaderSideEffect();
  std::printf("S21B_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
