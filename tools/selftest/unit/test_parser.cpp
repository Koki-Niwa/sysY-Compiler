// ============================================================================
// test_parser.cpp —— 语法分析器单元测试（S02 验证标准 §10 第 4/6/9 项）
//
// ── 本文件覆盖什么 ──────────────────────────────────────────────────────
//
//   A. ★★ **轨 B：树形断言**（唯一能抓"优先级/结合性写错"的手段）
//      prompt §7.1 的 **34 条**逐条断言，另加 20 余条边界（`!` 的处理、
//      空实参表、形参 `[]`、嵌套初始化、空语句、位宽溢出…）。
//      ⚠️ 为什么必须有轨 B：往返（轨 A）对优先级错误**全盲** —— 若把
//         `a+b*c` 错解析成 `a+b` 再 `*c`，打印器会忠实打印错树，
//         重读又得到同一棵错树，往返照样通过。见 prompt §2.1/§6.3。
//
//   B. §5 的单例逐字节比对：`int main(){return 1+2*3;}` 的 --emit=ast
//      必须与 prompt 里给的例子**逐字节相同**（含末尾换行）。
//
//   C. 往返自检：真实调用编译器二进制的
//        `--emit=ast` → `--from-ast --emit=ast` → 两份文本逐字节相同。
//      （整批 490 个文件由 tools/selftest/check_parser.py 负责，这里只做
//        小样本冒烟，保证"改了 printer 而忘了改 reader"能立刻被单测挡住。）
//
//   D. 边界与错误恢复：540 个用例里 50 个含 `@`/`tensor`，它们是**结构化
//      语法错误**（不是崩溃、不是静默通过）。这里用最小样例固定该行为。
//
// ── 为什么用"紧凑 S-表达式"做断言 ────────────────────────────────────────
//   --emit=ast 的正式格式是**一行一个节点 + 缩进**（prompt §5）；
//   prompt §7.1 的表里给的却是紧凑形式 `(+ a (* b c))`。两者信息等价，
//   本文件先把源码解析成 AST、再压成紧凑形式，然后与表里的期望**字符串相等**
//   比对 —— 这样断言和 prompt 的表能一眼对上，也便于失败时定位。
//
// 依赖：同目录 run.sh（clang++ --std=c++17，无第三方测试框架）
// ============================================================================
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "frontend/Ast.h"
#include "frontend/AstPrinter.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

using sysy::Binary;
using sysy::BlockStmt;
using sysy::Call;
using sysy::CompUnit;
using sysy::Decl;
using sysy::DiagnosticEngine;
using sysy::Expr;
using sysy::FloatLit;
using sysy::FuncDef;
using sysy::InitVal;
using sysy::IntLit;
using sysy::IfStmt;
using sysy::Lexer;
using sysy::LVal;
using sysy::Node;
using sysy::Parser;
using sysy::SourceFile;
using sysy::Stmt;
using sysy::TokKind;
using sysy::Unary;
using sysy::VarDef;

static int g_checks = 0;
static int g_failed = 0;

static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (ok) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  X " << what << "\n";
  }
}

static void checkEq(const std::string& what, const std::string& got,
                    const std::string& want) {
  ++g_checks;
  if (got == want) {
    std::cout << "  . " << what << "\n";
  } else {
    ++g_failed;
    std::cout << "  X " << what << "\n      want: " << want << "\n      got : " << got
              << "\n";
  }
}

