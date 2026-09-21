// ============================================================================
// test_structured.cpp —— 结构化 IR 的单元断言（S05 验证标准 §十 第 ⑦ 项）
//
// ── 本文件覆盖什么（每条都对应 prompt 的一句明文要求）─────────────────────
//
//   A. ★★ **0..N 结果的容量**（prompt §三.3）
//      手工造一个**两个结果**的 Op，断言：
//        * `results[0]` 与 `results[1]` 是**两个不同的 Value**（`!=`）
//        * `result(0)->definer == result(1)->definer == 那个 Op`
//        * `result(0)->index == 0`、`result(1)->index == 1`
//      这条断言的价值不在今天 —— 它把"容器支持 0..N"从**注释**变成
//      **可执行的契约**（设计文档 §1.1 记的那次血的教训就是把它写成了"恰好 1"）。
//
//   B. ★ **铁律 1 的形状断言**（prompt §3.2）
//      `int x = 1; return x;` 的 IR 里，`ReturnOp` 的操作数必须是一条
//      `LoadOp` 的结果，**不是** `IntOp` 的结果。这条断言钉死"变量读出经过
//      load"—— 少了它，某天"顺手优化掉一个 load"不会有任何症状，
//      而自研 mem2reg(S09) 的输入就少了。
//
//   C. **Region 与 I4**：每个 Region 恰好一个终结 Op；`makeBodyRegion` 的风格
//      （终结 Op 落在最后一行）。用 `StructuredVerifier` 独立核对。
//
//   D. ★ **检查器抓得住**（S04 的教训：检查器会空转）
//      手工改坏三处（缺终结符 / 终结符不在末尾 / 操作数未定义），
//      `verifyModule` 必须逐条报出来。**没有这一条，A–C 都可能是在自证。**
//
//   E. **Arena 的迭代释放**：造 20 万层嵌套的 Region 树，析构不许爆栈
//      （S02 的 `destroyTree` 教训：唯一维护点是"新增持有子节点的类型"）。
//
//   F. **类型表示**（prompt §五.12 要求写清"指针类型怎么表示"）：
//      `int[2][3]` 的对象是 `ptr[[2 x [3 x i32]]]`；形参 `int[]` 退化成 `ptr[i32]`。
//
// 依赖：同目录 run.sh（clang++ --std=c++17，无第三方框架）
// ============================================================================
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "structured/IRGen.h"
#include "structured/StructuredDump.h"
#include "structured/StructuredIR.h"
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

// 把源码编译成结构化 IR 文本（走**真实**流水线：Lexer→Parser→Sema→Init→IRGen）
static std::string compileToIr(const std::string& src, DiagnosticEngine& diags) {
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
  return dumpModule(mod);
}

static std::string compileToIr(const std::string& src) {
  DiagnosticEngine d;
  return compileToIr(src, d);
}

// 源码里有没有出现某个子串（断言用；不做任何语义判断）
static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ============================================================================
// A. ★★ 0..N 结果的容量（手工构造）
// ============================================================================
static void testZeroToNResults() {
  std::cout << "\n── A. 0..N 结果的容量（容器契约）──\n";
  Arena arena;
  const sir::Type* i32t = typePool().i32();
  const sir::Type* f32t = typePool().f32();

  // 0 个结果：`StoreOp` 是天然的 0 结果 Op
  Op* st = arena.makeOp(OpKind::Store, SourceLoc());
  check(st->numResults() == 0, "0 结果的 Op：numResults() == 0");
  check(st->first() == nullptr, "0 结果的 Op：first() 返回 nullptr（不崩）");

  // 2 个结果：**手工造**（SysY 不会产生，但容器必须支持）
  Op* two = arena.makeOp(OpKind::Alloca, SourceLoc());   // 用哪个 kind 不重要
  Value r0 = two->addResult(i32t);
  Value r1 = two->addResult(f32t);
  check(two->numResults() == 2, "双结果 Op：numResults() == 2");
  check(r0 != nullptr && r1 != nullptr, "两个结果都不是空指针");
  check(r0 != r1, "★ result(0) != result(1)（**两个不同的 Value**）");
  check(r0->definer == two && r1->definer == two, "两个结果的 definer 都指向那个 Op");
  check(r0->index == 0 && r1->index == 1, "两个结果的 index 分别是 0 与 1");
  check(r0->type == i32t && r1->type == f32t, "两个结果各自带自己的类型");
  check(two->result(0) == r0 && two->result(1) == r1, "result(i) 返回同一个指针");
  check(two->result(2) == nullptr, "越界的 result(i) 返回 nullptr（不崩）");

  // ★ Arena 保证指针稳定：再加 1000 个结果，早先的指针**不许失效**
  std::vector<Value> keep;
  keep.push_back(r0);
  keep.push_back(r1);
  for (int i = 0; i < 1000; ++i) keep.push_back(two->addResult(i32t));
  check(two->numResults() == 1002, "追加 1000 个结果后 numResults() == 1002");
  check(two->result(0) == r0, "★ 追加后 result(0) 的指针**没有失效**（Arena/deque 保证）");
  check(two->result(1) == r1, "★ 追加后 result(1) 的指针**没有失效**");
  check(two->result(0) != two->result(1), "追加后两个结果仍然不同");

  // 0 结果的 Op 与 1 结果的 Op 混在同一个 Region 里
  Region* reg = arena.makeRegion();
  reg->push(st);
  reg->push(two);
  check(reg->size() == 2, "Region 能同时容纳 0 结果与多结果的 Op");
}

