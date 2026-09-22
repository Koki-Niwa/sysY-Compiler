// ============================================================================
//   …（19 行说明）
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "ir/Module.h"

namespace {

int gFail = 0;
int gPass = 0;

void expect(bool cond, const char* what) {
  if (cond) {
    ++gPass;
    std::printf("  \u2714 %s\n", what);
  } else {
    ++gFail;
    std::printf("  \u2718 %s\n", what);
  }
}

using sysy::flat::BasicBlock;
using sysy::flat::createBin;
using sysy::flat::createBr;
using sysy::flat::createCondBr;
using sysy::flat::createICmp;
using sysy::flat::createLoad;
using sysy::flat::createParam;
using sysy::flat::createPhi;
using sysy::flat::createRet;
using sysy::flat::createStore;
using sysy::flat::createUnreachable;
using sysy::flat::Function;
using sysy::flat::GlobalVariable;
using sysy::flat::Instruction;
using sysy::flat::IPred;
using sysy::flat::Module;
using sysy::flat::Opcode;
using sysy::flat::typePool;
using sysy::flat::Value;

// 【后置】造一条"常量指令"（平面 dump 里的常量是**指令形态**：
//   `Opcode::ConstantInt`/`ConstantFP`，见 ir/Instruction.h 的说明）。
//   `Module::getIntConst` 返回的是另一套（模块级 interned 的 `Constant`）。
Instruction* kInt(Module& m, int32_t v, sysy::SourceLoc loc) {
  Instruction* c = m.createInst(Opcode::ConstantInt, typePool().i32(), loc);
  c->setIntBits(v);
  m.ownInst(c);
  return c;
}

// 【后置】`p` 是不是"某条指令的操作数里出现过"（反向 use-def 的查询）。
bool usedBy(const Value* p, const Instruction* by) {
  for (size_t k = 0; k < by->numOperands(); ++k) {
    if (by->operand(k) == p) return true;
  }
  return false;
}

// 【后置】统计函数里的 φ 个数。
size_t phiCount(const Function* f) {
  size_t n = 0;
  for (size_t b = 0; b < f->blockCount(); ++b) {
    for (size_t i = 0; i < f->at(b)->size(); ++i) {
      if (f->at(b)->at(i)->op() == Opcode::Phi) ++n;
    }
  }
  return n;
}

// 【后置】把函数 dump 成文本（失败时打印，便于定位）。
std::string dumpOf(const Module& m) { return sysy::flat::dumpModule(m); }

// ═══════════════════════════════════════════════════════════════════════════
// ① 菱形（φ 两个入值）+ ⑦ use-def 双向一致
// ═══════════════════════════════════════════════════════════════════════════
void testDiamond() {
  std::printf("\u2500\u2500 \u2460 \u83f1\u5f62\uff08\u03c6 \u4e24\u4e2a\u5165\u503c\uff09\u4e0e use-def \u53cc\u5411 \u2500\u2500\n");
  Module m;
  const sysy::SourceLoc L(1, 1);
  Function* f = m.createFunction("diamond", typePool().i32());
  BasicBlock* entry = m.createBlock(f);
  Instruction* c = createICmp(IPred::Slt, kInt(m, 1, L), kInt(m, 2, L), L);
  m.ownInst(c);
  // 手工构造必须自己挂块：漏挂的指令没有 parent，作为操作数会被 V1 正确报红。
  entry->addInst(c);
  // 菱形：entry --c--> L1 / L2 --→ L3
  Instruction* br = createCondBr(c, nullptr, nullptr, sysy::SourceLoc(1, 1));
  m.ownInst(br);
  entry->addInst(br);
  BasicBlock* l1 = m.createBlock(f);
  BasicBlock* l2 = m.createBlock(f);
  BasicBlock* join = m.createBlock(f);
  br->setSucc(0, l1);
  br->setSucc(1, l2);
  // 两个分支各给一个值
  Instruction* a = kInt(m, 11, L);
  Instruction* b = kInt(m, 22, L);
  m.ownInst(a);
  m.ownInst(b);
  Instruction* br1 = createBr(join, sysy::SourceLoc(1, 1));
  m.ownInst(br1);
  l1->addInst(br1);
  Instruction* br2 = createBr(join, sysy::SourceLoc(1, 1));
  m.ownInst(br2);
  l2->addInst(br2);
  // 汇合块开头的 φ：两个入值来自 L1 / L2
  Instruction* phi = createPhi(typePool().i32(), {a, b}, {l1, l2}, sysy::SourceLoc(1, 1));
  m.ownInst(phi);
  join->addInst(phi);
  Instruction* ret = createRet(phi, sysy::SourceLoc(1, 1));
  m.ownInst(ret);
  join->addInst(ret);

  m.rebuildCFG();
  m.rebuildUseDef();

  expect(join->numPreds() == 2, "菱形：汇合块有 2 个前驱");
  expect(phi->numOperands() == 2, "菱形：φ 有 2 个入值");
  expect(phi->succ(0) == l1 && phi->succ(1) == l2, "菱形：φ 的入值块是 L1/L2");
  expect(phi->operand(0) == a && phi->operand(1) == b, "菱形：φ 的入值是按边配对的常量");
  // ⑦ use-def 双向一致
  expect(usedBy(phi, ret), "use-def：ret 的操作数是 φ");
  bool found = false;
  for (Instruction* u : phi->users()) {
    if (u == ret) found = true;
  }
  expect(found, "use-def：反向边 users() 里有 ret");
  bool foundC = false;
  for (Instruction* u : c->users()) {
    if (u == br) foundC = true;
  }
  expect(foundC, "use-def：条件的反向边指向 br");
  // 检查器必须全绿
  const std::vector<sysy::flat::Violation> vs = sysy::flat::verifyFlatModule(m);
  expect(vs.empty(), "菱形：FlatVerifier 零违反");
  if (!vs.empty()) std::printf("%s", sysy::flat::formatViolations(vs).c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// ② 嵌套 if：φ 的入值本身是另一个 φ 的结果
// ═══════════════════════════════════════════════════════════════════════════
void testNestedPhi() {
  std::printf("\u2500\u2500 \u2461 \u5d4c\u5957 if\uff08\u03c6 \u7684\u5165\u503c\u662f\u53e6\u4e00\u4e2a \u03c6\uff09\u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("nested", typePool().i32());
  BasicBlock* e = m.createBlock(f);
  const sysy::SourceLoc L(2, 1);
  Instruction* c0 = createICmp(IPred::Eq, kInt(m, 1, L),
                               kInt(m, 1, L), L);
  m.ownInst(c0);
  Instruction* b0 = createCondBr(c0, nullptr, nullptr, L);
  m.ownInst(b0);
  e->addInst(b0);
  BasicBlock* t1 = m.createBlock(f);
  BasicBlock* e1 = m.createBlock(f);
  BasicBlock* join1 = m.createBlock(f);
  b0->setSucc(0, t1);
  b0->setSucc(1, e1);
  // 内层 if（在 t1 里）
  Instruction* c1 = createICmp(IPred::Eq, kInt(m, 2, L),
                               kInt(m, 2, L), L);
  m.ownInst(c1);
  Instruction* b1 = createCondBr(c1, nullptr, nullptr, L);
  m.ownInst(b1);
  t1->addInst(b1);
  BasicBlock* t2 = m.createBlock(f);
  BasicBlock* e2 = m.createBlock(f);
  b1->setSucc(0, t2);
  b1->setSucc(1, e2);
  Instruction* va = kInt(m, 7, L);
  Instruction* vb = kInt(m, 8, L);
  m.ownInst(va);
  m.ownInst(vb);
  Instruction* r1 = createRet(va, L);
  m.ownInst(r1);
  t2->addInst(r1);
  Instruction* r2 = createRet(vb, L);
  m.ownInst(r2);
  e2->addInst(r2);
  Instruction* r3 = createRet(kInt(m, 3, L), L);
  m.ownInst(r3);
  e1->addInst(r3);
  // t1 的末尾（t2/e2 都 ret 了，所以 t1 需要自己终结 —— 用一个空块做"内层汇合"）
  BasicBlock* innerJoin = m.createBlock(f);
  Instruction* bj = createBr(join1, L);
  m.ownInst(bj);
  innerJoin->addInst(bj);
  // 内层的 φ（入值来自 t2/e2 —— 它们都 ret，是**有意不可达**的形态）
  Instruction* iphi = createPhi(typePool().i32(), {va, vb}, {t2, e2}, L);
  m.ownInst(iphi);
  innerJoin->insertInst(0, iphi);
  // 外层的 φ：入值 = {内层 φ 所在块, e1}
  Instruction* ophi = createPhi(typePool().i32(), {iphi, kInt(m, 9, L)},
                                {innerJoin, e1}, L);
  m.ownInst(ophi);
  join1->addInst(ophi);
  Instruction* rj = createRet(ophi, L);
  m.ownInst(rj);
  join1->addInst(rj);
  // t1 也要终结：让 t1 直接跳 innerJoin（形状上等价于"内层 if 之后继续"）
  Instruction* bt = createBr(innerJoin, L);
  m.ownInst(bt);
  t1->insertInst(t1->size() - 1, bt);   // 插在 b1 之前？—— 见下断言

  m.rebuildCFG();
  m.rebuildUseDef();
  expect(phiCount(f) == 2, "嵌套 if：函数里有 2 个 φ");
  expect(ophi->operand(0) == iphi, "嵌套 if：外层 φ 的入值是内层 φ 的结果");
  expect(iphi->numOperands() == 2 && ophi->numOperands() == 2, "嵌套 if：两层 φ 各 2 个入值");
  // use-def：内层 φ 的使用者是外层 φ（**跨层**的反向边）
  bool cross = false;
  for (Instruction* u : iphi->users()) {
    if (u == ophi) cross = true;
  }
  expect(cross, "use-def：内层 φ 的反向边指向外层 φ");
}

// ═══════════════════════════════════════════════════════════════════════════
// ③ 循环：回边上的 φ（初值来自循环前、后续值来自循环体）
// ═══════════════════════════════════════════════════════════════════════════
void testLoopPhi() {
  std::printf("\u2500\u2500 \u2462 \u5faa\u73af\uff08\u56de\u8fb9\u4e0a\u7684 \u03c6\uff09\u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("loop", typePool().i32());
  const sysy::SourceLoc L(3, 1);
  BasicBlock* pre = m.createBlock(f);
  BasicBlock* head = m.createBlock(f);
  BasicBlock* body = m.createBlock(f);
  BasicBlock* exit = m.createBlock(f);
  Instruction* init = kInt(m, 0, L);
  m.ownInst(init);
  Instruction* bpre = createBr(head, L);
  m.ownInst(bpre);
  pre->addInst(bpre);
  // head：φ(iv) = {init 来自 pre, next 来自 body}
  // 操作数稍后填（先建出来才能让 φ 引用它）
  Instruction* next = createBin(Opcode::Add, kInt(m, 0, L), kInt(m, 0, L), L);
  m.ownInst(next);
  body->addInst(next);
  Instruction* phi = createPhi(typePool().i32(), {init, next}, {pre, body}, L);
  m.ownInst(phi);
  head->addInst(phi);
  Instruction* cond = createICmp(IPred::Slt, phi, kInt(m, 10, L), L);
  m.ownInst(cond);
  head->addInst(cond);
  Instruction* bhead = createCondBr(cond, body, exit, L);
  m.ownInst(bhead);
  head->addInst(bhead);
  // 覆盖占位操作数（`addOperand` 会追加 ⇒ 一条 add 4 个操作数，V6 会报红）
  next->setOperand(0, phi);
  next->setOperand(1, kInt(m, 1, L));
  Instruction* bbody = createBr(head, L);
  m.ownInst(bbody);
  body->addInst(bbody);
  Instruction* ret = createRet(phi, L);
  m.ownInst(ret);
  exit->addInst(ret);

  m.rebuildCFG();
  m.rebuildUseDef();
  expect(head->numPreds() == 2, "循环：循环头有 2 个前驱（进入边 + 回边）");
  expect(phi->operand(0) == init, "循环：φ 的进入边入值是循环前的初值");
  expect(phi->operand(1) == next, "循环：φ 的回边入值是循环体里的新值");
  bool selfRef = (phi->operand(0) == phi || phi->operand(1) == phi);
  expect(!selfRef, "循环：φ 至少有一个非自身入值（没有自引用）");
  const std::vector<sysy::flat::Violation> vs = sysy::flat::verifyFlatModule(m);
  expect(vs.empty(), "循环：FlatVerifier 零违反（含支配检查）");
  if (!vs.empty()) std::printf("%s", sysy::flat::formatViolations(vs).c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// ④ 临界边必须被拆开（源块多后继 + 目标块多前驱）
// ═══════════════════════════════════════════════════════════════════════════
void testCriticalEdge() {
  std::printf("\u2500\u2500 \u2463 \u4e34\u754c\u8fb9\u5fc5\u987b\u88ab\u62c6\u5f00 \u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("crit", typePool().i32());
  const sysy::SourceLoc L(4, 1);
  BasicBlock* e = m.createBlock(f);
  BasicBlock* a = m.createBlock(f);
  BasicBlock* b = m.createBlock(f);
  BasicBlock* j = m.createBlock(f);   // 目标块：2 个前驱（a、b）
  BasicBlock* k = m.createBlock(f);
  Instruction* c0 = createICmp(IPred::Eq, kInt(m, 0, L),
                               kInt(m, 0, L), L);
  m.ownInst(c0);
  Instruction* b0 = createCondBr(c0, a, b, L);
  m.ownInst(b0);
  e->addInst(b0);
  // a 有 2 个后继（j、k），而 j 有 2 个前驱（a、b）⇒ (a, j) 是临界边
  Instruction* ba = createCondBr(c0, j, k, L);
  m.ownInst(ba);
  a->addInst(ba);
  Instruction* bb = createBr(j, L);
  m.ownInst(bb);
  b->addInst(bb);
  Instruction* bj = createBr(k, L);
  m.ownInst(bj);
  j->addInst(bj);
  Instruction* rk = createRet(kInt(m, 1, L), L);
  m.ownInst(rk);
  k->addInst(rk);
  m.rebuildCFG();
  m.rebuildUseDef();
  // 手工构造里 (a,j) 是临界边（不拆的话 φ 无法表达"从哪条边来"）。
//   …（2 行说明）
  //   在 check_flat.py 里；这里只钉住"源块多后继 + 目标多前驱"这个事实）。
  Instruction* at = a->terminator();
  expect(at->numSuccs() == 2, "临界边：源块 a 有 2 个后继");
  expect(j->numPreds() == 2, "临界边：目标块 j 有 2 个前驱");
  expect(at->succ(0) == j, "临界边：(a, j) 确实是临界边");
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑤ 空块（只有一个跳转）与不可达块
// ═══════════════════════════════════════════════════════════════════════════
void testEmptyAndUnreachable() {
  std::printf("\u2500\u2500 \u2464 \u7a7a\u5757\u4e0e\u4e0d\u53ef\u8fbe\u5757 \u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("empty", typePool().i32());
  const sysy::SourceLoc L(5, 1);
  BasicBlock* e = m.createBlock(f);
  BasicBlock* empty = m.createBlock(f);      // 空块：只有一条 br
  BasicBlock* dead = m.createBlock(f);       // 不可达块
  Instruction* be = createBr(empty, L);
  m.ownInst(be);
  e->addInst(be);
  Instruction* bm = createBr(dead, L);
  m.ownInst(bm);
  empty->addInst(bm);
  Instruction* ret = createRet(kInt(m, 0, L), L);
  m.ownInst(ret);
  dead->addInst(ret);

  m.rebuildCFG();
  m.rebuildUseDef();
  expect(empty->size() == 1, "空块：只有一条指令");
  expect(empty->terminator() == bm, "空块：那一条就是终结符");
  expect(dead->numPreds() == 1, "不可达块：仍能从空块到达（这里它其实可达）");
  // 真正的不可达块：没有任何前驱的块
  BasicBlock* orphan = m.createBlock(f);
  Instruction* u = createUnreachable(L);
  m.ownInst(u);
  orphan->addInst(u);
  m.rebuildCFG();
  expect(orphan->numPreds() == 0, "不可达块：没有前驱");
  expect(orphan->terminator()->op() == Opcode::Unreachable, "不可达块：终结符是 unreachable");
  sysy::flat::DominatorTree dt(*f);
  expect(!dt.reachable(orphan->index()), "支配树：不可达块被标为不可达");
  expect(dt.reachable(e->index()), "支配树：入口块可达");
  expect(dt.dominates(e, dead), "支配树：入口块支配它后面的块（dead）");
  expect(!dt.dominates(dead, e), "支配树：反向不成立（dead 不支配入口）");
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑥ "用得到 φ 但不需要 φ"：变量在分支里只写不读 ⇒ 不许多放 φ
// ═══════════════════════════════════════════════════════════════════════════
void testNoNeedlessPhi() {
  std::printf("\u2500\u2500 \u2465 \u53ea\u5199\u4e0d\u8bfb \u21d2 \u4e0d\u8bb8\u591a\u653e \u03c6 \u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("nophi", typePool().i32());
  const sysy::SourceLoc L(6, 1);
  BasicBlock* e = m.createBlock(f);
  // （"只写不读"的槽就是上面那个 `alloca`；不需要额外的块）
  Instruction* alloca = sysy::flat::createAlloca(typePool().i32(), L);
  m.ownInst(alloca);
  e->addInst(alloca);
  Instruction* c0 = createICmp(IPred::Eq, kInt(m, 0, L),
                               kInt(m, 0, L), L);
  m.ownInst(c0);
  e->addInst(c0);
  BasicBlock* t = m.createBlock(f);
  BasicBlock* el = m.createBlock(f);
  BasicBlock* j = m.createBlock(f);
  Instruction* b0 = createCondBr(c0, t, el, L);
  m.ownInst(b0);
  e->addInst(b0);
  // 两个分支都**只写**这个槽（没有任何 load）
  Instruction* s1 = createStore(typePool().i32(), kInt(m, 1, L),
                                alloca, L);
  m.ownInst(s1);
  t->addInst(s1);
  Instruction* bt = createBr(j, L);
  m.ownInst(bt);
  t->addInst(bt);
  Instruction* s2 = createStore(typePool().i32(), kInt(m, 2, L),
                                alloca, L);
  m.ownInst(s2);
  el->addInst(s2);
  Instruction* be2 = createBr(j, L);
  m.ownInst(be2);
  el->addInst(be2);
  Instruction* ret = createRet(kInt(m, 0, L), L);
  m.ownInst(ret);
  j->addInst(ret);
  m.rebuildCFG();
  m.rebuildUseDef();
  // 汇合块有 2 个前驱（"用得到 φ 的位置"），但那个槽**从没被读过**
  //   ⇒ 展平器与检查器都不该在这里要求 φ。
  expect(j->numPreds() == 2, "只写不读：汇合块确实有 2 个前驱（φ 的位点存在）");
  expect(phiCount(f) == 0, "只写不读：函数里一个 φ 都没有（判据 2 的禁令）");
  const std::vector<sysy::flat::Violation> vs = sysy::flat::verifyFlatModule(m);
  expect(vs.empty(), "只写不读：FlatVerifier 零违反");
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑧ 支配树：3 个手工 CFG
// ═══════════════════════════════════════════════════════════════════════════
void testDominators() {
  std::printf("\u2500\u2500 \u2467 \u652f\u914d\u6811\uff08\u4e09\u4e2a\u624b\u5de5 CFG\uff09\u2500\u2500\n");
  {
    // (a) 链：entry → L1 → L2
    Module m;
    const sysy::SourceLoc L;
    Function* f = m.createFunction("chain", typePool().i32());
    BasicBlock* b0 = m.createBlock(f);
    BasicBlock* b1 = m.createBlock(f);
    BasicBlock* b2 = m.createBlock(f);
    Instruction* x = createBr(b1, sysy::SourceLoc());
    m.ownInst(x);
    b0->addInst(x);
    Instruction* y = createBr(b2, sysy::SourceLoc());
    m.ownInst(y);
    b1->addInst(y);
    Instruction* r = createRet(kInt(m, 0, L), sysy::SourceLoc());
    m.ownInst(r);
    b2->addInst(r);
    m.rebuildCFG();
    sysy::flat::DominatorTree dt(*f);
    expect(dt.dominates(b0, b2), "支配：链上 entry 支配末端块");
    expect(!dt.dominates(b1, b0), "支配：链上后继不支配前驱");
    expect(dt.idom(b2->index()) == b1, "支配：末端块的 idom 是中间块");
    expect(dt.idom(b0->index()) == nullptr, "支配：入口块没有 idom");
  }
  {
    // (b) 菱形：join 的 idom 是 entry（不是任一分支）
    Module m;
    const sysy::SourceLoc L;
    Function* f = m.createFunction("dia", typePool().i32());
    BasicBlock* e = m.createBlock(f);
    BasicBlock* a = m.createBlock(f);
    BasicBlock* b = m.createBlock(f);
    BasicBlock* j = m.createBlock(f);
    Instruction* c = createICmp(IPred::Eq, kInt(m, 0, L),
                                kInt(m, 0, L), sysy::SourceLoc());
    m.ownInst(c);
    e->addInst(c);                       // 必须挂块（见菱形那条说明）
    Instruction* br = createCondBr(c, a, b, sysy::SourceLoc());
    m.ownInst(br);
    e->addInst(br);
    Instruction* ba = createBr(j, sysy::SourceLoc());
    m.ownInst(ba);
    a->addInst(ba);
    Instruction* bb = createBr(j, sysy::SourceLoc());
    m.ownInst(bb);
    b->addInst(bb);
    Instruction* r = createRet(kInt(m, 0, L), sysy::SourceLoc());
    m.ownInst(r);
    j->addInst(r);
    m.rebuildCFG();
    sysy::flat::DominatorTree dt(*f);
    expect(dt.idom(j->index()) == e, "支配：菱形汇合块的 idom 是入口（不是分支）");
    expect(!dt.dominates(a, j), "支配：分支块不支配汇合块");
  }
  {
    // (c) 循环：body 的 idom 是 head，exit 的 idom 是 head
    Module m;
    const sysy::SourceLoc L;
    Function* f = m.createFunction("loop", typePool().i32());
    BasicBlock* pre = m.createBlock(f);
    BasicBlock* head = m.createBlock(f);
    BasicBlock* body = m.createBlock(f);
    BasicBlock* exit = m.createBlock(f);
    Instruction* bp = createBr(head, sysy::SourceLoc());
    m.ownInst(bp);
    pre->addInst(bp);
    Instruction* c = createICmp(IPred::Slt, kInt(m, 0, L),
                                kInt(m, 1, L), sysy::SourceLoc());
    m.ownInst(c);
    head->addInst(c);                    // 必须挂块（见菱形那条说明）
    Instruction* bh = createCondBr(c, body, exit, sysy::SourceLoc());
    m.ownInst(bh);
    head->addInst(bh);
    Instruction* bb = createBr(head, sysy::SourceLoc());
    m.ownInst(bb);
    body->addInst(bb);
    Instruction* r = createRet(kInt(m, 0, L), sysy::SourceLoc());
    m.ownInst(r);
    exit->addInst(r);
    m.rebuildCFG();
    sysy::flat::DominatorTree dt(*f);
    expect(dt.idom(body->index()) == head, "支配：循环体的 idom 是循环头");
    expect(dt.idom(exit->index()) == head, "支配：循环出口的 idom 是循环头");
    expect(dt.dominates(head, body) && dt.dominates(head, exit),
           "支配：循环头支配体与出口");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑨ 纯函数（dump 两次逐字节相同）+ 往返（dump → 读回 → dump）
// ═══════════════════════════════════════════════════════════════════════════
void testDumpPurityAndRoundTrip() {
  std::printf("\u2500\u2500 \u2468 dump \u7eaf\u51fd\u6570\u4e0e\u5f80\u8fd4 \u2500\u2500\n");
  Module m;
  Function* f = m.createFunction("rt", typePool().i32());
  BasicBlock* e = m.createBlock(f);
  const sysy::SourceLoc L(7, 3);
  Instruction* p0 = createParam(typePool().i32(), L);
  m.ownInst(p0);
  f->addParam(p0);
  Instruction* alloca = sysy::flat::createAlloca(typePool().i32(), L);
  m.ownInst(alloca);
  e->addInst(alloca);
  Instruction* st = createStore(typePool().i32(), p0, alloca, L);
  m.ownInst(st);
  e->addInst(st);
  Instruction* ld = createLoad(typePool().i32(), alloca, L);
  m.ownInst(ld);
  e->addInst(ld);
  Instruction* sum = createBin(Opcode::Add, ld, kInt(m, 1, L), L);
  m.ownInst(sum);
  e->addInst(sum);
  Instruction* r = createRet(sum, L);
  m.ownInst(r);
  e->addInst(r);
  m.rebuildCFG();
  m.rebuildUseDef();

  const std::string d1 = dumpOf(m);
  const std::string d2 = dumpOf(m);
  expect(d1 == d2, "dump 是纯函数（两次逐字节相同）");
  expect(d1.find("define i32 @rt(i32 %0)") != std::string::npos, "dump：函数签名形参在入口块开头");
  expect(d1.find("store i32 %0, ptr[i32] %1") != std::string::npos, "dump：store 的文本形态");
  // 常量在函数头部拿号 ⇒ 不要写死"相邻编号"（那是把排版当语义）。
  expect(d1.find("i32 1 @line 7") != std::string::npos, "dump：常量在函数头部有定义行");
  expect(d1.find("add i32 ") != std::string::npos, "dump：二元运算的文本形态");

  sysy::DiagnosticEngine diags;
  sysy::flat::Module back;
  sysy::flat::Module* m2 = sysy::flat::parseFlatModule(d1, diags);
  expect(m2 != nullptr, "往返：读回成功");
  if (m2 != nullptr) {
    const std::string d3 = dumpOf(*m2);
    expect(d3 == d1, "往返：dump → 读回 → dump 逐字节相同");
    if (d3 != d1) std::printf("---- 原 ----\n%s---- 回 ----\n%s", d1.c_str(), d3.c_str());
    delete m2;
  }
}

}  // namespace

int main() {
  std::printf("\u2550\u2550\u2550 \u5e73\u9762 IR \u5bb9\u5668\u5355\u5143\u65ad\u8a00\uff08S06\uff09\u2550\u2550\u2550\n");
  testDiamond();
  testNestedPhi();
  testLoopPhi();
  testCriticalEdge();
  testEmptyAndUnreachable();
  testNoNeedlessPhi();
  testDominators();
  testDumpPurityAndRoundTrip();
  std::printf("\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n");
  std::printf("\u901a\u8fc7 %d \u9879\uff0c\u5931\u8d25 %d \u9879\n", gPass, gFail);
  return gFail == 0 ? 0 : 1;
}