// ============================================================================
// 一、紧凑打印（把 AST 压成 prompt §7.1 表里的那种形式）
// ============================================================================
namespace {

// 原文当名字用时的转义：语料里的标识符不会与表达式语法冲突，但把
// "以数字开头"或"是运算符"的原文加引号，能让断言在任何输入下都无歧义。
bool riskyName(const std::string& s) {
  if (s.empty()) return true;
  const char c0 = s[0];
  if (c0 >= '0' && c0 <= '9') return true;
  if (c0 == '.') return true;
  if (s == "+" || s == "-" || s == "*" || s == "/" || s == "%" || s == "!" ||
      s == "<" || s == ">" || s == "<=" || s == ">=" || s == "==" || s == "!=" ||
      s == "&&" || s == "||") {
    return true;
  }
  return false;
}

std::string atom(const std::string& s) { return riskyName(s) ? "|" + s + "|" : s; }

std::string opText(TokKind k) {
  switch (k) {
    case TokKind::Plus:      return "+";
    case TokKind::Minus:     return "-";
    case TokKind::Star:      return "*";
    case TokKind::Slash:     return "/";
    case TokKind::Percent:   return "%";
    case TokKind::Less:      return "<";
    case TokKind::Greater:   return ">";
    case TokKind::LessEq:    return "<=";
    case TokKind::GreaterEq: return ">=";
    case TokKind::EqEq:      return "==";
    case TokKind::NotEq:     return "!=";
    case TokKind::AmpAmp:    return "&&";
    case TokKind::PipePipe:  return "||";
    case TokKind::Not:       return "!";
    default:                 return "?";
  }
}

std::string compactExpr(const Expr& e) {
  const std::string_view k = e.nodeKind();
  if (k == "IntLit" || k == "FloatLit") {
    // 字面量直接输出原文（与 prompt §7.1 的写法一致：`(+ 1 2)`）
    return std::string(static_cast<const IntLit&>(e).text);
  }
  if (k == "LVal") {
    const auto& n = static_cast<const LVal&>(e);
    std::string s = atom(n.name);
    for (const auto& idx : n.indices) s += "(" + compactExpr(*idx) + ")";
    return s;
  }
  if (k == "Call") {
    const auto& n = static_cast<const Call&>(e);
    std::string s = atom(n.callee);
    s += "(";
    for (size_t i = 0; i < n.args.size(); ++i) {
      if (i != 0) s += ",";
      s += compactExpr(*n.args[i]);
    }
    s += ")";
    return s;
  }
  if (k == "Unary") {
    const auto& n = static_cast<const Unary&>(e);
    return "(" + opText(n.op) + " " + compactExpr(*n.operand) + ")";
  }
  if (k == "Binary") {
    const auto& n = static_cast<const Binary&>(e);
    return "(" + opText(n.op) + " " + compactExpr(*n.lhs) + " " + compactExpr(*n.rhs) + ")";
  }
  return "<?expr>";
}

std::string compactInit(const InitVal& iv) {
  if (iv.expr != nullptr) return compactExpr(*iv.expr);
  std::string s = "{";
  for (size_t i = 0; i < iv.list.size(); ++i) {
    if (i != 0) s += ",";
    s += compactInit(*iv.list[i]);
  }
  s += "}";
  return s;
}

std::string compactDim(const sysy::Dim& d) {
  return d.expr == nullptr ? "" : compactExpr(*d.expr);
}

std::string compactVarDef(const VarDef& v) {
  std::string s = atom(v.name);
  for (const auto& d : v.dims) s += "[" + compactDim(d) + "]";
  if (v.init != nullptr) s += "=" + compactInit(*v.init);
  return s;
}

std::string compactDecl(const Decl& d) {
  std::string s = d.isConst ? "const " : "";
  s += (d.base == sysy::BType::Int ? "int " : "float ");
  for (size_t i = 0; i < d.defs.size(); ++i) {
    if (i != 0) s += ",";
    s += compactVarDef(*d.defs[i]);
  }
  return s;
}

std::string compactStmt(const Stmt& s) {
  const std::string_view k = s.nodeKind();
  if (k == "BlockStmt") {
    const auto& b = static_cast<const BlockStmt&>(s);
    std::string r = "{";
    for (const auto& item : b.items) {
      if (item == nullptr) continue;
      const std::string_view ik = item->nodeKind();
      if (ik == "Decl") {
        r += compactDecl(static_cast<const Decl&>(*item)) + ";";
      } else {
        r += compactStmt(static_cast<const Stmt&>(*item)) + ";";
      }
    }
    r += "}";
    return r;
  }
  if (k == "AssignStmt") {
    const auto& n = static_cast<const sysy::AssignStmt&>(s);
    return compactExpr(*n.lhs) + "=" + compactExpr(*n.rhs);
  }
  if (k == "ExprStmt") {
    const auto& n = static_cast<const sysy::ExprStmt&>(s);
    return n.expr == nullptr ? std::string(";") : compactExpr(*n.expr);
  }
  if (k == "IfStmt") {
    const auto& n = static_cast<const IfStmt&>(s);
    const std::string thenS = compactStmt(*n.thenS);
    std::string r = "if(" + compactExpr(*n.cond) + ")" + thenS;
    if (n.elseS != nullptr) {
      // then 分支不是块时（`if(a)b; else c;`）要补一个空格，否则会粘成 `belse`
      if (!thenS.empty() && thenS.back() != '}') r += " ";
      r += "else " + compactStmt(*n.elseS);
    }
    return r;
  }
  if (k == "WhileStmt") {
    const auto& n = static_cast<const sysy::WhileStmt&>(s);
    return "while(" + compactExpr(*n.cond) + ")" + compactStmt(*n.body);
  }
  if (k == "BreakStmt")    return "break";
  if (k == "ContinueStmt") return "continue";
  if (k == "ReturnStmt") {
    const auto& n = static_cast<const sysy::ReturnStmt&>(s);
    return n.value == nullptr ? std::string("return") : "return " + compactExpr(*n.value);
  }
  return "<?stmt>";
}

std::string compactFunc(const FuncDef& f) {
  std::string r = (f.isVoid ? "void " : (f.retType == sysy::BType::Int ? "int " : "float "));
  r += atom(f.name) + "(";
  for (size_t i = 0; i < f.params.size(); ++i) {
    if (i != 0) r += ",";
    r += (f.params[i].type.base == sysy::BType::Int ? "int " : "float ");
    r += atom(f.params[i].name);
    for (const auto& d : f.params[i].type.dims) r += "[" + compactDim(d) + "]";
  }
  r += ")";
  if (f.body != nullptr) r += compactStmt(*f.body);
  return r;
}

std::string compactUnit(const CompUnit& u) {
  std::string r;
  for (const auto& item : u.items) {
    if (item == nullptr) continue;
    const std::string_view k = item->nodeKind();
    if (k == "Decl") {
      r += compactDecl(static_cast<const Decl&>(*item)) + ";";
    } else if (k == "FuncDef") {
      r += compactFunc(static_cast<const FuncDef&>(*item));
    } else {
      r += "<?item>";
    }
  }
  return r;
}

}  // namespace

