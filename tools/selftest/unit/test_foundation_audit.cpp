// S16b: revision/invalidation matrix over the S12-S16 analyses.
#include <cstdio>
#include <string>
#include <vector>
#include "analysis/AliasAnalysis.h"
#include "analysis/AnalysisManager.h"
#include "analysis/BlockFrequencyAnalysis.h"
#include "analysis/BranchProbabilityAnalysis.h"
#include "analysis/CallGraph.h"
#include "analysis/DominatorAnalysis.h"
#include "analysis/FunctionSummary.h"
#include "analysis/LoopAnalysis.h"
#include "analysis/ValueFacts.h"
#include "ir/CFGEditor.h"
#include "ir/Instruction.h"
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
  m.ownInst(i);
  b->addInst(i);
  return i;
}
struct Fixture {
  Module m;
  const Type* i32 = typePool().i32();
  Function* caller = nullptr;
  Function* callee = nullptr;
  Instruction* value = nullptr;
  BasicBlock* entry = nullptr;
  BasicBlock* header = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* exit = nullptr;
  GlobalAddr* global = nullptr;
  Fixture() {
    GlobalVariable* gv = m.createGlobal("g", i32);
    global = m.globalAddr(gv);
    callee = m.createFunction("leaf", i32);
    BasicBlock* cb = m.createBlock(callee);
    add(m, cb, createRet(m.getIntConst(i32, 1), {}));
    caller = m.createFunction("main", i32);
    entry = m.createBlock(caller);
    header = m.createBlock(caller);
    body = m.createBlock(caller);
    exit = m.createBlock(caller);
    value = add(m, entry, createBin(Opcode::Add, m.getIntConst(i32, 1),
                                    m.getIntConst(i32, 2), {}));
    add(m, entry, createBr(header, {}));
    add(m, header, createCondBr(m.getIntConst(i32, 1), body, exit, {}));
    add(m, body, createStore(i32, value, global, {}));
    add(m, body, createBr(header, {}));
    add(m, exit, createRet(value, {}));
    m.rebuildCFG();
    m.rebuildUseDef();
  }
};
struct Handles {
  AnalysisHandle<CallGraphInfo> graph;
  AnalysisHandle<FunctionSummaryInfo> summaries;
  AnalysisHandle<DominatorInfo> dominators;
  AnalysisHandle<LoopInfoResult> loops;
  AnalysisHandle<LoopView> loopView;
  AnalysisHandle<AliasInfo> alias;
  AnalysisHandle<BranchProbabilityInfo> probabilities;
  AnalysisHandle<BlockFrequencyInfo> frequencies;
  AnalysisHandle<ValueFactsInfo> facts;
};
Handles acquire(AnalysisManager& manager, Function& function) {
  Handles h;
  h.graph = manager.get<CallGraphAnalysis>();
  h.summaries = manager.get<FunctionSummaryAnalysis>();
  h.dominators = manager.get<DominatorAnalysis>(function);
  h.loops = manager.get<LoopInfoAnalysis>(function);
  h.loopView = manager.get<LoopViewAnalysis>(function, 0);
  h.alias = manager.get<AliasAnalysis>();
  h.probabilities = manager.get<BranchProbabilityAnalysis>(function);
  h.frequencies = manager.get<BlockFrequencyAnalysis>(function);
  h.facts = manager.get<ValueFactsAnalysis>(function);
  return h;
}
void checkAllCurrent(const Handles& h, const char* what) {
  check(h.graph.isCurrent() && h.summaries.isCurrent() &&
        h.dominators.isCurrent() && h.loops.isCurrent() &&
        h.loopView.isCurrent() && h.alias.isCurrent() &&
        h.probabilities.isCurrent() && h.frequencies.isCurrent() &&
        h.facts.isCurrent(), what);
}
void testMatrix() {
  Fixture f;
  AnalysisManager manager(f.m);
  Handles h = acquire(manager, *f.caller);
  checkAllCurrent(h, "all S12-S16 analyses start current");
  // DataFlow: rewrites the add operands only.
  IRMutator mutator(f.m);
  check(mutator.rewriteOperand(f.value, 0, f.m.getIntConst(f.i32, 9)) ==
            MutationResult::Applied,
        "tracked data-flow edit");
  check(!h.facts.isCurrent(), "DataFlow invalidates ValueFacts");
  check(h.dominators.isCurrent() && h.loops.isCurrent() &&
        h.probabilities.isCurrent() && h.frequencies.isCurrent() &&
        !h.alias.isCurrent() && h.graph.isCurrent() && h.summaries.isCurrent(),
        "DataFlow invalidates alias; CFG/call summaries stay current");
  h = acquire(manager, *f.caller);
  // CFG: split the header -> body edge.
  CFGEditor editor(mutator);
  check(editor.splitEdge(f.header, 0).result == MutationResult::Applied,
        "tracked CFG edit");
  check(!h.dominators.isCurrent() && !h.loops.isCurrent() &&
        !h.loopView.isCurrent() && !h.probabilities.isCurrent() &&
        !h.frequencies.isCurrent() && !h.facts.isCurrent(),
        "CFG invalidates dominator/loop/BPI/BFI/value facts");
  check(h.alias.isCurrent() && h.graph.isCurrent() && h.summaries.isCurrent(),
        "CFG leaves alias/call graph/summaries current");
  h = acquire(manager, *f.caller);
  // Memory: insert a store.
  Instruction* store = createStore(f.i32, f.value, f.global, {});
  check(mutator.insertInstruction(f.body, 0, store) == MutationResult::Applied,
        "tracked memory edit");
  check(!h.alias.isCurrent() && !h.summaries.isCurrent(),
        "Memory invalidates alias and summaries");
  check(h.graph.isCurrent() && h.dominators.isCurrent() &&
        h.loops.isCurrent() && h.probabilities.isCurrent() &&
        h.frequencies.isCurrent() && !h.facts.isCurrent(),
        "Memory invalidates value facts (new instruction) but not CFG analyses");
  h = acquire(manager, *f.caller);
  // Calls: insert a call to the defined leaf.
  Instruction* call = createCall("leaf", f.i32, {}, {});
  check(mutator.insertInstruction(f.body, 0, call) == MutationResult::Applied,
        "tracked call edit");
  check(!h.graph.isCurrent() && !h.summaries.isCurrent() && !h.alias.isCurrent(),
        "Calls invalidates call graph, summaries and alias");
  check(h.dominators.isCurrent() && h.loops.isCurrent() &&
        h.probabilities.isCurrent() && h.frequencies.isCurrent() &&
        !h.facts.isCurrent(), "Calls leaves CFG-only analyses current; value facts see the new instruction");
  // Lazy recompute returns a usable generation after every invalidation.
  Handles rebuilt = acquire(manager, *f.caller);
  checkAllCurrent(rebuilt, "all analyses recompute lazily after mutations");
}
}  // namespace
int main() {
  testMatrix();
  std::printf("S16B_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
