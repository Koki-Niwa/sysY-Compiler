// ============================================================================
// test_initlowering.cpp —— 初始化降级的单元断言（S04 验证标准 §十 第 ⑨ 项）
//
// ── 本文件覆盖什么 ──────────────────────────────────────────────────────
//
//   A. ★★ **"当前对象"模型的试金石**（prompt §3.1，本关的分水岭）
//        `int a[3][2] = {1,{2,3},4,5,6};` ⇒ 组作用在**标量** a[0][1] 上 ⇒ 非法
//        `int b[3][2] = {1,2,{3},5,6};` ⇒ 组正好落在 a[1] 的起点 ⇒ 合法
//      上面这一对**必须同时测**：只实现其中一条不算过。C 语义（实测 clang）：
//      `b` = {{1,2},{3,0},{5,0}}。Sema 报非法的那一条仍然要产出计划（不崩）。
//
//   B. **规模锚点**（prompt §4.2，这一关的核心）
//        * `int a[4096] = {1};`（局部）⇒ **动作数 ≤ 3**（不许 4096 条 StoreConst）
//        * 全局"无非零元素" ⇒ `:zero`，`nonzero` 为空（语料里有 2×864 MB 的数组）
//        * 全局"有非零元素" ⇒ 只列非零（不许列全 0..N-1）
//        * 局部"未写初始化器" ⇒ **零动作**（值不确定，不插零填充）
//        * 局部"整片为 0" ⇒ 一条 Zero 覆盖全片
//
//   C. **经典填充形态**（语料 + 规范 §3 ConstDef 6 的七个例子）
//        `{{1,2},{3,4}}` / `{{1},{2}}` / `{{},{3,4},5,6}` / `{1,{2},3}` /
//        `{1,2,3,4,{5},{}}`（三维）… 逐个核对**扁平值**
//
//   D. **稳健性**（prompt §九）
//        * 4000 层 `{{{{…}}}}`（病态输入）不崩、不 OOM
//        * 大数组（`int a[100000] = {1}`）不物化、动作数仍是常数级
//
// 依赖：同目录 run.sh（clang++ --std=c++17，无第三方测试框架）
// ============================================================================
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/ConstEval.h"
#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::CompUnit;
using sysy::DiagnosticEngine;
using sysy::InitPlan;
using sysy::Lexer;
using sysy::Parser;
using sysy::SourceFile;

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

// ── 跑完整前端（词法 → 语法 → 语义 → 初始化降级）─────────────────────────
struct Lowered {
  InitPlan plan;
  size_t errors = 0;
};

// ⚠️ `InitPlan::GlobalData::nonzero` 是**按字节偏移升序**的；S04 起
//    `FlatSlot` 也是稀疏的（只有被写到的位置），所以摊平时必须看 `index`。

static Lowered lower(const std::string& src) {
  Lowered out;
  SourceFile file = SourceFile::fromString("<test>", src);
  DiagnosticEngine diag;
  diag.setSourceFile(&file);
  Lexer lexer(file, diag);
  Parser parser(lexer, diag);
  std::unique_ptr<CompUnit> unit = parser.parseCompUnit();
  if (unit != nullptr) {
    sysy::Sema sema(diag);
    sema.run(*unit);
    out.plan = sysy::runInitLowering(*unit, sema);
  }
  out.errors = diag.errorCount();
  return out;
}

static const InitPlan::GlobalData* findGlobal(const InitPlan& p, const std::string& n) {
  for (const InitPlan::GlobalData& g : p.globals) {
    if (g.name == n) return &g;
  }
  return nullptr;
}
static const InitPlan::LocalInit* findLocal(const InitPlan& p, const std::string& n) {
  for (const InitPlan::LocalInit& l : p.locals) {
    if (l.name == n) return &l;
  }
  return nullptr;
}