// ============================================================================
// 二、解析辅助
// ============================================================================
struct ParseResult {
  SourceFile src;
  DiagnosticEngine diags;
  std::unique_ptr<CompUnit> unit;
  size_t parserErrors = 0;

  ParseResult(const std::string& path, std::string content)
      : src(SourceFile::fromString(path, std::move(content))) {
    diags.setSourceFile(&src);
    Lexer lexer(src, diags);
    Parser parser(lexer, diags);
    unit = parser.parseCompUnit();
    parserErrors = parser.errorCount();
  }
};

// 把一段源码解析成紧凑形式（出错时把诊断也带上，便于定位）
static std::string shapeOf(const std::string& src, size_t* errs = nullptr) {
  ParseResult r("<test>", src);
  if (errs != nullptr) *errs = r.parserErrors;
  if (r.unit == nullptr) return "<?null>";
  return compactUnit(*r.unit);
}

// 一条断言的完整描述：源码 → 期望的紧凑树形
struct ShapeCase {
  const char* src;
  const char* want;
  const char* note;
};

// ============================================================================
// 三、★ 轨 B：prompt §7.1 的 34 条（逐条断言）（+ 边界用例）
// ============================================================================
static void testTrackB() {
  std::cout << "\n── 轨 B：树形断言（prompt §7.1，34 条 + 边界）──\n";

  // 包一层 main，只断言 return 表达式的形状
  auto wrap = [](const char* body) {
    return std::string("int main(){ int a; int b; int c; int d; int i; int j; ") + body + " }";
  };
  // 从 index 表达式的 AST 中把 return 的操作数取出来压成紧凑形式。
  // 加声明是为了让断言用例读起来像普通 SysY（不需要事先声明 a/b/c…），
  // 而断言本身只关心**表达式树**，所以这里把 return 的子树单独取出来。
  auto exprShape = [&wrap](const char* body) -> std::string {
    ParseResult r("<expr>", wrap(body));
    for (const auto& it : r.unit->items) {
      if (!it || std::string_view(it->nodeKind()) != "FuncDef") continue;
      const auto& fn = static_cast<const FuncDef&>(*it);
      if (fn.body == nullptr) continue;
      for (const auto& st : fn.body->items) {
        if (!st || std::string_view(st->nodeKind()) != "ReturnStmt") continue;
        const auto& ret = static_cast<const sysy::ReturnStmt&>(*st);
        if (ret.value != nullptr) return compactExpr(*ret.value);
      }
    }
    return "<?no-return>";
  };

  struct ExprCase {
    const char* expr;
    const char* want;
  };
  // —— 优先级（1–8）——
  const ExprCase prec[] = {
      {"return a + b * c;",           "(+ a (* b c))"},          // 1
      {"return a * b + c;",           "(+ (* a b) c)"},          // 2
      {"return a < b == c;",          "(== (< a b) c)"},         // 3 ★关系高于相等
      {"return a == b < c;",          "(== a (< b c))"},         // 4
      {"return a && b || c;",         "(|| (&& a b) c)"},        // 5
      {"return a || b && c;",         "(|| a (&& b c))"},        // 6
      {"return a == b && c != d;",    "(&& (== a b) (!= c d))"}, // 7
      {"return a + b < c * d;",       "(< (+ a b) (* c d))"},    // 8
  };
  for (const auto& t : prec) {
    checkEq(std::string("优先级  ") + t.expr, exprShape(t.expr), t.want);
  }

  // —— 左结合（9–12）——
  const ExprCase left[] = {
      {"return a - b - c;",  "(- (- a b) c)"},   // 9
      {"return a / b / c;",  "(/ (/ a b) c)"},   // 10
      {"return a - b + c;",  "(+ (- a b) c)"},   // 11
      {"return a != b == c;", "(== (!= a b) c)"},// 12
  };
  for (const auto& t : left) {
    checkEq(std::string("左结合  ") + t.expr, exprShape(t.expr), t.want);
  }

  // —— 一元（13–16）——
  const ExprCase un[] = {
      {"return - - a;",     "(- (- a))"},      // 13
      {"return !!a;",       "(! (! a))"},      // 14
      {"return -a * b;",    "(* (- a) b)"},    // 15
      {"return !a && b;",   "(&& (! a) b)"},   // 16
  };
  for (const auto& t : un) {
    checkEq(std::string("一元    ") + t.expr, exprShape(t.expr), t.want);
  }

  // —— 括号覆盖优先级（17–19）——
  const ExprCase par[] = {
      {"return (a + b) * c;", "(* (+ a b) c)"},  // 17
      {"return a * (b + c);", "(* a (+ b c))"},  // 18
      {"return (a);",         "a"},              // 19 ★括号不改变树形
  };
  for (const auto& t : par) {
    checkEq(std::string("括号    ") + t.expr, exprShape(t.expr), t.want);
  }

  // —— 下标与调用（20–26）——
  const ExprCase idx[] = {
      {"return a[i][j];",     "a(i)(j)"},                 // 20
      {"return a[1 + 2];",    "a((+ 1 2))"},              // 21
      {"return f(1, 2);",     "f(1,2)"},                  // 22
      {"return f();",         "f()"},                     // 23
      {"return f(a + b) * c;", "(* f((+ a b)) c)"},       // 24
      {"return f(x);",        "f(x)"},                    // 25 ★Ident '(' = 调用
      {"return a;",           "a"},                       // 26
  };
  for (const auto& t : idx) {
    checkEq(std::string("下标/调用 ") + t.expr, exprShape(t.expr), t.want);
  }

  // —— 语句与结构（27–34）——
  {
    // 27/28：else 绑定到最近的未配对 if
    const std::string s27 =
        "int main(){ int a; int b; int c; if (a) b; else c; return 0; }";
    checkEq("27 else 就近配对（有 else）", shapeOf(s27),
            "int main(){int a;int b;int c;if(a)b else c;return 0;}");

    const std::string s28 =
        "int main(){ int a; int b; int c; int d; if (a) if (b) c; else d; return 0; }";
    checkEq("28 else 属于内层 if", shapeOf(s28),
            "int main(){int a;int b;int c;int d;if(a)if(b)c else d;return 0;}");

    // 29：空语句体
    checkEq("29 while 空语句体", shapeOf("int main(){ int a; while (a) ; return 0; }"),
            "int main(){int a;while(a);;return 0;}");

    // 30：空块
    checkEq("30 空块", shapeOf("int main(){ { } return 0; }"),
            "int main(){{};return 0;}");

    // 31：一条 Decl 两个 VarDef
    const std::string s31 = "int main(){ int a, b = 2; return 0; }";
    ParseResult r31("<t>", s31);
    size_t decls = 0, defs = 0;
    for (const auto& it : r31.unit->items) {
      if (!it) continue;
      if (std::string_view(it->nodeKind()) == "FuncDef") {
        const auto& fn = static_cast<const FuncDef&>(*it);
        for (const auto& st : fn.body->items) {
          if (st && std::string_view(st->nodeKind()) == "Decl") {
            ++decls;
            defs += static_cast<const Decl&>(*st).defs.size();
          }
        }
      }
    }
    check(decls == 1 && defs == 2, "31 `int a, b = 2;` == 1 条 Decl + 2 条 VarDef");
    checkEq("31 紧凑形式", shapeOf(s31), "int main(){int a,b=2;return 0;}");

    // 32：嵌套 InitVal
    checkEq("32 嵌套 InitVal",
            shapeOf("int main(){ int a[2][3] = {{1,2},{3}}; return 0; }"),
            "int main(){int a[2][3]={{1,2},{3}};return 0;}");

    // 33：形参第一维为空
    checkEq("33 形参 `int a[]`", shapeOf("int f(int a[]) { return 0; }"),
            "int f(int a[]){return 0;}");

    // 34：void 无参无返回
    checkEq("34 `void f() { }`", shapeOf("void f() { }"), "void f(){}");
  }

  // —— 边界（prompt §7.1 第 6 条要求"在测试里固化 `!` 的处理"）——
  std::cout << "\n── 边界与 §7.1 第 6 条：`!` 的处理 ──\n";
  {
    // ★ 选择：语法层**统一接受** `!` 作为一元运算符（不区分 Cond 与 Exp），
    //   把"`!` 只能出现在 Cond 里"留给 S03 语义阶段报错。
    //   理由见 ParserStmtExpr.cpp 文件头；这两条断言把这个选择**钉死**。
    size_t errs = 0;
    const std::string got = shapeOf("int main(){ int x; int y = !x; return y; }", &errs);
    checkEq("! 在 Exp 里【被语法接受】（选择：留给 S03 报错）", got,
            "int main(){int x;int y=(! x);return y;}");
    check(errs == 0, "! 在 Exp 里不产生语法错误（errorCount == 0）");
    checkEq("! 在 Cond 里同样接受", shapeOf("int main(){ int x; while (!x) ; return 0; }"),
            "int main(){int x;while((! x));;return 0;}");
    checkEq("!!a 右结合（前缀）", exprShape("return !!a;"), "(! (! a))");
  }

  // —— 空实参表 / 空参数表（原型踩过的坑）——
  {
    checkEq("空实参表 f()", exprShape("return f();"), "f()");
    checkEq("空参数表 f(){}", shapeOf("void f(){}"), "void f(){}");
    checkEq("带逗号的实参", exprShape("return f(1);"), "f(1)");
    checkEq("多参数", shapeOf("int f(int a, int b){ return a; }"), "int f(int a,int b){return a;}");
    checkEq("二维形参 `int a[][5]`", shapeOf("int f(int a[][5]){ return 0; }"),
            "int f(int a[][5]){return 0;}");
  }

  // —— 字面量原文 ——（`%` 是原型踩过的第三个坑，这里一起固定）
  {
    checkEq("% 运算符", exprShape("return a % b;"), "(% a b)");
    checkEq("十六进制整数原文", exprShape("return 0x1F;"), "0x1F");
    checkEq("八进制整数原文", exprShape("return 010;"), "010");
    checkEq("十六进制浮点原文", exprShape("return 0x1.921fb6p+1;"), "0x1.921fb6p+1");
    checkEq("小写 e 指数原文", exprShape("return 1e-3;"), "1e-3");
    checkEq("`03.14159` 浮点原文", exprShape("return 03.14159;"), "03.14159");
    checkEq("点开头浮点原文", exprShape("return .5;"), ".5");
    checkEq("点结尾浮点原文", exprShape("return 1.;"), "1.");
  }

  // —— 空初始化列表 / 全局数组维度是表达式 ——
  {
    checkEq("空初始化 `{}`", shapeOf("int main(){ int a[4][2] = {}; return 0; }"),
            "int main(){int a[4][2]={};return 0;}");
    checkEq("维度是表达式（不求值）", shapeOf("int a[3 + 1][2];"),
            "int a[(+ 3 1)][2];");
    checkEq("局部 const", shapeOf("int main(){ const int a[2] = {1,2}; return a[0]; }"),
            "int main(){const int a[2]={1,2};return a(0);}");
    checkEq("return;", shapeOf("void f(){ return; }"), "void f(){return;}");
  }
}

