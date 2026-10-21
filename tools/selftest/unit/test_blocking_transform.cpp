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
  Nest(int n, int innerBound, bool extraStore = false, bool latchStore = false,
       bool badHeaderCond = false, bool preOperand = false,
       bool headerOperand = false, bool exitPhi = false,
       bool sameStore = false, bool innerLatchStore = false) {
    global = m.globalAddr(m.createGlobal("g", i32));
    GlobalAddr* other = m.globalAddr(m.createGlobal("h", i32));
    f = m.createFunction("main", i32);
    pre = m.createBlock(f); outer = m.createBlock(f); inner = m.createBlock(f);
    body = m.createBlock(f); innerLatch = m.createBlock(f);
    outerLatch = m.createBlock(f); exit = m.createBlock(f);
    Instruction* flag = badHeaderCond
        ? add(m, pre, createICmp(IPred::Slt, m.getIntConst(i32, 1),
                                 m.getIntConst(i32, 0), {}))
        : nullptr;
    Instruction* preValue = preOperand
        ? add(m, pre, createBin(Opcode::Add, m.getIntConst(i32, 1),
                                m.getIntConst(i32, 2), {}))
        : nullptr;
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {pre, outerLatch}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, n), {}));
    add(m, outer, createCondBr(badHeaderCond ? flag : oc, inner, exit, {}));
    Instruction* ij = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {outer, innerLatch}, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, ij,
                                               m.getIntConst(i32, innerBound), {}));
    add(m, inner, createCondBr(ic, body, outerLatch, {}));
    Instruction* old = add(m, body, createLoad(i32, global, {}));
    Instruction* rhs = oi;
    if (preOperand) rhs = preValue;
    if (headerOperand) rhs = ic;
    Instruction* value = add(m, body, createBin(Opcode::Mul, rhs, ij, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, value, {}));
    add(m, body, createStore(i32, sum, global, {}));
    if (extraStore) add(m, body, createStore(i32, sum, other, {}));
    if (sameStore) add(m, body, createStore(i32, sum, global, {}));
    add(m, body, createBr(innerLatch, {}));
    Instruction* jn = add(m, innerLatch, createBin(Opcode::Add, ij,
                                                   m.getIntConst(i32, 1), {}));
    if (innerLatchStore) add(m, innerLatch, createStore(i32, jn, other, {}));
    add(m, innerLatch, createBr(inner, {}));
    Instruction* in = add(m, outerLatch, createBin(Opcode::Add, oi,
                                                   m.getIntConst(i32, 1), {}));
    if (latchStore) add(m, outerLatch, createStore(i32, in, other, {}));
    add(m, outerLatch, createBr(outer, {}));
    Instruction* result = add(m, exit, createLoad(i32, global, {}));
    if (exitPhi) {
      Instruction* ep = createPhi(i32, {m.getIntConst(i32, 0)}, {outer}, {});
      m.ownInst(ep);
      exit->insertInst(0, ep);
      add(m, exit, createRet(ep, {}));
    } else {
      add(m, exit, createRet(result, {}));
    }
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
void runCase(const char* name, int n, int m, bool extraStore, bool expectChange,
             bool latchStore = false, bool badHeaderCond = false,
             bool preOperand = false, bool headerOperand = false,
             bool exitPhi = false, bool sameStore = false,
             bool innerLatchStore = false) {
  Nest nest(n, m, extraStore, latchStore, badHeaderCond, preOperand, headerOperand,
            exitPhi, sameStore, innerLatchStore);
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

struct MapNest {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  GlobalAddr* input = nullptr;
  GlobalAddr* input2 = nullptr;
  GlobalAddr* output = nullptr;
  explicit MapNest(int n) {
    const Type* inRowTy = typePool().arrayOf(i32, n + 1);
    const Type* inMat = typePool().arrayOf(inRowTy, n + 1);
    const Type* outRowTy = typePool().arrayOf(i32, n);
    const Type* outMat = typePool().arrayOf(outRowTy, n);
    input = m.globalAddr(m.createGlobal("in", inMat));
    input2 = m.globalAddr(m.createGlobal("in2", inMat));
    output = m.globalAddr(m.createGlobal("out", outMat));
    f = m.createFunction("main", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* outer = m.createBlock(f);
    BasicBlock* inner = m.createBlock(f); BasicBlock* body = m.createBlock(f);
    BasicBlock* innerLatch = m.createBlock(f); BasicBlock* outerLatch = m.createBlock(f);
    BasicBlock* exit = m.createBlock(f);
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
    Instruction* oi64 = add(m, body, createCast(Opcode::SExt, oi, {}));
    Instruction* ij64 = add(m, body, createCast(Opcode::SExt, ij, {}));
    Instruction* ijNext = add(m, body, createBin(Opcode::Add, ij,
                                                 m.getIntConst(i32, 1), {}));
    Instruction* ijNext64 = add(m, body, createCast(Opcode::SExt, ijNext, {}));
    Instruction* inRowPtr = add(m, body, createGEP(inRowTy, input, oi64, {}));
    Instruction* inPtr = add(m, body, createGEP(i32, inRowPtr, ij64, {}));
    Instruction* old = add(m, body, createLoad(i32, inPtr, {}));
    Instruction* neighborPtr = add(m, body, createGEP(i32, inRowPtr, ijNext64, {}));
    Instruction* neighbor = add(m, body, createLoad(i32, neighborPtr, {}));
    Instruction* in2Row = add(m, body, createGEP(inRowTy, input2, oi64, {}));
    Instruction* in2Ptr = add(m, body, createGEP(i32, in2Row, ij64, {}));
    Instruction* second = add(m, body, createLoad(i32, in2Ptr, {}));
    Instruction* oldSum = add(m, body, createBin(Opcode::Add, old, neighbor, {}));
    Instruction* fullSum = add(m, body, createBin(Opcode::Add, oldSum, second, {}));
    Instruction* val = add(m, body, createBin(Opcode::Add, fullSum, oi, {}));
    Instruction* outRowPtr = add(m, body, createGEP(outRowTy, output, oi64, {}));
    Instruction* outPtr = add(m, body, createGEP(i32, outRowPtr, ij64, {}));
    add(m, body, createStore(i32, val, outPtr, {}));
    add(m, body, createBr(innerLatch, {}));
    Instruction* jn = add(m, innerLatch, createBin(Opcode::Add, ij,
                                                   m.getIntConst(i32, 1), {}));
    add(m, innerLatch, createBr(inner, {}));
    Instruction* in = add(m, outerLatch, createBin(Opcode::Add, oi,
                                                   m.getIntConst(i32, 1), {}));
    add(m, outerLatch, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, in);
    ij->setOperand(1, jn);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testMap() {
  MapNest nest(128);
  check(verifyFlatModule(nest.m).empty(), "map fixture verifies");
  const std::string before = dumpModule(nest.m);
  FlatPassManager pm; pm.addPass(std::make_unique<LoopBlockingPass>());
  PassManagerOptions options; options.verifyEach = true; options.instrument = true;
  const auto r = pm.run(nest.m, options);
  check(r.ok() && r.changed && before != dumpModule(nest.m) &&
            verifyFlatModule(nest.m).empty(),
        "map nest is transformed");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.count("blocked-maps") &&
            r.invocations[0].stats.counters.at("blocked-maps") == 1,
        "map pattern is distinguished from reduction");
  saveIR("map", before, nest.m);
}

struct ArrayReduction {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  GlobalAddr* acc = nullptr;
  explicit ArrayReduction(int n) {
    const Type* row = typePool().arrayOf(i32, n);
    const Type* mat = typePool().arrayOf(row, n);
    acc = m.globalAddr(m.createGlobal("acc", mat));
    f = m.createFunction("main", i32);
    BasicBlock* pre = m.createBlock(f); BasicBlock* outer = m.createBlock(f);
    BasicBlock* inner = m.createBlock(f); BasicBlock* body = m.createBlock(f);
    BasicBlock* innerLatch = m.createBlock(f); BasicBlock* outerLatch = m.createBlock(f);
    BasicBlock* exit = m.createBlock(f);
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
    Instruction* i64 = add(m, body, createCast(Opcode::SExt, oi, {}));
    Instruction* j64 = add(m, body, createCast(Opcode::SExt, ij, {}));
    Instruction* rowPtr = add(m, body, createGEP(row, acc, i64, {}));
    Instruction* ptr = add(m, body, createGEP(i32, rowPtr, j64, {}));
    Instruction* old = add(m, body, createLoad(i32, ptr, {}));
    Instruction* value = add(m, body, createBin(Opcode::Mul, oi, ij, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, value, {}));
    add(m, body, createStore(i32, sum, ptr, {}));
    add(m, body, createBr(innerLatch, {}));
    Instruction* jn = add(m, innerLatch, createBin(Opcode::Add, ij,
                                                   m.getIntConst(i32, 1), {}));
    add(m, innerLatch, createBr(inner, {}));
    Instruction* in = add(m, outerLatch, createBin(Opcode::Add, oi,
                                                   m.getIntConst(i32, 1), {}));
    add(m, outerLatch, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, in);
    ij->setOperand(1, jn);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testArrayReduction() {
  ArrayReduction nest(128);
  check(verifyFlatModule(nest.m).empty(), "array-reduction fixture verifies");
  const std::string before = dumpModule(nest.m);
  FlatPassManager pm; pm.addPass(std::make_unique<LoopBlockingPass>());
  PassManagerOptions options; options.verifyEach = true; options.instrument = true;
  const auto r = pm.run(nest.m, options);
  check(r.ok() && r.changed && before != dumpModule(nest.m) &&
            verifyFlatModule(nest.m).empty(),
        "in-place array reduction is transformed");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.count("blocked-reductions") &&
            r.invocations[0].stats.counters.at("blocked-reductions") == 1,
        "array reduction uses the reduction path");
  saveIR("array_reduction", before, nest.m);
}

struct NestedReduction {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  GlobalAddr* g = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* innerPre = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* innerLatch = nullptr;
  BasicBlock* outerLatch = nullptr;
  BasicBlock* exit = nullptr;
  explicit NestedReduction(int n) {
    g = m.globalAddr(m.createGlobal("g", i32));
    f = m.createFunction("main", i32);
    BasicBlock* pre = m.createBlock(f);
    outer = m.createBlock(f); innerPre = m.createBlock(f);
    inner = m.createBlock(f); body = m.createBlock(f);
    innerLatch = m.createBlock(f); outerLatch = m.createBlock(f);
    exit = m.createBlock(f);
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {pre, outerLatch}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, n), {}));
    add(m, outer, createCondBr(oc, innerPre, exit, {}));
    add(m, innerPre, createBr(inner, {}));
    Instruction* ij = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {innerPre, innerLatch}, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, ij,
                                               m.getIntConst(i32, n), {}));
    add(m, inner, createCondBr(ic, body, outerLatch, {}));
    Instruction* old = add(m, body, createLoad(i32, g, {}));
    Instruction* value = add(m, body, createBin(Opcode::Mul, oi, ij, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, old, value, {}));
    add(m, body, createStore(i32, sum, g, {}));
    add(m, body, createBr(innerLatch, {}));
    Instruction* jn = add(m, innerLatch, createBin(Opcode::Add, ij,
                                                   m.getIntConst(i32, 1), {}));
    add(m, innerLatch, createBr(inner, {}));
    Instruction* in = add(m, outerLatch, createBin(Opcode::Add, oi,
                                                   m.getIntConst(i32, 1), {}));
    add(m, outerLatch, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, in);
    ij->setOperand(1, jn);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testNestedPreheader() {
  NestedReduction nest(128);
  check(verifyFlatModule(nest.m).empty(), "nested-preheader fixture verifies");
  const std::string before = dumpModule(nest.m);
  FlatPassManager pm; pm.addPass(std::make_unique<LoopBlockingPass>());
  PassManagerOptions options; options.verifyEach = true; options.instrument = true;
  const auto r = pm.run(nest.m, options);
  check(r.ok() && r.changed && before != dumpModule(nest.m) &&
            verifyFlatModule(nest.m).empty(),
        "nested preheader/trampoline loop is blocked");
  check(r.invocations.size() == 1 &&
            r.invocations[0].stats.counters.at("blocked-reductions") == 1,
        "nested blocking counter recorded");
  saveIR("nested", before, nest.m);
}
}  // namespace
int main(int argc, char** argv) {
  if (argc > 2) return 2;
  if (argc == 2) artifactDir = argv[1];
  runCase("reduction", 128, 128, false, true);
  runCase("partial", 65, 63, false, true);
  runCase("small", 32, 32, false, false);
  runCase("inner_full", 65, 64, false, false);
  runCase("outer_full", 16, 1000, false, false);
  runCase("extra_store", 128, 128, true, false);
  runCase("same_store", 128, 128, false, false, false, false, false, false,
          true);
  runCase("inner_latch_store", 128, 128, false, false, false, false, false,
          false, false, true);
  runCase("latch_store", 128, 128, false, false, true);
  runCase("bad_header_cond", 128, 128, false, false, false, true);
  runCase("preheader_operand", 128, 128, false, false, false, false, true);
  runCase("header_operand", 128, 128, false, false, false, false, false, true);
  runCase("exit_phi", 128, 128, false, false, false, false, false, false, true);
  testMap();
  testArrayReduction();
  testNestedPreheader();
  std::printf("S24C_BLOCKING_TRANSFORM_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
