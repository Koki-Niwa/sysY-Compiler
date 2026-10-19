// S14: dominators, natural loops, Loop scope and CFG invalidation.
#include <cstdio>
#include <string>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "analysis/DominatorAnalysis.h"
#include "analysis/LoopAnalysis.h"
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
      m.ownInst(createRet(nullptr, {})); block->addInst(block->terminator());
      return;
    }
    if (successors.size() == 1) {
      Instruction* br = createBr(blocks[successors[0]], {});
      m.ownInst(br); block->addInst(br);
      return;
    }
    Value* c = m.getIntConst(typePool().i32(), 1);
    Instruction* br = createCondBr(c, blocks[successors[0]], blocks[successors[1]], {});
    m.ownInst(br); block->addInst(br);
  }
  void finish() { m.rebuildCFG(); m.rebuildUseDef(); }
};
void testDominatorsAndLoops() {
  // 1: chain
  { Cfg c(3); c.edges(0,{1}); c.edges(1,{2}); c.edges(2,{}); c.finish();
    AnalysisManager am(c.m);
    auto h = am.get<DominatorAnalysis>(*c.f); check(h.isCurrent(), "chain dominators");
    const DominatorInfo& d = *h.get();
    check(d.idom(1) == c.blocks[0] && d.idom(2) == c.blocks[1] &&
          d.dominates(c.blocks[0], c.blocks[2]) && !d.dominates(c.blocks[2], c.blocks[1]),
          "chain idom and dominance");
    auto loops = am.get<LoopInfoAnalysis>(*c.f);
    check(loops.isCurrent() && loops.get()->loops.empty(), "chain has no loop"); }
  // 2: diamond
  { Cfg c(4); c.edges(0,{1,2}); c.edges(1,{3}); c.edges(2,{3}); c.edges(3,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<DominatorAnalysis>(*c.f);
    check(h.get()->idom(3) == c.blocks[0] && !h.get()->dominates(c.blocks[1], c.blocks[3]),
          "diamond join idom"); }
  // 3: if-else with two joins
  { Cfg c(5); c.edges(0,{1,2}); c.edges(1,{3}); c.edges(2,{3}); c.edges(3,{4}); c.edges(4,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<DominatorAnalysis>(*c.f);
    check(h.get()->idom(4) == c.blocks[3], "if-else chain idom"); }
  // 4: single loop
  { Cfg c(4); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.isCurrent() && h.get()->loops.size() == 1 &&
          h.get()->loops[0].header == 1 &&
          h.get()->loops[0].blocks == std::vector<size_t>({1,2}) &&
          h.get()->loops[0].parent == -1 && h.get()->loops[0].depth == 0,
          "single loop header and blocks"); }
  // 5: nested loops
  { Cfg c(6); c.edges(0,{1}); c.edges(1,{2,5}); c.edges(2,{3,4}); c.edges(3,{2});
    c.edges(4,{1}); c.edges(5,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.isCurrent() && h.get()->loops.size() == 2 &&
          h.get()->loops[0].header == 1 && h.get()->loops[0].blocks == std::vector<size_t>({1,2,3,4}) &&
          h.get()->loops[0].depth == 0 && h.get()->loops[1].header == 2 &&
          h.get()->loops[1].parent == 0 && h.get()->loops[1].depth == 1,
          "nested loop forest"); }
  // 6: two sequential loops
  { Cfg c(7); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{4});
    c.edges(4,{5,6}); c.edges(5,{4}); c.edges(6,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.get()->loops.size() == 2 && h.get()->loops[0].header == 1 &&
          h.get()->loops[1].header == 4 && h.get()->loops[0].parent == -1 &&
          h.get()->loops[1].parent == -1, "sequential loops are siblings"); }
  // 7: self loop
  { Cfg c(3); c.edges(0,{1}); c.edges(1,{1,2}); c.edges(2,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.get()->loops.size() == 1 && h.get()->loops[0].header == 1 &&
          h.get()->loops[0].blocks == std::vector<size_t>({1}), "self loop"); }
  // 8: unreachable block
  { Cfg c(3); c.edges(0,{1}); c.edges(1,{}); c.edges(2,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<DominatorAnalysis>(*c.f);
    check(!h.get()->reachable(2) && h.get()->idom(2) == nullptr &&
          !h.get()->dominates(c.blocks[0], c.blocks[2]), "unreachable block has no dominator"); }
  // 9: multi-backedge loop
  { Cfg c(5); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{1,4});
    c.edges(4,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.get()->loops.size() == 1 && h.get()->loops[0].blocks ==
          std::vector<size_t>({1,2,3}), "multiple back edges merge into one loop"); }
  // 10: irreducible cycle (no dominator header)
  { Cfg c(4); c.edges(0,{1,2}); c.edges(1,{3}); c.edges(2,{3}); c.edges(3,{1}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.isCurrent() && h.get()->loops.empty(), "irreducible cycle has no natural loop"); }
  // 11: loop exit from header
  { Cfg c(4); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.get()->loops.size() == 1 && h.get()->loops[0].blocks.size() == 2,
          "header exit stays outside the natural loop"); }
  // 12: deeply nested with inner back edge
  { Cfg c(5); c.edges(0,{1}); c.edges(1,{2,4}); c.edges(2,{3,1}); c.edges(3,{2});
    c.edges(4,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<LoopInfoAnalysis>(*c.f);
    check(h.get()->loops.size() == 2 && h.get()->loops[0].header == 1 &&
          h.get()->loops[1].header == 2 && h.get()->loops[1].parent == 0,
          "inner back edge nests under outer header"); }
  // Direct DominatorTree and DominatorAnalysis agree (one implementation).
  { Cfg c(6); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{4,5});
    c.edges(4,{3}); c.edges(5,{}); c.finish();
    AnalysisManager am(c.m); auto h = am.get<DominatorAnalysis>(*c.f);
    DominatorTree direct(*c.f);
    bool same = true;
    for (size_t i = 0; i < c.blocks.size(); ++i)
      same = same && h.get()->idom(i) == direct.idom(i) &&
             h.get()->reachable(i) == direct.reachable(i);
    check(same, "analysis wraps the FlatVerifier dominator implementation"); }
}
void testLoopScopeAndInvalidation() {
  Cfg c(4); c.edges(0,{1}); c.edges(1,{2,3}); c.edges(2,{1}); c.edges(3,{}); c.finish();
  AnalysisManager am(c.m);
  auto dom = am.get<DominatorAnalysis>(*c.f);
  auto loops = am.get<LoopInfoAnalysis>(*c.f);
  auto view = am.get<LoopViewAnalysis>(*c.f, 0);
  auto view2 = am.get<LoopViewAnalysis>(*c.f, 0);
  check(view.isCurrent() && view.get()->header == 1 && view.get()->blockCount == 2 &&
        view.generation() == view2.generation(), "loop scope caches per loop slot");
  auto outOfRange = am.get<LoopViewAnalysis>(*c.f, 7);
  check(!outOfRange.isCurrent(), "out-of-range loop slot fails closed");
  IRMutator mutator(c.m);
  CFGEditor cfg(mutator);
  // Redirect entry 0 -> 1, creating a new predecessor on 1; the loop body and
  // the loop itself remain, but revisions must invalidate every handle.
  check(cfg.splitEdge(c.blocks[1], 0).result == MutationResult::Applied,
        "tracked CFG edge split");
  check(!dom.isCurrent() && !loops.isCurrent() && !view.isCurrent(),
        "CFG mutation invalidates dominator, loop and Loop-scope handles");
  auto rebuilt = am.get<LoopInfoAnalysis>(*c.f);
  check(rebuilt.isCurrent() && rebuilt.get()->loops.size() == 1,
        "loop information recomputes after CFG mutation");
}
}  // namespace
int main() {
  testDominatorsAndLoops();
  testLoopScopeAndInvalidation();
  std::printf("S14_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