// ============================================================================
// 四、§5 的例子逐字节比对
// ============================================================================
static void testSpecExample() {
  std::cout << "\n── prompt §5 的 --emit=ast 例子（逐字节）──\n";
  const std::string src = "int main() { return 1 + 2 * 3; }\n";
  const std::string want =
      "(CompUnit\n"
      "  (FuncDef main :int\n"
      "    (params)\n"
      "    (Block\n"
      "      (Return\n"
      "        (+\n"
      "          (IntLit 1)\n"
      "          (*\n"
      "            (IntLit 2)\n"
      "            (IntLit 3)))))))\n";

  ParseResult r("<spec>", src);
  const std::string got = sysy::printAst(*r.unit);
  checkEq("§5 例子逐字节相同", got, want);
  check(got.size() == want.size(),
        "§5 例子长度相同（" + std::to_string(got.size()) + " == " +
            std::to_string(want.size()) + "）");
  checkEq("紧凑形式", compactUnit(*r.unit), "int main(){return (+ 1 (* 2 3));}");
}

// ============================================================================
// 五、内部往返：printAst → parseAstText → printAst 必须逐字节相同
//
//   ⚠️ 这条只验证"同一进程内 printer 与 reader 互逆"，**不能**代替
//      check_parser.py 对 490 个真实文件的往返（那要跑 CLI）。
//      但它能在单测里立刻抓到"改了 printer 忘了改 reader"。
// ============================================================================
static void testInternalRoundTrip() {
  std::cout << "\n── 内部往返（printAst → parseAstText → printAst）──\n";
  const char* cases[] = {
      "int main(){ return 1 + 2 * 3; }",
      "int main(){ int a; int b; int c; return (a + b) * c; }",
      "int a[4][2] = {{1,2},{3,4},{},{7}};",
      "int f(int a[][5]){ return a[0][0]; }",
      "void f(){ return; }",
      "int main(){ int a; while (a) { if (a) break; continue; } return 0; }",
      "int main(){ int a; if (a) if (a) a = 1; else a = 2; return a; }",
      "int main(){ f(); return g(1, 2) + h(); }",
      "const float g = 1e-3; const int N = 5;",
      "int main(){ int a[2] = {}; return 0; }",
      "int main(){ int a; return - - !a; }",
      "int main(){ return 0x1.921fb6p+1; }",
  };
  for (const char* s : cases) {
    ParseResult r("<rt>", s);
    const std::string once = sysy::printAst(*r.unit);

    DiagnosticEngine d2;
    SourceFile src2 = SourceFile::fromString("<rt.ast>", once);
    d2.setSourceFile(&src2);
    auto unit2 = sysy::parseAstText(once, d2);
    const std::string twice = sysy::printAst(*unit2);

    checkEq(std::string("往返一致: ") + s, twice, once);
    check(!d2.hasError(), std::string("往返读回无诊断: ") + s);
  }
}

