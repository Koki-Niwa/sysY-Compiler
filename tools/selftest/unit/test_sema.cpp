// ============================================================================
// test_sema.cpp —— 语义分析单元测试（S03 验证标准 §十 第 ⑨ 项）
//
// ── 本文件覆盖什么 ──────────────────────────────────────────────────────
//
//   A. ★ **转换插入的节点形状**（prompt §一 的交付物 #10 点名要求）
//      隐式转换必须**物化成 `Cast` 节点**、种类与目标类型都要对：
//        * `f + i`（float/int）→ int 那一侧插 `IntToFloat`
//        * `i = f`（float→int）→ `FloatToInt`
//        * `if (f)`（float 条件）→ `ToBool`（**不是** FloatToInt！
//          `(int)0.5 == 0` 但 `if (0.5)` 必须为真）
//        * `if (i)`（int 条件）→ **不插**任何节点（多插会让 IR 变脏）
//        * 实参 / 返回值 / 初始化器 → 转换到目标类型
//      这些断言用的是"紧凑 S-表达式"（把正式转储的换行缩进压平），
//      所以既能一眼对照，又不依赖缩进细节。
//
//   B. **作用域与遮蔽**（prompt §3.1）
//        * 块内遮蔽外层 = 合法（0 error）
//        * 同作用域重复定义 = E-REDEF（**恰好 1 条**）
//        * 局部变量遮蔽函数名 ⇒ `v(...)` 是 E-CALL-NONFUNC
//        * 调用后文才定义的函数 ⇒ E-UNDEF（抓"偷偷前向声明"）
//
//   C. ★★ **诊断条数探针**（prompt §九 第 4 条，S02 的教训）
//      "报一次诊断"和"只报一条诊断"是两件事。Sema 里每一条
//      "放弃 / 跳过 / 截断"的路径，这里都**逐例声明"该报几条"**并断言：
//        * 初始化器元素过多 / 花括号过深 / 标量用花括号 / 数组用裸标量
//        * 超过 100 条上限之后：**恰好 100 条**（仍然继续遍历、仍退出 1）
//        * 深树（3 万个 `+`）与深初始化器（4000 层花括号）**不崩**
//      ⚠️ 为什么必须实测条数：S02 的读取器深度哨兵修好后，报告说"报一条"，
//         实测是 1904 条；490 个用例全照不出来，只有人为构造的输入才暴露。
//
// 依赖：同目录 run.sh（clang++ --std=c++17，无第三方测试框架）
// ============================================================================
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "frontend/SemaDump.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::CompUnit;
using sysy::DiagnosticEngine;
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

static void checkEq(const std::string& got, const std::string& want,
                    const std::string& what) {
  ++g_checks;
  if (got == want) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  ✘ " << what << "\n      got:  " << got << "\n      want: " << want
              << "\n";
  }
}

// ── 跑完整前端（词法 → 语法 → 语义），返回 `--emit=sema` 的文本 ────────────
struct Run {
  std::string dump;     // 完整转储（含 RuntimeLib 头）
  size_t errors = 0;
  size_t warnings = 0;
};

// wantDump=false 时**不打印转储**：深树（3 万个 `+`）的 --emit=sema 文本按
// 格式的平方律要 1.8 GB（AstPrinter.cpp 头注里有实测），单测里没必要付这个代价；
// 这里要验的是"**遍历**是迭代的、不崩"，与打印体积无关。
static Run analyze(const std::string& src, bool wantDump = true) {
  Run r;
  SourceFile file = SourceFile::fromString("<test>", src);
  DiagnosticEngine diag;
  diag.setSourceFile(&file);
  Lexer lexer(file, diag);
  Parser parser(lexer, diag);
  std::unique_ptr<CompUnit> unit = parser.parseCompUnit();
  if (unit != nullptr) {
    sysy::runSema(*unit, diag);
    if (wantDump) r.dump = sysy::printSemaDump(*unit);
  }
  r.errors = diag.errorCount();
  r.warnings = diag.warningCount();
  return r;
}

// 压成紧凑 S-表达式（丢掉 RuntimeLib 头与全部缩进/换行）。
// 只用于断言"节点形状"，不参与任何对外格式。
static std::string compact(const std::string& dump) {
  const size_t at = dump.find("(CompUnit");
  std::string out;
  bool sp = false;
  for (size_t i = (at == std::string::npos ? 0 : at); i < dump.size(); ++i) {
    const char c = dump[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { sp = true; continue; }
    if (c == ')') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out += c;
      sp = false;
      continue;
    }
    if (sp && !out.empty()) out += ' ';
    sp = false;
    out += c;
  }
  return out;
}

static bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ============================================================================
// A. 转换插入的节点形状
// ============================================================================
static void testCastShapes() {
  std::cout << "\n── A. 隐式转换的节点形状（prompt §3.3 的六个决策点）──\n";

  {  // 决策点 ①：二元算术，int 侧插 IntToFloat
    Run r = analyze("int main() { float f = 1.0; int i = 2; f = f + i; return 0; }\n");
    checkEq(std::to_string(r.errors), "0", "算术提升：零诊断");
    check(contains(compact(r.dump),
                   "(= (LVal f :obj float :float) (+ :float (LVal f :obj float :float) "
                   "(Cast :IntToFloat :float (LVal i :obj int :int))))"),
          "算术提升：`f + i` ⇒ int 侧被 (Cast :IntToFloat :float ...) 包住");
  }
  {  // 决策点 ③：关系运算的结果恒为 int，两侧提升到公共类型
    Run r = analyze("int main() { float f = 1.0; int i = 2; if (f < i) return 1; return 0; }\n");
    checkEq(std::to_string(r.errors), "0", "关系提升：零诊断");
    check(contains(compact(r.dump), "(< :int (LVal f :obj float :float) "
                                    "(Cast :IntToFloat :float (LVal i :obj int :int)))"),
          "关系提升：两侧同型且结果类型是 int");
  }
  {  // 决策点 ④：赋值的 float→int
    Run r = analyze("int main() { float f = 1.0; int i = 0; i = f; return i; }\n");
    check(contains(compact(r.dump),
                   "(Cast :FloatToInt :int (LVal f :obj float :float))"),
          "赋值：`i = f` ⇒ (Cast :FloatToInt :int ...)");
  }
  {  // 决策点 ⑥：float 条件 ⇒ ToBool（不是 FloatToInt！）
    Run r = analyze("int main() { float f = 1.0; if (f) return 1; return 0; }\n");
    checkEq(std::to_string(r.errors), "0", "float 条件：零诊断");
    check(contains(compact(r.dump),
                   "(If (Cast :ToBool :int (LVal f :obj float :float))"),
          "布尔位置：`if (f)` ⇒ (Cast :ToBool :int ...)，**不是** FloatToInt");
    check(!contains(compact(r.dump), "(If (Cast :FloatToInt"),
          "布尔位置：没有把 float 条件截断成 int（(int)0.5 == 0 但 if (0.5) 必须为真）");
  }
  {  // 决策点 ⑥ 的反面：int 条件**不插**任何转换
    Run r = analyze("int main() { int i = 1; if (i) return 1; return 0; }\n");
    check(contains(compact(r.dump), "(If (LVal i :obj int :int)"),
          "布尔位置：int 条件**不插**多余转换（多插会让 IR 变脏）");
  }
  {  // 决策点 ⑥：`&&` 的 float 侧同样要 ToBool
    Run r = analyze("int main() { float f = 1.0; int i = 1; if (f && i) return 1; return 0; }\n");
    check(contains(compact(r.dump), "(&& :int (Cast :ToBool :int (LVal f :obj float :float)) "
                                    "(LVal i :obj int :int))"),
          "`&&`：float 侧被 ToBool 包住，int 侧保持原样");
  }
  {  // 决策点 ⑤：实参转换
    Run r = analyze("int f(int a) { return a; }\n"
                    "int main() { float x = 1.5; return f(x); }\n");
    check(contains(compact(r.dump), "(Call f :int (Cast :FloatToInt :int "
                                    "(LVal x :obj float :float)))"),
          "实参：`f(x)`（float→int 形参）⇒ 实参被 FloatToInt 包住");
  }
  {  // 决策点 ⑤：返回值转换
    Run r = analyze("float f() { return 1; }\nint main() { return 0; }\n");
    check(contains(compact(r.dump), "(Return (Cast :IntToFloat :float (IntLit 1 :int)))"),
          "返回值：`return 1;`（float 函数）⇒ IntToFloat");
  }
  {  // 决策点 ⑤：初始化器（浮点数组用整型初值 = 合法的不对称规则）
    Run r = analyze("int main() { float b[2] = {1, 2}; return 0; }\n");
    checkEq(std::to_string(r.errors), "0", "`float b[2] = {1,2}` 合法（规范 §3 ConstDef 8）");
    check(contains(compact(r.dump), "(Cast :IntToFloat :float (IntLit 1 :int))") &&
              contains(compact(r.dump), "(Cast :IntToFloat :float (IntLit 2 :int))"),
          "初始化：整型初值被提升到 float（两个元素各一个 Cast）");
  }
  {  // 反向：整型数组里不许有浮点
    Run r = analyze("int main() { int a[2] = {1.5, 2.5}; return 0; }\n");
    // 两个**不同**的元素节点各报一次（"一条错误只报一次"约束的是同一个节点）
    checkEq(std::to_string(r.errors), "2",
            "`int a[2] = {1.5,2.5}` ⇒ 2 个元素各 1 条 E-TYPE（同一节点不重复报）");
    check(!contains(compact(r.dump), "(Cast :FloatToInt"),
          "整型数组的浮点元素**不插**转换（规范 §3 ConstDef 8 的不对称规则）");
  }
  {  // 完全下标 vs 子数组
    Run r = analyze("int g(int a[]) { return a[0]; }\n"
                    "int main() { int b[2][3]; return g(b); }\n");
    checkEq(std::to_string(r.errors), "0", "整片多维数组传给 int[]：零误报");
    check(contains(compact(r.dump), "(LVal b :obj int[2][3] :int[2][3])"),
          "子数组实参：值类型是整个数组 int[2][3]（对象类型 int[2][3]）");
  }
  {  // LVal 的 :obj 与值类型必须都正确
    Run r = analyze("int main() { int a[2][3]; return a[1][2]; }\n");
    check(contains(compact(r.dump),
                   "(LVal a :obj int[2][3] :int (IntLit 1 :int) (IntLit 2 :int))"),
          "LVal：`:obj` 是对象类型 int[2][3]，值类型是元素 int");
  }
  {  // 决策点 ②：`%` 不做提升
    Run r = analyze("int main() { float x = 1.0; int y = x % 2; return y; }\n");
    checkEq(std::to_string(r.errors), "1", "`%` 遇到 float ⇒ 恰好 1 条 E-MOD-FLOAT");
    check(!contains(compact(r.dump), "IntToFloat (LVal x"),
          "`%` 不做 int→float 提升（指令集里没有 frem）");
  }
  {  // 决策点 ②：int 的 `%` 正常
    Run r = analyze("int main() { int x = 5; int y = x % 2; return y; }\n");
    checkEq(std::to_string(r.errors), "0", "int 的 `%`：零诊断");
  }
}

