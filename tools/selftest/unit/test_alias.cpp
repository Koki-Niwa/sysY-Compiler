// S13: alias provenance over locals, globals, parameters and returns.
#include <cstdio>
#include <string>
#include "analysis/AliasAnalysis.h"
#include "analysis/AnalysisManager.h"
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
bool queryIs(const AliasInfo& info, const Value* a, const Value* b,
             AliasKind kind) {
  return info.query(a, b) == kind;
}
struct Fixture {
  Module m;
  const Type* i32 = typePool().i32();
  const Type* ptr = typePool().ptrTo(typePool().i32());
  Instruction *alloca1 = nullptr, *alloca2 = nullptr;
  Instruction *gepA0 = nullptr, *gepA0b = nullptr, *gepA1 = nullptr, *gepDyn = nullptr;
  Instruction *castA = nullptr, *callIdA = nullptr, *callIdB = nullptr;
  Instruction *callGlobal = nullptr, *callGlobal2 = nullptr, *callExternal = nullptr;
  Instruction *callOffset = nullptr;
  Instruction *phiSame = nullptr, *phiDiff = nullptr;
  GlobalAddr *addrA = nullptr, *addrB = nullptr;
  Argument *n = nullptr, *p1 = nullptr, *p2 = nullptr;
  Function *probe = nullptr;