// ============================================================================
// 六、错误恢复：必须"报错误 + 继续 + 不崩"，且绝不静默通过
// ============================================================================
static void testErrorRecovery() {
  std::cout << "\n── 错误恢复（@ / tensor / 语法错误）──\n";

  auto errorsOf = [](const std::string& src) {
    ParseResult r("<err>", src);
    return r.parserErrors;
  };
  auto diagText = [](const std::string& src) {
    ParseResult r("<err>", src);
    std::ostringstream os;
    for (const auto& d : r.diags.all()) os << d.message << "\n";
    return os.str();
  };

  // `tensor` 在词法层是普通 Ident（D3：不设关键字、不预留标志位）
  const std::string t1 = "int main(){ tensor int a[4]; return 0; }";
  check(errorsOf(t1) > 0, "`tensor int a[4];` 报结构化语法错误");
  check(diagText(t1).find("expected") != std::string::npos,
        "tensor 的诊断是 expected ... 形式（结构化）");

  // `@` 已被 S01 报为 Invalid，语法器遇到它必须报错并恢复
  const std::string t2 = "int main(){ return 1 @ 2; }";
  check(errorsOf(t2) > 0, "`@` 报错误");
  check(diagText(t2).find("invalid character") != std::string::npos,
        "`@` 的诊断是 invalid character");

  // 多个错误一次报全（比赛的"准确识别、定位"要求）
  const std::string t3 = "int main(){ int ; return 0; } int f(){ }";
  check(errorsOf(t3) > 0, "多个语法错误能被报出");

  // 恢复必须**收敛**：畸形输入不能让解析器卡死或吃掉整个文件
  const std::string t4 = "int main(){ ((((((((( return 0; } int g(){ return 1; }";
  ParseResult r4("<err>", t4);
  check(r4.unit != nullptr, "深度/畸形输入下仍返回非空 CompUnit");
  check(r4.unit->items.size() >= 1, "畸形输入后仍能恢复出顶层项");

  // 空文件 / 只有注释
  checkEq("空文件", shapeOf(""), "");
  checkEq("只有注释", shapeOf("// hi\n/* there */\n"), "");
}


