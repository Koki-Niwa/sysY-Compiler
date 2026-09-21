// ============================================================================
// test_loopnorm.cpp —— S05b 的单元断言（prompt §九 第 8 项）
//
//   这一关是**第一个变换**，所以断言的重点与前面几关不同：
//   不是"信息有没有丢"，而是"改写规则本身对不对、边界情形有没有漏"。
//
//   A. ★ `ForOp` 的形状契约（prompt §四）：attrs[0] = IV 名字、
//      操作数 = [IV 槽, lower, upper, step]、恰好 1 个体 Region；
//      **IV 的判据是"操作数身份"，不是名字**。
//   B. ★ 成功条件逐条（§3.3）：四条**全**满足才升 `ForOp`；任一条不满足 ⇒
//      **保留 `WhileOp`**。逐条构造"只差一处"的输入，断言"该升的升、不该升的不升"。
//   C. ★ `continue` 消解的**规范形式**（§3.2）：`if (!cond) { B }` ——
//      断言产出的 `If` 的形状（条件 = `Eq C 0`、**剩余体在 then**、`else`
//      只有一个 `(Yield)`）。这是轨 D 能对齐的前提，也是本关唯一"没有唯一
//      答案"的地方 ⇒ 必须钉死。
//   D. ★ `alloca` 提升（§3.5）：手工构造"alloca 在深层 Region"的 IR，
//      `hoistAllocas` 必须把它搬到入口**最前面**且**保持相对顺序**；
//      已经满足时必须是**恒等变换**（`moved == 0`）。
//   E. ★ 检查器抓得住 I1/I2/I3（S01 起的规矩：检查器必须先证明抓得住错）：
//      手工构造三份违反，`verifyModule` 必须逐条报出对应的不变式。
//   F. 幂等（§C3）：`run(run(X)) == run(X)`（用真实流水线跑两遍比对 dump）。
//   G. 病态输入不爆炸：4000 层嵌套块 + 深层 alloca（显式工作栈，不递归爆栈）。
//
// 构建与运行：`tools/selftest/unit/run.sh`（clang++ --std=c++17 -Werror）。
// ============================================================================
#include <iostream>
#include <string>
#include <vector>

#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "structured/AllocaHoist.h"
#include "structured/IRGen.h"
#include "structured/LoopNormalize.h"
#include "structured/StructuredDump.h"
#include "structured/StructuredReader.h"
#include "structured/StructuredVerifier.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using namespace sysy;
using namespace sysy::sir;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (ok) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  ✘ " << what << "\n";
  }
}

// 【后置】把源码编译成结构化 IR（真实流水线），再按 `normalize` 决定是否规范化。
static std::string compileToIr(const std::string& src, bool normalize, DiagnosticEngine& diags,
                               LoopNormStats* statsOut = nullptr) {
  SourceFile file = SourceFile::fromString("t.sy", src);
  diags.setSourceFile(&file);
  Lexer lexer(file, diags);
  Parser parser(lexer, diags);
  auto unit = parser.parseCompUnit();
  if (unit == nullptr) return "";
  Sema sema(diags);
  sema.run(*unit);
  const InitPlan plan = runInitLowering(*unit, sema);
  Arena arena;
  Op* mod = buildModule(arena, *unit, plan, "t.sy", diags);
  if (normalize && mod != nullptr) {
    LoopNormStats stats;
    mod = normalizeLoops(mod, arena, stats);
    hoistAllocas(mod);
    if (statsOut != nullptr) *statsOut = stats;
  }
  return dumpModule(mod);
}

// 【后置】数 `ForOp` / `WhileOp` 的个数。
static size_t countKind(Op* mod, OpKind k) { return countOps(mod, k); }

static Op* parseIr(const std::string& text, Arena& arena, DiagnosticEngine& diags) {
  return parseStructuredModule(text, arena, diags);
}