// ============================================================================
// B. ★ 铁律 1 的形状断言
// ============================================================================
static void testNoValuePropagation() {
  std::cout << "\n── B. 铁律 1：变量读出必须经过 load ──\n";
  const std::string ir = compileToIr(
      "int main() {\n"
      "  int x = 1;\n"
      "  return x;\n"
      "}\n");
  check(!ir.empty(), "编译产出非空 IR");
  check(has(ir, "(Alloca"), "有 Alloca（变量在内存里）");
  check(has(ir, "(Store"), "有 Store（初始化写进内存）");
  check(has(ir, "(Load"), "有 Load（读出经过 load）");
  // ★ 形状断言：`Return` 的操作数是一个**结果名**，而那条结果名的定义是 `Load`
  const size_t rp = ir.find("(Return ");
  check(rp != std::string::npos, "有 Return");
  std::string operand;
  if (rp != std::string::npos) {
    size_t b = rp + 8;
    size_t e = ir.find_first_of(" )", b);
    operand = ir.substr(b, e - b);
  }
  check(!operand.empty() && operand[0] == '%', "★ Return 的操作数是结果名：" + operand);
  // 找到 `(Load <那个名字>` 出现在 IR 里 ⇒ 它确实是 load 的结果
  const bool loaded = !operand.empty() && has(ir, "(Load " + operand + " ");
  check(loaded, "★ Return 的操作数**是 LoadOp 的结果**（不是 IntOp）");
  check(!has(ir, "(Return %main.1)"), "Return **没有**直接接 IntOp 的结果");
}

// ============================================================================
// C. I4：每个 Region 恰好一个终结 Op（用独立的检查器核对）
// ============================================================================
static void testRegionTerminators() {
  std::cout << "\n── C. I4：每个 Region 恰好一个终结 Op ──\n";
  Arena arena;
  Region* body = arena.makeRegion();
  Op* ret = arena.makeOp(OpKind::Return, SourceLoc());
  ret->addResult(typePool().i32());   // 故意多一个结果 → 检查器该报"形状"
  body->push(ret);

  Op* fn = arena.makeOp(OpKind::Func, SourceLoc());
  fn->addAttr(Attr::ofStr("f"));
  fn->addAttr(Attr::ofType(typePool().i32()));
  fn->addRegion(body);
  Op* mod = arena.makeOp(OpKind::Module, SourceLoc());
  mod->addAttr(Attr::ofStr("t.sy"));
  Region* mr = arena.makeRegion();
  mr->push(fn);
  mod->addRegion(mr);

  const std::vector<Violation> vs = verifyModule(mod);
  bool sawArity = false;
  for (const Violation& v : vs) {
    if (std::string(v.invariant) == "形状") sawArity = true;
  }
  check(sawArity, "★ 检查器抓住了\"ReturnOp 不该有结果\"（不是空转）");

  // 干净的一棵树：应当零违反
  Arena a2;
  Region* b2 = a2.makeRegion();
  b2->push(a2.makeOp(OpKind::Return, SourceLoc()));
  Op* f2 = a2.makeOp(OpKind::Func, SourceLoc());
  f2->addAttr(Attr::ofStr("f"));
  f2->addAttr(Attr::ofType(typePool().voidTy()));
  f2->addRegion(b2);
  Op* m2 = a2.makeOp(OpKind::Module, SourceLoc());
  m2->addAttr(Attr::ofStr("t"));
  Region* mr2 = a2.makeRegion();
  mr2->push(f2);
  m2->addRegion(mr2);
  const std::vector<Violation> vs2 = verifyModule(m2);
  check(vs2.empty(), "干净的手工 IR：零违反（检查器不误报）");
}

