// S19: GVN dominance safety, DSE alias conservatism and DLE.
#include <cstdio>
#include <memory>
#include <vector>
#include "ir/FlatVerifier.h"
#include "opt/DeadLoadElimination.h"
#include "opt/DeadStoreElimination.h"
#include "opt/GVN.h"
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
size_t countOp(const Function& f, Opcode op) {
  size_t n = 0;
  for (BasicBlock* block : f.blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == op) ++n;
  return n;
}
Function* makeArgs(Module& m, const char* name, Argument** a, Argument** b) {
  Function* f = m.createFunction(name, typePool().i32());
  m.createBlock(f);
  *a = m.createArgument(typePool().i32(), {});
  *b = m.createArgument(typePool().i32(), {});
  f->addParam(*a); f->addParam(*b);
  return f;
}
void testGVN() {
  Module m; Argument *a, *b;
  Function* f = makeArgs(m, "same", &a, &b);
  BasicBlock* entry = f->entry();
  Instruction* first = add(m, entry, createBin(Opcode::Add, a, b, {}));
  Instruction* second = add(m, entry, createBin(Opcode::Add, a, b, {}));
  Instruction* sum = add(m, entry, createBin(Opcode::Add, first, second, {}));
  add(m, entry, createRet(sum, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager pm; pm.addPass(std::make_unique<GVNPass>());
  const auto r = pm.run(m);
  check(r.ok() && r.changed && countOp(*f, Opcode::Add) == 2 &&
        verifyFlatModule(m).empty(), "GVN eliminates a dominated duplicate");
  // Sibling branches must not be CSE'd.
  Module d; Argument *da, *db;
  Function* df = makeArgs(d, "siblings", &da, &db);
  BasicBlock* de = df->entry();
  BasicBlock* left = d.createBlock(df);
  BasicBlock* right = d.createBlock(df);
  BasicBlock* join = d.createBlock(df);
  add(d, de, createCondBr(d.getIntConst(typePool().i32(), 1), left, right, {}));
  Instruction* l = add(d, left, createBin(Opcode::Add, da, db, {}));
  add(d, left, createBr(join, {}));
  Instruction* rr = add(d, right, createBin(Opcode::Add, da, db, {}));
  add(d, right, createBr(join, {}));
  Instruction* phi = add(d, join, createPhi(typePool().i32(), {l, rr}, {left, right}, {}));
  add(d, join, createRet(phi, {}));
  d.rebuildCFG(); d.rebuildUseDef();
  FlatPassManager dm; dm.addPass(std::make_unique<GVNPass>());
  const auto dr = dm.run(d);
  check(dr.ok() && !dr.changed && countOp(*df, Opcode::Add) == 2 &&
        verifyFlatModule(d).empty(), "GVN does not merge sibling-branch values");
  // Commutative operands.
  Module c; Argument *ca, *cb;
  Function* cf = makeArgs(c, "commute", &ca, &cb);
  BasicBlock* ce = cf->entry();
  Instruction* x = add(c, ce, createBin(Opcode::Add, ca, cb, {}));
  Instruction* y = add(c, ce, createBin(Opcode::Add, cb, ca, {}));
  Instruction* z = add(c, ce, createBin(Opcode::Add, x, y, {}));
  add(c, ce, createRet(z, {}));
  c.rebuildCFG(); c.rebuildUseDef();
  FlatPassManager cm; cm.addPass(std::make_unique<GVNPass>());
  const auto cr = cm.run(c);
  check(cr.ok() && cr.changed && countOp(*cf, Opcode::Add) == 2 &&
        verifyFlatModule(c).empty(), "GVN canonicalizes commutative operands");
}
void testDSEAndDLE() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("dse", i32);
  BasicBlock* entry = m.createBlock(f);
  Instruction* slot = add(m, entry, createAlloca(i32, {}));
  add(m, entry, createStore(i32, m.getIntConst(i32, 1), slot, {}));
  add(m, entry, createStore(i32, m.getIntConst(i32, 2), slot, {}));
  Instruction* load = add(m, entry, createLoad(i32, slot, {}));
  add(m, entry, createRet(load, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager pm; pm.addPass(std::make_unique<DeadStoreEliminationPass>());
  const auto r = pm.run(m);
  check(r.ok() && r.changed && countOp(*f, Opcode::Store) == 1 &&
        verifyFlatModule(m).empty(), "DSE removes the overwritten store");
  // A load between the stores keeps the first store.
  Module d; const Type* t = typePool().i32();
  Function* df = d.createFunction("keep", t);
  BasicBlock* de = d.createBlock(df);
  Instruction* ds = add(d, de, createAlloca(t, {}));
  add(d, de, createStore(t, d.getIntConst(t, 1), ds, {}));
  Instruction* dl = add(d, de, createLoad(t, ds, {}));
  add(d, de, createStore(t, d.getIntConst(t, 2), ds, {}));
  add(d, de, createRet(dl, {}));
  d.rebuildCFG(); d.rebuildUseDef();
  FlatPassManager dm; dm.addPass(std::make_unique<DeadStoreEliminationPass>());
  const auto dr = dm.run(d);
  check(dr.ok() && !dr.changed && countOp(*df, Opcode::Store) == 2 &&
        verifyFlatModule(d).empty(), "an intervening load keeps the earlier store");
  // DLE removes only unused loads.
  Module l; const Type* lt = typePool().i32();
  Function* lf = l.createFunction("dle", lt);
  BasicBlock* le = l.createBlock(lf);
  Instruction* ls = add(l, le, createAlloca(lt, {}));
  add(l, le, createStore(lt, l.getIntConst(lt, 7), ls, {}));
  add(l, le, createLoad(lt, ls, {}));
  Instruction* used = add(l, le, createLoad(lt, ls, {}));
  add(l, le, createRet(used, {}));
  l.rebuildCFG(); l.rebuildUseDef();
  FlatPassManager lm; lm.addPass(std::make_unique<DeadLoadEliminationPass>());
  const auto lr = lm.run(l);
  check(lr.ok() && lr.changed && countOp(*lf, Opcode::Load) == 1 &&
        verifyFlatModule(l).empty(), "DLE removes only unused loads");
}
}  // namespace
int main() {
  testGVN();
  testDSEAndDLE();
  std::printf("S19_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
