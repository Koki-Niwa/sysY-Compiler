// S24c-b: blocked order for an integer-add reduction nest.
#include <cstdio>
#include <fstream>
#include <string>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopBlocking.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
std::string artifactDir;
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
  BasicBlock* outerLatch = nullptr;
  BasicBlock* innerLatch = nullptr;
  BasicBlock* exit = nullptr;
  GlobalAddr* global = nullptr;
  Nest(int n, bool extraStore = false) {
    global = m.globalAddr(m.createGlobal("g", i32));
    GlobalAddr* other = m.globalAddr(m.createGlobal("h", i32));
    f = m.createFunction("main", i32);
    pre = m.createBlock(f); outer = m.createBlock(f); inner = m.createBlock(f);
    body = m.createBlock(f); innerLatch = m.createBlock(f);
    outerLatch = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {pre, outerLatch}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, n), {}));
    add(m, outer, createCondBr(oc, inner, exit, {}));
    Instruction* ij = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {outer, innerLatch}, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, ij,
                                               m.getIntConst(i32, n), {}));
    add(m, inner, createCondBr(ic, body, outerLatch, {}));
    Instruction* old = add(m, body, createLoad(i32, global, {}));
    Instruction* value = add(m, body, createBin(Opcode::Mul, oi, ij, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, value, {}));
    add(m, body, createStore(i32, sum, global, {}));
    if (extraStore) add(m, body, createStore(i32, sum, other, {}));
    add(m, body, createBr(innerLatch, {}));
    Instruction* jn = add(m, innerLatch, createBin(Opcode::Add, ij,
                                                   m.getIntConst(i32, 1), {}));
    add(m, innerLatch, createBr(inner, {}));
    Instruction* in = add(m, outerLatch, createBin(Opcode::Add, oi,
                                                   m.getIntConst(i32, 1), {}));
    add(m, outerLatch, createBr(outer, {}));
    Instruction* result = add(m, exit, createLoad(i32, global, {}));
    add(m, exit, createRet(result, {}));
    oi->setOperand(1, in);
    ij->setOperand(1, jn);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void saveIR(const std::string& name, const std::string& before, const Module& m) {
  if (artifactDir.empty()) return;
  std::ofstream input(artifactDir + "/" + name + ".before.flat");
  std::ofstream output(artifactDir + "/" + name + ".after.flat");
  input << before; output << dumpModule(m);
  check(input.good() && output.good(), "blocking artifacts written");
}
void runCase(const char* name, int n, bool extraStore, bool expectChange) {
  Nest nest(n, extraStore);
  check(verifyFlatModule(nest.m).empty(), "blocking fixture verifies");
  const std::string before = dumpModule(nest.m);
  FlatPassManager pm; pm.addPass(std::make_unique<LoopBlockingPass>());
  PassManagerOptions options; options.verifyEach = true; options.instrument = true;
  const auto r = pm.run(nest.m, options);
  check(r.ok() && r.changed == expectChange && verifyFlatModule(nest.m).empty(), name);
  if (expectChange) {
    check(before != dumpModule(nest.m), "blocking actually changed the IR");
    check(nest.pre->terminator()->succ(0) != nest.outer &&
              nest.exit->preds().size() == 2,
          "preheader enters the tile nest and exit has two predecessors");
    size_t stores = 0;
    for (BasicBlock* block : nest.f->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Store) ++stores;
    check(stores == 2, "blocked nest clones the reduction body once");
    check(nest.outer->preds().size() == 1 &&
              nest.outer->preds()[0] == nest.outerLatch,
          "original outer header is left as an unreachable valid cycle");
    check(r.invocations.size() == 1 &&
              r.invocations[0].stats.counters.count("blocked-reductions") &&
              r.invocations[0].stats.counters.at("blocked-reductions") == 1,
          "blocked reduction counter recorded");
    saveIR(name, before, nest.m);
  } else {
    check(before == dumpModule(nest.m), "unsupported nest is left unchanged");
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc > 2) return 2;
  if (argc == 2) artifactDir = argv[1];
  runCase("reduction", 128, false, true);
  runCase("small", 32, false, false);
  runCase("extra_store", 128, true, false);
  std::printf("S24C_BLOCKING_TRANSFORM_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