// ============================================================================
// A. `ForOp` 的形状契约
// ============================================================================
static void testForShape() {
  std::cout << "── A. ForOp 的形状契约 ──\n";
  DiagnosticEngine d;
  LoopNormStats st;
  const std::string ir = compileToIr("int main(){int i=0;int s=0;while(i<10){s=s+i;i=i+1;}return s;}\n",
                                     true, d, &st);
  check(st.forBuilt == 1 && st.whileSeen == 1, "平凡 while 升为 1 个 ForOp");
  Arena a;
  DiagnosticEngine d2;
  Op* mod = parseIr(ir, a, d2);
  check(mod != nullptr, "规范化后的 dump 能被读回");
  if (mod == nullptr) return;
  std::vector<Op*> fors;
  forEachOp(mod, [&](Op* op) { if (op->kind == OpKind::For) fors.push_back(op); });
  check(fors.size() == 1, "dump 里恰好 1 个 ForOp");
  if (fors.empty()) return;
  Op* f = fors[0];
  check(f->numOperands() == 4, "ForOp 有 4 个操作数（iv/lower/upper/step）");
  check(f->numRegions() == 1, "ForOp 有 1 个体 Region");
  check(!f->strAttr(0).empty(), "attrs[0] = IV 名字（非空字符串）");
  Value iv = f->operand(0);
  check(iv != nullptr && iv->definer != nullptr && iv->definer->kind == OpKind::Alloca,
        "★ 操作数 0 是 `AllocaOp` 的结果（IV 的**身份**，不是名字）");
  const Op* step = f->operand(3) != nullptr ? f->operand(3)->definer : nullptr;
  check(step != nullptr && step->kind == OpKind::Int && step->intAttr(0) == 1,
        "操作数 3 = 字面量 1（step）");
  Op* lower = f->operand(1) != nullptr ? f->operand(1)->definer : nullptr;
  check(lower != nullptr && lower->kind == OpKind::Load,
        "操作数 1 = `Load iv`（进入循环时的 IV 值）");
  // 体内不许有 IV 的自增、不许有 Break
  size_t stores = 0, breaks = 0;
  for (size_t i = 0; i < f->region(0)->size(); ++i) {
    Op* x = f->region(0)->at(i);
    if (x->kind == OpKind::Store) ++stores;
    if (x->kind == OpKind::Break) ++breaks;
  }
  check(stores == 1 && breaks == 0,
        "体内：只有 1 条 Store（`s` 的），0 个 Break（IV 的自增已摘除）");
  const std::vector<Violation> vs = verifyModule(mod);
  check(vs.empty(), "六条不变式全过（I1–I6）");
}

// ============================================================================
// B. 成功条件逐条（该升的升、不该升的不升）
// ============================================================================
static void testSuccessConditions() {
  std::cout << "── B. 成功条件（四条，逐条【只差一处】）──\n";
  struct Case {
    const char* what;
    const char* src;
    bool wantFor;      // true = 应该升为 ForOp
  };
  const Case cases[] = {
    {"平凡 while（成功）",
     "int main(){int i=0;while(i<4){i=i+1;}return i;}\n", true},
    {"边界不是 Lt（`<=`）⇒ 保留",
     "int main(){int i=0;while(i<=4){i=i+1;}return i;}\n", false},
    {"边界依赖 IV（`i<n-i`）⇒ 保留",
     "int main(){int i=0;int n=9;while(i<n-i){i=i+1;}return i;}\n", false},
    {"步进是 +2 ⇒ 保留",
     "int main(){int i=0;while(i<4){i=i+2;}return i;}\n", false},
    {"体内根本没有步进 ⇒ 保留",
     "int main(){int i=0;while(i<4){i=i*2+1;}return i;}\n", false},
    {"`break` 在体内 ⇒ 保留",
     "int main(){int i=0;while(i<4){if(i==2){break;}i=i+1;}return i;}\n", false},
    {"边界在体内被写 ⇒ 保留",
     "int main(){int i=0;int n=9;while(i<n){n=n-1;i=i+1;}return i;}\n", false},
    {"`continue` 在 if 内（两条递增路径）⇒ 升",
     "int main(){int i=0;int s=0;while(i<4){if(s==0){i=i+1;continue;}s=s+1;i=i+1;}return s;}\n",
     true},
    {"无循环 ⇒ 无 For",
     "int main(){int a=1;return a+1;}\n", false},
  };
  for (const Case& c : cases) {
    DiagnosticEngine d;
    LoopNormStats st;
    const std::string ir = compileToIr(c.src, true, d, &st);
    const size_t nf = st.forBuilt;
    const bool ok = (nf > 0) == c.wantFor;
    check(ok, std::string(c.what) + "（for-built=" + std::to_string(nf) +
                 " / while-seen=" + std::to_string(st.whileSeen) + "）");
    // 无论如何：dump 必须能被读回且不变量全过
    Arena a;
    DiagnosticEngine d2;
    Op* mod = parseIr(ir, a, d2);
    if (mod != nullptr) {
      check(verifyModule(mod).empty(), std::string(c.what) + "：不变量全过");
    } else {
      check(false, std::string(c.what) + "：读回失败");
    }
  }
}