// ============================================================================
// 七、病态输入下的健壮性（★ 这一组是 S02 实测抓到 bug 之后补的回归）
//
//   三条独立的失效模式，各自由一个"必须成立"的性质钉住：
//     ① **一定前进**：错误恢复不得原地打转 —— `int main(){ return 0; }}`
//        多一个 `}` 曾让 parseCompUnit 的主循环死循环
//     ② **不递归析构**：`unique_ptr` 的析构是递归的，左结合长链（`1+1+1+…`）
//        建树只用循环、解析不占栈，但 `delete root` 会递归 20 万层 → 段错误
//     ③ **深度有界**：`{{{{…` / `((((…` 这类"每层几乎不耗 token"的输入
//        必须有哨兵，且超限后仍要把 token 推过去（否则第 ① 条又被破坏）
//
//   这些都**不是**语料里有的形态，但验收标准是"540 个文件全部不崩"，
//   而"不崩"这件事只有在病态输入上才有意义 —— 所以它们必须进单测。
// ============================================================================
// 把 s 重复 n 次（`std::string(n, s)` 只能重复**单字符**，这里要重复串）
static std::string rep(const char* s, size_t n) {
  std::string r;
  const size_t len = std::strlen(s);
  r.reserve(len * n);
  for (size_t i = 0; i < n; ++i) r += s;
  return r;
}