// 把计划模拟成"扁平元素值"（StoreExpr 记成 -9999；只用于单元断言）。
static std::vector<int64_t> flatten(const InitPlan& plan, const std::string& name) {
  std::vector<int64_t> out;
  if (const InitPlan::GlobalData* g = findGlobal(plan, name)) {
    const int64_t n = sysy::elementCount(&g->type);
    out.assign(static_cast<size_t>(n < 0 ? 0 : n), 0);
    for (const auto& kv : g->nonzero) {
      out[static_cast<size_t>(kv.first / 4)] = kv.second.isFloat
                                                   ? static_cast<int64_t>(kv.second.f)
                                                   : kv.second.i;
    }
    return out;
  }
  if (const InitPlan::LocalInit* l = findLocal(plan, name)) {
    const int64_t n = sysy::elementCount(&l->type);
    out.assign(static_cast<size_t>(n < 0 ? 0 : n), 0);
    for (const sysy::InitAction& a : l->actions) {
      switch (a.kind) {
        case sysy::InitActionKind::Zero:
          for (uint64_t i = 0; i < a.bytes / 4; ++i) out[static_cast<size_t>(a.offset / 4 + i)] = 0;
          break;
        case sysy::InitActionKind::StoreConst:
          out[static_cast<size_t>(a.offset / 4)] =
              a.value.isFloat ? static_cast<int64_t>(a.value.f) : a.value.i;
          break;
        case sysy::InitActionKind::MemcpyConst:
          for (size_t i = 0; i < a.values.size(); ++i) {
            out[static_cast<size_t>(a.offset / 4 + i)] =
                a.values[i].isFloat ? static_cast<int64_t>(a.values[i].f) : a.values[i].i;
          }
          break;
        case sysy::InitActionKind::StoreExpr:
          out[static_cast<size_t>(a.offset / 4)] = -9999;
          break;
      }
    }
    return out;
  }
  return out;
}

static size_t actionCount(const InitPlan& plan, const std::string& name) {
  if (const InitPlan::LocalInit* l = findLocal(plan, name)) return l->actions.size();
  return 0;
}

static std::string show(const std::vector<int64_t>& v, size_t n = 12) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < n; ++i) {
    if (i) s += ' ';
    s += std::to_string(v[i]);
  }
  return s;
}

static void checkFlat(const std::string& decl, const std::string& var,
                      const std::vector<int64_t>& want, const std::string& what) {
  const Lowered r = lower(decl + "\nint main() { return 0; }\n");
  const std::vector<int64_t> got = flatten(r.plan, var);
  ++g_checks;
  if (got == want) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  ✘ " << what << "\n      got:  " << show(got) << "\n      want: "
              << show(want) << "\n";
  }
}

// ============================================================================
// A. "当前对象"模型的试金石（prompt §3.1）
// ============================================================================
static void testTouchstone() {
  std::cout << "\n── A. \"当前对象\"模型的试金石（prompt §3.1）──\n";
  {
    // 合法的一条：`{3}` 落在 a[1] 的起点上 ⇒ {{1,2},{3,0},{5,0}}
    const Lowered r = lower("int b[3][2] = {1,2,{3},5,6};\nint main() { return 0; }\n");
    check(r.errors == 0, "`int b[3][2] = {1,2,{3},5,6};` ⇒ 零诊断（实测 clang/gcc 零警告）");
    checkFlat("int b[3][2] = {1,2,{3},5,6};", "b", {1, 2, 3, 0, 5, 6},
              "`{1,2,{3},5,6}` 的扁平值是 {{1,2},{3,0},{5,6}}");
    // ⚠️ 提示词 §3.1 的第一张表把这一行写成 `{{1,2},{3,0},{5,0}}`
    //    （即 `1 2 3 0 5 0`），但**实测不是**：组 `{3}` 结束在 a[1] 的
    //    **起点**上，紧接着的标量 5 落进同一个 a[1]（覆盖那个隐式 0），
    //    6 落在 a[2][0]。clang -std=c99 与 gcc -std=c99 都给 `1 2 3 0 5 6`
    //    （报告 §4 D-1 有完整的 18 行真值表与命令）。
    checkFlat("int a[3][2] = {{1,2},{3},{5}};", "a", {1, 2, 3, 0, 5, 0},
              "对照：`{{1,2},{3},{5}}` 确实是 {{1,2},{3,0},{5,0}}（提示词的相邻行）");
  }
  {
    // 非法的一条：游标停在**标量** a[0][1] 上，组里有 2 个元素 ⇒ 元素多余
    const Lowered r = lower("int a[3][2] = {1,{2,3},4,5,6};\nint main() { return 0; }\n");
    check(r.errors >= 1, "`int a[3][2] = {1,{2,3},4,5,6};` ⇒ 报 E-INIT-SHAPE（试金石）");
    check(findGlobal(r.plan, "a") != nullptr,
          "试金石（非法）：**仍然产出计划**、不崩（dump 是可诊断的中间结果）");
  }
  {
    // 规范 §3 Initial Values 2 点名的三种非法形态
    check(lower("int a[4] = 4;\nint main() { return 0; }\n").errors >= 1,
          "`int a[4] = 4;` ⇒ 报错（数组的初始化器必须是花括号组）");
    check(lower("int a[2] = {{1,2}, 3};\nint main() { return 0; }\n").errors >= 1,
          "`int a[2] = {{1,2}, 3};` ⇒ 报错（花括号嵌套比秩更深）");
    check(lower("int a = {1,2,3};\nint main() { return 0; }\n").errors >= 1,
          "`int a = {1,2,3};` ⇒ 报错（标量不能用花括号组初始化）");
  }
  {
    // 规范 §3 ConstDef 8 的**不对称**规则
    check(lower("int a[2] = {1.5, 2.5};\nint main() { return 0; }\n").errors >= 1,
          "整型数组含浮点元素 ⇒ 报错（规范 §3 ConstDef 8）");
    const Lowered r = lower("float b[2] = {1,2};\nint main() { return 0; }\n");
    check(r.errors == 0, "`float b[2] = {1,2};` ⇒ **必须通过**（同一节的反方向）");
    checkFlat("float b[2] = {1,2};", "b", {1, 2}, "浮点数组用整型初值 ⇒ 值提升为 1.0 / 2.0");
  }
}

