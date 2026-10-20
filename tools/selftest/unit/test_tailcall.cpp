// S22: tail-call classification and stack-safety oracle.
#include <cstdio>
#include <vector>
#include "analysis/TailCallAnalysis.h"
#include "ir/FlatVerifier.h"
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
  m.ownInst(i); b->addInst(i); return i;
}
struct Fixture {
  Module m;
  const Type* i32 = typePool().i32();
  Function* self = nullptr;
  Function* caller = nullptr;
  Function* callee = nullptr;
  Fixture() {
    self = m.createFunction("self", i32);
    BasicBlock* sb = m.createBlock(self);
    Argument* x = m.createArgument(i32, {});
    self->addParam(x);
    Instruction* next = add(m, sb, createBin(Opcode::Add, x, m.getIntConst(i32, 1), {}));
    Instruction* call = add(m, sb, createCall("self", i32, {next}, {}));
    add(m, sb, createRet(call, {}));
    callee = m.createFunction("callee", i32);
    BasicBlock* cb = m.createBlock(callee);
    add(m, cb, createRet(m.getIntConst(i32, 1), {}));
    caller = m.createFunction("caller", i32);
    BasicBlock* eb = m.createBlock(caller);
    Instruction* c2 = add(m, eb, createCall("callee", i32, {}, {}));
    add(m, eb, createRet(c2, {}));
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testDirectAndGeneral() {
  Fixture f;
  check(verifyFlatModule(f.m).empty(), "tail fixture verifies");
  AnalysisManager manager(f.m);
  auto self = manager.get<TailCallAnalysis>(*f.self);
  check(self.isCurrent() && self.get()->hasDirectSelfTail &&
        self.get()->calls.size() == 1 &&
        self.get()->calls[0].kind == TailCallKind::DirectSelfTail &&
        self.get()->calls[0].stackSafe &&
        self.get()->allDirectSelfTailsStackSafe,
        "direct self tail call is classified stack-safe");
  auto caller = manager.get<TailCallAnalysis>(*f.caller);
  check(caller.isCurrent() && !caller.get()->hasDirectSelfTail &&
        caller.get()->calls[0].kind == TailCallKind::GeneralTail,
        "non-self tail call needs backend legality");
}
void testNonTailAndStackCapture() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("nontail", i32);
  Argument* x = m.createArgument(i32, {}); m.createBlock(f); f->addParam(x);
  BasicBlock* b = f->entry();
  Instruction* call = add(m, b, createCall("nontail", i32, {x}, {}));
  Instruction* after = add(m, b, createBin(Opcode::Add, call, m.getIntConst(i32, 1), {}));
  add(m, b, createRet(after, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  AnalysisManager manager(m);
  auto info = manager.get<TailCallAnalysis>(*f);
  check(info.isCurrent() && info.get()->calls[0].kind == TailCallKind::NotTail,
        "self call whose result is used is not a tail call");
  // Passing a local alloca into the recursive call is not stack-safe.
  Module s; const Type* t = typePool().i32();
  Function* g = s.createFunction("capture", t);
  BasicBlock* gb = s.createBlock(g);
  Argument* p = s.createArgument(typePool().ptrTo(t), {});
  g->addParam(p);
  Instruction* slot = add(s, gb, createAlloca(t, {}));
  Instruction* c = add(s, gb, createCall("capture", t, {slot}, {}));
  (void)c;
  add(s, gb, createRet(c, {}));
  s.rebuildCFG(); s.rebuildUseDef();
  AnalysisManager sm(s);
  auto sinfo = sm.get<TailCallAnalysis>(*g);
  check(sinfo.isCurrent() && sinfo.get()->calls[0].kind == TailCallKind::DirectSelfTail &&
        !sinfo.get()->calls[0].stackSafe &&
        !sinfo.get()->allDirectSelfTailsStackSafe,
        "tail call passing a stack object is rejected");
}
void testMutualTail() {
  Module m; const Type* i32 = typePool().i32();
  Function* a = m.createFunction("a", i32);
  Function* b = m.createFunction("b", i32);
  BasicBlock* ab = m.createBlock(a);
  BasicBlock* bb = m.createBlock(b);
  Instruction* ca = add(m, ab, createCall("b", i32, {}, {}));
  add(m, ab, createRet(ca, {}));
  Instruction* cb = add(m, bb, createCall("a", i32, {}, {}));
  add(m, bb, createRet(cb, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  AnalysisManager manager(m);
  auto info = manager.get<TailCallAnalysis>(*a);
  check(info.isCurrent() && !info.get()->hasDirectSelfTail &&
        info.get()->calls[0].kind == TailCallKind::MutualTail,
        "mutual recursion stays a backend decision");
}
}  // namespace
int main() {
  testDirectAndGeneral();
  testNonTailAndStackCapture();
  testMutualTail();
  std::printf("S22_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
