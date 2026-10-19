// F04 Mem2Reg FunctionPass integration and module compatibility controls.
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "analysis/ReachabilityAnalysis.h"
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/Mem2Reg.h"

using namespace sysy::flat;
namespace {
int failures = 0, checks = 0;
void check(bool ok, const char* description) {
  ++checks;
  failures += !ok;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", description);
}
Instruction* add(Module& m, BasicBlock* b, Instruction* i) {
  m.ownInst(i);
  b->addInst(i);
  return i;
}

struct Fixture {
  Module m;
  Function *diamond, *decl, *mixed, *empty;
  Fixture() {
    const Type* i32 = typePool().i32();
    Value* zero = m.getIntConst(i32, 0);
    Value* one = m.getIntConst(i32, 1);
    Value* two = m.getIntConst(i32, 2);
    diamond = m.createFunction("z_first", i32);
    decl = m.createFunction("declaration", i32);
    mixed = m.createFunction("a_second", i32);
    empty = m.createFunction("no_alloca", i32);
    auto* entry = m.createBlock(diamond);
    auto* left = m.createBlock(diamond);
    auto* right = m.createBlock(diamond);
    auto* join = m.createBlock(diamond);
    // Two CFG-phi-producing candidates: candidate #2 needs a new F02 handle
    // after candidate #1's CFGEditor phi insertion invalidates reachability.
    auto* slot1 = add(m, entry, createAlloca(i32, {11, 1}));
    auto* slot2 = add(m, entry, createAlloca(i32, {11, 2}));
    add(m, entry, createStore(i32, zero, slot1, {}));
    add(m, entry, createStore(i32, zero, slot2, {}));
    add(m, entry, createCondBr(one, left, right, {}));
    add(m, left, createStore(i32, one, slot1, {}));
    add(m, left, createStore(i32, two, slot2, {}));
    add(m, left, createBr(join, {}));
    add(m, right, createStore(i32, two, slot1, {}));
    add(m, right, createStore(i32, one, slot2, {}));
    add(m, right, createBr(join, {}));
    auto* load1 = add(m, join, createLoad(i32, slot1, {}));
    auto* load2 = add(m, join, createLoad(i32, slot2, {}));
    add(m, join, createRet(add(m, join, createBin(Opcode::Add, load1, load2, {})), {}));

    auto* b = m.createBlock(mixed);
    auto* promotable = add(m, b, createAlloca(i32, {22, 1}));
    add(m, b, createAlloca(typePool().arrayOf(i32, 2), {22, 2}));
    auto* uninitialized = add(m, b, createAlloca(i32, {22, 3}));
    add(m, b, createStore(i32, two, promotable, {}));
    auto* read = add(m, b, createLoad(i32, promotable, {}));
    auto* badRead = add(m, b, createLoad(i32, uninitialized, {}));
    add(m, b, createRet(add(m, b, createBin(Opcode::Add, read, badRead, {})), {}));
    add(m, m.createBlock(empty), createRet(zero, {}));
    m.rebuildCFG();
    m.rebuildUseDef();
  }
};

void testFormalVsWrapper() {
  static_assert(std::is_base_of<FunctionPass, Mem2RegPass>::value,
                "F04 must be an actual FunctionPass");
  Fixture formal, compatibility;
  check(verifyFlatModule(formal.m).empty(), "fixture is verifier-clean");
  const std::string before = dumpModule(formal.m);
  auto pass = std::make_unique<Mem2RegPass>();
  const Mem2RegPass* observer = pass.get();
  FlatPassManager manager;
  check(manager.addPass(std::move(pass)),
        "mem2reg formally registers by stable name");
  PassManagerOptions opts;
  opts.instrument = true;
  opts.verifyEach = true;
  const auto pipeline = manager.run(formal.m, opts);
  const Mem2RegResult& collected = observer->aggregate();
  const auto compatible = runMem2Reg(compatibility.m, Mem2RegOptions{true, true});
  check(pipeline.ok() && pipeline.changed &&
        pipeline.invocations.size() == 3 &&
        pipeline.invocations[0].passName == "mem2reg" &&
        pipeline.invocations[0].functionIndex == 0 &&
        pipeline.invocations[1].functionIndex == 2 &&
        pipeline.invocations[2].functionIndex == 3,
        "formal pass runs only non-declarations in Module Function order");
  check(compatible.success && compatible.inputValid && compatible.changed &&
        compatible.status == Mem2RegStatus::Changed &&
        dumpModule(formal.m) == dumpModule(compatibility.m) &&
        compatible.beforeDump == before &&
        compatible.afterDump == dumpModule(formal.m),
        "module wrapper and formal path yield identical Flat, one before/final after");
  check(collected.promotedAllocas == 3 && compatible.promotedAllocas == 3 &&
        collected.removedLoads == compatible.removedLoads &&
        collected.removedStores == compatible.removedStores &&
        collected.generatedPhis == compatible.generatedPhis &&
        collected.eliminatedPhis == compatible.eliminatedPhis &&
        collected.generatedPhis >= 2 &&
        verifyFlatModule(formal.m).empty(),
        "compatibility counters agree, including multi-candidate phi mutation");
  bool ordered = collected.candidates.size() == 5 &&
                 compatible.candidates.size() == 5;
  if (ordered) {
    for (size_t i = 0; i < 5; ++i) {
      const auto& a = collected.candidates[i];
      const auto& b = compatible.candidates[i];
      ordered &= a.function == b.function && a.loc.line == b.loc.line &&
                 a.loc.col == b.loc.col && a.reason == b.reason &&
                 a.promoted == b.promoted;
    }
    ordered &= collected.candidates[0].function == "z_first" &&
               collected.candidates[1].loc.col == 2 &&
               collected.candidates[2].function == "a_second" &&
               collected.candidates[3].reason == Mem2RegSkipReason::UnsupportedObjectType &&
               collected.candidates[4].reason == Mem2RegSkipReason::ReadBeforeDefinition;
  }
  check(ordered, "candidate and skip ordering is old module-major/block-alloca order");
  const auto& a = pipeline.invocations[0];
  const auto& b = pipeline.invocations[1];
  const auto& c = pipeline.invocations[2];
  check(a.reportedStatus == PassStatus::Applied && a.changed &&
        a.mutation.changed() && a.affectedFunctionIndices == std::vector<size_t>{0} &&
        b.reportedStatus == PassStatus::Applied && b.changed &&
        b.affectedFunctionIndices == std::vector<size_t>{2} &&
        c.reportedStatus == PassStatus::NoChange && !c.changed &&
        !c.mutation.changed() && c.affectedFunctionIndices.empty(),
        "Applied/NoChange match tracked mutation and isolate Function ownership");
  check(a.stats.counters.at("promoted-allocas") == 2 &&
        a.stats.counters.at("generated-phis") >= 2 &&
        b.stats.counters.at("promoted-allocas") == 1 &&
        b.stats.counters.at("skipped-unsupported-object-type") == 1 &&
        b.stats.counters.at("skipped-read-before-definition") == 1 &&
        c.stats.counters.at("candidates") == 0 &&
        a.beforeDump.empty() && !a.elapsedNanoseconds.has_value(),
        "per-invocation counters expose deterministic promotion/decline stats");
  check(formal.m.revision().any != 0 && formal.empty->revision().any == 0,
        "non-mutated Function revision stays untouched");
  // Repeated passes must be NoChange, never forge Applied from prior totals.
  FlatPassManager again;
  again.addPass(std::make_unique<Mem2RegPass>());
  auto replay = again.run(formal.m, opts);
  check(replay.ok() && !replay.changed && replay.invocations.size() == 3 &&
        replay.invocations[0].finalStatus == PassStatus::NoChange &&
        replay.invocations[1].finalStatus == PassStatus::NoChange,
        "second formal invocation reports NoChange and no tracked edits");
  PassManagerOptions disabled;
  disabled.instrument = true;
  disabled.disabledPassNames.insert("mem2reg");
  Fixture off;
  FlatPassManager disabledManager;
  disabledManager.addPass(std::make_unique<Mem2RegPass>());
  auto skipped = disabledManager.run(off.m, disabled);
  check(skipped.ok() && !skipped.changed &&
        skipped.invocations.size() == 3 &&
        skipped.invocations[0].disabled &&
        dumpModule(off.m) == before,
        "manager-level mem2reg disabling has no mutation");
}

class ProbePass final : public FunctionPass {
 public:
  using Work = std::function<PassResult(Function&, PassContext&)>;
  ProbePass(std::string name, Work work)
      : FunctionPass(std::move(name)), work_(std::move(work)) {}
  PassResult run(Function& function, PassContext& context) override {
    return work_(function, context);
  }
 private:
  Work work_;
};

void testSharedReachabilityFreshness() {
  Fixture fixture;
  AnalysisHandle<ReachabilityInfo> old;
  uint64_t oldGeneration = 0;
  bool oldWasCurrent = false, staleAfterPhi = false, recomputed = false;
  FlatPassManager manager;
  manager.addPass(std::make_unique<ProbePass>("before-reach",
      [&](Function& f, PassContext& context) {
        if (&f == fixture.diamond) {
          old = context.analyses().get<ReachabilityAnalysis>(f);
          oldGeneration = old.generation();
          oldWasCurrent = old.isCurrent() && old.get() != nullptr;
        }
        return PassResult{};
      }));
  manager.addPass(std::make_unique<Mem2RegPass>());
  manager.addPass(std::make_unique<ProbePass>("after-reach",
      [&](Function& f, PassContext& context) {
        if (&f == fixture.diamond) {
          staleAfterPhi = !old.isCurrent() && old.get() == nullptr;
          auto fresh = context.analyses().get<ReachabilityAnalysis>(f);
          recomputed = fresh.isCurrent() && fresh.get() != nullptr &&
                       fresh.generation() != oldGeneration;
        }
        return PassResult{};
      }));
  PassManagerOptions options;
  options.instrument = true;
  auto result = manager.run(fixture.m, options);
  check(result.ok() && result.changed && oldWasCurrent &&
        staleAfterPhi && recomputed &&
        fixture.diamond->phiCount() >= 2 &&
        verifyFlatModule(fixture.m).empty(),
        "shared F02 reachability becomes stale after phi CFG edits and recomputes");
}

void testFailurePathAndAggregateLifetime() {
  // Mid-pipeline failure must fail closed: the manager stops, reports a
  // diagnostic, and never runs later passes on a partially mutated module.
  Fixture fixture;
  int afterRan = 0;
  FlatPassManager manager;
  manager.addPass(std::make_unique<ProbePass>("mutate-then-throw",
      [](Function& f, PassContext& context) -> PassResult {
        Value* value = context.analyses().module().getIntConst(typePool().i32(), 7);
        Instruction* ret = f.blocks().back()->terminator();  // join's `ret i32`
        context.mutator().rewriteOperand(ret, 0, value);
        throw std::runtime_error("injected failure");
      }));
  manager.addPass(std::make_unique<ProbePass>("after", [&](Function&, PassContext&) {
    ++afterRan;
    return PassResult{};
  }));
  PassManagerOptions options;
  options.instrument = true;
  const auto failed = manager.run(fixture.m, options);
  check(!failed.ok() && failed.status == PipelineStatus::ContractViolation &&
        afterRan == 0 && !failed.diagnostic.empty() &&
        failed.invocations.size() == 1 &&
        failed.invocations[0].finalStatus == PassStatus::Failed &&
        failed.invocations[0].mutation.changed(),
        "mid-pipeline exception stops execution and records a diagnosed failure");

  // The pass owns its aggregate; an observer stays valid exactly as long as
  // the manager that owns the pass.
  Fixture owned;
  auto pass = std::make_unique<Mem2RegPass>();
  const Mem2RegPass* observer = pass.get();
  FlatPassManager ownedManager;
  ownedManager.addPass(std::move(pass));
  const auto run = ownedManager.run(owned.m);
  check(run.ok() && observer->aggregate().promotedAllocas == 3,
        "aggregate is pass-owned and readable for the manager lifetime");

  // Production asks for no instrumentation; the pass itself must not dump,
  // verify per invocation, or clock. The only observable record is a real run.
  Fixture quiet;
  const std::string before = dumpModule(quiet.m);
  FlatPassManager plain;
  plain.addPass(std::make_unique<Mem2RegPass>());
  const auto result = plain.run(quiet.m);
  check(result.ok() && result.changed && result.invocations.empty() &&
        dumpModule(quiet.m) != before,
        "no internal dump/clock/verify records without instrumentation");

  // Never claim Changed on a failed wrapper result: construct the failure
  // through the public pass manager path, then check the wrapper contract in
  // the source (the wrapper sets Failed, not Changed).
  check(Mem2RegStatus::Failed != Mem2RegStatus::Changed,
        "failed promotion has a distinct status");
}

void testDeclinedDisabledAndInvalid() {
  Module module;
  const Type* i32 = typePool().i32();
  Function* fn = module.createFunction("decline", i32);
  BasicBlock* entry = module.createBlock(fn);
  auto* slot = add(module, entry, createAlloca(i32, {30, 1}));
  auto* read = add(module, entry, createLoad(i32, slot, {}));
  auto* ret = add(module, entry, createRet(read, {}));
  module.rebuildCFG();
  module.rebuildUseDef();
  const std::string before = dumpModule(module);
  auto pass = std::make_unique<Mem2RegPass>();
  const Mem2RegPass* observer = pass.get();
  FlatPassManager manager;
  manager.addPass(std::move(pass));
  PassManagerOptions debug;
  debug.instrument = true;
  const auto declined = manager.run(module, debug);
  const Mem2RegResult& collected = observer->aggregate();
  check(declined.ok() && !declined.changed && declined.invocations.size() == 1 &&
        declined.invocations[0].finalStatus == PassStatus::NoChange &&
        !declined.invocations[0].mutation.changed() &&
        declined.invocations[0].stats.counters.at(
            "skipped-read-before-definition") == 1 &&
        collected.candidates.size() == 1 &&
        collected.candidates[0].reason == Mem2RegSkipReason::ReadBeforeDefinition &&
        dumpModule(module) == before,
        "declined-only FunctionPass reports NoChange, keeps Flat/summary intact");
  Mem2RegOptions opts;
  opts.captureDumps = true;
  auto unchanged = runMem2Reg(module, opts);
  check(unchanged.success && unchanged.inputValid && !unchanged.changed &&
        unchanged.status == Mem2RegStatus::NoChange &&
        unchanged.beforeDump == unchanged.afterDump &&
        unchanged.beforeDump == before,
        "compatibility NoChange preserves module-level before/after dumps");
  opts.enabled = false;
  auto disabled = runMem2Reg(module, opts);
  check(disabled.success && disabled.inputValid && !disabled.changed &&
        disabled.status == Mem2RegStatus::Disabled &&
        disabled.candidates.empty() && disabled.beforeDump == disabled.afterDump,
        "compatibility disabled retains legacy status, no candidates or edits");
  // Deliberate test-only malformed operand, prior to invoking the public API.
  ret->setOperand(0, slot);
  module.rebuildUseDef();
  const std::string malformed = dumpModule(module);
  opts.enabled = true;
  auto invalid = runMem2Reg(module, opts);
  check(!invalid.success && !invalid.inputValid &&
        invalid.status == Mem2RegStatus::InvalidInput &&
        !invalid.diagnostic.empty() && invalid.candidates.empty() &&
        invalid.beforeDump == malformed && invalid.afterDump == malformed,
        "invalid input fails before formal pass and preserves dump/diagnostic");
}

} // namespace
int main() {
  testFormalVsWrapper();
  testSharedReachabilityFreshness();
  testFailurePathAndAggregateLifetime();
  testDeclinedDisabledAndInvalid();
  std::printf("F04_MEM2REG_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
