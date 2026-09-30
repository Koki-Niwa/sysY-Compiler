// S08：通过公开 FlatVerifier 边界检查手工构造的平面 IR。
//
// 这个文件只负责造 fixture、调用 verifyFlatModule、记录首个生产诊断。
// CFG、支配、use-def、phi 和 opcode 规则全部由生产检查器执行。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ir/FlatVerifier.h"

namespace {

using sysy::SourceLoc;
using namespace sysy::flat;

const SourceLoc L(1, 1);

Instruction* own(Module& m, Instruction* i) {
  m.ownInst(i);
  return i;
}

Constant* cconst(Module& m, int value) {
  return m.getIntConst(typePool().i32(), value);
}

Function* fn(Module& m, const char* name) {
  Function* f = m.createFunction(name, typePool().i32());
  m.createBlock(f);
  return f;
}

void add(BasicBlock* b, Instruction* i) { b->addInst(i); }

void finish(Module& m) {
  m.rebuildCFG();
  m.rebuildUseDef();
}

struct Diamond {
  Function* f;
  BasicBlock* entry;
  BasicBlock* left;
  BasicBlock* right;
  BasicBlock* join;
  Constant* zero;
  Constant* one;
};

Diamond makeDiamond(Module& m) {
  Diamond d;
  d.f = fn(m, "diamond");
  d.entry = d.f->entry();
  d.left = m.createBlock(d.f);
  d.right = m.createBlock(d.f);
  d.join = m.createBlock(d.f);
  d.zero = cconst(m, 0);
  d.one = cconst(m, 1);
  add(d.entry, own(m, createCondBr(d.one, d.left, d.right, L)));
  add(d.left, own(m, createBr(d.join, L)));
  add(d.right, own(m, createBr(d.join, L)));
  return d;
}

std::unique_ptr<Module> validBranch() {
  std::unique_ptr<Module> m(new Module());
  Diamond d = makeDiamond(*m);
  add(d.join, own(*m, createRet(d.zero, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validPhi() {
  std::unique_ptr<Module> m(new Module());
  Diamond d = makeDiamond(*m);
  Instruction* p = own(*m, createPhi(typePool().i32(), {d.zero, d.one},
                                      {d.left, d.right}, L));
  add(d.join, p);
  add(d.join, own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validLoop() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "loop");
  BasicBlock* pre = f->entry();
  BasicBlock* head = m->createBlock(f);
  BasicBlock* body = m->createBlock(f);
  BasicBlock* exit = m->createBlock(f);
  Constant* zero = cconst(*m, 0);
  Instruction* next = own(*m, createBin(Opcode::Add, zero, zero, L));
  add(body, next);
  add(pre, own(*m, createBr(head, L)));
  Instruction* phi = own(*m, createPhi(typePool().i32(), {zero, next},
                                        {pre, body}, L));
  add(head, phi);
  next->setOperand(0, phi);
  next->setOperand(1, cconst(*m, 1));
  Instruction* cond = own(*m, createICmp(IPred::Slt, phi, cconst(*m, 3), L));
  add(head, cond);
  add(head, own(*m, createCondBr(cond, body, exit, L)));
  add(body, own(*m, createBr(head, L)));
  add(exit, own(*m, createRet(phi, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validCall() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "call");
  Constant* one = cconst(*m, 1);
  Instruction* call = own(*m, createCall("callee", typePool().i32(), {one}, L));
  add(f->entry(), call);
  add(f->entry(), own(*m, createRet(call, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validEntryAlloca() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "alloca");
  BasicBlock* e = f->entry();
  Instruction* slot = own(*m, createAlloca(typePool().i32(), L));
  add(e, slot);
  add(e, own(*m, createStore(typePool().i32(), cconst(*m, 7), slot, L)));
  Instruction* load = own(*m, createLoad(typePool().i32(), slot, L));
  add(e, load);
  add(e, own(*m, createRet(load, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validParamUse() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "param");
  Instruction* p = own(*m, createParam(typePool().i32(), L));
  f->addParam(p);
  add(f->entry(), own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> validUnreachableComponent() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "disconnected");
  BasicBlock* entry = f->entry();
  BasicBlock* dead0 = m->createBlock(f);
  BasicBlock* dead1 = m->createBlock(f);
  add(entry, own(*m, createRet(cconst(*m, 0), L)));
  add(dead0, own(*m, createBr(dead1, L)));
  add(dead1, own(*m, createRet(cconst(*m, 1), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badNullOperand() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_null");
  Instruction* addi = own(*m, createBin(Opcode::Add, nullptr, cconst(*m, 1), L));
  add(f->entry(), addi);
  add(f->entry(), own(*m, createRet(addi, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badMissingUseDef() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_missing_use");
  Instruction* addi = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 2), L));
  add(f->entry(), addi);
  add(f->entry(), own(*m, createRet(addi, L)));
  // Deliberately omit rebuildUseDef: the public verifier must inspect both directions.
  m->rebuildCFG();
  return m;
}

std::unique_ptr<Module> badStaleUseDef() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_stale_use");
  Instruction* def = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 1), L));
  add(f->entry(), def);
  Instruction* addi = own(*m, createBin(Opcode::Add, def, cconst(*m, 2), L));
  add(f->entry(), addi);
  Instruction* ret = own(*m, createRet(addi, L));
  add(f->entry(), ret);
  finish(*m);
  def->addUser(ret);  // ret uses addi, so this reverse edge is stale.
  return m;
}

std::unique_ptr<Module> badDuplicateUseDef() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_duplicate_use");
  Instruction* def = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 2), L));
  add(f->entry(), def);
  Instruction* use = own(*m, createBin(Opcode::Add, def, def, L));
  add(f->entry(), use);
  add(f->entry(), own(*m, createRet(use, L)));
  finish(*m);
  def->removeUser(use);  // leave one reverse edge for two operand occurrences.
  return m;
}

std::unique_ptr<Module> badOrphanDefinition() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_orphan");
  Instruction* orphan = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 2), L));
  Instruction* addi = own(*m, createBin(Opcode::Add, orphan, cconst(*m, 3), L));
  add(f->entry(), addi);
  add(f->entry(), own(*m, createRet(addi, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badCrossFunctionUse() {
  std::unique_ptr<Module> m(new Module());
  Function* first = fn(*m, "first");
  Function* second = fn(*m, "second");
  Instruction* value = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 2), L));
  add(first->entry(), value);
  add(first->entry(), own(*m, createRet(value, L)));
  Instruction* use = own(*m, createBin(Opcode::Add, value, cconst(*m, 3), L));
  add(second->entry(), use);
  add(second->entry(), own(*m, createRet(use, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badNotDominatedUse() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_nondom");
  BasicBlock* entry = f->entry();
  BasicBlock* define = m->createBlock(f);
  BasicBlock* skip = m->createBlock(f);
  BasicBlock* join = m->createBlock(f);
  Constant* one = cconst(*m, 1);
  add(entry, own(*m, createCondBr(one, define, skip, L)));
  Instruction* value = own(*m, createBin(Opcode::Add, one, one, L));
  add(define, value);
  add(define, own(*m, createBr(join, L)));
  add(skip, own(*m, createBr(join, L)));
  add(join, own(*m, createRet(value, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badParamMissingReverseUse() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_param_use_def");
  Instruction* p = own(*m, createParam(typePool().i32(), L));
  f->addParam(p);
  add(f->entry(), own(*m, createRet(p, L)));
  // Deliberately leave the parameter's reverse use list empty.
  m->rebuildCFG();
  return m;
}

std::unique_ptr<Module> badUnregisteredParamUse() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_unregistered_param");
  Instruction* p = own(*m, createParam(typePool().i32(), L));
  add(f->entry(), own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badCrossFunctionParamOwner() {
  std::unique_ptr<Module> m(new Module());
  Function* owner = fn(*m, "param_owner");
  Function* borrower = fn(*m, "param_borrower");
  Instruction* p = own(*m, createParam(typePool().i32(), L));
  owner->addParam(p);
  borrower->addParam(p);  // first owner remains fixed; this second registration is invalid.
  add(owner->entry(), own(*m, createRet(cconst(*m, 0), L)));
  add(borrower->entry(), own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badBranchForeignTarget() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_foreign_branch");
  Function* other = fn(*m, "other");
  add(f->entry(), own(*m, createBr(other->entry(), L)));
  add(other->entry(), own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badUseBeforeDefinition() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_order");
  Constant* one = cconst(*m, 1);
  Instruction* late = own(*m, createBin(Opcode::Add, one, one, L));
  Instruction* use = own(*m, createBin(Opcode::Add, late, one, L));
  add(f->entry(), use);
  add(f->entry(), late);
  add(f->entry(), own(*m, createRet(use, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badSelfUse() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_self");
  Instruction* self = own(*m, createBin(Opcode::Add, cconst(*m, 1), cconst(*m, 2), L));
  self->setOperand(0, self);
  add(f->entry(), self);
  add(f->entry(), own(*m, createRet(self, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badTerminatorMiddle() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_term_middle");
  BasicBlock* entry = f->entry();
  add(entry, own(*m, createRet(cconst(*m, 0), L)));
  add(entry, own(*m, createAlloca(typePool().i32(), L)));
  add(entry, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badMissingTerminator() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_no_term");
  add(f->entry(), own(*m, createAlloca(typePool().i32(), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badBranchArity() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_br_arity");
  BasicBlock* entry = f->entry();
  BasicBlock* yes = m->createBlock(f);
  BasicBlock* no = m->createBlock(f);
  Instruction* br = own(*m, createCondBr(cconst(*m, 1), yes, no, L));
  br->clearOperands();
  add(entry, br);
  add(yes, own(*m, createRet(cconst(*m, 1), L)));
  add(no, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badUnconditionalBranchOperand() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_br_operand");
  BasicBlock* exit = m->createBlock(f);
  Instruction* br = own(*m, createBr(exit, L));
  br->addOperand(cconst(*m, 1));
  add(f->entry(), br);
  add(exit, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badRetSuccessor() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_ret_successor");
  BasicBlock* dead = m->createBlock(f);
  Instruction* ret = own(*m, createRet(cconst(*m, 0), L));
  ret->addSucc(dead);
  add(f->entry(), ret);
  add(dead, own(*m, createRet(cconst(*m, 1), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badUnreachableShape() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_unreachable_shape");
  BasicBlock* dead = m->createBlock(f);
  Instruction* unreachable = own(*m, createInst(Opcode::Unreachable, nullptr, L));
  unreachable->addOperand(cconst(*m, 1));
  unreachable->addSucc(dead);
  add(f->entry(), unreachable);
  add(dead, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badAllocaPlacement() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_alloca");
  BasicBlock* entry = f->entry();
  BasicBlock* body = m->createBlock(f);
  add(entry, own(*m, createBr(body, L)));
  add(body, own(*m, createAlloca(typePool().i32(), L)));
  add(body, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badOpcode() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_opcode");
  Instruction* weird = own(*m, m->createInst(static_cast<Opcode>(255), typePool().i32(), L));
  add(f->entry(), weird);
  add(f->entry(), own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badResultType() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_type");
  Instruction* addi = own(*m, m->createInst(Opcode::Add, typePool().f32(), L));
  addi->addOperand(cconst(*m, 1));
  addi->addOperand(cconst(*m, 2));
  add(f->entry(), addi);
  add(f->entry(), own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badPhiNotFirst() {
  std::unique_ptr<Module> m(new Module());
  Diamond d = makeDiamond(*m);
  add(d.join, own(*m, createCall("side_effect", nullptr, {}, L)));
  Instruction* p = own(*m, createPhi(typePool().i32(), {d.zero, d.one},
                                      {d.left, d.right}, L));
  add(d.join, p);
  add(d.join, own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badPhiPredSet() {
  std::unique_ptr<Module> m(new Module());
  Diamond d = makeDiamond(*m);
  Instruction* p = own(*m, createPhi(typePool().i32(), {d.zero, d.one},
                                      {d.left, d.left}, L));
  add(d.join, p);
  add(d.join, own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badPhiArity() {
  std::unique_ptr<Module> m(new Module());
  Diamond d = makeDiamond(*m);
  Instruction* p = own(*m, createPhi(typePool().i32(), {d.zero}, {d.left}, L));
  add(d.join, p);
  add(d.join, own(*m, createRet(p, L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badPhiSelfOnly() {
  std::unique_ptr<Module> m(new Module());
  Function* f = fn(*m, "bad_phi_self");
  BasicBlock* pre = f->entry();
  BasicBlock* head = m->createBlock(f);
  BasicBlock* body = m->createBlock(f);
  BasicBlock* exit = m->createBlock(f);
  Instruction* p = own(*m, createPhi(typePool().i32(), {nullptr, nullptr},
                                      {pre, body}, L));
  p->setOperand(0, p);
  p->setOperand(1, p);
  add(pre, own(*m, createBr(head, L)));
  add(head, p);
  Instruction* cond = own(*m, createICmp(IPred::Slt, cconst(*m, 0), cconst(*m, 1), L));
  add(head, cond);
  add(head, own(*m, createCondBr(cond, body, exit, L)));
  add(body, own(*m, createBr(head, L)));
  add(exit, own(*m, createRet(cconst(*m, 0), L)));
  finish(*m);
  return m;
}

std::unique_ptr<Module> badPhiNullPred() {
  std::unique_ptr<Module> m = validPhi();
  Function* f = m->function(0);
  f->at(3)->addPred(nullptr);
  return m;
}

struct MatrixCase {
  const char* name;
  const char* expected;
  std::unique_ptr<Module> (*build)();
};

const char* firstInvariant(const std::vector<Violation>& vs) {
  return vs.empty() ? "ACCEPT" : vs.front().invariant.c_str();
}

int runCase(const MatrixCase& c) {
  std::unique_ptr<Module> m = c.build();
  const std::vector<Violation> vs = verifyFlatModule(*m);
  const bool expectedAccept = std::string(c.expected) == "ACCEPT";
  const bool passed = expectedAccept ? vs.empty() :
      (!vs.empty() && vs.front().invariant == c.expected);
  std::printf("MATRIX %-32s expected=%-6s actual=%-6s %s\n", c.name, c.expected,
              firstInvariant(vs), passed ? "PASS" : "FAIL");
  if (!passed) std::printf("%s", formatViolations(vs).c_str());
  return passed ? 0 : 1;
}

int runCrossModuleParamCase() {
  std::unique_ptr<Module> source(new Module());
  Function* sourceFunction = fn(*source, "foreign_param_owner");
  Instruction* p = own(*source, createParam(typePool().i32(), L));
  sourceFunction->addParam(p);
  add(sourceFunction->entry(), own(*source, createRet(cconst(*source, 0), L)));
  finish(*source);

  std::unique_ptr<Module> target(new Module());
  Function* targetFunction = fn(*target, "foreign_param_user");
  targetFunction->addParam(p);  // borrowed, not owned by target; source stays alive for the check.
  add(targetFunction->entry(), own(*target, createRet(p, L)));
  finish(*target);

  const std::vector<Violation> vs = verifyFlatModule(*target);
  const bool passed = !vs.empty() && vs.front().invariant == "V1";
  std::printf("MATRIX %-32s expected=V1     actual=%-6s %s\n",
              "param_cross_module_owner", firstInvariant(vs), passed ? "PASS" : "FAIL");
  if (!passed) std::printf("%s", formatViolations(vs).c_str());
  return passed ? 0 : 1;
}

struct LegacyMap {
  const char* name;
  const char* boundary;
  const char* first;
  const char* replacement;
  const char* overlap;
};

// Recorded from the existing check_flat.py BAD_CASES through `--from-flat`.
// Cases marked read never reach verifyFlatModule; the text reader rejects them.
const LegacyMap kLegacy[] = {
    {"未支配的使用", "reader", "E-FLAT-READ:条件 br 的分支无法解析", "ssa_use_not_dominated", "read masks intended V2"},
    {"φ 入值少一个", "verifier", "V4:终结符不是最后一条", "phi_predecessor_count", "V4 masks V3 count/set"},
    {"两个终结符", "verifier", "V4:终结符不是最后一条", "terminator_middle", "V4 only"},
    {"alloca 在循环里", "verifier", "V4:终结符不是最后一条", "alloca_non_entry", "V4 masks intended V5"},
    {"发明的 opcode", "reader", "E-FLAT-READ:指令出现在任何标签之前", "opcode_unknown", "read masks intended V6"},
    {"引用了不存在的标签", "reader", "E-FLAT-READ:指令出现在任何标签之前", "branch_foreign_target", "read masks intended V4"},
    {"φ 不在块首", "verifier", "V4:终结符不是最后一条", "phi_not_at_block_start", "V4 masks intended V3 position"},
    {"φ 前驱集合不匹配", "verifier", "V4:终结符不是最后一条", "phi_predecessor_set", "V4 masks V3 count"},
};

int printLegacyMap() {
  std::puts("LEGACY BAD_CASES map (observed public boundary)");
  int failures = 0;
  for (const LegacyMap& c : kLegacy) {
    const bool complete = c.name[0] != '\0' && c.boundary[0] != '\0' &&
                          c.first[0] != '\0' && c.replacement[0] != '\0';
    std::printf("LEGACY %-24s boundary=%-8s replacement=%-26s first=%s overlap=%s %s\n",
                c.name, c.boundary, c.replacement, c.first, c.overlap,
                complete ? "PASS" : "FAIL");
    if (!complete) ++failures;
  }
  return failures;
}

}  // namespace

int main() {
  const MatrixCase cases[] = {
      {"valid_branch", "ACCEPT", validBranch},
      {"valid_loop_phi", "ACCEPT", validLoop},
      {"valid_phi", "ACCEPT", validPhi},
      {"valid_call", "ACCEPT", validCall},
      {"valid_entry_alloca", "ACCEPT", validEntryAlloca},
      {"valid_param_use", "ACCEPT", validParamUse},
      {"valid_unreachable_component", "ACCEPT", validUnreachableComponent},
      {"negative_control_null_operand", "V1", badNullOperand},
      {"use_def_missing_reverse", "V1", badMissingUseDef},
      {"use_def_stale_reverse", "V1", badStaleUseDef},
      {"use_def_duplicate_reverse", "V1", badDuplicateUseDef},
      {"use_def_orphan_definition", "V1", badOrphanDefinition},
      {"use_def_cross_function", "V1", badCrossFunctionUse},
      {"use_def_param_missing_reverse", "V1", badParamMissingReverseUse},
      {"param_not_registered_in_function", "V1", badUnregisteredParamUse},
      {"param_cross_function_owner", "V1", badCrossFunctionParamOwner},
      {"ssa_use_not_dominated", "V2", badNotDominatedUse},
      {"ssa_same_block_order", "V2", badUseBeforeDefinition},
      {"ssa_self_use", "V2", badSelfUse},
      {"branch_foreign_target", "V4", badBranchForeignTarget},
      {"terminator_middle", "V4", badTerminatorMiddle},
      {"terminator_missing", "V4", badMissingTerminator},
      {"terminator_bad_condition_arity", "V4", badBranchArity},
      {"terminator_unconditional_has_operand", "V4", badUnconditionalBranchOperand},
      {"terminator_ret_has_successor", "V4", badRetSuccessor},
      {"terminator_unreachable_shape", "V4", badUnreachableShape},
      {"alloca_non_entry", "V5", badAllocaPlacement},
      {"opcode_unknown", "V6", badOpcode},
      {"opcode_result_type", "V6", badResultType},
      {"phi_not_at_block_start", "V3", badPhiNotFirst},
      {"phi_predecessor_set", "V3", badPhiPredSet},
      {"phi_predecessor_count", "V3", badPhiArity},
      {"phi_all_self", "V3", badPhiSelfOnly},
      {"phi_null_cached_predecessor", "V3", badPhiNullPred},
  };
  int failures = printLegacyMap();
  size_t invalidCount = 0;
  size_t positiveCount = 0;
  for (const MatrixCase& c : cases) failures += runCase(c);
  failures += runCrossModuleParamCase();
  ++invalidCount;
  for (const MatrixCase& c : cases) {
    if (std::string(c.expected) == "ACCEPT") ++positiveCount;
    else ++invalidCount;
  }
  if (failures == 0 && invalidCount >= 12 && positiveCount >= 6 &&
      sizeof(kLegacy) / sizeof(kLegacy[0]) == 8) {
    std::printf("S08 FlatVerifier API matrix PASS: invalid=%zu positive=%zu legacy=8\n",
                invalidCount, positiveCount);
  } else if (failures == 0) {
    std::puts("S08 FlatVerifier API matrix FAIL: acceptance thresholds not met");
    ++failures;
  }
  std::printf("S08 matrix: %zu cases, %d failures\n",
              sizeof(cases) / sizeof(cases[0]), failures);
  return failures == 0 ? 0 : 1;
}
