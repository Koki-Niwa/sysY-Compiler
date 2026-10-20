// S24b: order-preserving strip-mining of a canonical loop.
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopTiling.h"
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
void saveIR(const std::string& name, const std::string& before, const Module& m) {
  if (artifactDir.empty()) return;
  std::ofstream input(artifactDir + "/" + name + ".before.flat");
  std::ofstream output(artifactDir + "/" + name + ".after.flat");
  input << before;
  output << dumpModule(m);
  check(input.good() && output.good(), "equivalence artifacts written");
}
struct Nested {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* latch = nullptr;
  BasicBlock* exit = nullptr;
  GlobalAddr* global = nullptr;
  explicit Nested(int bound) {
    global = m.globalAddr(m.createGlobal("g", i32));
    f = m.createFunction("main", i32);
    pre = m.createBlock(f); outer = m.createBlock(f); inner = m.createBlock(f);
    body = m.createBlock(f); latch = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                             {pre, latch}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, bound), {}));
    add(m, outer, createCondBr(oc, inner, exit, {}));
    Instruction* ii = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {outer, body}, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, ii,
                                               m.getIntConst(i32, bound), {}));
    add(m, inner, createCondBr(ic, body, latch, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, oi, ii, {}));
    add(m, body, createStore(i32, sum, global, {}));
    Instruction* iiNext = add(m, body, createBin(Opcode::Add, ii,
                                                 m.getIntConst(i32, 1), {}));
    add(m, body, createBr(inner, {}));
    Instruction* oiNext = add(m, latch, createBin(Opcode::Add, oi,
                                                  m.getIntConst(i32, 1), {}));
    add(m, latch, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, oiNext);
    ii->setOperand(1, iiNext);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testTiling() {
  Nested n(64);
  check(verifyFlatModule(n.m).empty(), "tiling fixture verifies");
  const std::string before = dumpModule(n.m);
  FlatPassManager pm; pm.addPass(std::make_unique<LoopTilingPass>());
  const auto r = pm.run(n.m);
  check(r.ok() && r.changed && verifyFlatModule(n.m).empty(),
        "strip-mine transform verifies");
  Instruction* preTerm = n.pre->terminator();
  check(preTerm != nullptr && preTerm->numSuccs() == 1 &&
        preTerm->succ(0) != n.outer,
        "preheader enters the tile header, not the old loop directly");
  BasicBlock* tileHeader = preTerm->succ(0);
  Instruction* tileTerm = tileHeader->terminator();
  check(tileTerm != nullptr && tileTerm->op() == Opcode::Br &&
        tileTerm->numSuccs() == 2 && tileTerm->succ(0) == n.outer &&
        tileTerm->succ(1) == n.exit,
        "tile header guards the original loop and final exit");
  check(n.outer->preds().size() == 2 &&
        std::find(n.outer->preds().begin(), n.outer->preds().end(), tileHeader) !=
            n.outer->preds().end(),
        "original header gains the tile predecessor");
  Instruction* outerTerm = n.outer->terminator();
  check(outerTerm->numSuccs() == 2 && outerTerm->succ(0) == n.inner,
        "original body edge remains");
  BasicBlock* tileLatch = outerTerm->succ(1);
  check(tileLatch != n.exit && tileLatch->terminator() != nullptr &&
        tileLatch->terminator()->numSuccs() == 1 &&
        tileLatch->terminator()->succ(0) == tileHeader,
        "tile latch increments and returns to the tile header");
  size_t stores = 0;
  for (BasicBlock* block : n.f->blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Store) ++stores;
  check(stores == 1, "strip-mining does not duplicate the body store");
  saveIR("nested", before, n.m);
}
void testNoTileWhenFits() {
  Nested n(8);
  check(verifyFlatModule(n.m).empty(), "small-nest fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopTilingPass>());
  const auto r = pm.run(n.m);
  check(r.ok() && !r.changed && n.pre->terminator()->succ(0) == n.outer,
        "small working set is left untiled");
}
// A sum in global memory makes both the induction values and their order
// observable. The Python CTest executes these exact before/after dumps.
void testStride(const char* name, int start, int bound, int step,
                bool liveOut = false, bool expectedChange = true) {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("main", i32);
  BasicBlock* pre = m.createBlock(f);
  BasicBlock* header = m.createBlock(f);
  BasicBlock* body = m.createBlock(f);
  BasicBlock* exit = m.createBlock(f);
  GlobalAddr* global = m.globalAddr(m.createGlobal("g", i32));
  add(m, pre, createBr(header, {}));
  Instruction* phi = add(m, header, createPhi(
      i32, {m.getIntConst(i32, start), nullptr}, {pre, body}, {}));
  Instruction* cmp = add(m, header, createICmp(
      IPred::Slt, phi, m.getIntConst(i32, bound), {}));
  add(m, header, createCondBr(cmp, body, exit, {}));
  Instruction* old = add(m, body, createLoad(i32, global, {}));
  Instruction* sum = add(m, body, createBin(Opcode::Add, old, phi, {}));
  add(m, body, createStore(i32, sum, global, {}));
  Instruction* next = add(m, body, createBin(
      Opcode::Add, phi, m.getIntConst(i32, step), {}));
  add(m, body, createBr(header, {}));
  Instruction* result = add(m, exit, createLoad(i32, global, {}));
  add(m, exit, createRet(liveOut ? phi : result, {}));
  phi->setOperand(1, next);
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "stride fixture verifies");
  const std::string before = dumpModule(m);
  FlatPassManager pm;
  pm.addPass(std::make_unique<LoopTilingPass>());
  PassManagerOptions options;
  options.verifyEach = true;
  options.instrument = true;
  const auto r = pm.run(m, options);
  check(r.ok() && r.changed == expectedChange && verifyFlatModule(m).empty(),
        name);
  if (expectedChange) {
    check(r.invocations.size() == 1 &&
          r.invocations[0].stats.counters.count("tiled-loops") &&
          r.invocations[0].stats.counters.at("tiled-loops") == 1 &&
          before != dumpModule(m), "equivalence requires an actual tiled loop");
    saveIR(name, before, m);
  } else {
    check(before == dumpModule(m), "unsupported loop declines without mutation");
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc > 2) return 2;
  if (argc == 2) artifactDir = argv[1];
  testTiling();
  testNoTileWhenFits();
  testStride("step1", 0, 64, 1);
  testStride("step3", 0, 192, 3);
  testStride("tail", 5, 202, 3);
  testStride("near_max", 2147483440, 2147483646, 3);
  testStride("near_min", std::numeric_limits<int>::min(), -2147483449, 3);
  testStride("wide_distance", std::numeric_limits<int>::min(), 1, 33554432);
  testStride("live_out", 0, 64, 1, true, false);
  testStride("wide_span", std::numeric_limits<int>::min(), 2000000000,
             100000000, false, false);
  std::printf("S24B_TILE_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
