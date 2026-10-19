// S17: constant propagation, DCE and constant-branch CFG simplification.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ir/FlatVerifier.h"
#include "opt/ConstantPropagation.h"
#include "opt/DeadCodeElimination.h"
#include "opt/FlatPassManager.h"
#include "opt/SimplifyCFG.h"
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
size_t instructionCount(const Function& f) {
  size_t count = 0;
  for (BasicBlock* block : f.blocks()) count += block->size();
  return count;
}
void testConstantPropagation() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("fold", i32);
  BasicBlock* b = m.createBlock(f);
  Instruction* addInst = add(m, b, createBin(Opcode::Add, m.getIntConst(i32, 2),
                                             m.getIntConst(i32, 3), {}));
  Instruction* mul = add(m, b, createBin(Opcode::Mul, addInst,
                                         m.getIntConst(i32, 4), {}));
  Instruction* cmp = add(m, b, createICmp(IPred::Slt, mul, m.getIntConst(i32, 100), {}));
  Instruction* sel = add(m, b, createSelect(cmp, m.getIntConst(i32, 1),
                                            m.getIntConst(i32, 2), {}));
  add(m, b, createRet(sel, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager manager; manager.addPass(std::make_unique<ConstantPropagationPass>());
  const auto r = manager.run(m);
  bool hasBin = false, hasCmp = false, hasSelect = false;
  for (BasicBlock* block : f->blocks())
    for (Instruction* inst : block->insts()) {
      hasBin = hasBin || inst->op() == Opcode::Add || inst->op() == Opcode::Mul;
      hasCmp = hasCmp || inst->op() == Opcode::ICmp;
      hasSelect = hasSelect || inst->op() == Opcode::Select;
    }
  check(r.ok() && r.changed && !hasBin && !hasCmp && !hasSelect &&
        verifyFlatModule(m).empty(), "constants fold through add/mul/icmp/select");
  const Instruction* ret = f->entry()->terminator();
  check(ret != nullptr && ret->numOperands() == 1 && ret->operand(0)->isConstant() &&
        static_cast<const Constant*>(ret->operand(0))->intVal() == 1,
        "ret observes the folded select result 1");
}
void testDce() {
  Module m; const Type* i32 = typePool().i32();
  GlobalVariable* g = m.createGlobal("g", i32);
  GlobalAddr* addr = m.globalAddr(g);
  Function* f = m.createFunction("dce", i32);
  BasicBlock* b = m.createBlock(f);
  Instruction* deadAdd = add(m, b, createBin(Opcode::Add, m.getIntConst(i32, 1),
                                             m.getIntConst(i32, 2), {}));
  (void)deadAdd;
  add(m, b, createAlloca(i32, {}));
  add(m, b, createLoad(i32, addr, {}));
  add(m, b, createStore(i32, m.getIntConst(i32, 7), addr, {}));
  add(m, b, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  const size_t before = instructionCount(*f);
  FlatPassManager manager; manager.addPass(std::make_unique<DeadCodeEliminationPass>());
  const auto r = manager.run(m);
  check(r.ok() && r.changed && instructionCount(*f) == 2 &&
        verifyFlatModule(m).empty(), "DCE removes dead add/alloca/load, keeps store/ret");
  check(before > instructionCount(*f), "DCE reduces instruction count");
}
void testSimplifyCfg() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("branch", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* hot = m.createBlock(f);
  BasicBlock* cold = m.createBlock(f);
  BasicBlock* other = m.createBlock(f);
  add(m, entry, createCondBr(m.getIntConst(i32, 1), hot, cold, {}));
  add(m, hot, createRet(m.getIntConst(i32, 1), {}));
  add(m, cold, createRet(m.getIntConst(i32, 2), {}));
  add(m, other, createBr(cold, {}));   // cold keeps a predecessor
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager manager; manager.addPass(std::make_unique<SimplifyCFGPass>());
  const auto r = manager.run(m);
  Instruction* terminator = entry->terminator();
  check(r.ok() && r.changed && terminator != nullptr &&
        terminator->op() == Opcode::Br && terminator->numSuccs() == 1 &&
        terminator->succ(0) == hot && verifyFlatModule(m).empty(),
        "constant conditional branch becomes unconditional");
  // Dropping the last predecessor is refused: the target would become
  // unreachable and its outgoing phi edges would lose dominance.
  {
    Module u; const Type* t = typePool().i32();
    Function* uf = u.createFunction("last_pred", t);
    BasicBlock* ue = u.createBlock(uf);
    BasicBlock* uh = u.createBlock(uf);
    BasicBlock* uc = u.createBlock(uf);
    add(u, ue, createCondBr(u.getIntConst(t, 1), uh, uc, {}));
    add(u, uh, createRet(u.getIntConst(t, 1), {}));
    add(u, uc, createRet(u.getIntConst(t, 2), {}));
    u.rebuildCFG(); u.rebuildUseDef();
    FlatPassManager um; um.addPass(std::make_unique<SimplifyCFGPass>());
    const auto ur = um.run(u);
    check(ur.ok() && !ur.changed && ue->terminator()->numSuccs() == 2 &&
          verifyFlatModule(u).empty(),
          "conditional branch with a last-predecessor target is not folded");
  }
  // Duplicate-target branch also simplifies.
  Module d; const Type* t = typePool().i32();
  Function* df = d.createFunction("dup", t);
  BasicBlock* de = d.createBlock(df);
  BasicBlock* target = d.createBlock(df);
  add(d, de, createCondBr(d.getIntConst(t, 0), target, target, {}));
  add(d, target, createRet(d.getIntConst(t, 0), {}));
  d.rebuildCFG(); d.rebuildUseDef();
  FlatPassManager dm; dm.addPass(std::make_unique<SimplifyCFGPass>());
  const auto dr = dm.run(d);
  check(dr.ok() && dr.changed && de->terminator()->numSuccs() == 1 &&
        verifyFlatModule(d).empty(), "duplicate-target branch becomes unconditional");
}
void testPhiIsNotBroken() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("phi_guard", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* left = m.createBlock(f);
  BasicBlock* right = m.createBlock(f);
  BasicBlock* join = m.createBlock(f);
  add(m, entry, createCondBr(m.getIntConst(i32, 1), left, right, {}));
  add(m, left, createBr(join, {}));
  add(m, right, createBr(join, {}));
  add(m, join, createPhi(i32, {m.getIntConst(i32, 1), m.getIntConst(i32, 2)},
                         {left, right}, {}));
  add(m, join, createRet(join->at(0), {}));
  m.rebuildCFG(); m.rebuildUseDef();
  const size_t before = instructionCount(*f);
  FlatPassManager manager; manager.addPass(std::make_unique<SimplifyCFGPass>());
  const auto r = manager.run(m);
  // left/right lose their conditional edge to the unselected side only when
  // that side has no phi; here the phi is in join, so the branch may fold.
  check(r.ok() && verifyFlatModule(m).empty() && instructionCount(*f) <= before,
        "phi-bearing join remains valid after simplification");
}
}  // namespace
int main() {
  testConstantPropagation();
  testDce();
  testSimplifyCfg();
  testPhiIsNotBroken();
  std::printf("S17_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
