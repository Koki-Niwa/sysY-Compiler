// S15: cost-gated inlining V0 and conservative fallbacks.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/InlineV0.h"
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
  m.ownInst(i);
  b->addInst(i);
  return i;
}
struct Model final : TargetCostModel {
  size_t cap;
  explicit Model(size_t c) : cap(c) {}
  double callOverhead(const CostContext&) const override { return 5.0; }
  double instructionCost(const Instruction&, const CostContext&) const override { return 1.0; }
  size_t maxInlineInstructions(const CostContext&) const override { return cap; }
};
struct Fixture {
  Module m;
  const Type* i32 = typePool().i32();
  Function* main = nullptr;
  Instruction* call = nullptr;
  Fixture() {
    Function* callee = m.createFunction("add1", i32);
    BasicBlock* cb = m.createBlock(callee);
    Argument* x = m.createArgument(i32, {});
    callee->addParam(x);
    Instruction* sum = createBin(Opcode::Add, x, m.getIntConst(i32, 1), {});
    add(m, cb, sum);
    add(m, cb, createRet(sum, {}));
    main = m.createFunction("main", i32);
    BasicBlock* mb = m.createBlock(main);
    call = add(m, mb, createCall("add1", i32, {m.getIntConst(i32, 5)}, {}));
    add(m, mb, createRet(call, {}));
    m.rebuildCFG();
    m.rebuildUseDef();
  }
};
void testInlinesHotTinyCallee() {
  Fixture f;
  check(verifyFlatModule(f.m).empty(), "fixture verifies");
  FlatPassManager manager;
  manager.addPass(std::make_unique<InlineV0Pass>());
  PassManagerOptions options;
  options.instrument = true;
  const auto result = manager.run(f.m, options);
  bool hasCall = false;
  for (BasicBlock* block : f.main->blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Call) hasCall = true;
  check(result.ok() && result.changed && !hasCall &&
        verifyFlatModule(f.m).empty(),
        "hot tiny callee is inlined and the caller verifies");
  bool hasClone = false;
  for (BasicBlock* block : f.main->blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Add) hasClone = true;
  check(hasClone, "callee body is cloned into the caller");
}
void testColdAndUnsupportedAreDeclined() {
  // Cold call site: entry splits 50/50 twice, so the call runs at 0.25.
  Module m;
  const Type* i32 = typePool().i32();
  Function* callee = m.createFunction("leaf", i32);
  BasicBlock* cb = m.createBlock(callee);
  add(m, cb, createRet(m.getIntConst(i32, 7), {}));
  Function* caller = m.createFunction("main", i32);
  BasicBlock* entry = m.createBlock(caller);
  BasicBlock* mid = m.createBlock(caller);
  BasicBlock* cold = m.createBlock(caller);
  BasicBlock* other = m.createBlock(caller);
  BasicBlock* done = m.createBlock(caller);
  add(m, entry, createCondBr(m.getIntConst(i32, 1), mid, done, {}));
  add(m, mid, createCondBr(m.getIntConst(i32, 1), cold, done, {}));
  Instruction* call = add(m, cold, createCall("leaf", i32, {}, {}));
  add(m, cold, createRet(call, {}));
  add(m, other, createRet(m.getIntConst(i32, 0), {}));
  add(m, done, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  FlatPassManager manager;
  manager.addPass(std::make_unique<InlineV0Pass>());
  PassManagerOptions options;
  options.instrument = true;
  const auto result = manager.run(m, options);
  bool callRemains = false;
  for (BasicBlock* block : caller->blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Call) callRemains = true;
  check(result.ok() && !result.changed && callRemains,
        "cold call site is not inlined");
}
void testMultiBlockAllocaRecursiveAndModel() {
  // Multi-block callee.
  {
    Module m; const Type* i32 = typePool().i32();
    Function* callee = m.createFunction("multi", i32);
    BasicBlock* a = m.createBlock(callee); BasicBlock* b = m.createBlock(callee);
    add(m, a, createBr(b, {})); add(m, b, createRet(m.getIntConst(i32, 1), {}));
    Function* main = m.createFunction("main", i32);
    BasicBlock* mb = m.createBlock(main);
    Instruction* call = add(m, mb, createCall("multi", i32, {}, {}));
    add(m, mb, createRet(call, {}));
    m.rebuildCFG(); m.rebuildUseDef();
    FlatPassManager pm; pm.addPass(std::make_unique<InlineV0Pass>());
    const auto r = pm.run(m);
    bool remains = false;
    for (BasicBlock* block : main->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Call) remains = true;
    check(r.ok() && !r.changed && remains, "multi-block callee is declined");
  }
  // Alloca callee.
  {
    Module m; const Type* i32 = typePool().i32();
    Function* callee = m.createFunction("with_alloca", i32);
    BasicBlock* cb = m.createBlock(callee);
    add(m, cb, createAlloca(i32, {}));
    add(m, cb, createRet(m.getIntConst(i32, 1), {}));
    Function* main = m.createFunction("main", i32);
    BasicBlock* mb = m.createBlock(main);
    Instruction* call = add(m, mb, createCall("with_alloca", i32, {}, {}));
    add(m, mb, createRet(call, {}));
    m.rebuildCFG(); m.rebuildUseDef();
    FlatPassManager pm; pm.addPass(std::make_unique<InlineV0Pass>());
    const auto r = pm.run(m);
    bool remains = false;
    for (BasicBlock* block : main->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Call) remains = true;
    check(r.ok() && !r.changed && remains, "alloca callee is declined");
  }
  // Recursive callee.
  {
    Module m; const Type* i32 = typePool().i32();
    Function* callee = m.createFunction("rec", i32);
    BasicBlock* cb = m.createBlock(callee);
    Instruction* call = add(m, cb, createCall("rec", i32, {}, {}));
    add(m, cb, createRet(call, {}));
    Function* main = m.createFunction("main", i32);
    BasicBlock* mb = m.createBlock(main);
    Instruction* c2 = add(m, mb, createCall("rec", i32, {}, {}));
    add(m, mb, createRet(c2, {}));
    m.rebuildCFG(); m.rebuildUseDef();
    FlatPassManager pm; pm.addPass(std::make_unique<InlineV0Pass>());
    const auto r = pm.run(m);
    check(r.ok() && !r.changed, "recursive callee is declined");
  }
  // Cost model cap zero.
  {
    Fixture f;
    Model model(0);
    FlatPassManager pm;
    pm.addPass(std::make_unique<InlineV0Pass>(&model));
    const auto r = pm.run(f.m);
    bool remains = false;
    for (BasicBlock* block : f.main->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Call) remains = true;
    check(r.ok() && !r.changed && remains, "cost model cap is respected");
  }
}
}  // namespace
int main() {
  testInlinesHotTinyCallee();
  testColdAndUnsupportedAreDeclined();
  testMultiBlockAllocaRecursiveAndModel();
  std::printf("S15_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
