// S24b: phi-aware terminator replacement on newly added CFG edges.
#include <cstdio>
#include "ir/CFGEditor.h"
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
  m.ownInst(i); b->addInst(i); return i;
}
void testAddPhiEdge() {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("f", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* bridge = m.createBlock(f);
  BasicBlock* header = m.createBlock(f);
  BasicBlock* latch = m.createBlock(f);
  add(m, entry, createBr(bridge, {}));
  Instruction* placeholder = createUnreachable({});
  m.ownInst(placeholder);
  bridge->addInst(placeholder);
  add(m, header, createBr(latch, {}));
  add(m, latch, createBr(header, {}));
  Instruction* phi = createPhi(i32, {m.getIntConst(i32, 0)}, {latch}, {});
  m.ownInst(phi);
  header->insertInst(0, phi);
  m.rebuildCFG(); m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "base phi-edge fixture verifies");
  IRMutator mutator(m);
  CFGEditor cfg(mutator);
  check(cfg.replaceTerminator(bridge, createBr(header, {}),
                              {{m.getIntConst(i32, 5)}}) == MutationResult::Applied,
        "new bridge edge supplies the header phi value");
  check(verifyFlatModule(m).empty(), "phi edge addition verifies");
  bool foundFive = false, foundZero = false;
  for (size_t i = 0; i < phi->numSuccs(); ++i) {
    if (phi->succ(i) != bridge && phi->succ(i) != latch) continue;
    const Value* value = phi->operand(i);
    if (value != nullptr && value->isConstant()) {
      const int64_t n = static_cast<const Constant*>(value)->intVal();
      foundFive = foundFive || (phi->succ(i) == bridge && n == 5);
      foundZero = foundZero || (phi->succ(i) == latch && n == 0);
    }
  }
  check(foundFive && foundZero, "phi keeps both predecessor values");
  check(cfg.replaceTerminator(bridge, createBr(header, {}),
                              {{m.getIntConst(i32, 5)}}) == MutationResult::Rejected,
        "existing phi edge rejects redundant values");
}