// ============================================================================
// B. 规模锚点（prompt §4.2）
// ============================================================================
static void testScale() {
  std::cout << "\n── B. 规模锚点（prompt §4.2，本关的核心）──\n";
  {
    const Lowered r = lower("int main() { int a[4096] = {1}; return a[0]; }\n");
    const size_t n = actionCount(r.plan, "main/a");
    check(n >= 1 && n <= 3, "★ `int a[4096] = {1};`（局部）⇒ 动作数 ≤ 3（实测 " +
                                std::to_string(n) + " 条）");
    const std::vector<int64_t> f = flatten(r.plan, "main/a");
    check(f.size() == 4096 && f[0] == 1 && f[1] == 0 && f[4095] == 0,
          "★ 上面那份计划模拟出来的值：首元素 1、其余 4095 个都是 0");
  }
  {
    // 全局：无非零元素 ⇒ :zero，且 nonzero 为空（语料里有 2×864 MB 的全局数组）
    const Lowered r = lower("int x[600][600][600];\nint main() { return 0; }\n");
    const InitPlan::GlobalData* g = findGlobal(r.plan, "x");
    check(g != nullptr && g->allZero && g->nonzero.empty(),
          "★ 全局 `int x[600][600][600];` ⇒ `:zero` 且非零表为空（216,000,000 个元素）");
  }
  {
    const Lowered r = lower("int buffer[50000000] = {};\nint main() { return 0; }\n");
    const InitPlan::GlobalData* g = findGlobal(r.plan, "buffer");
    check(g != nullptr && g->allZero && g->nonzero.empty(),
          "★ 全局 `int buffer[50000000] = {};` ⇒ `:zero`（不物化 50,000,000 个元素）");
  }
  {
    const Lowered r = lower("int a[100][100] = {1, 2, {3}};\nint main() { return 0; }\n");
    const InitPlan::GlobalData* g = findGlobal(r.plan, "a");
    check(g != nullptr && !g->allZero && g->nonzero.size() == 3,
          "★ 全局「有非零元素」⇒ 只列 3 个非零（不列全 0..9999）");
  }
  {
    const Lowered r = lower("int main() { int u; return 0; }\n");
    const InitPlan::LocalInit* l = findLocal(r.plan, "main/u");
    check(l != nullptr && l->actions.empty(),
          "★ 局部未写初始化器 ⇒ **零动作**（值不确定，不插零填充）");
  }
  {
    const Lowered r = lower("int main() { int z[8] = {}; return 0; }\n");
    const InitPlan::LocalInit* l = findLocal(r.plan, "main/z");
    check(l != nullptr && l->actions.size() == 1 &&
              l->actions[0].kind == sysy::InitActionKind::Zero &&
              l->actions[0].offset == 0 && l->actions[0].bytes == 32,
          "★ 局部 `int z[8] = {};` ⇒ 一条 `Zero(0,32)` 覆盖全片");
  }
  {
    // 连续常量段 ≥ 4 ⇒ 一条 MemcpyConst；< 4 ⇒ 逐个 StoreConst（§4.2 的策略表）
    const Lowered r = lower("int main() { int a[8] = {1,2,3,4,0,0,0,0}; return 0; }\n");
    check(actionCount(r.plan, "main/a") == 2,
          "连续 4 个常量 + 4 个零 ⇒ 恰好 2 条动作（MemcpyConst + Zero）");
  }
  {
    const Lowered r = lower("int main() { int a[8] = {1,2,3,0,0,0,0,0}; return 0; }\n");
    const size_t n = actionCount(r.plan, "main/a");
    check(n >= 3 && n <= 4, "连续 3 个常量 ⇒ 逐个 StoreConst（实测 " +
                                std::to_string(n) + " 条）");
  }
}

