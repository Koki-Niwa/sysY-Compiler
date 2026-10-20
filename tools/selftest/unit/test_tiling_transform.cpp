// S24b: order-preserving strip-mining of a canonical loop.
#include <cstdio>
#include <memory>
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/LoopTiling.h"
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
struct Nested {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = nullptr;
  BasicBlock* pre = nullptr;
  BasicBlock* outer = nullptr;
  BasicBlock* inner = nullptr;
  BasicBlock* body = nullptr;
  BasicBlock* latch = nullptr;
  BasicBlock* exit = nullptr;
  GlobalAddr* global = nullptr;
  explicit Nested(int bound) {
    global = m.globalAddr(m.createGlobal("g", i32));
    f = m.createFunction("nest", i32);
    pre = m.createBlock(f); outer = m.createBlock(f); inner = m.createBlock(f);
    body = m.createBlock(f); latch = m.createBlock(f); exit = m.createBlock(f);
    add(m, pre, createBr(outer, {}));
    Instruction* oi = add(m, outer, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                             {pre, latch}, {}));
    Instruction* oc = add(m, outer, createICmp(IPred::Slt, oi,
                                               m.getIntConst(i32, bound), {}));
    add(m, outer, createCondBr(oc, inner, exit, {}));
    Instruction* ii = add(m, inner, createPhi(i32, {m.getIntConst(i32, 0), nullptr},
                                              {outer, body}, {}));
    Instruction* ic = add(m, inner, createICmp(IPred::Slt, ii,
                                               m.getIntConst(i32, bound), {}));
    add(m, inner, createCondBr(ic, body, latch, {}));
    Instruction* sum = add(m, body, createBin(Opcode::Add, oi, ii, {}));
    add(m, body, createStore(i32, sum, global, {}));
    Instruction* iiNext = add(m, body, createBin(Opcode::Add, ii,
                                                 m.getIntConst(i32, 1), {}));
    add(m, body, createBr(inner, {}));
    Instruction* oiNext = add(m, latch, createBin(Opcode::Add, oi,
                                                  m.getIntConst(i32, 1), {}));
    add(m, latch, createBr(outer, {}));
    add(m, exit, createRet(m.getIntConst(i32, 0), {}));
    oi->setOperand(1, oiNext);
    ii->setOperand(1, iiNext);
    m.rebuildCFG(); m.rebuildUseDef();
  }
};
void testTiling() {
  Nested n(64);
  check(verifyFlatModule(n.m).empty(), "tiling fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopTilingPass>());
  const auto r = pm.run(n.m);
  check(r.ok() && r.changed && verifyFlatModule(n.m).empty(),
        "strip-mine transform verifies");
  Instruction* preTerm = n.pre->terminator();
  check(preTerm != nullptr && preTerm->numSuccs() == 1 &&
        preTerm->succ(0) != n.outer,
        "preheader enters the tile header, not the old loop directly");
  BasicBlock* tileHeader = preTerm->succ(0);
  Instruction* tileTerm = tileHeader->terminator();
  check(tileTerm != nullptr && tileTerm->op() == Opcode::Br &&
        tileTerm->numSuccs() == 2 && tileTerm->succ(0) == n.outer &&
        tileTerm->succ(1) == n.exit,
        "tile header guards the original loop and final exit");
  check(n.outer->preds().size() == 2 &&
        std::find(n.outer->preds().begin(), n.outer->preds().end(), tileHeader) !=
            n.outer->preds().end(),
        "original header gains the tile predecessor");
  Instruction* outerTerm = n.outer->terminator();
  check(outerTerm->numSuccs() == 2 && outerTerm->succ(0) == n.inner,
        "original body edge remains");
  BasicBlock* tileLatch = outerTerm->succ(1);
  check(tileLatch != n.exit && tileLatch->terminator() != nullptr &&
        tileLatch->terminator()->numSuccs() == 1 &&
        tileLatch->terminator()->succ(0) == tileHeader,
        "tile latch increments and returns to the tile header");
  size_t stores = 0;
  for (BasicBlock* block : n.f->blocks())
    for (Instruction* inst : block->insts())
      if (inst->op() == Opcode::Store) ++stores;
  check(stores == 1, "strip-mining does not duplicate the body store");
}
void testNoTileWhenFits() {
  Nested n(8);
  check(verifyFlatModule(n.m).empty(), "small-nest fixture verifies");
  FlatPassManager pm; pm.addPass(std::make_unique<LoopTilingPass>());
  const auto r = pm.run(n.m);
  check(r.ok() && !r.changed && n.pre->terminator()->succ(0) == n.outer,
        "small working set is left untiled");
}
}  // namespace
int main() {
  testTiling();
  testNoTileWhenFits();
  std::printf("S24B_TILE_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