static void testPathologicalInputs() {
  std::cout << "\n── 病态输入健壮性（深度/长链/多余括号）──\n";

  struct Case {
    const char* name;
    std::string src;
    // 该输入**必须**报出诊断吗？
    //   ⚠️ 这一列是必须的，不许再靠名字去猜：原先的判据是"只有含 NUL 的那例要求报错"，
    //   于是"深度超限后静默丢掉上千个节点、退出码仍然是 0"这个真 bug
    //   从单测里漏了过去 —— 它是被一份**独立实现**的语法分析器交叉验证抓出来的。
    //   "合法但超出我们处理能力"的输入同样**必须报错**：静默产出错误的 AST 比崩溃更糟。
    bool expectError = true;
  };
  std::vector<Case> cases;
  cases.push_back({"多余右括号 100 个", "int main(){ return 0; }" + std::string(100, '}')});
  cases.push_back({"未闭合左括号 20 万个", "int main(){ return " + std::string(200000, '(') + ";"});
  cases.push_back({"未闭合左花括号 20 万个", "int main(){ " + std::string(200000, '{')});
  cases.push_back({"嵌套块 10 万层",
                   "int main(){ " + std::string(100000, '{') + std::string(100000, '}') +
                       " return 0; }"});
  cases.push_back({"一元链 5 万层", "int main(){ int a; return " + std::string(50000, '-') + "a; }"});
  cases.push_back({"二元链 3 万个 +（左倾 3 万层深的树）",
                   "int main(){ int a; a = 1" + rep("+1", 30000) + "; return a; }",
                   /*expectError=*/false});
  // ★ 深度上限的**边界**回归：3997 层合法且必须不报错；4001 层必须报错。
  //   两者只差 4 层，但修复前的行为是"4001 层静默成功"，所以这个边界必须钉住。
  cases.push_back({"嵌套块 3997 层（上限内）",
                   "int main(){ " + std::string(3997, '{') + std::string(3997, '}') +
                       " return 0; }",
                   /*expectError=*/false});
  cases.push_back({"嵌套块 4001 层（刚过上限）",
                   "int main(){ " + std::string(4001, '{') + std::string(4001, '}') +
                       " return 0; }",
                   /*expectError=*/true});
  cases.push_back({"初始化列表 10 万层",
                   "int a = " + std::string(100000, '{') + std::string(100000, '}') + ";"});
  cases.push_back({"赋值链 10 万层",
                   "int main(){ int a; " + rep("a = ", 100000) + "1; return a; }"});
  cases.push_back({"NUL 字节", std::string("int main(){ ") + '\0' + " return 0; }"});

  for (const Case& c : cases) {
    const auto t0 = std::chrono::steady_clock::now();
    ParseResult r("<pathological>", c.src);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ① 不卡死：解析必须在合理时间内返回（这里给 10 s 的宽松上限，
    //    真正要抓的是"死循环"，而那是无限时间）
    check(ms < 10000.0, std::string(c.name) + "：解析返回（" +
                            std::to_string(static_cast<int>(ms)) + " ms）");
    // ② 不崩：走到这里就说明没有段错误/栈溢出
    check(r.unit != nullptr, std::string(c.name) + "：返回非空 CompUnit");
    // ③ 该报错的必须报错（静默产出错误的 AST 比崩溃更糟，见 Case::expectError）
    if (c.expectError) {
      check(r.parserErrors > 0, std::string(c.name) + "：报出诊断");
    } else {
      check(r.parserErrors == 0, std::string(c.name) + "：合法输入不该报错");
    }
    // ④ 能打印（打印是另一条容易崩的路径：递归 + 巨大的缩进串）
    if (r.unit != nullptr) {
      const std::string txt = sysy::printAst(*r.unit);
      check(!txt.empty() && txt[0] == '(', std::string(c.name) + "：能打印出 AST 文本");
    }
  }
}

