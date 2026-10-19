// S18: rewrite DSL patterns, guards and the production identity rules.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ir/FlatVerifier.h"
#include "ir/IRMutator.h"
#include "opt/RewriteEngine.h"
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
struct Case {
  const char* rule;
  Opcode op;
  bool constantFirst;
  int64_t constant;
  IPred pred;
  bool compare;
  int64_t expected;   // -1 means the captured x must be returned
};
void runCase(const Case& c) {
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("r", i32);
  Argument* x = m.createArgument(i32, {});
  BasicBlock* b = m.createBlock(f);
  f->addParam(x);
  Instruction* result = nullptr;
  if (c.compare) {
    result = add(m, b, createICmp(c.pred, x, x, {}));
  } else if (c.rule == std::string("select-same-arms")) {
    result = add(m, b, createSelect(m.getIntConst(i32, 1), x, x, {}));
  } else if (c.rule == std::string("sub-x-x")) {
    result = add(m, b, createBin(Opcode::Sub, x, x, {}));
  } else {
    Value* left = c.constantFirst ? static_cast<Value*>(m.getIntConst(i32, c.constant)) : x;
    Value* right = c.constantFirst ? x : static_cast<Value*>(m.getIntConst(i32, c.constant));
    result = add(m, b, createBin(c.op, left, right, {}));
  }
  add(m, b, createRet(result, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  FlatPassManager manager; manager.addPass(std::make_unique<RewritePass>());
  const auto run = manager.run(m);
  const Instruction* ret = f->entry()->terminator();
  const Value* returned = ret->numOperands() == 1 ? ret->operand(0) : nullptr;
  bool ok = run.ok() && run.changed && verifyFlatModule(m).empty() &&
            returned != nullptr;
  if (ok) {
    if (c.expected < 0) {
      ok = returned == x;
    } else {
      ok = returned->isConstant() &&
           static_cast<const Constant*>(returned)->intVal() == c.expected;
    }
  }
  check(ok, c.rule);
}
void testAllRules() {
  const Case cases[] = {
      {"add-x-0", Opcode::Add, false, 0, IPred::Eq, false, -1},
      {"add-0-x", Opcode::Add, true, 0, IPred::Eq, false, -1},
      {"sub-x-0", Opcode::Sub, false, 0, IPred::Eq, false, -1},
      {"mul-x-1", Opcode::Mul, false, 1, IPred::Eq, false, -1},
      {"mul-1-x", Opcode::Mul, true, 1, IPred::Eq, false, -1},
      {"and-x-allones", Opcode::And, false, -1, IPred::Eq, false, -1},
      {"or-x-0", Opcode::Or, false, 0, IPred::Eq, false, -1},
      {"xor-x-0", Opcode::Xor, false, 0, IPred::Eq, false, -1},
      {"shl-x-0", Opcode::Shl, false, 0, IPred::Eq, false, -1},
      {"lshr-x-0", Opcode::LShr, false, 0, IPred::Eq, false, -1},
      {"ashr-x-0", Opcode::AShr, false, 0, IPred::Eq, false, -1},
      {"mul-x-0", Opcode::Mul, false, 0, IPred::Eq, false, 0},
      {"sub-x-x", Opcode::Sub, false, 0, IPred::Eq, false, 0},
      {"select-same-arms", Opcode::Select, false, 0, IPred::Eq, false, -1},
      {"icmp-eq-self", Opcode::ICmp, false, 0, IPred::Eq, true, 1},
      {"icmp-ne-self", Opcode::ICmp, false, 0, IPred::Ne, true, 0},
      {"icmp-sle-self", Opcode::ICmp, false, 0, IPred::Sle, true, 1},
      {"icmp-sge-self", Opcode::ICmp, false, 0, IPred::Sge, true, 1},
      {"icmp-slt-self", Opcode::ICmp, false, 0, IPred::Slt, true, 0},
      {"icmp-sgt-self", Opcode::ICmp, false, 0, IPred::Sgt, true, 0},
  };
  for (const Case& c : cases) runCase(c);
}
void testGuardAndFixedPoint() {
  // A false guard must block the rewrite and be counted.
  Module m; const Type* i32 = typePool().i32();
  Function* f = m.createFunction("guard", i32);
  Argument* x = m.createArgument(i32, {});
  BasicBlock* b = m.createBlock(f);
  f->addParam(x);
  Instruction* addInst = add(m, b, createBin(Opcode::Add, x,
                                             m.getIntConst(i32, 1), {}));
  add(m, b, createRet(addInst, {}));
  m.rebuildCFG(); m.rebuildUseDef();
  IRMutator mutator(m);
  RewriteEngine engine;
  engine.addRule(RewriteRule{
      "guarded-rule",
      Pattern(Opcode::Add).operand(OperandPattern::capture("x"))
          .operand(OperandPattern::constantInt(1)).intResult(),
      [](const MatchContext&) { return false; },
      [](const MatchContext& match, RewriteContext&) -> Value* {
        return const_cast<Value*>(match.find("x"));
      }});
  RewriteContext rewrite{mutator, m};
  check(!engine.rewrite(addInst, rewrite), "false !only-if guard blocks the rule");
  check(engine.counters().count("guarded-rule.guarded") == 1,
        "guarded rule is counted");
  check(addInst->parent() == b, "guarded instruction remains live");
  // Fixed point: (x * 1) + 0 -> x through two rules.
  Module d; const Type* t = typePool().i32();
  Function* df = d.createFunction("chain", t);
  Argument* y = d.createArgument(t, {});
  BasicBlock* db = d.createBlock(df);
  df->addParam(y);
  Instruction* mul = add(d, db, createBin(Opcode::Mul, y, d.getIntConst(t, 1), {}));
  Instruction* plus = add(d, db, createBin(Opcode::Add, mul, d.getIntConst(t, 0), {}));
  add(d, db, createRet(plus, {}));
  d.rebuildCFG(); d.rebuildUseDef();
  FlatPassManager manager; manager.addPass(std::make_unique<RewritePass>());
  const auto run = manager.run(d);
  check(run.ok() && run.changed && df->entry()->terminator()->operand(0) == y &&
        verifyFlatModule(d).empty(), "rewrite pass reaches a fixed point");
}
}  // namespace
int main() {
  testAllRules();
  testGuardAndFixedPoint();
  std::printf("S18_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
