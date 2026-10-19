// S12: call graph, SCC order, summaries, CGSCC cache and invalidation.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "analysis/CallGraph.h"
#include "analysis/FunctionSummary.h"
#include "ir/FlatVerifier.h"
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
const FunctionSummary* summaryOf(const FunctionSummaryInfo& info,
                                 const CallGraphInfo& graph, const char* name) {
  for (size_t i = 0; i < graph.nodes.size(); ++i)
    if (graph.nodes[i].function->name() == name) return &info.perFunction[i];
  return nullptr;
}
bool hasCallee(const CallGraphNode& node, size_t index) {
  for (size_t callee : node.callees)
    if (callee == index) return true;
  return false;
}
void testLinearScc() {
  Module m;
  const Type* i32 = typePool().i32();
  Value* zero = m.getIntConst(i32, 0);
  Function* mainFn = m.createFunction("main", i32);
  BasicBlock* mainBlock = m.createBlock(mainFn);
  add(m, mainBlock, createCall("a", i32, {}, {}));
  add(m, mainBlock, createRet(zero, {}));
  Function* a = m.createFunction("a", i32);
  BasicBlock* aBlock = m.createBlock(a);
  add(m, aBlock, createCall("b", i32, {}, {}));
  add(m, aBlock, createRet(zero, {}));
  Function* b = m.createFunction("b", i32);
  add(m, m.createBlock(b), createRet(zero, {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "linear fixture verifies");

  AnalysisManager manager(m);
  AnalysisHandle<CallGraphInfo> handle = manager.get<CallGraphAnalysis>();
  check(handle.isCurrent() && handle.get() != nullptr, "call graph is cached");
  const CallGraphInfo& graph = *handle.get();
  check(graph.nodes.size() == 3, "three non-declaration nodes");
  check(graph.nodes[0].callees == std::vector<size_t>{1} &&
        graph.nodes[1].callees == std::vector<size_t>{2} &&
        graph.nodes[2].callees.empty(), "main->a->b edges");
  check(graph.nodes[1].callers == std::vector<size_t>{0} &&
        graph.nodes[2].callers == std::vector<size_t>{1}, "reverse callers");
  check(graph.sccs.size() == 3 &&
        graph.sccs[0].members == std::vector<size_t>{2} &&
        graph.sccs[1].members == std::vector<size_t>{1} &&
        graph.sccs[2].members == std::vector<size_t>{0},
        "SCCs are emitted bottom-up");
  check(graph.nodes[0].scc == 2 && graph.nodes[1].scc == 1 &&
        graph.nodes[2].scc == 0 && !graph.sccs[0].recursive,
        "bottom-up numbering and non-recursive flag");
  check(hasCallee(graph.nodes[0], 1) && !hasCallee(graph.nodes[0], 2),
        "call graph keeps only direct edges");
}
void testPurityAndEffects() {
  Module m;
  const Type* i32 = typePool().i32();
  Value* zero = m.getIntConst(i32, 0);
  Value* one = m.getIntConst(i32, 1);
  GlobalVariable* global = m.createGlobal("g", i32);
  GlobalAddr* address = m.globalAddr(global);
  auto def = [&](const char* name) {
    Function* f = m.createFunction(name, i32);
    return std::make_pair(f, m.createBlock(f));
  };
  { auto [f, b] = def("calc"); add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("reader"); add(m, b, createLoad(i32, address, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("writer"); add(m, b, createStore(i32, one, address, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("ext"); add(m, b, createCall("putint", nullptr, {one}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("caller_writer"); add(m, b, createCall("writer", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("caller_calc"); add(m, b, createCall("calc", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("rec_a"); add(m, b, createCall("rec_b", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("rec_b"); add(m, b, createCall("rec_a", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("rec_write_a"); add(m, b, createCall("rec_write_b", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  { auto [f, b] = def("rec_write_b"); add(m, b, createStore(i32, one, address, {}));
    add(m, b, createCall("rec_write_a", i32, {}, {}));
    add(m, b, createRet(zero, {})); }
  m.rebuildCFG();
  m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "purity fixture verifies");

  AnalysisManager manager(m);
  auto graphHandle = manager.get<CallGraphAnalysis>();
  check(graphHandle.isCurrent(), "purity call graph");
  const CallGraphInfo& graph = *graphHandle.get();
  auto summaryHandle = manager.get<FunctionSummaryAnalysis>();
  check(summaryHandle.isCurrent(), "function summaries computed");
  const FunctionSummaryInfo& info = *summaryHandle.get();
  auto pureOf = [&](const char* name) {
    const FunctionSummary* s = summaryOf(info, graph, name);
    return s != nullptr && s->pure;
  };
  auto readsOf = [&](const char* name) {
    const FunctionSummary* s = summaryOf(info, graph, name);
    return s != nullptr && s->readsMemory;
  };
  auto writesOf = [&](const char* name) {
    const FunctionSummary* s = summaryOf(info, graph, name);
    return s != nullptr && s->writesMemory;
  };
  auto externalOf = [&](const char* name) {
    const FunctionSummary* s = summaryOf(info, graph, name);
    return s != nullptr && s->callsExternal;
  };
  check(pureOf("calc") && !readsOf("calc") && !writesOf("calc"),
        "arithmetic-only function is pure");
  check(!pureOf("reader") && readsOf("reader"), "loads are not pure (mutable state)");
  check(!pureOf("writer") && writesOf("writer"), "store is impure and writes");
  check(!pureOf("ext") && externalOf("ext"), "external call is impure");
  check(!pureOf("caller_writer") && writesOf("caller_writer"),
        "impurity and writes propagate to callers");
  check(pureOf("caller_calc"), "calling a pure function stays pure");
  check(pureOf("rec_a") && pureOf("rec_b") && !writesOf("rec_a") &&
        !writesOf("rec_b"),
        "mutual recursion without effects is pure");
  check(!pureOf("rec_write_a") && !pureOf("rec_write_b") &&
        writesOf("rec_write_a") && writesOf("rec_write_b"),
        "recursive SCC writes reach every member");
  const FunctionSummary* recA = summaryOf(info, graph, "rec_a");
  check(recA != nullptr && recA->recursive && recA->scc ==
        summaryOf(info, graph, "rec_b")->scc, "recursive flag and SCC id");
}
void testCgsccCacheAndInvalidation() {
  Module m;
  const Type* i32 = typePool().i32();
  Value* zero = m.getIntConst(i32, 0);
  GlobalVariable* global = m.createGlobal("g", i32);
  GlobalAddr* address = m.globalAddr(global);
  Function* pure = m.createFunction("pure", i32);
  BasicBlock* block = m.createBlock(pure);
  add(m, block, createRet(zero, {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  AnalysisManager manager(m);
  auto graphHandle = manager.get<CallGraphAnalysis>();
  auto summaryHandle = manager.get<FunctionSummaryAnalysis>();
  check(graphHandle.isCurrent() && summaryHandle.isCurrent(),
        "fixture analyses are current");
  const CallGraphInfo& graph = *graphHandle.get();
  const CallGraphInfo* staleGraph = graphHandle.get();
  const FunctionSummaryInfo& before = *summaryHandle.get();
  check(before.perFunction[0].pure, "fixture starts pure");
  const uint64_t summaryGeneration = summaryHandle.generation();

  auto sccHandle = manager.get<SccSummaryAnalysis>(graph, graph.nodes[0].scc);
  auto sccAgain = manager.get<SccSummaryAnalysis>(graph, graph.nodes[0].scc);
  check(sccHandle.isCurrent() && sccAgain.isCurrent() &&
        sccHandle.generation() == sccAgain.generation(),
        "CGSCC results are reused across queries");

  IRMutator mutator(m);
  Instruction* store = createStore(i32, m.getIntConst(i32, 1), address, {});
  check(mutator.insertInstruction(block, 0, store) == MutationResult::Applied,
        "tracked store insertion");
  check(!summaryHandle.isCurrent() && summaryHandle.get() == nullptr,
        "memory mutation invalidates the summary handle");
  check(!sccHandle.isCurrent(), "memory mutation invalidates the SCC handle");
  check(graphHandle.isCurrent(), "call graph survives a memory-only mutation");
  auto rebuilt = manager.get<FunctionSummaryAnalysis>();
  check(rebuilt.isCurrent() && rebuilt.generation() != summaryGeneration &&
        !rebuilt.get()->perFunction[0].pure &&
        rebuilt.get()->perFunction[0].writesMemory,
        "recomputed summary sees the store");

  Instruction* call = createCall("putint", nullptr, {zero}, {});
  check(mutator.insertInstruction(block, 0, call) == MutationResult::Applied,
        "tracked external call insertion");
  check(!graphHandle.isCurrent(), "call mutation invalidates the call graph");
  check(!rebuilt.isCurrent(), "call mutation invalidates the old summary");
  auto afterCall = manager.get<FunctionSummaryAnalysis>();
  check(afterCall.isCurrent() && afterCall.get()->perFunction[0].callsExternal,
        "recomputed summary sees the external call");
  auto stale = manager.get<SccSummaryAnalysis>(*staleGraph, 0);
  check(!stale.isCurrent(), "stale call graph cannot seed a CGSCC result");
}
}  // namespace
int main() {
  testLinearScc();
  testPurityAndEffects();
  testCgsccCacheAndInvalidation();
  std::printf("S12_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