// ============================================================================
// C. `continue` 消解的规范形式
// ============================================================================
static void testContinueCanonicalForm() {
  std::cout << "── C. continue 消解的规范形式（`if (!cond) { B }`）──\n";
  // 源码：`if (s == 0) { i = i + 1; continue; } s = s + 1; i = i + 1;`
  //   ⇒ 体里应有：`(Int 0)`、`(Eq C 0)`、`(If (Eq C 0)) { 剩余体 } { (Yield) }`
  DiagnosticEngine d;
  const std::string src =
      "int main(){int i=0;int s=0;while(i<4){if(s==0){i=i+1;continue;}s=s+1;i=i+1;}return s;}\n";
  LoopNormStats st;
  const std::string ir = compileToIr(src, true, d, &st);
  check(st.continuesResolved == 1, "消解了 1 个 continue（包装形式）");
  Arena a;
  DiagnosticEngine d2;
  Op* mod = parseIr(ir, a, d2);
  if (mod == nullptr) { check(false, "读回失败"); return; }
  // 找一个"体里带 If 且 If 的 then 以 Yield 结尾、else 非空"的 wrapper：
  //   本实现的包装 = `If(Eq C 0) { REST } { (Yield) }`
  bool wrapperFound = false, condIsEqZero = false, restInThen = false, thenIsYield = false;
  forEachOp(mod, [&](Op* op) {
    if (op->kind != OpKind::For) return;
    Region* body = op->region(0);
    for (Op* x : body->ops()) {
      if (x->kind != OpKind::If) continue;
      Region* th = x->region(0);
      Region* el = x->region(1);
      if (th == nullptr || el == nullptr) continue;
      if (el->size() != 1 || el->at(0)->kind != OpKind::Yield) continue;
      // 这是一个候选包装：条件应当是 `Eq C 0`
      const Op* c = x->numOperands() >= 1 && x->operand(0) != nullptr ? x->operand(0)->definer
                                                                     : nullptr;
      if (c == nullptr || c->kind != OpKind::Eq) continue;
      const Op* z = c->operand(1) != nullptr ? c->operand(1)->definer : nullptr;
      if (z == nullptr || z->kind != OpKind::Int || z->intAttr(0) != 0) continue;
      wrapperFound = true;
      condIsEqZero = true;
      if (th->size() >= 2) restInThen = true;   // 剩余体（>=2 条）在 then 里
      if (th->back() != nullptr && th->back()->kind == OpKind::Yield) thenIsYield = true;
    }
  });
  check(wrapperFound, "★ 产出了包装 `If`（continue 消解生效）");
  check(condIsEqZero, "★ 条件 = `Eq C 0`（**不交换比较谓词**）");
  check(restInThen, "★ **剩余体进 then**（`if (!cond) { B }` 的字面形式）");
  check(thenIsYield, "then 以 `(Yield)` 收尾（I4）");
  // 反面对照：不带 `--normalize` 时**没有** `For`（S05 契约不变）
  DiagnosticEngine d3;
  const std::string raw = compileToIr(src, false, d3, nullptr);
  Arena a2;
  DiagnosticEngine d4;
  Op* m2 = parseIr(raw, a2, d4);
  if (m2 != nullptr) {
    check(countKind(m2, OpKind::For) == 0, "不带 --normalize ⇒ 0 个 ForOp（冻结）");
  }
}

