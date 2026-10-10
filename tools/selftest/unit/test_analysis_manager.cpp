// F02 focused tests: cache, precision, dependencies, failures, lifetime.
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include "analysis/AnalysisManager.h"
#include "analysis/ReachabilityAnalysis.h"
#include "ir/CFGEditor.h"
#include "ir/FlatVerifier.h"
#include "ir/IRMutator.h"
using namespace sysy::flat;
namespace {
int failures = 0;
void check(bool test, const char* label) {
  std::printf("%s: %s\n", test ? "PASS" : "FAIL", label);
  if (!test) ++failures;
}
Instruction* add(Module& m, BasicBlock* b, Instruction* i) {
  m.ownInst(i); b->addInst(i); return i;
}
struct Fixture {
  Module m;
  Function *a, *b, *c;
  Instruction *arithA, *arithB;
  Constant *one, *two;
  Fixture() {
    const Type* t = typePool().i32();
    one = m.getIntConst(t, 1);
    two = m.getIntConst(t, 2);
    a = m.createFunction("A", t);
    b = m.createFunction("B", t);
    c = m.createFunction("C", t);
    BasicBlock* ba = m.createBlock(a);
    BasicBlock* bb = m.createBlock(b);
    BasicBlock* bc = m.createBlock(c);
    arithA = add(m, ba, createBin(Opcode::Add, one, one, {}));
    add(m, ba, createRet(arithA, {}));
    arithB = add(m, bb, createBin(Opcode::Add, one, one, {}));
    add(m, bb, createRet(arithB, {}));
    add(m, bc, createRet(two, {}));
    m.rebuildCFG();
  }
};
int dfRuns = 0, cfgRuns = 0, moduleRuns = 0, noneRuns = 0;
struct DataFlowAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::DataFlow; }
  static std::optional<int> run(const Function&, AnalysisManager&) { return ++dfRuns; }
};
struct CFGAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::CFG; }
  static std::optional<int> run(const Function&, AnalysisManager&) { return ++cfgRuns; }
};
struct ModuleDF {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Module;
  static constexpr MutationKind revisionDependencies() { return MutationKind::DataFlow; }
  static std::optional<int> run(const Module&, AnalysisManager&) { return ++moduleRuns; }
};
struct ModuleMemory {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Module;
  static constexpr MutationKind revisionDependencies() { return MutationKind::Memory; }
  static std::optional<int> run(const Module&, AnalysisManager&) { return ++moduleRuns; }
};
struct NoneAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function&, AnalysisManager&) { return ++noneRuns; }
};
int baseRuns = 0, dependentRuns = 0, transitiveRuns = 0;
struct BaseAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::DataFlow; }
  static std::optional<int> run(const Function&, AnalysisManager&) { return ++baseRuns; }
};
struct DependentAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function& f, AnalysisManager& manager) {
    manager.get<BaseAnalysis>(f);
    return ++dependentRuns;
  }
};
struct TransitiveAnalysis {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function& f, AnalysisManager& manager) {
    manager.get<DependentAnalysis>(f);
    return ++transitiveRuns;
  }
};
struct CycleB;
struct CycleA {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function&, AnalysisManager&);
};
struct CycleB {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function& f, AnalysisManager& manager) {
    manager.get<CycleA>(f);
    return 2;
  }
};
std::optional<int> CycleA::run(const Function& f, AnalysisManager& manager) {
  manager.get<CycleB>(f);
  return 1;
}
struct SelfCycle {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function& f, AnalysisManager& manager) {
    manager.get<SelfCycle>(f);
    return 1;
  }
};
struct FailedBase {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function&, AnalysisManager&) { return std::nullopt; }
};
struct IgnoresFailure {
  using Result = int;
  static constexpr AnalysisScope scope = AnalysisScope::Function;
  static constexpr MutationKind revisionDependencies() { return MutationKind::None; }
  static std::optional<int> run(const Function& f, AnalysisManager& manager) {
    manager.get<FailedBase>(f);
    return 42;
  }
};
void testSnapshots() {
  const MutationKind kinds[] = {MutationKind::DataFlow, MutationKind::CFG,
      MutationKind::Memory, MutationKind::Calls, MutationKind::Globals,
      MutationKind::Interface};
  bool all = true;
  for (MutationKind k : kinds) {
    IRRevision r;
    auto snapshot = captureRevision(r, k);
    r.bump(MutationKind::None);
    all &= matchesRevision(snapshot, r);
    for (MutationKind other : kinds) {
      IRRevision next;
      next.bump(other);
      all &= matchesRevision(snapshot, next) == (k != other);
    }
    r.bump(k);
    all &= !matchesRevision(snapshot, r);
    all &= matchesRevision(captureRevision(r, k), r);
  }
  IRRevision r;
  auto none = captureRevision(r, MutationKind::None);
  for (MutationKind k : kinds) r.bump(k);
  check(all, "all six revision masks detect their category only");
  check(matchesRevision(none, r), "None snapshot ignores all IR changes");
}
void testCacheIsolation() {
  Fixture f;
  AnalysisManager manager(f.m);
  IRMutator mut(f.m);
  dfRuns = cfgRuns = moduleRuns = noneRuns = 0;
  auto a = manager.get<DataFlowAnalysis>(*f.a);
  auto b = manager.get<DataFlowAnalysis>(*f.b);
  auto mod = manager.get<ModuleDF>();
  auto memory = manager.get<ModuleMemory>();
  auto cfg = manager.get<CFGAnalysis>(*f.a);
  auto none = manager.get<NoneAnalysis>(*f.a);
  check(a.get() && b.get() && mod.get() && memory.get() && cfg.get() &&
        none.get() && dfRuns == 2 && moduleRuns == 2, "Module/Function miss");
  check(manager.get<DataFlowAnalysis>(*f.a).generation() == a.generation() &&
        manager.get<ModuleDF>().generation() == mod.generation() &&
        dfRuns == 2 && moduleRuns == 2, "Module/Function hit");
  auto rev = f.m.revision().any;
  check(mut.rewriteOperand(f.arithA, 0, f.one) == MutationResult::NoChange &&
        mut.rewriteOperand(f.arithA, 0,
            f.m.getIntConst(typePool().i64(), 9)) == MutationResult::Rejected &&
        f.m.revision().any == rev && a.isCurrent(), "NoChange/Rejected no invalidation");
  check(mut.rewriteOperand(f.arithA, 0, f.two) == MutationResult::Applied,
        "live DataFlow mutation");
  check(!a.isCurrent() && a.get() == nullptr && b.isCurrent() &&
        !mod.isCurrent() && memory.isCurrent() && cfg.isCurrent() &&
        none.isCurrent(), "A stale, B and unrelated categories/None current; Module DF stale");
  auto a2 = manager.get<DataFlowAnalysis>(*f.a);
  check(a2.isCurrent() && a2.generation() != a.generation() &&
        a.get() == nullptr && dfRuns == 3, "lazy recompute changes generation; old stale");
  check(manager.get<ModuleDF>().get() && moduleRuns == 3 &&
        manager.get<ModuleMemory>().generation() == memory.generation(),
        "Module relevant recompute, unrelated category hit");
  check(manager.get<NoneAnalysis>(*f.a).generation() == none.generation() &&
        noneRuns == 1, "None cache remains reusable");
}
void testMemory() {
  Module m;
  const Type* t = typePool().i32();
  Function* f = m.createFunction("mem", t);
  BasicBlock* b = m.createBlock(f);
  auto* one = m.getIntConst(t, 1);
  auto* two = m.getIntConst(t, 2);
  auto* slot = add(m, b, createAlloca(t, {}));
  auto* store = add(m, b, createStore(t, one, slot, {}));
  auto* sum = add(m, b, createBin(Opcode::Add, one, one, {}));
  add(m, b, createRet(sum, {}));
  m.rebuildCFG();
  AnalysisManager manager(m);
  IRMutator mut(m);
  auto memory = manager.get<ModuleMemory>();
  check(mut.rewriteOperand(sum, 0, two) == MutationResult::Applied &&
        memory.isCurrent(), "arithmetic rewrite preserves Module Memory analysis");
  check(mut.rewriteOperand(store, 0, two) == MutationResult::Applied &&
        !memory.isCurrent() && memory.get() == nullptr &&
        manager.get<ModuleMemory>().isCurrent(), "Memory edit invalidates Module Memory");
}
void testCross() {
  Fixture f;
  AnalysisManager manager(f.m);
  IRMutator mut(f.m);
  auto a = manager.get<DataFlowAnalysis>(*f.a);
  auto b = manager.get<DataFlowAnalysis>(*f.b);
  auto c = manager.get<DataFlowAnalysis>(*f.c);
  auto mod = manager.get<ModuleDF>();
  check(mut.replaceUses(f.one, f.two) == MutationResult::Applied,
        "cross-function canonical Constant RAUW applied");
  check(!a.isCurrent() && !b.isCurrent() && c.isCurrent() &&
        !mod.isCurrent() && c.get() != nullptr,
        "cross-function RAUW: A/B/Module stale; C current");
}
void testDependencies() {
  Fixture f;
  AnalysisManager manager(f.m);
  IRMutator mut(f.m);
  baseRuns = dependentRuns = transitiveRuns = 0;
  auto a = manager.get<TransitiveAnalysis>(*f.a);
  auto b = manager.get<DependentAnalysis>(*f.a);
  auto base = manager.get<BaseAnalysis>(*f.a);
  check(a.get() && b.get() && base.get() &&
        baseRuns == 1 && dependentRuns == 1 && transitiveRuns == 1,
        "automatic A->B->Base dependency captured");
  check(manager.get<TransitiveAnalysis>(*f.a).generation() == a.generation() &&
        baseRuns == 1 && dependentRuns == 1 && transitiveRuns == 1,
        "dependency chain hit skips all computations");
  check(mut.rewriteOperand(f.arithA, 1, f.two) == MutationResult::Applied &&
        !base.isCurrent() && !b.isCurrent() && !a.isCurrent(),
        "transitive None-dependent results stale after Base change");
  auto a2 = manager.get<TransitiveAnalysis>(*f.a);
  check(a2.isCurrent() && a2.generation() != a.generation() &&
        baseRuns == 2 && dependentRuns == 2 && transitiveRuns == 2 &&
        !a.isCurrent() && a.get() == nullptr,
        "lazy transitive recompute regenerates each dependent");
}
void testFailures() {
  Fixture f;
  AnalysisManager manager(f.m);
  const IRRevision before = f.m.revision();
  auto cycle = manager.get<CycleA>(*f.a);
  auto again = manager.get<CycleB>(*f.a);
  auto self = manager.get<SelfCycle>(*f.a);
  auto ignored = manager.get<IgnoresFailure>(*f.a);
  check(!cycle.get() && !again.get() && !self.get(),
        "mutual and self cycles fail closed, without cached partials");
  check(!ignored.isCurrent() && !ignored.get() &&
        !manager.get<IgnoresFailure>(*f.a).get(),
        "ignored failed dependency cannot cache parent");
  check(before.any == f.m.revision().any &&
        manager.get<DataFlowAnalysis>(*f.a).isCurrent(),
        "failures leave IR unchanged and unrelated manager usable");
  Module foreign;
  Function* f2 = foreign.createFunction("foreign", typePool().i32());
  foreign.createBlock(f2);
  const int runs = dfRuns;
  auto bad = manager.get<DataFlowAnalysis>(*f2);
  check(bad.get() == nullptr && dfRuns == runs &&
        before.any == f.m.revision().any,
        "foreign Function rejected without invoking analysis or mutating IR");
}
void testLifetime() {
  Fixture f;
  AnalysisHandle<int> handle;
  {
    auto manager = std::make_unique<AnalysisManager>(f.m);
    handle = manager->get<DataFlowAnalysis>(*f.a);
    check(handle.isCurrent(), "handle current while manager lives");
  }
  check(!handle.isCurrent() && handle.get() == nullptr,
        "manager destruction makes old handle safely stale");
}
void testReachability() {
  Module m;
  const Type* t = typePool().i32();
  auto* one = m.getIntConst(t, 1);
  auto* two = m.getIntConst(t, 2);
  Function* f = m.createFunction("branches", t);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* a = m.createBlock(f);
  BasicBlock* b = m.createBlock(f);
  BasicBlock* exit = m.createBlock(f);
  BasicBlock* dead = m.createBlock(f);
  add(m, entry, createCondBr(one, a, b, {}));
  Instruction* sum = add(m, a, createBin(Opcode::Add, one, one, {}));
  add(m, a, createBr(exit, {}));
  add(m, b, createBr(exit, {}));
  add(m, exit, createRet(one, {}));
  add(m, dead, createRet(two, {}));
  m.rebuildCFG();
  check(verifyFlatModule(m).empty(), "hand-built diamond + dead block valid");
  AnalysisManager manager(m);
  IRMutator mut(m);
  CFGEditor cfg(mut);
  auto h = manager.get<ReachabilityAnalysis>(*f);
  const auto* bits = h.get();
  check(bits && bits->reachable(entry->index()) &&
        bits->reachable(a->index()) && bits->reachable(b->index()) &&
        bits->reachable(exit->index()) && !bits->reachable(dead->index()),
        "Reachability follows terminators and excludes unreachable block");
  const uint64_t generation = h.generation();
  check(mut.rewriteOperand(sum, 0, two) == MutationResult::Applied &&
        h.isCurrent() &&
        manager.get<ReachabilityAnalysis>(*f).generation() == generation,
        "DataFlow edit preserves CFG-only reachability generation");
  check(cfg.redirectEdge(entry, 1, a) == MutationResult::Applied &&
        !h.isCurrent() && h.get() == nullptr,
        "CFGEditor redirect invalidates old reachability handle");
  auto newReach = manager.get<ReachabilityAnalysis>(*f);
  check(newReach.isCurrent() && newReach.generation() != generation &&
        newReach.get() && !newReach.get()->reachable(b->index()) &&
        newReach.get()->reachable(a->index()),
        "new reachability recomputes updated graph");
  check(verifyFlatModule(m).empty(), "CFG mutation keeps FlatVerifier PASS");
}
} // namespace

int main() {
  testSnapshots();
  testCacheIsolation();
  testMemory();
  testCross();
  testDependencies();
  testFailures();
  testLifetime();
  testReachability();
  std::printf("ANALYSIS_MANAGER_FAILURES=%d\n", failures);
  return failures ? 1 : 0;
}
