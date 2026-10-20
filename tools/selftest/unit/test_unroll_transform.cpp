// S25b: pair-unrolling of an even-trip canonical loop.
#include <cstdio>
#include <fstream>
#include <string>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopUnroll.h"
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
struct Loop {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* header = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* exit = nullptr;
  Instruction* phi = nullptr;
  GlobalAddr* global = nullptr;
  Loop(int start, int bound, int step, bool liveOut) {
    global = m.globalAddr(m.createGlobal("g", i32));
    f = m.createFunction("main", i32);
    pre = m.createBlock(f); header = m.createBlock(f);
    body = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    phi = add(m, header, createPhi(i32, {m.getIntConst(i32, start), nullptr},
                                   {pre, body}, {}));
    Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi,
                                                 m.getIntConst(i32, bound), {}));
    add(m, header, createCondBr(cmp, body, exit, {}));
    Instruction* old = add(m, body, createLoad(i32, global, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, phi, {}));
    add(m, body, createStore(i32, sum, global, {}));
    Instruction* next = add(m, body, createBin(Opcode::Add, phi,
                                               m.getIntConst(i32, step), {}));
    add(m, body, createBr(header, {}));
    Instruction* result = add(m, exit, createLoad(i32, global, {}));
    add(m, exit, createRet(liveOut ? phi : result, {}));
    phi->setOperand(1, next);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void saveIR(const std::string& name, const std::string& before, const Module& m) {
  if (artifactDir.empty()) return;
  std::ofstream input(artifactDir + "/" + name + ".before.flat");
  std::ofstream output(artifactDir + "/" + name + ".after.flat");
  input << before;
  output << dumpModule(m);
  check(input.good() && output.good(), "unroll artifacts written");
}
void runCase(const char* name, int start, int bound, int step, bool liveOut,
             bool expectChange) {
  Loop loop(start, bound, step, liveOut);
  check(verifyFlatModule(loop.m).empty(), "unroll fixture verifies");
  const std::string before = dumpModule(loop.m);
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopUnrollPass>(UnrollProfile{
      UnrollTarget::AArch64, 32, 1}));
  PassManagerOptions options;
  options.verifyEach = true;
  options.instrument = true;
  const auto r = pm.run(loop.m, options);
  check(r.ok() && r.changed == expectChange && verifyFlatModule(loop.m).empty(),
        name);
  if (expectChange) {
    check(r.invocations.size() == 1 &&
          r.invocations[0].stats.counters.count("unrolled-loops") &&
          r.invocations[0].stats.counters.at("unrolled-loops") == 1 &&
          before != dumpModule(loop.m), "unroll actually changed the IR");
    check(loop.header->terminator()->numSuccs() == 2 &&
          loop.header->terminator()->succ(0) == loop.body &&
          loop.body->terminator()->numSuccs() == 1 &&
          loop.body->terminator()->succ(0) != loop.header,
          "header body edge is preserved and body leaves the pair loop");
    BasicBlock* second = loop.body->terminator()->succ(0);
    check(second != loop.exit && second->terminator() != nullptr &&
          second->terminator()->numSuccs() == 1 &&
          second->terminator()->succ(0) == loop.header,
          "second body clone returns to the header");
    check(loop.header->preds().size() == 2 &&
          loop.header->preds()[1] == second,
          "header has the original preheader and the pair back edge");
    size_t stores = 0;
    for (BasicBlock* block : loop.f->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Store) ++stores;
    check(stores == 2, "pair unrolling keeps one store per body copy");
    saveIR(name, before, loop.m);
  } else {
    check(before == dumpModule(loop.m), "unsupported loop is left unchanged");
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc > 2) return 2;
  if (argc == 2) artifactDir = argv[1];
  runCase("even", 0, 10, 1, false, true);
  runCase("step3", 0, 12, 3, false, true);
  runCase("odd", 0, 11, 1, false, false);
  runCase("live_out", 0, 10, 1, true, false);
  std::printf("S25B_UNROLL_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
