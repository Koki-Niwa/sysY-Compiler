// S14b: branch probabilities, block frequencies and integer value facts.
#include <cstdio>
#include <string>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/BranchProbabilityAnalysis.h"
#include "analysis/ValueFacts.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
struct Cfg {
  Module m;
  Function* f = nullptr;
  std::vector<BasicBlock*> blocks;
  explicit Cfg(size_t n) {
    f = m.createFunction("cfg", typePool().voidTy());
    for (size_t i = 0; i < n; ++i) blocks.push_back(m.createBlock(f));
  }
  void edges(size_t from, std::vector<size_t> successors) {
    BasicBlock* block = blocks[from];
    if (successors.empty()) {
      Instruction* ret = createRet(nullptr, {});
      m.ownInst(ret); block->addInst(ret);
    } else if (successors.size() == 1) {
      Instruction* br = createBr(blocks[successors[0]], {});
      m.ownInst(br); block->addInst(br);
    } else {
      Instruction* br = createCondBr(m.getIntConst(typePool().i32(), 1),
                                     blocks[successors[0]], blocks[successors[1]], {});
      m.ownInst(br); block->addInst(br);
    }
  }
  void finish() { m.rebuildCFG(); m.rebuildUseDef(); }
};
void testProbabilityAndFrequency() {
  // Unconditional latch: while loop with header exit.
  Cfg c(4); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{}); c.finish();
  AnalysisManager am(c.m);
  auto bp = am.get<BranchProbabilityAnalysis>(*c.f);
  auto bf = am.get<BlockFrequencyAnalysis>(*c.f);
  check(bp.isCurrent() && bf.isCurrent(), "hotness analyses computed");
  check(bp.get()->probability(1,0) > bp.get()->probability(1,1) &&
        bp.get()->probability(2,0) == 1.0,
        "loop header favours the body; unconditional latch is certain");
  check(bf.get()->frequency(1) > bf.get()->frequency(2) &&
        bf.get()->frequency(2) > bf.get()->frequency(3) &&
        bf.get()->frequency(3) > 0.0,
        "header/body/exit frequency ordering");
  // Conditional latch: back edge is rare, exit is common.
  Cfg d(4); d.edges(0,{1}); d.edges(1,{2,3}); d.edges(2,{1,3}); d.edges(3,{}); d.finish();
  AnalysisManager dm(d.m);
  auto dbp = dm.get<BranchProbabilityAnalysis>(*d.f);
  auto dbf = dm.get<BlockFrequencyAnalysis>(*d.f);
  check(dbp.get()->probability(2,0) == 0.1 && dbp.get()->probability(2,1) == 0.9,
        "conditional latch back edge is rare");
  check(dbf.get()->frequency(1) > dbf.get()->frequency(2) &&
        dbf.get()->frequency(2) > 0.9e6 &&
        dbf.get()->frequency(3) > 0.9e6,
        "conditional loop header is hottest; both exits are frequent");
  // Diamond: even split.
  Cfg e(4); e.edges(0,{1,2}); e.edges(1,{3}); e.edges(2,{3}); e.edges(3,{}); e.finish();
  AnalysisManager em(e.m);
  auto ebp = em.get<BranchProbabilityAnalysis>(*e.f);
  check(ebp.get()->probability(0,0) == 0.5 && ebp.get()->probability(0,1) == 0.5,
        "non-loop diamond is even");
}
void testValueFacts() {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("facts", i32);
  BasicBlock* entry = m.createBlock(f);
  Constant* five = m.getIntConst(i32, 5);
  Constant* zero = m.getIntConst(i32, 0);
  Instruction* add = createBin(Opcode::Add, five, five, {});
  Instruction* mul = createBin(Opcode::Mul, five, five, {});
  Instruction* sub = createBin(Opcode::Sub, five, five, {});
  Instruction* div = createBin(Opcode::SDiv, five, five, {});
  Instruction* cmp = createICmp(IPred::Slt, five, five, {});
  Instruction* big = createBin(Opcode::Mul, m.getIntConst(i32, 100000),
                               m.getIntConst(i32, 100000), {});
  Instruction* byZero = createBin(Opcode::SDiv, five, zero, {});
  Instruction* load = createLoad(i32, createAlloca(i32, {}), {});
  for (Instruction* inst : {add, mul, sub, div, cmp, big, byZero, load}) {
    m.ownInst(inst); entry->addInst(inst);
  }
  Instruction* ret = createRet(add, {});
  m.ownInst(ret); entry->addInst(ret);
  m.rebuildCFG();
  m.rebuildUseDef();
  AnalysisManager am(m);
  auto facts = am.get<ValueFactsAnalysis>(*f);
  check(facts.isCurrent(), "value facts computed");
  auto range = [&](const Value* v) { return facts.get()->rangeOf(v); };
  check(range(add).known && range(add).lo == 10 && range(add).hi == 10, "add exact range");
  check(range(mul).known && range(mul).lo == 25, "mul exact range");
  check(range(sub).known && range(sub).lo == 0, "sub exact range");
  check(range(div).known && range(div).lo == 1 && range(div).hi == 1, "div exact range");
  check(range(cmp).known && range(cmp).lo == 0 && range(cmp).hi == 1, "icmp boolean range");
  check(!range(big).known, "overflowing product is unknown");
  check(!range(byZero).known, "division by zero is unknown");
  check(!range(load).known, "load is unknown");
}
void testInvalidation() {
  Cfg c(4); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{}); c.finish();
  AnalysisManager am(c.m);
  auto bp = am.get<BranchProbabilityAnalysis>(*c.f);
  auto bf = am.get<BlockFrequencyAnalysis>(*c.f);
  auto facts = am.get<ValueFactsAnalysis>(*c.f);
  check(bp.isCurrent() && bf.isCurrent() && facts.isCurrent(), "fixture analyses current");
  IRMutator mutator(c.m);
  CFGEditor editor(mutator);
  check(editor.splitEdge(c.blocks[1], 0).result == MutationResult::Applied,
        "tracked CFG edge split");
  check(!bp.isCurrent() && !bf.isCurrent() && !facts.isCurrent(),
        "CFG mutation invalidates probability, frequency and value facts");
  auto rebuilt = am.get<BranchProbabilityAnalysis>(*c.f);
  check(rebuilt.isCurrent(), "probability recomputes after CFG mutation");
}
}  // namespace
int main() {
  testProbabilityAndFrequency();
  testValueFacts();
  testInvalidation();
  std::printf("S14B_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