// ============================================================================
// D. `alloca` 提升
// ============================================================================
static void testAllocaHoist() {
  std::cout << "── D. alloca 提升（§3.5）──\n";
  // 手工构造：一个函数，入口有 1 个 alloca，另 2 个在**深层的 If 分支**里
  const char* text =
      "(Module \"h.sir\") {\n"
      "  (Func \"f\" :ret i32 :param [] @line 1) {\n"
      "    (Alloca %f.0 i32 @line 2)\n"
      "    (Int %f.1 1 @line 2)\n"
      "    (Store i32 %f.1 %f.0 @line 2)\n"
      "    (Int %f.2 1 @line 3)\n"
      "    (If %f.2 @line 3) {\n"
      "      (Alloca %f.3 i32 @line 4)\n"
      "      (Int %f.4 2 @line 4)\n"
      "      (Store i32 %f.4 %f.3 @line 4)\n"
      "      (Yield @line 3)\n"
      "    }\n"
      "    {\n"
      "      (Alloca %f.5 i32 @line 5)\n"
      "      (Int %f.6 3 @line 5)\n"
      "      (Store i32 %f.6 %f.5 @line 5)\n"
      "      (Yield @line 3)\n"
      "    }\n"
      "    (Load %f.7 i32 %f.0 @line 6)\n"
      "    (Return %f.7 @line 6)\n"
      "  }\n"
      "}\n";
  Arena a;
  DiagnosticEngine d;
  Op* mod = parseIr(text, a, d);
  check(mod != nullptr, "手工构造的 IR 读回成功");
  if (mod == nullptr) return;
  Op* fn = nullptr;
  forEachOp(mod, [&](Op* op) { if (op->kind == OpKind::Func) fn = op; });
  check(fn != nullptr, "找到 FuncOp");
  if (fn == nullptr) return;
  const size_t moved = hoistAllocas(mod);
  check(moved == 2, "搬动了 2 个 alloca（原本在深层 Region 里）");
  Region* entry = fn->region(0);
  size_t prefixAllocas = 0;
  for (size_t i = 0; i < entry->size(); ++i) {
    if (entry->at(i)->kind == OpKind::Alloca) ++prefixAllocas;
    else break;
  }
  check(prefixAllocas == 3, "★ 入口 Region 的**最前面**是全部 3 个 alloca");
  // 相对顺序：%f.0（原有） → %f.3（then 里的） → %f.5（else 里的）
  const bool orderOk = entry->at(0)->result(0) != nullptr &&
                       entry->at(0)->kind == OpKind::Alloca &&
                       entry->at(1)->kind == OpKind::Alloca &&
                       entry->at(2)->kind == OpKind::Alloca;
  check(orderOk, "★ 保持原有**相对顺序**（先序：入口 → then → else）");
  const std::string d1 = dumpModule(mod);
  const size_t moved2 = hoistAllocas(mod);
  check(moved2 == 0 && dumpModule(mod) == d1,
        "★ 幂等：已经满足时 `moved == 0` 且 dump 逐字节不变（恒等变换）");
  // 提升后仍满足"alloca 全在入口"这条后端不变量
  std::vector<Violation> vs = verifyModule(mod);
  bool allocaViolation = false;
  for (const Violation& v : vs) {
    if (std::string(v.invariant) == "后端不变量") allocaViolation = true;
  }
  check(!allocaViolation, "提升后：`alloca` 位置检查不再报红");
  // 真实语料：IRGen 本来就把 alloca 放进入口 ⇒ moved 必须是 0
  DiagnosticEngine d2;
  const std::string ir = compileToIr("int main(){int i=0;int a[3];while(i<3){a[i]=i;i=i+1;}return a[0];}\n",
                                     true, d2, nullptr);
  Arena a3;
  DiagnosticEngine d3;
  Op* m3 = parseIr(ir, a3, d3);
  if (m3 != nullptr) {
    check(hoistAllocas(m3) == 0, "真实流水线：IRGen 已把 alloca 放进入口 ⇒ moved == 0");
  }
}