// ============================================================================
// 八、CLI 往返（调用真实编译器二进制；找不到就跳过）
// ============================================================================
static std::string shellQuote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  r += "'";
  return r;
}

static std::string readAll(const std::string& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *ok = false;
    return std::string();
  }
  std::ostringstream os;
  os << in.rdbuf();
  *ok = true;
  return os.str();
}

static bool fileExists(const std::string& p) {
  std::ifstream in(p);
  return static_cast<bool>(in);
}

static void testCliRoundTrip(const std::string& compilerBin, const std::string& workDir) {
  std::cout << "\n── CLI 往返（--emit=ast → --from-ast --emit=ast）──\n";
  if (compilerBin.empty() || !fileExists(compilerBin)) {
    std::cout << "  − 跳过：找不到编译器二进制（先 cmake --build compiler/build）\n";
    return;
  }
  const std::string sy = workDir + "/cli_rt.sy";
  const std::string a1 = workDir + "/cli_rt1.ast";
  const std::string a2 = workDir + "/cli_rt2.ast";
  {
    std::ofstream out(sy);
    out << "int g[3] = {1,{2},3};\n"
           "float h = 0x1.8p+2;\n"
           "int f(int a[], int n) { int i; int s = 0; while (i < n) { s = s + a[i]; "
           "i = i + 1; } return s; }\n"
           "int main() { int x; x = f(g, 3); if (x > 0) return x; else return -x; }\n";
  }
  const std::string c = shellQuote(compilerBin);
  const int rc1 = std::system((c + " " + shellQuote(sy) + " --emit=ast -o " + shellQuote(a1) +
                               " 2>/dev/null").c_str());
  check(rc1 == 0, "编译器对样本 .sy 退出码 0");
  const int rc2 = std::system((c + " " + shellQuote(a1) + " --from-ast --emit=ast -o " +
                               shellQuote(a2) + " 2>/dev/null").c_str());
  check(rc2 == 0, "--from-ast 退出码 0");
  bool ok1 = false, ok2 = false;
  const std::string s1 = readAll(a1, &ok1);
  const std::string s2 = readAll(a2, &ok2);
  check(ok1 && ok2, "两份 AST 产物都能读出来");
  checkEq("CLI 往返逐字节相同", s2, s1);
  check(!s1.empty() && s1[0] == '(', "产物以 '(' 开头（第一行就是根节点）");
  check(!s1.empty() && s1.back() == '\n', "产物以换行结尾");
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
  const std::string bin = (argc > 1) ? argv[1] : std::string();
  const std::string workDir = (argc > 2) ? argv[2] : std::string(".");
  std::cout << "═══ test_parser：SysY 语法分析器单元测试 ═══\n";

  testSpecExample();
  testTrackB();
  testInternalRoundTrip();
  testErrorRecovery();
  testPathologicalInputs();
  testCliRoundTrip(bin, workDir);

  std::cout << "\n────────────────────────────────────────────\n";
  std::cout << " 检查项 " << g_checks << " 个，失败 " << g_failed << " 个\n";
  if (g_failed == 0) std::cout << " ✔ test_parser 全部通过\n";
  else std::cout << " ✘ test_parser 有失败\n";
  return g_failed == 0 ? 0 : 1;
}