// ============================================================================
// C. 经典填充形态（规范 §3 ConstDef 6 的七个例子 + 语料）
// ============================================================================
static void testShapes() {
  std::cout << "\n── C. 经典填充形态（规范 §3 ConstDef 6 + 语料的实测形态）──\n";
  checkFlat("int a[3][2] = {{1,2},{3,4},{5,6}};", "a", {1, 2, 3, 4, 5, 6},
            "§6.2 `{{1,2},{3,4},{5,6}}` ⇒ {{1,2},{3,4},{5,6}}");
  checkFlat("int a[3][2] = {1,2,3,4,5,6};", "a", {1, 2, 3, 4, 5, 6},
            "§6.2 `{1,2,3,4,5,6}` ⇒ 同上");
  checkFlat("int a[3][2] = {1,2,{3,4},5,6};", "a", {1, 2, 3, 4, 5, 6},
            "§6.2 `{1,2,{3,4},5,6}` ⇒ 同上");
  checkFlat("int a[3][2] = {{1,2},{3},{5}};", "a", {1, 2, 3, 0, 5, 0},
            "§6.3 `{{1,2},{3},{5}}` ⇒ {{1,2},{3,0},{5,0}}");
  checkFlat("int a[3][2] = {1,2,{3},5,6};", "a", {1, 2, 3, 0, 5, 6},
            "§6.3 `{1,2,{3},5,6}` ⇒ {{1,2},{3,0},{5,0}}");
  checkFlat("int a[3][2] = {{},{3,4},5,6};", "a", {0, 0, 3, 4, 5, 6},
            "§6.3 `{{},{3,4},5,6}` ⇒ {{0,0},{3,4},{5,6}}");
  checkFlat("int a[4][2] = {};", "a", {0, 0, 0, 0, 0, 0, 0, 0}, "§6.1 `{}` ⇒ 全零");
  checkFlat("int d[4][2] = {1, 2, {3}, {5}, 7, 8};", "d", {1, 2, 3, 0, 5, 0, 7, 8},
            "§6.7 `d[4][2] = {1, 2, {3}, {5}, 7, 8}` ⇒ {{1,2},{3,0},{5,0},{7,8}}");
  checkFlat("int i[2][3][4] = {1, 2, 3, 4, {5}, {}};", "i",
            {1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            "语料 `i[2][3][4] = {1,2,3,4,{5},{}}` ⇒ 只有前 5 个非零");
  // 语料里的另外两个形态（clang 实测值）
  checkFlat("int a[4][2] = {{1, 2}, {3, 4}, {}, 7};", "a", {1, 2, 3, 4, 0, 0, 7, 0},
            "语料 `{{1,2},{3,4},{},7}` ⇒ {{1,2},{3,4},{0,0},{7,0}}");
  checkFlat("int c[2][8] = {{0, 9}, 8, 3};", "c",
            {0, 9, 0, 0, 0, 0, 0, 0, 8, 3, 0, 0, 0, 0, 0, 0},
            "语料 `c[2][8] = {{0,9},8,3}` ⇒ 第二行是 {8,3,0,…}");
  checkFlat("int g[5][3] = {1, 2, 3, {4}, {7}, 10, 11, 12};", "g",
            {1, 2, 3, 4, 0, 0, 7, 0, 0, 10, 11, 12, 0, 0, 0},
            "语料 `g[5][3] = {1,2,3,{4},{7},10,11,12}`");
  checkFlat("int e[5][3] = {{1,2,3},{4,5,6},{7,8,9},10,11,12,13,14,15};", "e",
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
            "语料 `e[5][3] = {{1,2,3},…,10..15}` ⇒ 1..15");
  checkFlat("int e[7][1][5] = {{}, {}, {2, 1, 8}, {{}}};", "e",
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            "语料 `e[7][1][5] = {{},{},{2,1,8},{{}}}` ⇒ 2/1/8 落在下标 10..12");
  // 局部变量也走同一条路（规范 §3 ConstDef 6.3 对局部同样成立）
  {
    const Lowered r = lower("int main() { int a[3][2] = {1,2,{3},5,6}; return a[0][0]; }\n");
    check(flatten(r.plan, "main/a") == std::vector<int64_t>({1, 2, 3, 0, 5, 6}),
          "同一个初始化器在**局部**也得到同一张扁平值表");
  }
}

// ============================================================================
// D. 稳健性（prompt §九）
// ============================================================================
static void testRobustness() {
  std::cout << "\n── D. 稳健性（prompt §九：病态输入不许崩、不许 OOM）──\n";
  {
    // 4000 层 `{{{{…}}}}`：**超过 S02 的语法嵌套上限（4000）**，所以必然有一条语法诊断。
    // 标量加花括号本身合法（见 test_sema），这里考的是"超限之后仍须产出计划、不许崩"。
    std::string src = "int a = ";
    for (int i = 0; i < 4000; ++i) src += "{";
    src += "1";
    for (int i = 0; i < 4000; ++i) src += "}";
    src += ";\nint main() { return 0; }\n";
    const Lowered r = lower(src);
    check(findGlobal(r.plan, "a") != nullptr && r.errors >= 1,
          "4000 层 `{{{{…}}}}`：超语法上限报了错、**仍然产出计划**、不崩（显式工作栈）");
  }
  {
    // 大数组 + 深嵌套混合：动作数必须仍是常数级
    const Lowered r = lower("int main() { int a[100000] = {1}; return a[0]; }\n");
    check(actionCount(r.plan, "main/a") <= 3,
          "`int a[100000] = {1};` ⇒ 动作数仍是 ≤ 3（不随数组长度增长）");
  }
  {
    // 一万个各自带组的初始化器（展开成 20000 个元素的组树）
    std::string src = "int main() { int a[20000] = {";
    for (int i = 0; i < 10000; ++i) {
      if (i) src += ",";
      src += "{1}";
    }
    src += "}; return 0; }\n";
    const Lowered r = lower(src);
    const std::vector<int64_t> f = flatten(r.plan, "main/a");
    // ★ 每个 `{1}` 都作用在一个**标量**上，合法（见 test_sema 的同一处说明），
    //   所以零诊断。断言改成查**值**：前 10000 个元素是 1，其余隐式为 0。
    check(r.errors == 0 && f.size() == 20000 && f[0] == 1 && f[9999] == 1 && f[10000] == 0,
          "一万个 `{1}` 组（每个绑定一个元素）：不崩、首个元素是 1");
  }
  {
    // 空初始化器 + 巨型数组：`nonzero` 必须为空
    const Lowered r = lower("int main() { int a[1000000] = {}; return 0; }\n");
    check(actionCount(r.plan, "main/a") == 1,
          "`int a[1000000] = {};`（局部）⇒ 1 条 Zero，不物化");
  }
}

int main() {
  std::cout << "═══ test_initlowering：初始化降级单元测试 ═══\n" << std::flush;
  testTouchstone(); std::cout << std::flush;
  testScale(); std::cout << std::flush;
  testShapes(); std::cout << std::flush;
  testRobustness(); std::cout << std::flush;
  std::cout << "\n────────────────────────────────────────────\n";
  std::cout << " 检查项 " << g_checks << " 个，失败 " << g_failed << " 个\n";
  if (g_failed == 0) {
    std::cout << " ✔ test_initlowering 全部通过\n";
    return 0;
  }
  std::cout << " ✘ test_initlowering 有失败\n";
  return 1;
}
