// Focused S09 mem2reg tests.  The test builds small verified flat modules so
// the checks exercise the public flat IR ownership, dump, verifier, and pass
// contracts together.
#include <cstdio>
#include <string>

#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "ir/Instruction.h"
#include "opt/Mem2Reg.h"

namespace {

using sysy::SourceLoc;
using sysy::flat::BasicBlock;
using sysy::flat::Function;
using sysy::flat::Instruction;
using sysy::flat::Mem2RegOptions;
using sysy::flat::Mem2RegResult;
using sysy::flat::Module;
using sysy::flat::Opcode;
using sysy::flat::IPred;
using sysy::flat::typePool;
using sysy::flat::Value;

int failures = 0;

void check(bool condition, const char* message) {
  if (condition) {
    std::printf("  ✔ %s\n", message);
  } else {
    std::printf("  ✘ %s\n", message);
    ++failures;
  }
}

Instruction* add(Module& module, BasicBlock* block, Instruction* instruction) {
  module.ownInst(instruction);
  block->addInst(instruction);
  return instruction;
}

void finish(Module& module) {
  module.rebuildCFG();
  module.rebuildUseDef();
}

bool valid(const Module& module) { return sysy::flat::verifyFlatModule(module).empty(); }

void testStraightAndFloat() {
  Module module;
  const SourceLoc loc(3, 2);
  Function* ints = module.createFunction("straight", typePool().i32());
  BasicBlock* ib = module.createBlock(ints);
  Instruction* ia = add(module, ib, sysy::flat::createAlloca(typePool().i32(), loc));
  Value* seven = module.getIntConst(typePool().i32(), 7);
  add(module, ib, sysy::flat::createStore(typePool().i32(), seven, ia, loc));
  Instruction* iload = add(module, ib, sysy::flat::createLoad(typePool().i32(), ia, loc));
  add(module, ib, sysy::flat::createRet(iload, loc));

  Function* floats = module.createFunction("float_straight", typePool().f32());
  BasicBlock* fb = module.createBlock(floats);
  Instruction* fa = add(module, fb, sysy::flat::createAlloca(typePool().f32(), loc));
  Value* one = module.getFloatConst(0x3f800000u);
  add(module, fb, sysy::flat::createStore(typePool().f32(), one, fa, loc));
  Instruction* fload = add(module, fb, sysy::flat::createLoad(typePool().f32(), fa, loc));
  add(module, fb, sysy::flat::createRet(fload, loc));
  finish(module);
  check(valid(module), "straight-line scalar module verifies before promotion");

  Mem2RegOptions options;
  options.captureDumps = true;
  Mem2RegResult result = sysy::flat::runMem2Reg(module, options);
  check(result.success && result.changed && result.promotedAllocas == 2,
        "i32 and f32 allocas are promoted");
  check(ints->instCount() == 1 && floats->instCount() == 1,
        "straight-line loads, stores, and allocas are removed");
  check(ints->entry()->back()->operand(0) == seven &&
            floats->entry()->back()->operand(0) == one,
        "loads are replaced by the reaching scalar values");
  check(result.beforeDump != result.afterDump && valid(module),
        "promoted module verifies and has a changed dump");
}

void testDiamondAndLoop() {
  Module module;
  const SourceLoc loc(10, 1);
  Function* diamond = module.createFunction("diamond", typePool().i32());
  BasicBlock* entry = module.createBlock(diamond);
  BasicBlock* left = module.createBlock(diamond);
  BasicBlock* right = module.createBlock(diamond);
  BasicBlock* join = module.createBlock(diamond);
  Instruction* slot = add(module, entry, sysy::flat::createAlloca(typePool().i32(), loc));
  Value* zero = module.getIntConst(typePool().i32(), 0);
  Value* one = module.getIntConst(typePool().i32(), 1);
  Value* two = module.getIntConst(typePool().i32(), 2);
  add(module, entry, sysy::flat::createStore(typePool().i32(), zero, slot, loc));
  Instruction* branch = add(module, entry,
                             sysy::flat::createCondBr(module.getIntConst(typePool().i32(), 1),
                                                      left, right, loc));
  (void)branch;
  add(module, left, sysy::flat::createStore(typePool().i32(), one, slot, loc));
  add(module, left, sysy::flat::createBr(join, loc));
  add(module, right, sysy::flat::createStore(typePool().i32(), two, slot, loc));
  add(module, right, sysy::flat::createBr(join, loc));
  Instruction* after = add(module, join, sysy::flat::createLoad(typePool().i32(), slot, loc));
  add(module, join, sysy::flat::createRet(after, loc));

  Function* loop = module.createFunction("loop", typePool().i32());
  BasicBlock* le = module.createBlock(loop);
  BasicBlock* head = module.createBlock(loop);
  BasicBlock* body = module.createBlock(loop);
  BasicBlock* exit = module.createBlock(loop);
  Instruction* counter = add(module, le, sysy::flat::createAlloca(typePool().i32(), loc));
  Value* limit = module.getIntConst(typePool().i32(), 3);
  add(module, le, sysy::flat::createStore(typePool().i32(), zero, counter, loc));
  add(module, le, sysy::flat::createBr(head, loc));
  Instruction* current = add(module, head, sysy::flat::createLoad(typePool().i32(), counter, loc));
  Instruction* test = add(module, head, sysy::flat::createICmp(IPred::Slt, current, limit, loc));
  add(module, head, sysy::flat::createCondBr(test, body, exit, loc));
  Instruction* increment = add(module, body,
                               sysy::flat::createBin(Opcode::Add, current, one, loc));
  add(module, body, sysy::flat::createStore(typePool().i32(), increment, counter, loc));
  add(module, body, sysy::flat::createBr(head, loc));
  Instruction* finalLoad = add(module, exit, sysy::flat::createLoad(typePool().i32(), counter, loc));
  add(module, exit, sysy::flat::createRet(finalLoad, loc));

  Function* carry = module.createFunction("carry_without_body_store", typePool().i32());
  BasicBlock* ce = module.createBlock(carry);
  BasicBlock* ch = module.createBlock(carry);
  BasicBlock* cb = module.createBlock(carry);
  BasicBlock* cx = module.createBlock(carry);
  Instruction* carrySlot = add(module, ce, sysy::flat::createAlloca(typePool().i32(), loc));
  add(module, ce, sysy::flat::createStore(typePool().i32(), two, carrySlot, loc));
  add(module, ce, sysy::flat::createBr(ch, loc));
  Instruction* carryRead = add(module, ch, sysy::flat::createLoad(typePool().i32(), carrySlot, loc));
  Instruction* carryTest = add(module, ch, sysy::flat::createICmp(IPred::Slt, carryRead, limit, loc));
  add(module, ch, sysy::flat::createCondBr(carryTest, cb, cx, loc));
  add(module, cb, sysy::flat::createBr(ch, loc));
  Instruction* carryExit = add(module, cx, sysy::flat::createLoad(typePool().i32(), carrySlot, loc));
  add(module, cx, sysy::flat::createRet(carryExit, loc));
  finish(module);
  check(valid(module), "diamond and loop modules verify before promotion");

  Mem2RegOptions options;
  Mem2RegResult result = sysy::flat::runMem2Reg(module, options);
  check(result.success && result.promotedAllocas == 3 && result.generatedPhis >= 2,
        "diamond and loop promotion creates reaching-definition phis");
  check(join->front()->op() == Opcode::Phi && head->front()->op() == Opcode::Phi,
        "phis are at block starts");
  check(join->front()->numSuccs() == 2 && head->front()->numSuccs() == 2,
        "phi predecessors are represented by sorted block CFG predecessors");
  Instruction* joinPhi = join->front();
  check(joinPhi->succ(0)->index() < joinPhi->succ(1)->index(),
        "diamond phi preserves canonical predecessor order");
  check(valid(module), "diamond and loop output verifies with use-def rebuilt");
}

void testInvalidInputIsAtomic() {
  Module module;
  const SourceLoc loc(30, 1);
  Function* function = module.createFunction("invalid_input", typePool().i32());
  BasicBlock* entry = module.createBlock(function);
  Instruction* slot = add(module, entry, sysy::flat::createAlloca(typePool().i32(), loc));
  Value* value = module.getIntConst(typePool().i32(), 7);
  Instruction* store = add(module, entry,
                           sysy::flat::createStore(typePool().i32(), value, slot, loc));
  Instruction* read = add(module, entry, sysy::flat::createLoad(typePool().i32(), slot, loc));
  add(module, entry, sysy::flat::createRet(read, loc));
  store->setOperand(1, value);
  finish(module);
  const std::string before = sysy::flat::dumpModule(module);

  Mem2RegResult result = sysy::flat::runMem2Reg(module);
  check(!result.success && !result.inputValid &&
            result.status == sysy::flat::Mem2RegStatus::InvalidInput,
        "invalid flat input is rejected before promotion");
  check(sysy::flat::dumpModule(module) == before,
        "invalid input rejection preserves the original dump");
}

void testUnsafeUsesAndMixedReachability() {
  Module module;
  const SourceLoc loc(35, 1);
  const auto* i32 = typePool().i32();
  const auto* i64 = typePool().i64();
  Value* zero = module.getIntConst(i32, 0);
  Value* index = module.getIntConst(i64, 0);

  Function* multiple = module.createFunction("multiple_use", i32);
  BasicBlock* mb = module.createBlock(multiple);
  Instruction* multiSlot = add(module, mb, sysy::flat::createAlloca(i32, loc));
  add(module, mb, sysy::flat::createStore(i32, zero, multiSlot, loc));
  add(module, mb, sysy::flat::createCall("sink", nullptr, {multiSlot, multiSlot}, loc));
  add(module, mb, sysy::flat::createRet(zero, loc));

  Function* derived = module.createFunction("derived_use", i32);
  BasicBlock* db = module.createBlock(derived);
  Instruction* derivedSlot = add(module, db, sysy::flat::createAlloca(i32, loc));
  add(module, db, sysy::flat::createStore(i32, zero, derivedSlot, loc));
  Instruction* gep = add(module, db, sysy::flat::createGEP(i32, derivedSlot, index, loc));
  Instruction* derivedLoad = add(module, db, sysy::flat::createLoad(i32, gep, loc));
  add(module, db, sysy::flat::createRet(derivedLoad, loc));

  Function* mixed = module.createFunction("mixed_reachability", i32);
  BasicBlock* entry = module.createBlock(mixed);
  BasicBlock* dead = module.createBlock(mixed);
  Instruction* mixedSlot = add(module, entry, sysy::flat::createAlloca(i32, loc));
  add(module, entry, sysy::flat::createStore(i32, zero, mixedSlot, loc));
  Instruction* liveLoad = add(module, entry, sysy::flat::createLoad(i32, mixedSlot, loc));
  add(module, entry, sysy::flat::createRet(liveLoad, loc));
  Instruction* deadLoad = add(module, dead, sysy::flat::createLoad(i32, mixedSlot, loc));
  add(module, dead, sysy::flat::createRet(deadLoad, loc));

  finish(module);
  check(valid(module), "unsafe-use and mixed-reachability input verifies");
  const std::string before = sysy::flat::dumpModule(module);
  Mem2RegResult result = sysy::flat::runMem2Reg(module);
  check(result.success && result.promotedAllocas == 0 &&
            sysy::flat::dumpModule(module) == before,
        "multiple uses, derived pointers, and mixed reachability decline unchanged");
  check(result.candidates.size() == 3 &&
            result.candidates[0].reason == sysy::flat::Mem2RegSkipReason::EscapedOrDerivedUse &&
            result.candidates[1].reason == sysy::flat::Mem2RegSkipReason::EscapedOrDerivedUse &&
            result.candidates[2].reason == sysy::flat::Mem2RegSkipReason::UnreachableUse,
        "unsafe direct, derived, and unreachable uses report their decline reasons");
}

void testTrivialAndSkips() {
  Module module;
  const SourceLoc loc(20, 1);
  Function* trivial = module.createFunction("trivial", typePool().i32());
  BasicBlock* entry = module.createBlock(trivial);
  BasicBlock* a = module.createBlock(trivial);
  BasicBlock* b = module.createBlock(trivial);
  BasicBlock* join = module.createBlock(trivial);
  Instruction* slot = add(module, entry, sysy::flat::createAlloca(typePool().i32(), loc));
  Value* same = module.getIntConst(typePool().i32(), 42);
  add(module, entry, sysy::flat::createCondBr(module.getIntConst(typePool().i32(), 1), a, b, loc));
  add(module, a, sysy::flat::createStore(typePool().i32(), same, slot, loc));
  add(module, a, sysy::flat::createBr(join, loc));
  add(module, b, sysy::flat::createStore(typePool().i32(), same, slot, loc));
  add(module, b, sysy::flat::createBr(join, loc));
  Instruction* read = add(module, join, sysy::flat::createLoad(typePool().i32(), slot, loc));
  add(module, join, sysy::flat::createRet(read, loc));
  finish(module);
  const std::string before = sysy::flat::dumpModule(module);
  Mem2RegResult first = sysy::flat::runMem2Reg(module);
  check(first.success && first.promotedAllocas == 1 && trivial->phiCount() == 0,
        "equivalent reaching definitions eliminate a trivial phi");
  check(valid(module), "trivial phi output verifies");

  Module skips;
  Function* f = skips.createFunction("skips", typePool().i32());
  BasicBlock* eb = skips.createBlock(f);
  Instruction* unsupported = add(skips, eb,
                                 sysy::flat::createAlloca(typePool().arrayOf(typePool().i32(), 2), loc));
  Instruction* escaped = add(skips, eb, sysy::flat::createAlloca(typePool().i32(), loc));
  add(skips, eb, sysy::flat::createCall("sink", nullptr, {escaped}, loc));
  Instruction* uninitialized = add(skips, eb, sysy::flat::createAlloca(typePool().i32(), loc));
  Instruction* deadSlot = add(skips, eb, sysy::flat::createAlloca(typePool().i32(), loc));
  Instruction* uninitRead = add(skips, eb,
                                sysy::flat::createLoad(typePool().i32(), uninitialized, loc));
  add(skips, eb, sysy::flat::createRet(uninitRead, loc));
  BasicBlock* dead = skips.createBlock(f);
  Instruction* deadRead = add(skips, dead, sysy::flat::createLoad(typePool().i32(), deadSlot, loc));
  add(skips, dead, sysy::flat::createRet(deadRead, loc));
  (void)unsupported;
  finish(skips);
  const std::string skipBefore = sysy::flat::dumpModule(skips);
  Mem2RegResult skipped = sysy::flat::runMem2Reg(skips);
  check(skipped.success && skipped.promotedAllocas == 0 &&
            sysy::flat::dumpModule(skips) == skipBefore,
        "aggregate, escaped, and read-before-definition candidates stay unchanged");
  check(!skipped.candidates.empty() &&
            skipped.candidates.back().reason == sysy::flat::Mem2RegSkipReason::UnreachableUse,
        "unreachable scalar accesses conservatively stay in memory");
  check(valid(skips), "declined candidates keep a verified module");
  check(!before.empty(), "test module dump is available");
}

void testBranchOnlyInitAndDisabled() {
  Module module;
  const SourceLoc loc(40, 1);
  Function* f = module.createFunction("branch_init", typePool().i32());
  BasicBlock* entry = module.createBlock(f);
  BasicBlock* initialized = module.createBlock(f);
  BasicBlock* missing = module.createBlock(f);
  BasicBlock* join = module.createBlock(f);
  Instruction* slot = add(module, entry, sysy::flat::createAlloca(typePool().i32(), loc));
  add(module, entry, sysy::flat::createCondBr(module.getIntConst(typePool().i32(), 1),
                                               initialized, missing, loc));
  add(module, initialized, sysy::flat::createStore(typePool().i32(),
                                                   module.getIntConst(typePool().i32(), 9), slot, loc));
  add(module, initialized, sysy::flat::createBr(join, loc));
  add(module, missing, sysy::flat::createBr(join, loc));
  Instruction* read = add(module, join, sysy::flat::createLoad(typePool().i32(), slot, loc));
  add(module, join, sysy::flat::createRet(read, loc));
  finish(module);
  const std::string before = sysy::flat::dumpModule(module);
  Mem2RegOptions disabled;
  disabled.enabled = false;
  disabled.captureDumps = true;
  Mem2RegResult result = sysy::flat::runMem2Reg(module, disabled);
  check(result.success && result.status == sysy::flat::Mem2RegStatus::Disabled &&
            result.beforeDump == result.afterDump && result.beforeDump == before,
        "disabled mode preserves identical before and after dumps");
  Mem2RegResult declined = sysy::flat::runMem2Reg(module);
  check(declined.success && declined.promotedAllocas == 0 &&
            sysy::flat::dumpModule(module) == before,
        "one-branch-only initialization is declined unchanged");
}

}  // namespace

int main() {
  testStraightAndFloat();
  testDiamondAndLoop();
  testInvalidInputIsAtomic();
  testUnsafeUsesAndMixedReachability();
  testTrivialAndSkips();
  testBranchOnlyInitAndDisabled();
  if (failures != 0) {
    std::printf("S09 mem2reg failures: %d\n", failures);
    return 1;
  }
  std::puts("S09 mem2reg tests passed");
  return 0;
}
