// S22b: direct-self tail recursion to a memory-carried loop.
#include <cstdio>
#include <memory>
#include <vector>
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/TailRecursion.h"
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
size_t callCount(const Function& f) {
  size_t count = 0;
  for (BasicBlock* block : f.blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Call) ++count;
  return count;
}
void testTransform() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("self", i32);
  BasicBlock* entry = m.createBlock(f);
  Argument* x = m.createArgument(i32, {});
  f->addParam(x);
  Instruction* next = add(m, entry, createBin(Opcode::Add, x, m.getIntConst(i32, 1), {}));
  Instruction* call = add(m, entry, createCall("self", i32, {next}, {}));
  add(m, entry, createRet(call, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "TCO fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<TailRecursionEliminationPass>());
  const auto r = pm.run(m);
  check(r.ok() && r.changed && f->blockCount() == 2 && callCount(*f) == 0 &&
        verifyFlatModule(m).empty(),
        "single-block self tail recursion becomes a loop without a call");
  Instruction* entryTerm = f->at(0)->terminator();
  Instruction* loopTerm = f->at(1)->terminator();
  check(entryTerm != nullptr && entryTerm->op() == Opcode::Br &&
        entryTerm->numSuccs() == 1 && loopTerm != nullptr &&
        loopTerm->op() == Opcode::Br && loopTerm->numSuccs() == 1 &&
        loopTerm->succ(0) == f->at(1),
        "entry branches to the loop and the loop branches to itself");
}
void testNegative() {
  // Non-tail self call.
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("nontail", i32);
  BasicBlock* b = m.createBlock(f);
  Argument* x = m.createArgument(i32, {}); f->addParam(x);
  Instruction* call = add(m, b, createCall("nontail", i32, {x}, {}));
  Instruction* after = add(m, b, createBin(Opcode::Add, call, m.getIntConst(i32, 1), {}));
  add(m, b, createRet(after, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager pm; pm.addPass(std::make_unique<TailRecursionEliminationPass>());
  const auto r = pm.run(m);
  check(r.ok() && !r.changed && f->blockCount() == 1 && callCount(*f) == 1,
        "non-tail self call is not eliminated");
  // Body alloca blocks the transform.
  Module a; const Type* t = typePool().i32();
  Function* g = a.createFunction("with_alloca", t);
  BasicBlock* gb = a.createBlock(g);
  Argument* y = a.createArgument(t, {}); g->addParam(y);
  add(a, gb, createAlloca(t, {}));
  Instruction* c = add(a, gb, createCall("with_alloca", t, {y}, {}));
  add(a, gb, createRet(c, {}));
  a.rebuildCFG(); a.rebuildUseDef();
  FlatPassManager am; am.addPass(std::make_unique<TailRecursionEliminationPass>());
  const auto ar = am.run(a);
  check(ar.ok() && !ar.changed && g->blockCount() == 1,
        "alloca in the body blocks tail-recursion elimination");
}
}  // namespace
int main() {
  testTransform();
  testNegative();
  std::printf("S22B_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
