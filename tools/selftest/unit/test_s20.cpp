// S20: SCEV, LICM, power-of-two strength reduction and AddressPlan.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "analysis/ScalarEvolution.h"
#include "ir/FlatVerifier.h"
#include "opt/AddressPlan.h"
#include "opt/LICM.h"
#include "opt/StrengthReduce.h"
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
struct LoopFixture {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* header = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* exit = nullptr;
  Argument* a = nullptr;
  Argument* b = nullptr;
  Instruction* phi = nullptr;
  Instruction* index = nullptr;
  LoopFixture() {
    f = m.createFunction("loop", i32);
    pre = m.createBlock(f); header = m.createBlock(f);
    body = m.createBlock(f); exit = m.createBlock(f);
    a = m.createArgument(i32, {}); b = m.createArgument(i32, {});
    f->addParam(a); f->addParam(b);
    add(m, pre, createBr(header, {}));
    phi = add(m, header, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                  {pre, body}, {}));
    Instruction* cmp = add(m, header, createICmp(IPred::Slt, phi,
                                                 m.getIntConst(i32, 10), {}));
    add(m, header, createCondBr(cmp, body, exit, {}));
    Instruction* next = add(m, body, createBin(Opcode::Add, phi,
                                               m.getIntConst(i32, 1), {}));
    index = add(m, body, createBin(Opcode::Mul, phi, m.getIntConst(i32, 4), {}));
    add(m, body, createBin(Opcode::Add, index, a, {}));   // keep index used
    add(m, body, createBr(header, {}));
    phi->setOperand(1, next);
    add(m, exit, createRet(b, {}));
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testSCEV() {
  LoopFixture f;
  check(verifyFlatModule(f.m).empty(), "SCEV fixture verifies");
  AnalysisManager manager(f.m);
  auto scev = manager.get<ScalarEvolutionAnalysis>(*f.f);
  check(scev.isCurrent(), "SCEV computed");
  auto phi = scev.get()->of(f.phi);
  check(phi && phi->kind == SCEV::Kind::AddRec &&
        phi->describe() == "{0,+,1}", "phi is {0,+,1}");
  auto index = scev.get()->of(f.index);
  check(index && index->kind == SCEV::Kind::Mul &&
        index->describe().find("* 4") != std::string::npos,
        "index is 4 times the induction variable");
}
void testLICM() {
  LoopFixture f;
  BasicBlock* body = f.body;
  auto insertAt = [&](BasicBlock* block, size_t index, Instruction* inst) {
    f.m.ownInst(inst); block->insertInst(index, inst); return inst;
  };
  Instruction* invariant = insertAt(body, 0, createBin(Opcode::Mul, f.a, f.b, {}));
  Instruction* slot = insertAt(f.pre, 0, createAlloca(f.i32, {}));
  insertAt(body, 1, createStore(f.i32, invariant, slot, {}));
  f.m.rebuildCFG(); f.m.rebuildUseDef();
  check(verifyFlatModule(f.m).empty(), "LICM fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LICMPass>());
  const auto r = pm.run(f.m);
  bool inPre = false;
  for (Instruction* inst : f.pre->insts())
    if (inst->op() == Opcode::Mul) inPre = true;
  check(r.ok() && r.changed && inPre && !invariant->parent() &&
        verifyFlatModule(f.m).empty(),
        "LICM hoists an invariant multiply to the preheader");
  check(f.index->parent() == body, "LICM leaves an induction-dependent multiply in the loop");
}
void testStrengthReduce() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("reduce", i32);
  BasicBlock* entry = m.createBlock(f);
  Argument* x = m.createArgument(i32, {});
  f->addParam(x);
  Instruction* div = add(m, entry, createBin(Opcode::SDiv, x, m.getIntConst(i32, 8), {}));
  Instruction* mul = add(m, entry, createBin(Opcode::Mul, div, m.getIntConst(i32, 16), {}));
  add(m, entry, createRet(mul, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager pm; pm.addPass(std::make_unique<StrengthReducePass>());
  const auto r = pm.run(m);
  bool hasDiv = false, hasMul = false, hasShl = false;
  for (BasicBlock* block : f->blocks())
    for (Instruction* inst : block->insts()) {
      hasDiv = hasDiv || inst->op() == Opcode::SDiv;
      hasMul = hasMul || inst->op() == Opcode::Mul;
      hasShl = hasShl || inst->op() == Opcode::Shl;
    }
  check(r.ok() && r.changed && !hasDiv && !hasMul && hasShl &&
        verifyFlatModule(m).empty(),
        "power-of-two div/mul become shifts");
}
void testAddressPlan() {
  CostContext facts{0.9, true};
  CostVector indexed{3.0, 0.0}, pointer{1.0, 0.0};
  check(chooseAddressPlan(indexed, pointer, facts).strategy ==
            AddressStrategy::PointerInduction,
        "target facts choose the cheaper pointer induction");
  check(chooseAddressPlan(pointer, indexed, facts).strategy == AddressStrategy::Indexed,
        "target facts can choose indexed addressing");
  CostContext none{0.0, false};
  const AddressPlan fallback = chooseAddressPlan(pointer, indexed, none);
  check(fallback.strategy == AddressStrategy::Indexed && !fallback.usedTargetFacts,
        "no target facts falls back to indexed addressing");
}
}  // namespace
int main() {
  testSCEV();
  testLICM();
  testStrengthReduce();
  testAddressPlan();
  std::printf("S20_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
