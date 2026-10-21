// S24/S25 opt-in loop canonicalization passes.
#include <cstdio>
#include <memory>
#include "ir/FlatVerifier.h"
#include "opt/AllocaGEPCanonicalization.h"
#include "opt/DuplicatePhiCoalescing.h"
#include "opt/FlatPassManager.h"
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
void testGEP() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("main", i32);
  BasicBlock* entry = m.createBlock(f);
  Instruction* slot = add(m, entry, createAlloca(i32, {}));
  Instruction* zero = add(m, entry, createCast(Opcode::SExt,
                                               m.getIntConst(i32, 0), {}));
  Instruction* gep = add(m, entry, createGEP(i32, slot, zero, {}));
  add(m, entry, createStore(i32, m.getIntConst(i32, 7), gep, {}));
  Instruction* load = add(m, entry, createLoad(i32, gep, {}));
  add(m, entry, createRet(load, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "zero-GEP fixture verifies");
  FlatPassManager pm;
  pm.addPass(std::make_unique<AllocaGEPCanonicalizationPass>());
  PassManagerOptions options; options.instrument = true;
  const auto r = pm.run(m, options);
  check(r.ok() && r.changed && verifyFlatModule(m).empty(),
        "zero-index alloca GEP is folded");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.count("folded-geps") &&
            r.invocations[0].stats.counters.at("folded-geps") == 1,
        "GEP fold counter recorded");
  check(gep->parent() == nullptr, "folded GEP is erased");
}
void testPhi() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("main", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* header = m.createBlock(f);
  BasicBlock* latch = m.createBlock(f);
  add(m, entry, createBr(header, {}));
  Instruction* a = add(m, header, createPhi(
      i32, {m.getIntConst(i32, 0), m.getIntConst(i32, 1)}, {entry, latch}, {}));
  Instruction* b = add(m, header, createPhi(
      i32, {m.getIntConst(i32, 0), m.getIntConst(i32, 1)}, {entry, latch}, {}));
  add(m, header, createBr(latch, {}));
  add(m, latch, createBr(header, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "duplicate-phi fixture verifies");
  FlatPassManager pm;
  pm.addPass(std::make_unique<DuplicatePhiCoalescingPass>());
  PassManagerOptions options; options.instrument = true;
  const auto r = pm.run(m, options);
  check(r.ok() && r.changed && verifyFlatModule(m).empty(),
        "duplicate phis are coalesced");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.count("coalesced-phis") &&
            r.invocations[0].stats.counters.at("coalesced-phis") == 1,
        "phi coalescing counter recorded");
  check(a->parent() == header && b->parent() == nullptr,
        "one duplicate phi is erased");
}
}  // namespace
int main() {
  testGEP();
  testPhi();
  std::printf("CANON_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