void testRetainedSecondSuccessor() {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("retained", i32);
  BasicBlock* entry = m.createBlock(f);
  BasicBlock* source = m.createBlock(f);
  BasicBlock* first = m.createBlock(f);
  BasicBlock* target = m.createBlock(f);
  BasicBlock* other = m.createBlock(f);
  Constant* zero = m.getIntConst(i32, 0);
  Constant* one = m.getIntConst(i32, 1);
  add(m, entry, createBr(source, {}));
  add(m, source, createCondBr(zero, first, target, {}));
  add(m, first, createRet(zero, {}));
  add(m, other, createBr(target, {}));
  Instruction* phi = add(m, target, createPhi(i32, {zero, one},
                                               {source, other}, {}));
  add(m, target, createRet(phi, {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "retained-second fixture verifies");
  IRMutator mutator(m);
  CFGEditor cfg(mutator);
  const uint64_t beforeRejection = m.revision().any;
  Instruction* rejectedRet = createRet(zero, {});
  check(cfg.replaceTerminator(source, rejectedRet) == MutationResult::Rejected,
        "removing a later phi edge is rejected");
  mutator.discardDetached({rejectedRet});
  check(m.revision().any == beforeRejection &&
            first->preds().size() == 1 && first->preds()[0] == source &&
            source->terminator()->numSuccs() == 2 &&
            verifyFlatModule(m).empty(),
        "later phi rejection preserves every earlier predecessor");
  check(cfg.replaceTerminator(source, createBr(target, {})) ==
            MutationResult::Applied,
        "retained second successor preserves phi without incoming values");
  check(verifyFlatModule(m).empty() && phi->numOperands() == 2 &&
            phi->succ(0) == source && phi->succ(1) == other,
        "retained second successor has no duplicate phi predecessor");

  Module rejected;
  Function* rf = rejected.createFunction("retained_reject", i32);
  BasicBlock* re = rejected.createBlock(rf);
  BasicBlock* rs = rejected.createBlock(rf);
  BasicBlock* rfirst = rejected.createBlock(rf);
  BasicBlock* rtarget = rejected.createBlock(rf);
  BasicBlock* rother = rejected.createBlock(rf);
  Constant* rzero = rejected.getIntConst(i32, 0);
  Constant* rone = rejected.getIntConst(i32, 1);
  add(rejected, re, createBr(rs, {}));
  add(rejected, rs, createCondBr(rzero, rfirst, rtarget, {}));
  add(rejected, rfirst, createRet(rzero, {}));
  add(rejected, rother, createBr(rtarget, {}));
  Instruction* rphi = add(rejected, rtarget,
                           createPhi(i32, {rzero, rone}, {rs, rother}, {}));
  add(rejected, rtarget, createRet(rphi, {}));
  rejected.rebuildCFG();
  rejected.rebuildUseDef();
  IRMutator rejectedMutator(rejected);
  CFGEditor rejectedCfg(rejectedMutator);
  const uint64_t revision = rejected.revision().any;
  check(rejectedCfg.replaceTerminator(rs, createBr(rtarget, {}), {{rzero}}) ==
            MutationResult::Rejected,
        "retained second successor rejects redundant incoming atomically");
  check(verifyFlatModule(rejected).empty() &&
            rejected.revision().any == revision &&
            rs->terminator()->op() == Opcode::Br &&
            rs->terminator()->numSuccs() == 2 && rphi->numOperands() == 2,
        "redundant retained incoming leaves CFG and phi unchanged");
}

void testCreateBlockOwnership() {
  Module m;
  const Type* i32 = typePool().i32();
  Function* f = m.createFunction("create_block", i32);
  BasicBlock* entry = m.createBlock(f);
  add(m, entry, createRet(m.getIntConst(i32, 0), {}));
  m.rebuildCFG();
  m.rebuildUseDef();
  check(verifyFlatModule(m).empty(), "create-block base fixture verifies");

  IRMutator mutator(m);
  CFGEditor cfg(mutator);
  const size_t blocksBefore = m.ownedBlocks().size();
  const IRRevision moduleBefore = m.revision();
  const IRRevision functionBefore = f->revision();
  BasicBlock* created = cfg.createBlock(f);
  check(created != nullptr && created->parent() == f &&
            created->ownerModule() == &m && m.ownsBlock(created) &&
            m.ownedBlocks().size() == blocksBefore + 1 &&
            f->blockCount() == 2 && created->size() == 1 &&
            created->terminator() != nullptr &&
            created->terminator()->op() == Opcode::Unreachable,
        "create-block makes an owned unreachable block");
  check(verifyFlatModule(m).empty(), "created unreachable block verifies");
  check(m.revision().any == moduleBefore.any + 1 &&
            m.revision().cfg == moduleBefore.cfg + 1 &&
            m.revision().dataFlow == moduleBefore.dataFlow + 1 &&
            f->revision().any == functionBefore.any + 1 &&
            f->revision().cfg == functionBefore.cfg + 1 &&
            f->revision().dataFlow == functionBefore.dataFlow + 1 &&
            mutator.summary().operations == 1 &&
            hasMutation(mutator.summary().kinds, MutationKind::CFG) &&
            hasMutation(mutator.summary().kinds, MutationKind::DataFlow) &&
            mutator.summary().affectedFunctions.size() == 1 &&
            mutator.summary().affectedFunctions.count(f) == 1,
        "create-block commits one CFG/data-flow mutation");

  const size_t stableBlocks = m.ownedBlocks().size();
  const IRRevision stableModule = m.revision();
  const IRRevision stableFunction = f->revision();
  check(cfg.createBlock(nullptr) == nullptr &&
            m.ownedBlocks().size() == stableBlocks &&
            m.revision().any == stableModule.any &&
            f->revision().any == stableFunction.any,
        "null create-block function is rejected without mutation");

  Module foreign;
  Function* foreignFunction = foreign.createFunction("foreign", i32);
  const size_t foreignBlocks = foreign.ownedBlocks().size();
  check(cfg.createBlock(foreignFunction) == nullptr &&
            m.ownedBlocks().size() == stableBlocks &&
            foreign.ownedBlocks().size() == foreignBlocks &&
            m.revision().any == stableModule.any &&
            f->revision().any == stableFunction.any,
        "cross-module create-block function is rejected atomically");

  Function* declaration = m.createFunction("declaration", i32);
  const size_t declarationBlocks = m.ownedBlocks().size();
  const uint64_t declarationRevision = m.revision().any;
  check(cfg.createBlock(declaration) == nullptr && declaration->isDeclaration() &&
            m.ownedBlocks().size() == declarationBlocks &&
            m.revision().any == declarationRevision,
        "declaration create-block function is rejected atomically");
}
}  // namespace
int main() {
  testAddPhiEdge();
  testRetainedSecondSuccessor();
  testCreateBlockOwnership();
  std::printf("S24B_CFG_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