// ============================================================================
// B. 作用域与遮蔽
// ============================================================================
static void testScopes() {
  std::cout << "\n── B. 作用域、遮蔽、定义前使用 ──\n";

  {  // 块内遮蔽外层：合法（规范 §3 Block 2）
    Run r = analyze("int main() { int a = 1; { int a = 2; a = a + 1; } return a; }\n");
    checkEq(std::to_string(r.errors), "0", "块内遮蔽外层：合法（0 诊断）");
  }
  {  // 同作用域重复定义：恰好 1 条
    Run r = analyze("int main() { int a = 1; int a = 2; return a; }\n");
    checkEq(std::to_string(r.errors), "1", "同作用域重复定义：恰好 1 条 E-REDEF");
    check(contains(r.dump, "(CompUnit"), "有错时仍然产出完整转储（不静默返回空树）");
  }
  {  // 顶层重定义：Decl vs Decl / Decl vs FuncDef / FuncDef vs FuncDef
    checkEq(std::to_string(analyze("int a;\nint a;\nint main() { return 0; }\n").errors),
            "1", "顶层 Decl vs Decl：1 条");
    checkEq(std::to_string(analyze("int a;\nint a() { return 0; }\nint main() { return 0; }\n").errors),
            "1", "顶层 Decl vs FuncDef：1 条");
    checkEq(std::to_string(analyze("int a() { return 0; }\nint a() { return 1; }\n"
                                   "int main() { return 0; }\n").errors),
            "1", "顶层 FuncDef vs FuncDef：1 条");
  }
  {  // 变量名可以与函数名相同（规范 §3 Conventions 3）
    Run r = analyze("int f() { return 1; }\nint main() { int f = 2; return f; }\n");
    checkEq(std::to_string(r.errors), "0", "变量名遮蔽函数名：合法（0 诊断）");
  }
  {  // 遮蔽之后再调用 ⇒ E-CALL-NONFUNC（25_scope3.sy 的形状）
    Run r = analyze("int f() { return 1; }\n"
                    "int main() { int f = 2; f(); return 0; }\n");
    checkEq(std::to_string(r.errors), "1", "遮蔽后调用 ⇒ 恰好 1 条 E-CALL-NONFUNC");
  }
  {  // 运行时函数同样可被局部变量遮蔽（TESTING-GUIDE §5 的 B 类用例）
    Run r = analyze("int main() { int putch = 0; putch(65); return putch; }\n");
    checkEq(std::to_string(r.errors), "1", "局部变量遮蔽 putch ⇒ 恰好 1 条 E-CALL-NONFUNC");
  }
  {  // 定义前使用：绝不隐式前向声明
    Run r = analyze("int main() { return f(); }\nint f() { return 1; }\n");
    checkEq(std::to_string(r.errors), "1", "调用后文才定义的函数 ⇒ 恰好 1 条 E-UNDEF");
  }
  {  // 函数体内可以递归
    Run r = analyze("int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }\n"
                    "int main() { return fact(5); }\n");
    checkEq(std::to_string(r.errors), "0", "自递归：合法（84 处递归用例依赖它）");
  }
  {  // 形参与函数体是两层作用域（规范 §3 Block 2 的字面规则）
    Run r = analyze("int f(int a) { int a = 2; return a; }\nint main() { return f(1); }\n");
    checkEq(std::to_string(r.errors), "0", "函数体内重定义形参名：按块作用域规则合法");
  }
  {  // 形参之间重名
    Run r = analyze("int f(int a, int a) { return a; }\nint main() { return f(1, 2); }\n");
    checkEq(std::to_string(r.errors), "1", "形参列表内重名：1 条");
  }
  {  // main 的三条签名规则
    checkEq(std::to_string(analyze("void main() { }\n").errors), "1", "void main ⇒ 1 条 E-MAIN");
    checkEq(std::to_string(analyze("int main(int a) { return a; }\n").errors), "1",
            "int main(int) ⇒ 1 条 E-MAIN");
    checkEq(std::to_string(analyze("float main() { return 0.0; }\n").errors), "1",
            "float main ⇒ 1 条 E-MAIN");
    checkEq(std::to_string(analyze("int f() { return 0; }\n").errors), "1",
            "没有 main ⇒ 1 条 E-MAIN");
    checkEq(std::to_string(analyze("int main() { return 0; }\n").errors), "0",
            "int main() ⇒ 0 诊断");
  }
}

