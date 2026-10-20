// S24b: phi-aware terminator replacement on newly added CFG edges.
#include <cstdio>
#include "ir/CFGEditor.h"
#include "ir/FlatVerifier.h"
#include "ir/IRMutator.h"
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
void testAddPhiEdge() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("f", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* bridge = m.createBlock(f);
  BasicBlock* header = m.createBlock(f);
  BasicBlock* latch = m.createBlock(f);
  add(m, entry, createBr(bridge, {}));
  Instruction* placeholder = createUnreachable({});
  m.ownInst(placeholder);
  bridge->addInst(placeholder);
  add(m, header, createBr(latch, {}));
  add(m, latch, createBr(header, {}));
  Instruction* phi = createPhi(i32, {m.getIntConst(i32, 0)}, {latch}, {});
  m.ownInst(phi);
  header->insertInst(0, phi);
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "base phi-edge fixture verifies");
  IRMutator mutator(m);
  CFGEditor cfg(mutator);
  check(cfg.replaceTerminator(bridge, createBr(header, {}),
                              {{m.getIntConst(i32, 5)}}) == MutationResult::Applied,
        "new bridge edge supplies the header phi value");
  check(verifyFlatModule(m).empty(), "phi edge addition verifies");
  bool foundFive = false, foundZero = false;
  for (size_t i = 0; i < phi->numSuccs(); ++i) {
    if (phi->succ(i) != bridge && phi->succ(i) != latch) continue;
    const Value* value = phi->operand(i);
    if (value != nullptr && value->isConstant()) {
      const int64_t n = static_cast<const Constant*>(value)->intVal();
      foundFive = foundFive || (phi->succ(i) == bridge && n == 5);
      foundZero = foundZero || (phi->succ(i) == latch && n == 0);
    }
  }
  check(foundFive && foundZero, "phi keeps both predecessor values");
  check(cfg.replaceTerminator(bridge, createBr(header, {}),
                              {{m.getIntConst(i32, 5)}}) == MutationResult::Rejected,
        "existing phi edge rejects redundant values");
}
}  // namespace
int main() {
  testAddPhiEdge();
  std::printf("S24B_CFG_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
