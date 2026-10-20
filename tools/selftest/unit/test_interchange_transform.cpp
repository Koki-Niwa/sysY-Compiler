// S23b: canonical two-level interchange with phi rewiring.
#include <cstdio>
#include <memory>
#include <vector>
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopInterchange.h"
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
struct Nest {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* innerExit = nullptr;
  BasicBlock* exit = nullptr;
  Nest(bool store) {
    f = m.createFunction("nest", i32);
    pre = m.createBlock(f); outer = m.createBlock(f); inner = m.createBlock(f);
    body = m.createBlock(f); innerExit = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                             {pre, innerExit}, {}));
    Instruction* iic = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                               {outer, body}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, 10), {}));
    add(m, outer, createCondBr(oc, inner, exit, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, iic,
                                               m.getIntConst(i32, 10), {}));
    add(m, inner, createCondBr(ic, body, innerExit, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, oi, iic, {}));
    if (store) add(m, body, createStore(i32, sum, m.globalAddr(m.createGlobal("g", i32)), {}));
    Instruction* iiNext = add(m, body, createBin(Opcode::Add, iic,
                                                 m.getIntConst(i32, 1), {}));
    add(m, body, createBr(inner, {}));
    Instruction* oiNext = add(m, innerExit, createBin(Opcode::Add, oi,
                                                      m.getIntConst(i32, 1), {}));
    add(m, innerExit, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, oiNext);
    iic->setOperand(1, iiNext);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testTransform() {
  Nest n(false);
  check(verifyFlatModule(n.m).empty(), "interchange fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopInterchangePass>());
  const auto r = pm.run(n.m);
  { auto vs = verifyFlatModule(n.m); for (auto& v : vs) std::printf("  V: %s\n", v.message.c_str()); }
  std::printf("  edges pre=%zu inner=[%p,%p] outer=[%p,%p]\n",
              n.pre->terminator()->numSuccs(),
              (void*)n.inner->terminator()->succ(0), (void*)n.inner->terminator()->succ(1),
              (void*)n.outer->terminator()->succ(0), (void*)n.outer->terminator()->succ(1));
  check(r.ok() && r.changed && verifyFlatModule(n.m).empty(),
        "interchange transform verifies");
  check(n.pre->terminator()->succ(0) == n.inner,
        "preheader enters the new outer (old inner) loop");
  check(n.inner->terminator()->succ(0) == n.outer &&
        n.inner->terminator()->succ(1) == n.exit,
        "new outer loop branches to the new inner loop or final exit");
  check(n.outer->terminator()->succ(0) == n.body &&
        n.outer->terminator()->succ(1) == n.innerExit,
        "new inner loop branches to the body or its exit");
  check(n.body->terminator()->succ(0) == n.outer &&
        n.innerExit->terminator()->succ(0) == n.inner,
        "body and inner exit close the swapped loops");
}
void testNoPhiDoesNotCrash() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("nophi", i32);
  BasicBlock* pre = m.createBlock(f); BasicBlock* o = m.createBlock(f);
  BasicBlock* i = m.createBlock(f); BasicBlock* b = m.createBlock(f);
  BasicBlock* l = m.createBlock(f); BasicBlock* e = m.createBlock(f);
  add(m, pre, createBr(o, {}));
  add(m, o, createCondBr(m.getIntConst(i32, 1), i, e, {}));
  add(m, i, createCondBr(m.getIntConst(i32, 1), b, l, {}));
  add(m, b, createBr(i, {}));
  add(m, l, createBr(o, {}));
  add(m, e, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "no-phi fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopInterchangePass>());
  const auto r = pm.run(m);
  check(r.ok() && !r.changed && verifyFlatModule(m).empty(),
        "loop without an induction-phi is declined without a crash");
}
void testHeaderStoreNegative() {
  Nest n(false);
  BasicBlock* outer = n.outer;
  Instruction* store = createStore(n.i32, n.m.getIntConst(n.i32, 1),
                                   n.m.globalAddr(n.m.createGlobal("h", n.i32)), {});
  n.m.ownInst(store);
  outer->insertInst(1, store);
  n.m.rebuildCFG(); n.m.rebuildUseDef();
  check(verifyFlatModule(n.m).empty(), "header-store interchange fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopInterchangePass>());
  const auto r = pm.run(n.m);
  check(r.ok() && !r.changed && n.pre->terminator()->succ(0) == n.outer,
        "extra header side effect blocks interchange");
}
void testStoreNegative() {
  Nest n(true);
  check(verifyFlatModule(n.m).empty(), "store fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopInterchangePass>());
  const auto r = pm.run(n.m);
  check(r.ok() && !r.changed && n.pre->terminator()->succ(0) == n.outer,
        "loop with a store is not interchanged");
}
}  // namespace
int main() {
  testTransform();
  testNoPhiDoesNotCrash();
  testHeaderStoreNegative();
  testStoreNegative();
  std::printf("S23B_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