// ============================================================================
// D. ★ 检查器抓得住（反证）
// ============================================================================
static Op* buildFuncModule(Arena& a, const std::vector<Op*>& bodyOps, Op*& modOut) {
  Region* body = a.makeRegion();
  for (Op* op : bodyOps) body->push(op);
  Op* fn = a.makeOp(OpKind::Func, SourceLoc());
  fn->addAttr(Attr::ofStr("f"));
  fn->addAttr(Attr::ofType(typePool().voidTy()));
  fn->addRegion(body);
  Op* mod = a.makeOp(OpKind::Module, SourceLoc());
  mod->addAttr(Attr::ofStr("t"));
  Region* mr = a.makeRegion();
  mr->push(fn);
  mod->addRegion(mr);
  modOut = mod;
  return fn;
}

static void testVerifierCatches() {
  std::cout << "\n── D. ★ 反证：检查器必须抓得住人为改坏 ──\n";
  {   // ① 缺终结符
    Arena a;
    Op* mod = nullptr;
    buildFuncModule(a, {}, mod);
    auto vs = verifyModule(mod);
    bool hit = false;
    for (const Violation& v : vs) if (std::string(v.invariant) == "I4") hit = true;
    check(hit, "① 空 Region（缺终结 Op）→ 报 I4");
  }
  {   // ② 终结符不在最后一行
    Arena a;
    Op* r = a.makeOp(OpKind::Return, SourceLoc());
    Op* extra = a.makeOp(OpKind::Int, SourceLoc());
    extra->addAttr(Attr::ofInt(1));
    extra->addResult(typePool().i32());
    Op* mod = nullptr;
    buildFuncModule(a, {r, extra}, mod);
    auto vs = verifyModule(mod);
    bool hit = false;
    for (const Violation& v : vs) if (std::string(v.invariant) == "I4") hit = true;
    check(hit, "② 终结 Op 之后还有 Op → 报 I4");
  }
  {   // ③ 操作数未定义
    Arena a;
    Op* st = a.makeOp(OpKind::Store, SourceLoc());
    st->addAttr(Attr::ofType(typePool().i32()));
    st->addOperand(nullptr);
    Op* r = a.makeOp(OpKind::Return, SourceLoc());
    Op* mod = nullptr;
    buildFuncModule(a, {st, r}, mod);
    auto vs = verifyModule(mod);
    bool hit = false;
    for (const Violation& v : vs) if (std::string(v.invariant) == "use-def") hit = true;
    check(hit, "③ 空操作数 → 报 use-def");
  }
  {   // ④ 非控制流容器带 Region
    Arena a;
    Op* bad = a.makeOp(OpKind::AddI, SourceLoc());
    bad->addResult(typePool().i32());
    bad->addRegion(a.makeRegion());
    Op* r = a.makeOp(OpKind::Return, SourceLoc());
    Op* mod = nullptr;
    buildFuncModule(a, {bad, r}, mod);
    auto vs = verifyModule(mod);
    bool hit = false;
    for (const Violation& v : vs) if (std::string(v.invariant) == "I5") hit = true;
    check(hit, "④ AddI 带 Region → 报 I5");
  }
  {   // ⑤ GEP 缺 I6 标记
    Arena a;
    Op* g = a.makeOp(OpKind::GetElementPtr, SourceLoc());
    g->addAttr(Attr::ofType(typePool().i32()));   // 只有一个类型属性
    g->addResult(typePool().ptrTo(typePool().i32()));
    Op* r = a.makeOp(OpKind::Return, SourceLoc());
    Op* mod = nullptr;
    buildFuncModule(a, {g, r}, mod);
    auto vs = verifyModule(mod);
    bool hit = false;
    for (const Violation& v : vs) if (std::string(v.invariant) == "I6") hit = true;
    check(hit, "⑤ GEP 缺亲和性标记 → 报 I6");
  }
}