  Fixture() {
    GlobalVariable* gA = m.createGlobal("A", typePool().arrayOf(i32, 4));
    GlobalVariable* gB = m.createGlobal("B", typePool().arrayOf(i32, 4));
    addrA = m.globalAddr(gA);
    addrB = m.globalAddr(gB);

    probe = m.createFunction("probe", typePool().voidTy());
    BasicBlock* entry = m.createBlock(probe);
    n = m.createArgument(i32, {});
    p1 = m.createArgument(ptr, {});
    p2 = m.createArgument(ptr, {});
    probe->addParam(n);
    probe->addParam(p1);
    probe->addParam(p2);
    alloca1 = add(m, entry, createAlloca(i32, {}));
    alloca2 = add(m, entry, createAlloca(i32, {}));
    castA = add(m, entry, createBitCast(ptr, addrA, {}));
    gepA0 = add(m, entry, createGEP(i32, castA, m.getIntConst(i32, 0), {}));
    gepA0b = add(m, entry, createGEP(i32, castA, m.getIntConst(i32, 0), {}));
    gepA1 = add(m, entry, createGEP(i32, castA, m.getIntConst(i32, 1), {}));
    gepDyn = add(m, entry, createGEP(i32, castA, n, {}));
    BasicBlock* left = m.createBlock(probe);
    BasicBlock* right = m.createBlock(probe);
    BasicBlock* join = m.createBlock(probe);
    add(m, entry, createCondBr(m.getIntConst(i32, 1), left, right, {}));
    add(m, left, createBr(join, {}));
    add(m, right, createBr(join, {}));
    phiSame = add(m, join, createPhi(ptr, {castA, castA}, {left, right}, {}));
    phiDiff = add(m, join, createPhi(ptr, {castA, addrB}, {left, right}, {}));
    add(m, join, createRet(nullptr, {}));

    // `id(p) { return p; }` and `retg() { return @A; }`.
    Function* id = m.createFunction("id", ptr);
    BasicBlock* idEntry = m.createBlock(id);
    Argument* idArg = m.createArgument(ptr, {});
    id->addParam(idArg);
    add(m, idEntry, createRet(idArg, {}));
    Function* retg = m.createFunction("retg", ptr);
    BasicBlock* retgEntry = m.createBlock(retg);
    add(m, retgEntry, createRet(addrA, {}));
    Function* ret1 = m.createFunction("ret1", ptr);
    BasicBlock* ret1Entry = m.createBlock(ret1);
    Instruction* ret1Cast = add(m, ret1Entry, createBitCast(ptr, addrA, {}));
    Instruction* ret1Gep =
        add(m, ret1Entry, createGEP(i32, ret1Cast, m.getIntConst(i32, 1), {}));
    add(m, ret1Entry, createRet(ret1Gep, {}));
    BasicBlock* tail = m.createBlock(probe);
    add(m, join, createBr(tail, {}));
    callIdA = add(m, tail, createCall("id", ptr, {addrA}, {}));
    callIdB = add(m, tail, createCall("id", ptr, {addrB}, {}));
    callGlobal = add(m, tail, createCall("retg", ptr, {}, {}));
    callGlobal2 = add(m, tail, createCall("retg", ptr, {}, {}));
    callExternal = add(m, tail, createCall("mystery", ptr, {}, {}));
    callOffset = add(m, tail, createCall("ret1", ptr, {}, {}));
    add(m, tail, createRet(nullptr, {}));
    m.rebuildCFG();
    m.rebuildUseDef();
  }
};
void testProvenance() {
  Fixture f;
  AnalysisManager manager(f.m);
  auto handle = manager.get<AliasAnalysis>();
  check(handle.isCurrent() && handle.get() != nullptr, "alias analysis computed");
  const AliasInfo& info = *handle.get();
  check(queryIs(info, f.alloca1, f.alloca1, AliasKind::MustAlias),
        "same alloca is must-alias");
  check(queryIs(info, f.alloca1, f.alloca2, AliasKind::NoAlias),
        "distinct allocas do not alias");
  check(queryIs(info, f.alloca1, f.addrA, AliasKind::NoAlias),
        "local alloca and global do not alias");
  check(queryIs(info, f.addrA, f.addrB, AliasKind::NoAlias),
        "distinct globals do not alias");
  check(queryIs(info, f.addrA, f.addrA, AliasKind::MustAlias),
        "same global is must-alias");
  check(queryIs(info, f.castA, f.addrA, AliasKind::MustAlias),
        "bitcast preserves the global base");
  check(queryIs(info, f.gepA0, f.castA, AliasKind::MustAlias),
        "gep with constant zero stays at offset zero");
  check(queryIs(info, f.gepA0, f.gepA0b, AliasKind::MustAlias),
        "two gep zero results are must-alias");
  check(queryIs(info, f.gepA1, f.castA, AliasKind::NoAlias),
        "different constant offsets do not alias");
  check(queryIs(info, f.gepA1, f.gepA0, AliasKind::NoAlias),
        "distinct constant offsets do not alias");
  check(queryIs(info, f.gepDyn, f.castA, AliasKind::MayAlias),
        "dynamic offset may alias the base");
  check(queryIs(info, f.gepDyn, f.gepA1, AliasKind::MayAlias),
        "dynamic offset may alias another offset");
  check(queryIs(info, f.p1, f.p1, AliasKind::MustAlias),
        "same argument is must-alias");
  check(queryIs(info, f.p1, f.p2, AliasKind::MayAlias),
        "distinct arguments may alias");
  check(queryIs(info, f.p1, f.alloca1, AliasKind::NoAlias),
        "argument and local alloca do not alias");
  check(queryIs(info, f.p1, f.addrA, AliasKind::MayAlias),
        "argument may point at a global");
  check(queryIs(info, f.callIdA, f.addrA, AliasKind::MustAlias),
        "call returning param resolves to the caller argument");
  check(queryIs(info, f.callIdA, f.callIdB, AliasKind::NoAlias),
        "param-returning calls with disjoint arguments do not alias");
  check(queryIs(info, f.callGlobal, f.addrA, AliasKind::MustAlias),
        "call returning a global keeps the global base");
  check(queryIs(info, f.callGlobal, f.callGlobal2, AliasKind::MustAlias),
        "two calls returning the same global are must-alias");
  check(queryIs(info, f.callExternal, f.addrA, AliasKind::MayAlias),
        "unknown external result may alias anything");
  check(queryIs(info, f.callOffset, f.castA, AliasKind::MayAlias),
        "call returning an interior pointer keeps an unknown offset");
  check(queryIs(info, f.phiSame, f.addrA, AliasKind::MustAlias),
        "phi of the same base stays must-alias");
  check(queryIs(info, f.phiDiff, f.addrA, AliasKind::MayAlias),
        "phi of disjoint globals is unknown");
  check(queryIs(info, f.gepA0, f.callExternal, AliasKind::MayAlias),
        "unknown provenance never proves no-alias");
}
void testInvalidation() {
  Module m;
  const Type* i32 = typePool().i32();
  const Type* ptr = typePool().ptrTo(i32);
  GlobalVariable* g = m.createGlobal("g", i32);
  GlobalAddr* addr = m.globalAddr(g);
  Function* f = m.createFunction("f", ptr);
  BasicBlock* b = m.createBlock(f);
  add(m, b, createRet(addr, {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  AnalysisManager manager(m);
  auto handle = manager.get<AliasAnalysis>();
  check(handle.isCurrent() && handle.get()->query(addr, addr) == AliasKind::MustAlias,
        "fixture alias is current");
  IRMutator mutator(m);
  Instruction* store = createStore(i32, m.getIntConst(i32, 1), addr, {});
  check(mutator.insertInstruction(b, 0, store) == MutationResult::Applied,
        "tracked memory mutation");
  check(!handle.isCurrent() && handle.get() == nullptr,
        "memory mutation invalidates alias handle");
  auto rebuilt = manager.get<AliasAnalysis>();
  check(rebuilt.isCurrent(), "alias recomputes after memory mutation");
  Instruction* call = createCall("mystery", nullptr, {}, {});
  check(mutator.insertInstruction(b, 0, call) == MutationResult::Applied,
        "tracked call mutation");
  check(!rebuilt.isCurrent(), "call mutation invalidates alias handle");
  auto afterCall = manager.get<AliasAnalysis>();
  check(afterCall.isCurrent() && afterCall.get()->query(addr, addr) ==
        AliasKind::MustAlias, "alias recomputes after call mutation");
}
void testReturnDataFlowInvalidation() {
  Module m; const Type* i32 = typePool().i32();
  const Type* ptr = typePool().ptrTo(i32);
  GlobalVariable* gx = m.createGlobal("x", i32);
  GlobalVariable* gy = m.createGlobal("y", i32);
  GlobalAddr* ax = m.globalAddr(gx);
  GlobalAddr* ay = m.globalAddr(gy);
  Function* id = m.createFunction("id2", ptr);
  BasicBlock* ib = m.createBlock(id);
  Argument* a = m.createArgument(ptr, {});
  Argument* b = m.createArgument(ptr, {});
  id->addParam(a); id->addParam(b);
  Instruction* ret = add(m, ib, createRet(a, {}));
  Function* main = m.createFunction("main", i32);
  BasicBlock* mb = m.createBlock(main);
  Instruction* call = add(m, mb, createCall("id2", ptr, {ax, ay}, {}));
  add(m, mb, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  AnalysisManager manager(m);
  auto handle = manager.get<AliasAnalysis>();
  check(handle.isCurrent() &&
        handle.get()->query(call, ax) == AliasKind::MustAlias &&
        handle.get()->query(call, ay) == AliasKind::NoAlias,
        "return provenance resolves the first argument");
  IRMutator mutator(m);
  check(mutator.rewriteOperand(ret, 0, b) == MutationResult::Applied,
        "tracked data-flow rewrite of the returned pointer");
  check(!handle.isCurrent(), "pointer data-flow change invalidates alias");
  auto rebuilt = manager.get<AliasAnalysis>();
  check(rebuilt.isCurrent() &&
        rebuilt.get()->query(call, ay) == AliasKind::MustAlias &&
        rebuilt.get()->query(call, ax) == AliasKind::NoAlias,
        "alias recomputes the changed return address");
}
}  // namespace
int main() {
  testProvenance();
  testInvalidation();
  testReturnDataFlowInvalidation();
  std::printf("S13_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
