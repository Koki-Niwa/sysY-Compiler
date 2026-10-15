// F03 isolated function pipeline and fail-closed negative controls.
// Test-only raw corruption is deliberate; no production optimizer may do this.
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "analysis/ReachabilityAnalysis.h"
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"

using namespace sysy::flat;
namespace {
int failures = 0;
int checks = 0;
int negatives = 0;
void check(bool ok, const char* message) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
}
Instruction* add(Module& m, BasicBlock* b, Instruction* i) {
  m.ownInst(i);
  b->addInst(i);
  return i;
}
struct Fixture {
  Module m;
  Function *a, *decl, *b;
  Instruction *sumA, *sumB;
  Constant *one, *two;
  Fixture() {
    const Type* t = typePool().i32();
    one = m.getIntConst(t, 1);
    two = m.getIntConst(t, 2);
    a = m.createFunction("a", t);
    decl = m.createFunction("declaration", t);
    b = m.createFunction("b", t);
    BasicBlock* ba = m.createBlock(a);
    BasicBlock* bb = m.createBlock(b);
    sumA = add(m, ba, createBin(Opcode::Add, one, one, {}));
    add(m, ba, createRet(sumA, {}));
    sumB = add(m, bb, createBin(Opcode::Add, one, one, {}));
    add(m, bb, createRet(sumB, {}));
    m.rebuildCFG();
  }
};
using Work = std::function<PassResult(Function&, PassContext&)>;
class TestPass final : public FunctionPass {
 public:
  TestPass(std::string name, Work work)
      : FunctionPass(std::move(name)), work_(std::move(work)) {}
  PassResult run(Function& f, PassContext& ctx) override { return work_(f, ctx); }
 private:
  Work work_;
};
std::unique_ptr<FunctionPass> makePass(std::string name, Work work) {
  return std::make_unique<TestPass>(std::move(name), std::move(work));
}
PassResult outcome(PassStatus s, PreservedAnalyses p = PreservedAnalyses::none()) {
  PassResult r;
  r.status = s;
  r.preserved = std::move(p);
  return r;
}
void testOrderedPipeline() {
  Fixture x;
  check(verifyFlatModule(x.m).empty(), "fixture starts verified");
  FlatPassManager pm;
  std::vector<std::string> order;
  AnalysisManager* shared = nullptr;
  bool sameAM = true, freshSummary = true, sameModule = true;
  auto rewrite = [&](Function& f, PassContext& ctx) {
    order.push_back("first:" + f.name());
    if (!shared) shared = &ctx.analyses();
    sameAM &= shared == &ctx.analyses();
    sameModule &= &ctx.analyses().module() == &x.m;
    freshSummary &= !ctx.mutator().summary().changed();
    Instruction* sum = &f == x.a ? x.sumA : x.sumB;
    const auto edit = ctx.mutator().rewriteOperand(sum, 0, x.two);
    PassResult r = outcome(edit == MutationResult::Applied ?
                           PassStatus::Applied : PassStatus::Failed);
    r.stats.counters["z_last"] = 4;
    r.stats.counters["a_first"] = 1;
    return r;
  };
  check(pm.addPass(makePass("rewrite", rewrite)) &&
        !pm.addPass(makePass("rewrite", rewrite)) &&
        !pm.addPass({}), "pass name unique and null registration rejected");
  check(pm.addPass(makePass("observe", [&](Function& f, PassContext& ctx) {
    order.push_back("second:" + f.name());
    sameAM &= shared == &ctx.analyses();
    freshSummary &= !ctx.mutator().summary().changed();
    return outcome(PassStatus::NoChange);
  })), "second pass registered");
  PassManagerOptions opts;
  opts.instrument = true;
  opts.verifyEach = true;
  auto result = pm.run(x.m, opts);
  check(result.ok() && result.invocations.size() == 4 &&
        order == std::vector<std::string>({"first:a", "first:b", "second:a", "second:b"}) &&
        sameAM && freshSummary && sameModule,
        "pass-major/module-function order, shared AM, fresh editor and declaration skip");
  const auto& records = result.invocations;
  check(records[0].functionIndex == 0 && records[1].functionIndex == 2 &&
        records[2].functionIndex == 0 && records[3].functionIndex == 2 &&
        records[0].passName == "rewrite" && records[3].passName == "observe",
        "stable function indices and pass names");
  check(records[0].reportedStatus == PassStatus::Applied &&
        records[0].finalStatus == PassStatus::Applied &&
        records[0].changed && records[0].mutation.changed() &&
        records[0].mutation.operations == 1 &&
        hasMutation(records[0].mutation.kinds, MutationKind::DataFlow) &&
        records[0].affectedFunctionIndices == std::vector<size_t>{0} &&
        records[1].affectedFunctionIndices == std::vector<size_t>{2},
        "derived changed, actual MutationSummary and stable affected-function identity");
  check(records[2].reportedStatus == PassStatus::NoChange &&
        !records[2].changed && records[2].mutation.operations == 0 &&
        records[2].mutation.affectedFunctions.empty() &&
        records[2].affectedFunctionIndices.empty(),
        "real NoChange keeps empty fresh MutationSummary");
  check(records[0].stats.counters.begin()->first == "a_first" &&
        records[0].stats.counters.at("z_last") == 4 &&
        records[0].verification == VerificationOutcome::Passed &&
        records[3].verification == VerificationOutcome::Passed &&
        verifyFlatModule(x.m).empty(),
        "ordered stats and verify-each succeeds after every executed pass");
  check(records[0].beforeDump.empty() && records[0].afterDump.empty() &&
        !records[0].elapsedNanoseconds.has_value(),
        "instrumentation does not force dumps or clocks");
}
void testDisabledAndSemantics() {
  Fixture x;
  int disabledCalls = 0, declinedCalls = 0, noChangeCalls = 0;
  FlatPassManager pm;
  pm.addPass(makePass("disabled", [&](Function&, PassContext&) {
    ++disabledCalls;
    return outcome(PassStatus::Applied);
  }));
  pm.addPass(makePass("decline", [&](Function&, PassContext&) {
    ++declinedCalls;
    return outcome(PassStatus::Declined);
  }));
  pm.addPass(makePass("no-change", [&](Function& f, PassContext& c) {
    ++noChangeCalls;
    Instruction* sum = &f == x.a ? x.sumA : x.sumB;
    check(c.mutator().rewriteOperand(sum, 0, x.one) == MutationResult::NoChange,
          "tracked no-op edit reports NoChange");
    return outcome(PassStatus::NoChange);
  }));
  PassManagerOptions opts;
  opts.instrument = true;
  opts.verifyEach = true;
  opts.disabledPassNames.insert("disabled");
  auto result = pm.run(x.m, opts);
  check(result.ok() && disabledCalls == 0 &&
        declinedCalls == 2 && noChangeCalls == 2 &&
        result.invocations.size() == 6, "disabled pass never called; declined and no-change execute");
  check(result.invocations[0].disabled && result.invocations[1].disabled &&
        !result.invocations[0].reportedStatus &&
        !result.invocations[0].finalStatus &&
        result.invocations[0].verification == VerificationOutcome::NotRun &&
        !result.invocations[2].disabled &&
        result.invocations[2].reportedStatus == PassStatus::Declined &&
        result.invocations[2].verification == VerificationOutcome::Passed,
        "disabled/skipped is separate from real Declined/NoChange");
  check(result.invocations[4].reportedStatus == PassStatus::NoChange &&
        !result.invocations[4].changed &&
        result.invocations[4].mutation.operations == 0,
        "genuine no-change recorded without mutation");
  FlatPassManager empty;
  check(empty.run(x.m).invocations.empty(),
        "instrumentation default off, no invocation record allocations");
  bool threw = false;
  try { TestPass invalid("", [](Function&, PassContext&) {
      return outcome(PassStatus::NoChange);
    }); } catch (const std::invalid_argument&) { threw = true; }
  check(threw, "empty pass name is rejected at construction");
}
void testFailuresAndContractNegatives() {
  struct Case {
    const char* name;
    PassStatus claimed;
    bool mutate;
    PipelineStatus expected;
  };
  const Case cases[] = {
    {"failed-atomic", PassStatus::Failed, false, PipelineStatus::PassFailed},
    {"nochange-mutation", PassStatus::NoChange, true, PipelineStatus::ContractViolation},
    {"declined-mutation", PassStatus::Declined, true, PipelineStatus::ContractViolation},
    {"failed-mutation", PassStatus::Failed, true, PipelineStatus::ContractViolation},
    {"applied-no-mutation", PassStatus::Applied, false, PipelineStatus::ContractViolation}
  };
  for (const Case& item : cases) {
    Fixture x;
    int following = 0;
    FlatPassManager pm;
    pm.addPass(makePass(item.name, [&](Function& f, PassContext& ctx) {
      if (item.mutate) {
        check(ctx.mutator().rewriteOperand(x.sumA, 0, x.two) ==
              MutationResult::Applied && &f == x.a, "negative setup applied tracked edit");
      }
      return outcome(item.claimed);
    }));
    pm.addPass(makePass("must-not-run", [&](Function&, PassContext&) {
      ++following;
      return outcome(PassStatus::NoChange);
    }));
    PassManagerOptions opts;
    opts.instrument = true;
    auto r = pm.run(x.m, opts);
    ++negatives;
    check(r.status == item.expected && following == 0 &&
          r.invocations.size() == 1 &&
          r.invocations[0].reportedStatus == item.claimed &&
          r.invocations[0].changed == item.mutate &&
          r.invocations[0].mutation.changed() == item.mutate &&
          r.invocations[0].mutation.operations == (item.mutate ? 1u : 0u) &&
          r.invocations[0].finalStatus == PassStatus::Failed &&
          !r.diagnostic.empty(),
          item.name);
    if (item.expected == PipelineStatus::ContractViolation) {
      check(r.invocations[0].contractViolation &&
            r.diagnostic.find("contract violation") != std::string::npos,
            "status/mutation mismatch detected and fail-closed");
    }
  }
  Fixture x;
  FlatPassManager pm;
  pm.addPass(makePass("throws", [](Function&, PassContext&) -> PassResult {
    throw std::runtime_error("intentional error");
  }));
  auto r = pm.run(x.m);
  ++negatives;
  check(r.status == PipelineStatus::PassFailed &&
        r.diagnostic.find("intentional error") != std::string::npos &&
        r.invocations.size() == 1 && !r.invocations[0].changed,
        "exception without edits is a diagnosed fail-closed pass failure");
}
struct Diamond {
  Module m;
  Function* f;
  BasicBlock *entry, *left, *right, *exit, *dead;
  Instruction* sum;
  Constant *one, *two;
  Diamond() {
    const Type* t = typePool().i32();
    one = m.getIntConst(t, 1);
    two = m.getIntConst(t, 2);
    f = m.createFunction("diamond", t);
    entry = m.createBlock(f);
    left = m.createBlock(f);
    right = m.createBlock(f);
    exit = m.createBlock(f);
    dead = m.createBlock(f);
    add(m, entry, createCondBr(one, left, right, {}));
    sum = add(m, left, createBin(Opcode::Add, one, one, {}));
    add(m, left, createBr(exit, {}));
    add(m, right, createBr(exit, {}));
    add(m, exit, createRet(one, {}));
    add(m, dead, createRet(two, {}));
    m.rebuildCFG();
  }
};
void testPreservedClaimsAndRealRevisions() {
  for (bool all : {false, true}) {
    Diamond x;
    check(verifyFlatModule(x.m).empty(), "diamond fixture verified");
    bool oldStale = false, nextCurrent = false;
    FlatPassManager pm;
    pm.addPass(makePass(all ? "preserve-all" : "preserve-reachability",
      [&](Function& f, PassContext& ctx) {
        auto old = ctx.analyses().get<ReachabilityAnalysis>(f);
        uint64_t oldGen = old.generation();
        check(old.isCurrent() && old.get()->reachable(x.right->index()),
              "old reachability reflects pre-edit CFG");
        auto edit = ctx.cfg().redirectEdge(x.entry, 1, x.left);
        oldStale = !old.isCurrent() && old.get() == nullptr;
        auto next = ctx.analyses().get<ReachabilityAnalysis>(f);
        nextCurrent = next.isCurrent() && next.generation() != oldGen &&
                      next.get() && !next.get()->reachable(x.right->index());
        PreservedAnalyses preserved = all ? PreservedAnalyses::all()
                                         : PreservedAnalyses::none();
        if (!all) preserved.preserve<ReachabilityAnalysis>();
        return outcome(edit == MutationResult::Applied ? PassStatus::Applied :
                       PassStatus::Failed, preserved);
      }));
    PassManagerOptions opts;
    opts.instrument = true;
    opts.verifyEach = true;
    auto result = pm.run(x.m, opts);
    check(result.ok() && oldStale && nextCurrent &&
          result.invocations.size() == 1 &&
          result.invocations[0].preserved.preserves<ReachabilityAnalysis>() &&
          result.invocations[0].changed &&
          hasMutation(result.invocations[0].mutation.kinds, MutationKind::CFG) &&
          result.invocations[0].verification == VerificationOutcome::Passed,
          all ? "preserve all cannot revive stale CFG analysis; lazy recompute" :
                "typed preserve cannot override CFG revision; lazy recompute");
  }
  Diamond x;
  bool kept = false;
  FlatPassManager pm;
  pm.addPass(makePass("dataflow", [&](Function& f, PassContext& ctx) {
    auto old = ctx.analyses().get<ReachabilityAnalysis>(f);
    uint64_t generation = old.generation();
    auto edit = ctx.mutator().rewriteOperand(x.sum, 0, x.two);
    auto same = ctx.analyses().get<ReachabilityAnalysis>(f);
    kept = old.isCurrent() && same.isCurrent() &&
           same.generation() == generation && old.get() == same.get();
    return outcome(edit == MutationResult::Applied ? PassStatus::Applied :
                   PassStatus::Failed, PreservedAnalyses::none());
  }));
  PassManagerOptions opts;
  opts.instrument = true;
  opts.verifyEach = true;
  auto r = pm.run(x.m, opts);
  check(r.ok() && kept && r.invocations[0].changed &&
        !hasMutation(r.invocations[0].mutation.kinds, MutationKind::CFG) &&
        hasMutation(r.invocations[0].mutation.kinds, MutationKind::DataFlow),
        "unrelated DataFlow edit keeps CFG-only Reachability handle current");
}
void testVerifierAndInstrumentation() {
  Fixture valid;
  FlatPassManager pm;
  pm.addPass(makePass("rewrite", [&](Function& f, PassContext& ctx) {
    auto edit = ctx.mutator().rewriteOperand(
        &f == valid.a ? valid.sumA : valid.sumB, 0, valid.two);
    PassResult r = outcome(edit == MutationResult::Applied ?
                           PassStatus::Applied : PassStatus::Failed);
    r.stats.counters["applied"] = 1;
    return r;
  }));
  PassManagerOptions opts;
  opts.instrument = true;
  auto off = pm.run(valid.m, opts);
  check(off.ok() && off.invocations.size() == 2 &&
        off.invocations[0].verification == VerificationOutcome::NotRun &&
        off.invocations[0].beforeDump.empty() &&
        !off.invocations[0].elapsedNanoseconds,
        "verify/dumps/time absent when disabled");
  Fixture on;
  FlatPassManager pm2;
  pm2.addPass(makePass("rewrite", [&](Function& f, PassContext& ctx) {
    auto edit = ctx.mutator().rewriteOperand(
        &f == on.a ? on.sumA : on.sumB, 0, on.two);
    PassResult r = outcome(edit == MutationResult::Applied ?
                           PassStatus::Applied : PassStatus::Failed);
    r.stats.counters["applied"] = 1;
    return r;
  }));
  opts.verifyEach = true;
  opts.captureDumps = true;
  opts.measureTime = true;
  auto shown = pm2.run(on.m, opts);
  check(shown.ok() && shown.invocations.size() == 2 &&
        shown.invocations[0].beforeDump != shown.invocations[0].afterDump &&
        shown.invocations[0].afterDump == shown.invocations[1].beforeDump &&
        !shown.invocations[1].afterDump.empty() &&
        shown.invocations[0].elapsedNanoseconds.has_value() &&
        shown.invocations[0].stats.counters.at("applied") == 1 &&
        shown.invocations[1].verification == VerificationOutcome::Passed,
        "optional before/after Flat dumps, timing, stats and verify-each observable");
  Fixture corruptEntry;
  // Test-only raw corruption: return terminator may not carry successors.
  corruptEntry.a->entry()->terminator()->addSucc(corruptEntry.b->entry());
  int invoked = 0;
  FlatPassManager bad;
  bad.addPass(makePass("spy", [&](Function&, PassContext&) {
    ++invoked; return outcome(PassStatus::NoChange);
  }));
  opts.captureDumps = false;
  opts.measureTime = false;
  auto badInput = bad.run(corruptEntry.m, opts);
  ++negatives;
  check(badInput.status == PipelineStatus::InvalidInput &&
        invoked == 0 && badInput.invocations.empty() &&
        !badInput.diagnostic.empty(),
        "verify-each rejects corrupt entry before any invocation");
  Fixture corruptAfter;
  FlatPassManager corrupt;
  corrupt.addPass(makePass("test-raw-corruption", [&](Function& f, PassContext&) {
    f.entry()->terminator()->addSucc(corruptAfter.b->entry());
    return outcome(PassStatus::NoChange);
  }));
  corrupt.addPass(makePass("must-not-execute", [&](Function&, PassContext&) {
    ++invoked; return outcome(PassStatus::NoChange);
  }));
  auto after = corrupt.run(corruptAfter.m, opts);
  ++negatives;
  check(after.status == PipelineStatus::VerificationFailed &&
        after.invocations.size() == 1 &&
        after.invocations[0].verification == VerificationOutcome::Failed &&
        !after.invocations[0].changed &&
        !after.invocations[0].contractViolation &&
        invoked == 0 && !after.diagnostic.empty(),
        "verify-each catches test-only untracked raw corruption after invocation");
}
std::string stableRecord(const PipelineResult& result) {
  std::ostringstream out;
  out << static_cast<int>(result.status) << "|" << result.diagnostic;
  for (const auto& rec : result.invocations) {
    out << "/" << rec.passName << ":" << rec.functionName << ":" << rec.functionIndex
        << ":" << rec.disabled << ":" << rec.changed << ":"
        << rec.mutation.operations << ":" << static_cast<uint32_t>(rec.mutation.kinds)
        << ":" << static_cast<int>(rec.verification)
        << ":" << rec.beforeDump << ":" << rec.afterDump;
    for (size_t index : rec.affectedFunctionIndices) out << "#" << index;
    for (const auto& kv : rec.stats.counters) out << kv.first << "=" << kv.second;
  }
  return out.str();
}
void testDeterministicCapture() {
  auto buildAndCapture = [] {
    Fixture x;
    FlatPassManager pm;
    pm.addPass(makePass("rewrite", [&](Function& f, PassContext& ctx) {
      Instruction* sum = &f == x.a ? x.sumA : x.sumB;
      const auto edit = ctx.mutator().rewriteOperand(sum, 0, x.two);
      PassResult r = outcome(edit == MutationResult::Applied ?
                             PassStatus::Applied : PassStatus::Failed);
      r.stats.counters["z"] = 2;
      r.stats.counters["a"] = 1;
      return r;
    }));
    pm.addPass(makePass("noop", [](Function&, PassContext&) {
      return outcome(PassStatus::NoChange);
    }));
    PassManagerOptions options;
    options.instrument = true;
    options.captureDumps = true;
    options.verifyEach = true;
    return stableRecord(pm.run(x.m, options));
  };
  check(buildAndCapture() == buildAndCapture(),
        "two equivalent fresh Modules produce deterministic ordered records and dumps");
}
} // namespace
int main() {
  testOrderedPipeline();
  testDisabledAndSemantics();
  testFailuresAndContractNegatives();
  testPreservedClaimsAndRealRevisions();
  testVerifierAndInstrumentation();
  testDeterministicCapture();
  std::printf("PASS_MANAGER_CHECKS=%d NEGATIVE_CONTROLS=%d FAILURES=%d\n",
              checks, negatives, failures);
  return failures ? 1 : 0;
}