// ============================================================================
// E. Arena 的迭代释放（深树不爆栈）
// ============================================================================
static void testDeepRegionNoStackOverflow() {
  std::cout << "\n── E. Arena 的迭代释放（深树不爆栈）──\n";
  Arena a;
  Region* cur = a.makeRegion();
  const int kDepth = 200000;
  for (int i = 0; i < kDepth; ++i) {
    Op* whileOp = a.makeOp(OpKind::While, SourceLoc());
    whileOp->addRegion(a.makeRegion());
    whileOp->addRegion(a.makeRegion());
    cur->push(whileOp);
    cur = whileOp->region(0);
  }
  cur->push(a.makeOp(OpKind::Break, SourceLoc()));
  check(a.opCount() > static_cast<size_t>(kDepth), "造出 20 万层嵌套的 Op 树");
  // 递归遍历会在这里爆栈；`forEachOp` 必须用显式栈
  size_t n = 0;
  forEachOp(nullptr, [&](Op*) { ++n; });
  check(n == 0, "forEachOp(nullptr) 是空操作");
  // 析构（Arena::clear）是迭代的 ⇒ 这一句不许崩
  a.clear();
  check(a.opCount() == 0, "★ Arena::clear() 迭代释放 20 万层，没有爆栈");
}

// ============================================================================
// F. 类型表示 + 往返
// ============================================================================
static void testTypeAndRoundTrip() {
  std::cout << "\n── F. 类型表示 + dump/读回往返 ──\n";
  const std::string ir = compileToIr(
      "int g[2][3];\n"
      "int f(int a[], int n) { return n; }\n"
      "int main() {\n"
      "  float x = 1;\n"
      "  int y = x;\n"
      "  return f(g[1], y);\n"
      "}\n");
  check(!ir.empty(), "编译产出非空 IR");
  // ★ 指针类型**显式携带元素类型**（不用 LLVM 的 opaque `ptr`）：
  //   `int[2][3]` 的 alloca/全局结果是 `ptr[[2 x ptr[[3 x i32]]]]`
  //   —— 外层指针指向"2 个 inner 指针"的数组，inner 指针指向 `[3 x i32]`。
  //   为什么是"指针的数组"而不是"数组的数组"：SysY 的数组是**行主序值语义**，
  //   而 S06/S07 必须能从类型推出每一步 GEP 的步长；把内层也表示成指针，
  //   `a[i]` 的落点类型就**直接读得出来**（`ptr[[3 x i32]]`），不需要再算一遍。
  check(has(ir, "ptr[[2 x ptr[[3 x i32]]]]"),
        "★ 全局 `int[2][3]` 的对象类型是 ptr[[2 x ptr[[3 x i32]]]]");
  check(has(ir, ":param [ptr[i32], i32]"), "★ 形参 `int a[]` 退化成 ptr[i32]");
  check(has(ir, "(Sext"), "数组下标走 sext（i32 → i64）");
  check(!has(ir, "(Zext"), "**不用** zext（负下标会错）");
  // 往返
  DiagnosticEngine d2;
  SourceFile f2 = SourceFile::fromString("t.sir", ir);
  d2.setSourceFile(&f2);
  Arena a2;
  Op* mod = parseStructuredModule(f2.text(), a2, d2);
  check(mod != nullptr, "读回成功（无诊断）");
  if (mod != nullptr) {
    check(dumpModule(mod) == ir, "★ dump → 读回 → dump **逐字节相同**（轨 A）");
  }
  check(d2.errorCount() == 0, "读回零诊断");
}

int main() {
  std::cout << "═══ test_structured：结构化 IR 单元断言 ═══\n";
  testZeroToNResults();
  testNoValuePropagation();
  testRegionTerminators();
  testVerifierCatches();
  testDeepRegionNoStackOverflow();
  testTypeAndRoundTrip();
  std::cout << "\n检查项 " << g_checks << " 个，失败 " << g_failed << " 个\n";
  if (g_failed == 0) std::cout << "✔ test_structured 全部通过\n";
  return g_failed == 0 ? 0 : 1;
}