// ============================================================================
// C. 诊断条数探针（每一条"放弃/跳过"路径都要声明"该报几条"）
// ============================================================================
static void testDiagCounts() {
  std::cout << "\n── C. 放弃/跳过路径的诊断条数探针（prompt §九）──\n";

  struct Case {
    const char* what;
    std::string src;
    size_t errors;      // 期望的**恰好**条数
  };
  std::vector<Case> cases = {
      {"初始化器元素个数超过数组总数（跳过后续元素但继续遍历）",
       "int a[2] = {1,2,3};\nint main() { return 0; }\n", 1},
      {"花括号嵌套深于秩（跳过该组但继续遍历）",
       "int a[2] = {{1,2},3};\nint main() { return 0; }\n", 1},
      {"标量用花括号组初始化",
       "int a = {1,2,3};\nint main() { return 0; }\n", 1},
      {"数组用裸标量初始化",
       "int a[4] = 4;\nint main() { return 0; }\n", 1},
      {"全局初始化器不是常量表达式（函数调用）",
       "int f() { return 1; }\nint g = f();\nint main() { return 0; }\n", 1},
      {"维度不是常量（变量）", "int main() { int n = 3; int a[n]; return 0; }\n", 1},
      {"维度是负数", "int a[-1];\nint main() { return 0; }\n", 1},
      {"维度里出现关系运算符（不在 ConstExp 文法里）",
       "int a[1 < 2];\nint main() { return 0; }\n", 1},
      {"非法八进制字面量 `09`（S02 只给 warning，S03 必须报错）",
       "int main() { int a = 09; return a; }\n", 1},
      {"未定义标识符在**算术**里（占位类型不再引发第二条）",
       "int main() { int a = 1; return a + zzz; }\n", 1},
      {"一个 return 里两个不同的未定义标识符 ⇒ 两条（各自独立）",
       "int main() { return aaa + bbb; }\n", 2},
  };
  for (const Case& c : cases) {
    Run r = analyze(c.src);
    checkEq(std::to_string(r.errors), std::to_string(c.errors), c.what);
  }

  {  // ★ 诊断上限：超出之后**恰好 100 条**（仍然继续遍历、仍可产出完整转储）
    std::string src = "int main() {\n";
    for (int i = 0; i < 150; ++i) src += "  x" + std::to_string(i) + " = 1;\n";
    src += "  return 0;\n}\n";
    const size_t n = analyze(src).errors;
    checkEq(std::to_string(n), "100",
            "150 个未定义标识符 ⇒ **恰好 100 条**（kMaxReportedErrors 上限）");
    const Run r2 = analyze(src);
    check(contains(r2.dump, "(CompUnit") && contains(r2.dump, "x149"),
          "超限后仍然继续遍历（转储里仍能看到第 150 个未定义引用）");
  }
  {  // 同一节点的"类型不符"只报一次（不能既 E-TYPE 又 E-ARGTYPE）
    Run r = analyze("int f(int a) { return a; }\n"
                    "int main() { int b[2]; return f(b); }\n");
    checkEq(std::to_string(r.errors), "1", "数组实参传给标量形参 ⇒ 恰好 1 条（不叠加）");
  }
  {  // const 赋值：一条
    Run r = analyze("int main() { const int N = 1; N = 2; return N; }\n");
    checkEq(std::to_string(r.errors), "1", "给 const 赋值 ⇒ 恰好 1 条");
  }
}