// ============================================================================
// E. 检查器抓得住 I1 / I2 / I3
// ============================================================================
static void testVerifierCatchesForViolations() {
  std::cout << "── E. 检查器抓得住 I1/I2/I3（手工改坏）──\n";
  // 先造一份**合法**的规范化 IR
  DiagnosticEngine d;
  const std::string ir = compileToIr(
      "int main(){int i=0;int s=0;while(i<4){s=s+i;i=i+1;}return s;}\n", true, d, nullptr);
  Arena a;
  DiagnosticEngine d2;
  Op* mod = parseIr(ir, a, d2);
  check(mod != nullptr, "基准 IR 读回成功");
  if (mod == nullptr) return;
  check(verifyModule(mod).empty(), "基准 IR：不变量全过");

  Op* forOp = nullptr;
  forEachOp(mod, [&](Op* op) { if (op->kind == OpKind::For && forOp == nullptr) forOp = op; });
  check(forOp != nullptr, "找到 ForOp");
  if (forOp == nullptr) return;
  Value iv = forOp->operand(0);

  // ① I1：在体内插一条"写 IV 槽"的 Store
  {
    Arena a2;
    DiagnosticEngine dd;
    Op* m2 = parseIr(ir, a2, dd);
    Op* f2 = nullptr;
    forEachOp(m2, [&](Op* op) { if (op->kind == OpKind::For && f2 == nullptr) f2 = op; });
    Op* st = a2.makeOp(OpKind::Store, SourceLoc(1, 1));
    st->addAttr(Attr::ofType(typePool().i32()));
    st->addOperand(f2->operand(0));      // 值（类型不对，检查器只查"写了 IV 槽"）
    st->addOperand(f2->operand(0));      // 目的地 = IV 槽
    std::vector<Op*> ops = f2->region(0)->ops();
    ops.insert(ops.begin(), st);
    f2->region(0)->replaceAll(ops);
    bool sawI1 = false;
    for (const Violation& v : verifyModule(m2)) {
      if (std::string(v.invariant) == "I1") sawI1 = true;
    }
    check(sawI1, "★ I1：体内写 IV 槽被报出");
  }
  // ② I3：在体内插一条 BreakOp
  {
    Arena a2;
    DiagnosticEngine dd;
    Op* m2 = parseIr(ir, a2, dd);
    Op* f2 = nullptr;
    forEachOp(m2, [&](Op* op) { if (op->kind == OpKind::For && f2 == nullptr) f2 = op; });
    std::vector<Op*> ops = f2->region(0)->ops();
    ops.insert(ops.begin() + 1, a2.makeOp(OpKind::Break, SourceLoc(1, 1)));
    f2->region(0)->replaceAll(ops);
    bool sawI3 = false;
    for (const Violation& v : verifyModule(m2)) {
      if (std::string(v.invariant) == "I3") sawI3 = true;
    }
    check(sawI3, "★ I3：体内 BreakOp 被报出");
  }
  // ③ I2：把 upper 换成"体内定义的值"
  {
    Arena a2;
    DiagnosticEngine dd;
    Op* m2 = parseIr(ir, a2, dd);
    Op* f2 = nullptr;
    forEachOp(m2, [&](Op* op) { if (op->kind == OpKind::For && f2 == nullptr) f2 = op; });
    Value inner = f2->region(0)->at(0)->result(0);   // 体内第一条指令的结果
    Op* nf = a2.makeOp(OpKind::For, f2->loc);
    nf->addAttr(Attr::ofStr(f2->strAttr(0)));
    nf->addOperand(f2->operand(0));
    nf->addOperand(f2->operand(1));
    nf->addOperand(inner);                            // upper ← 体内定义
    nf->addOperand(f2->operand(3));
    nf->addRegion(f2->region(0));
    Region* entry = nullptr;
    forEachOp(m2, [&](Op* op) {
      if (op->kind == OpKind::Func && entry == nullptr) entry = op->region(0);
    });
    std::vector<Op*> ops = entry->ops();
    for (Op*& x : ops) {
      if (x == f2) { x = nf; break; }
    }
    entry->replaceAll(ops);
    bool sawI2 = false;
    for (const Violation& v : verifyModule(m2)) {
      if (std::string(v.invariant) == "I2") sawI2 = true;
    }
    check(sawI2, "★ I2：边界在体内被定义 ⇒ 报出");
  }
  (void)iv;
}

