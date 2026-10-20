// S25b: pair-unrolling of an even-trip canonical loop.
#include <cstdio>
#include <fstream>
#include <limits>
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
             bool expectChange, size_t registers = 32) {
  Loop loop(start, bound, step, liveOut);
  check(verifyFlatModule(loop.m).empty(), "unroll fixture verifies");
  const std::string before = dumpModule(loop.m);
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopUnrollPass>(UnrollProfile{
      UnrollTarget::AArch64, registers, 1}));
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
    const size_t factor = static_cast<size_t>(
        r.invocations[0].stats.counters.at("unroll-factor"));
    check(factor >= 2 && factor <= 4, "unroll factor is bounded");
    if (std::string(name) == "factor2")
      check(factor == 2, "factor2 profile selects exactly two");
    check(loop.header->terminator()->numSuccs() == 2 &&
          loop.header->terminator()->succ(0) == loop.body,
          "header body edge is preserved");
    BasicBlock* cursor = loop.body->terminator()->succ(0);
    size_t copies = 1;
    while (cursor != loop.header && copies < 8) {
      check(cursor->terminator() != nullptr &&
                cursor->terminator()->numSuccs() == 1,
            "chain body has one successor");
      cursor = cursor->terminator()->succ(0);
      ++copies;
    }
    check(cursor == loop.header && copies == factor,
          "chain has one body per unroll slot");
    BasicBlock* last = loop.header->preds().empty()
                           ? nullptr
                           : loop.header->preds().back();
    check(loop.header->preds().size() == 2 && last != loop.pre &&
              last != loop.body && last->terminator()->succ(0) == loop.header,
          "header has the original preheader and the last chain block");
    size_t stores = 0;
    for (BasicBlock* block : loop.f->blocks())
      for (Instruction* inst : block->insts())
        if (inst->op() == Opcode::Store) ++stores;
    check(stores == factor + 1,
          "one store per unrolled body plus one tail body");
    saveIR(name, before, loop.m);
  } else {
    check(before == dumpModule(loop.m), "unsupported loop is left unchanged");
  }
}
void testPreheaderOperandDeclined() {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("main", i32);
  GlobalAddr* global = m.globalAddr(m.createGlobal("g", i32));
  BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
  Instruction* outside = add(m, pre, createBin(Opcode::Add,
                                               m.getIntConst(i32, 1),
                                               m.getIntConst(i32, 2), {}));
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                             {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi,
                                               m.getIntConst(i32, 4), {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* old = add(m, body, createLoad(i32, global, {}));
  Instruction* sum = add(m, body, createBin(Opcode::Add, old, outside, {}));
  add(m, body, createStore(i32, sum, global, {}));
  Instruction* next = add(m, body, createBin(Opcode::Add, phi,
                                             m.getIntConst(i32, 1), {}));
  add(m, body, createBr(header, {}));
  Instruction* result = add(m, exit, createLoad(i32, global, {}));
  add(m, exit, createRet(result, {}));
  phi->setOperand(1, next);
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "preheader-operand fixture verifies");
  const std::string before = dumpModule(m);
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopUnrollPass>(UnrollProfile{
      UnrollTarget::AArch64, 32, 1}));
  PassManagerOptions options;
  options.verifyEach = true;
  const auto r = pm.run(m, options);
  check(r.ok() && !r.changed && before == dumpModule(m) &&
            verifyFlatModule(m).empty(),
        "unsupported preheader operand declines before mutation");
}