// ============================================================================
// D. 稳健性（深树、病态输入）：不崩、不静默
// ============================================================================
static void testRobustness() {
  std::cout << "\n── D. 深树与病态输入（不崩；迭代式遍历）──\n";

  {  // 3 万个 `+` 的左倾链：Sema 遍历 + 插 Cast + 析构都必须迭代
    std::string src = "int main() { int a = 0; a = 1";
    for (int i = 0; i < 30000; ++i) src += "+1";
    src += "; return a; }\n";
    const Run r = analyze(src, /*wantDump=*/false);
    checkEq(std::to_string(r.errors), "0", "3 万个 `+` 的链：零诊断（不崩、不栈溢出）");
  }
  {  // 3 万个 `+` 且**每个都带 Cast**（Cast 被插到很深的左倾链上）
    std::string src = "int main() { float f = 0.0; f = f";
    for (int i = 0; i < 30000; ++i) src += "+1";
    src += "; return 0; }\n";
    const Run r = analyze(src, /*wantDump=*/false);
    checkEq(std::to_string(r.errors), "0",
            "3 万个 `+` 且每个都带 IntToFloat：零诊断（含 Cast 的深链析构也不崩）");
  }
  {  // 4000 层嵌套块
    std::string src = "int main() {";
    for (int i = 0; i < 4000; ++i) src += "{";
    src += "int a = 1;";
    for (int i = 0; i < 4000; ++i) src += "}";
    src += "return 0; }\n";
    const Run r = analyze(src);
    check(contains(r.dump, "(CompUnit"), "4000 层嵌套块：有转储、不崩");
  }
  {  // 深初始化器：`{{{{…}}}}`（花括号组的处理也必须迭代）
    std::string src = "int a = ";
    for (int i = 0; i < 2000; ++i) src += "{";
    src += "1";
    for (int i = 0; i < 2000; ++i) src += "}";
    src += ";\nint main() { return 0; }\n";
    const Run r = analyze(src);
    check(contains(r.dump, "(CompUnit"), "2000 层 `{{{{…}}}}`：有转储、不崩");
    // ★ 标量加花括号是**合法**的（C11 §6.7.9；clang 对 `int x = {1};` 连警告都不给，
    //   对 `{{3}}` 只给风格警告），所以这里**不该**报错。
    //   "不许静默截断"这条纪律改由**值**来守：剥完之后必须是 1，不是 0。
    //   （早先这里断言"必须报错"，那是把合法程序判死的旧规则。）
    check(r.errors == 0, "2000 层 `{{{{…}}}}`：合法，零诊断");
    check(contains(r.dump, "(IntLit 1 :int)"),
          "2000 层 `{{{{…}}}}`：剥到最内层的 1（不是悄悄变成全零）");
  }
}

int main() {
  std::cout << "═══ test_sema：SysY 语义分析单元测试 ═══\n";
  testCastShapes();
  testScopes();
  testDiagCounts();
  testRobustness();
  std::cout << "\n────────────────────────────────────────────\n";
  std::cout << " 检查项 " << g_checks << " 个，失败 " << g_failed << " 个\n";
  if (g_failed == 0) {
    std::cout << " ✔ test_sema 全部通过\n";
    return 0;
  }
  std::cout << " ✘ test_sema 有失败\n";
  return 1;
}