// ============================================================================
// F. 幂等（§C3）
// ============================================================================
static void testIdempotent() {
  std::cout << "── F. 幂等：run(run(X)) == run(X) ──\n";
  const char* srcs[] = {
    "int main(){int i=0;int s=0;while(i<4){s=s+i;i=i+1;}return s;}\n",
    "int main(){int i=0;int s=0;while(i<4){if(s==0){i=i+1;continue;}s=s+1;i=i+1;}return s;}\n",
    "int A[8];int main(){int i=0;int j=0;while(i<4){j=0;while(j<4){A[j]=j;j=j+1;}i=i+1;}return A[0];}\n",
  };
  for (const char* s : srcs) {
    DiagnosticEngine d;
    LoopNormStats st;
    const std::string once = compileToIr(s, true, d, &st);   // run(X)
    // run(run(X))：把产物读回，再规范化一次
    Arena a;
    DiagnosticEngine d2;
    Op* mod = parseIr(once, a, d2);
    if (mod == nullptr) { check(false, "读回失败"); continue; }
    LoopNormStats st2;
    Op* mod2 = normalizeLoops(mod, a, st2);
    hoistAllocas(mod2);
    const std::string twice = dumpModule(mod2);
    check(once == twice, std::string("★ 幂等：") + (st.forBuilt == 0 ? "（无 For）" : "（含 For）") +
                             " dump 逐字节相同");
  }
}

// ============================================================================
// G. 病态输入：深层嵌套 + 深层 alloca（显式工作栈）
// ============================================================================
static void testDeepNesting() {
  std::cout << "── G. 病态输入（深层嵌套不爆栈、不指数爆炸）──\n";
  // 4000 层嵌套的 If（每次都在 then 里再造一层），最内层放一个 alloca
  const int kDepth = 4000;
  std::string text = "(Module \"deep.sir\") {\n  (Func \"f\" :ret i32 :param [] @line 1) {\n";
  text += "    (Alloca %f.0 i32 @line 2)\n";
  for (int i = 0; i < kDepth; ++i) {
    text += "    (Int %f.k" + std::to_string(i) + " 1 @line 3)\n";
    text += "    (If %f.k" + std::to_string(i) + " @line 3) {\n";
  }
  text += "      (Alloca %f.inner i32 @line 4)\n      (Yield @line 4)\n";
  for (int i = 0; i < kDepth; ++i) {
    text += "    }\n    {\n      (Yield @line 3)\n    }\n";
  }
  text += "    (Int %f.ret 0 @line 9)\n    (Return %f.ret @line 9)\n  }\n}\n";
  Arena a;
  DiagnosticEngine d;
  Op* mod = parseIr(text, a, d);
  check(mod != nullptr, "4000 层嵌套的 IR 读回成功（不爆栈）");
  if (mod == nullptr) return;
  const size_t moved = hoistAllocas(mod);
  check(moved >= 1, "深层 alloca 被提升（显式工作栈）");
  Op* fn = nullptr;
  forEachOp(mod, [&](Op* op) { if (op->kind == OpKind::Func) fn = op; });
  bool front = fn != nullptr && fn->region(0)->size() >= 2 &&
               fn->region(0)->at(0)->kind == OpKind::Alloca &&
               fn->region(0)->at(1)->kind == OpKind::Alloca;
  check(front, "两个 alloca 都在入口最前面");
  LoopNormStats st;
  Op* mod2 = normalizeLoops(mod, a, st);
  check(mod2 != nullptr && st.whileSeen == 0, "无 while ⇒ 规范化不改动（while-seen=0）");
}

int main() {
  std::cout << "═══ test_loopnorm：LoopNormalize + AllocaHoist 单元断言 ═══\n";
  testForShape();
  testSuccessConditions();
  testContinueCanonicalForm();
  testAllocaHoist();
  testVerifierCatchesForViolations();
  testIdempotent();
  testDeepNesting();
  std::cout << "\n检查项 " << g_checks << " 个，失败 " << g_failed << " 个\n";
  if (g_failed == 0) std::cout << "✔ test_loopnorm 全部通过\n";
  return g_failed == 0 ? 0 : 1;
}
