// S20: SCEV, LICM, power-of-two strength reduction and AddressPlan.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <random>
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
int64_t evaluate(const Value* value, int32_t input,
                 std::unordered_map<const Value*, int64_t>& memo) {
  if (value == nullptr) return 0;
  if (value->isConstant()) return static_cast<const Constant*>(value)->intVal();
  if (value->isArgument()) return input;
  const auto it = memo.find(value);
  if (it != memo.end()) return it->second;
  const auto* inst = static_cast<const Instruction*>(value);
  auto operand = [&](size_t i) { return evaluate(inst->operand(i), input, memo); };
  const uint32_t a = static_cast<uint32_t>(operand(0));
  const uint32_t b = inst->numOperands() > 1 ? static_cast<uint32_t>(operand(1)) : 0;
  int64_t result = 0;
  switch (inst->op()) {
    case Opcode::And: result = static_cast<int32_t>(a & b); break;
    case Opcode::Xor: result = static_cast<int32_t>(a ^ b); break;
    case Opcode::Add: result = static_cast<int32_t>(a + b); break;
    case Opcode::Sub: result = static_cast<int32_t>(a - b); break;
    case Opcode::Mul: result = static_cast<int32_t>(a * b); break;
    case Opcode::AShr: result = static_cast<int32_t>(a) >> (b & 31); break;
    case Opcode::LShr: result = static_cast<int32_t>(a >> (b & 31)); break;
    case Opcode::ICmp: {
      const int32_t left = static_cast<int32_t>(a), right = static_cast<int32_t>(b);
      bool value = false;
      switch (inst->ipred()) {
        case IPred::Slt: value = left < right; break;
        case IPred::Sge: value = left >= right; break;
        case IPred::Eq: value = left == right; break;
        default: break;
      }
      result = value ? 1 : 0;
      break;
    }
    case Opcode::Select: {
      const int32_t cond = static_cast<int32_t>(operand(0));
      result = evaluate(inst->operand(cond != 0 ? 1 : 2), input, memo);
      break;
    }
    default: result = 0; break;
  }
  memo[value] = result;
  return result;
}
void testMagicDivision() {
  const int64_t divisors[] = {3, 5, 7, 10, 1000, -3, -7};
  std::mt19937 rng(7);
  for (int64_t divisor : divisors) {
    Module m; const Type* i32 = typePool().i32();
    Function* f = m.createFunction("magic", i32);
    BasicBlock* entry = m.createBlock(f);
    Argument* x = m.createArgument(i32, {});
    f->addParam(x);
    Instruction* div = add(m, entry, createBin(Opcode::SDiv, x,
                                               m.getIntConst(i32, divisor), {}));
    add(m, entry, createRet(div, {}));
    m.rebuildCFG(); m.rebuildUseDef();
    FlatPassManager pm; pm.addPass(std::make_unique<StrengthReducePass>());
    const auto r = pm.run(m);
    bool ok = r.ok() && r.changed && verifyFlatModule(m).empty();
    bool hasDiv = false;
    for (BasicBlock* block : f->blocks())
      for (Instruction* inst : block->insts())
        hasDiv = hasDiv || inst->op() == Opcode::SDiv;
    ok = ok && !hasDiv;
    std::vector<int32_t> values = {INT32_MIN, INT32_MIN + 1, -1000000, -1, 0, 1,
                                   INT32_MAX};
    for (int i = 0; i < 2000; ++i)
      values.push_back(static_cast<int32_t>(rng()));
    const Instruction* ret = f->entry()->terminator();
    for (int32_t input : values) {
      std::unordered_map<const Value*, int64_t> memo;
      const int64_t got = evaluate(ret->operand(0), input, memo);
      const int64_t expected = input / static_cast<int32_t>(divisor);
      if (got != expected) {
        std::printf("  d=%lld x=%d got=%lld expected=%lld\n",
                    (long long)divisor, input, (long long)got, (long long)expected);
        ok = false;
        break;
      }
    }
    check(ok, "magic division matches truncating signed division");
  }
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
  // Two legal targets with opposite relative costs make opposite choices.
  const CostVector armIndexed{4.0, 0.0}, armPointer{1.0, 0.0};
  const CostVector rvIndexed{1.0, 0.0}, rvPointer{3.0, 0.0};
  check(chooseAddressPlan(armIndexed, armPointer, facts).strategy ==
                AddressStrategy::PointerInduction &&
            chooseAddressPlan(rvIndexed, rvPointer, facts).strategy ==
                AddressStrategy::Indexed,
        "different target costs select different address strategies");
}
}  // namespace
int main() {
  testSCEV();
  testLICM();
  testStrengthReduce();
  testMagicDivision();
  testAddressPlan();
  std::printf("S20_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