void testArgumentClone() {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("main", i32);
  Argument* n = m.createArgument(i32, {});
  f->addParam(n);
  GlobalAddr* global = m.globalAddr(m.createGlobal("g", i32));
  BasicBlock* pre = m.createBlock(f); BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f); BasicBlock* exit = m.createBlock(f);
  pre->addParam(n);
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                             {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi,
                                               m.getIntConst(i32, 4), {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* old = add(m, body, createLoad(i32, global, {}));
  Instruction* sum = add(m, body, createBin(Opcode::Add, old, n, {}));
  add(m, body, createStore(i32, sum, global, {}));
  Instruction* next = add(m, body, createBin(Opcode::Add, phi,
                                             m.getIntConst(i32, 1), {}));
  add(m, body, createBr(header, {}));
  Instruction* result = add(m, exit, createLoad(i32, global, {}));
  add(m, exit, createRet(result, {}));
  phi->setOperand(1, next);
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "argument-clone fixture verifies");
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopUnrollPass>(UnrollProfile{
      UnrollTarget::AArch64, 32, 1}));
  const auto r = pm.run(m);
  check(r.ok() && r.changed && verifyFlatModule(m).empty(),
        "argument operands are cloned without partial failure");
}

struct ThreeLoop {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* header = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* latch = nullptr;
  BasicBlock* exit = nullptr;
  GlobalAddr* global = nullptr;
  ThreeLoop() {
    global = m.globalAddr(m.createGlobal("g", i32));
    f = m.createFunction("main", i32);
    pre = m.createBlock(f); header = m.createBlock(f); body = m.createBlock(f);
    latch = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(header, {}));
    Instruction* phi = add(m, header, createPhi(
        i32, {m.getIntConst(i32, 0), nullptr}, {pre, latch}, {}));
    Instruction* cmp = add(m, header, createICmp(
        IPred::Slt, phi, m.getIntConst(i32, 10), {}));
    add(m, header, createCondBr(cmp, body, exit, {}));
    Instruction* old = add(m, body, createLoad(i32, global, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, phi, {}));
    add(m, body, createStore(i32, sum, global, {}));
    add(m, body, createBr(latch, {}));
    Instruction* next = add(m, latch, createBin(
        Opcode::Add, phi, m.getIntConst(i32, 1), {}));
    add(m, latch, createBr(header, {}));
    Instruction* result = add(m, exit, createLoad(i32, global, {}));
    add(m, exit, createRet(result, {}));
    phi->setOperand(1, next);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testThreeBlock() {
  ThreeLoop loop;
  check(verifyFlatModule(loop.m).empty(), "three-block fixture verifies");
  const std::string before = dumpModule(loop.m);
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopUnrollPass>(UnrollProfile{
      UnrollTarget::AArch64, 32, 1}));
  PassManagerOptions options; options.verifyEach = true; options.instrument = true;
  const auto r = pm.run(loop.m, options);
  check(r.ok() && r.changed && before != dumpModule(loop.m) &&
            verifyFlatModule(loop.m).empty(),
        "three-block loop is unrolled");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.at("unrolled-loops") == 1,
        "three-block unroll counter recorded");
  saveIR("three_block", before, loop.m);
}
}  // namespace
int main(int argc, char** argv) {
  if (argc > 2) return 2;
  if (argc == 2) artifactDir = argv[1];
  runCase("even", 0, 10, 1, false, true);
  runCase("step3", 0, 12, 3, false, true);
  runCase("odd", 0, 11, 1, false, true);
  runCase("odd_step3", 0, 9, 3, false, true);
  runCase("non_aligned_odd", 5, 15, 3, false, true);
  runCase("near_max", std::numeric_limits<int>::max() - 9,
          std::numeric_limits<int>::max() - 1, 3, false, true);
  runCase("near_min", std::numeric_limits<int>::min(),
          std::numeric_limits<int>::min() + 10, 3, false, true);
  runCase("wide_step", std::numeric_limits<int>::min(), 1, 33554432,
          false, true);
  runCase("too_many_trips", std::numeric_limits<int>::min(),
          std::numeric_limits<int>::max(), 1, false, false);
  runCase("terminal_overflow", std::numeric_limits<int>::max() - 5,
          std::numeric_limits<int>::max(), 2, false, false);
  runCase("factor2", 0, 10, 1, false, true, 12);
  runCase("live_out", 0, 10, 1, true, false);
  testArgumentClone();
  testPreheaderOperandDeclined();
  testThreeBlock();
  std::printf("S25B_UNROLL_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
