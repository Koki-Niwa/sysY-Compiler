// ============================================================================
// AstPrinter —— AST 的 S-表达式打印器 + 读取器（S02）
//
// ── 这一对接口**严格互逆**，所以声明放在一起（实现分在两个 .cpp）──────────
//
//   ① printAst()      AST  →  S-表达式文本（`--emit=ast` 的产物）
//   ② parseAstText()  S-表达式文本 → AST   （`--from-ast` 的实现）
//
//   ⚠️ 为什么必须成对出现（phases/S02-parser.md §6.1）：
//      轨 A（往返验证）的不变式是
//          source --emit=ast--> A.ast --from-ast --emit=ast--> A2.ast
//          要求 A.ast 与 A2.ast **逐字节相同**
//      若两者不严格互逆，往返就会失败，而失败原因可能是"打印器丢了个字段"
//      （那正好是我们要抓的 bug！）。所以这两个函数是同一份格式契约的两面，
//      **声明**在同一个头文件里（本文件），而**实现**按"打印 / 读取"分在
//      AstPrinter.cpp 与 AstReader.cpp 两个 .cpp 里，两边共用的记号表在
//      AstSexpFormat.h —— 改一个必须同时改另一个（检查器是 check_parser.py）。
//
//   ⚠️ 已知的**盲点**（必须写进报告，prompt §6.3）：
//      往返**不能**发现"优先级理解错了"。若 parser 把 `a+b*c` 错解析成
//      `(a+b)*c`，打印器会忠实打印 `(* (+ a b) c)`，重读又得到同一棵树
//      ⇒ 往返照样通过。抓到这类错误只能靠 test_parser.cpp 的树形断言（轨 B）。
//
//   ⚠️ **两侧的深度约束不对称**（"严格互逆"≠"对称"，别照抄另一侧的做法）：
//       * 打印器 printAst() 的深度 = **AST 深度**，与输入文本无关 ⇒ 已改成
//         显式工作栈的**迭代**遍历（内存 O(树深)），**不受调用栈限制**：
//         实测 6 万层 `+1` 链在默认 8 MB 栈下 rc=0（同一份输入在递归版下
//         rc=139），1 MB 栈也照样跑完。它剩下的唯一限制是**内存**：
//         输出体积 ≈ **2.008 × n²**（n = 链长；每行缩进 2·深度 ⇒ 平方增长，
//         实测 n=1000/2000/4000 → 2.03/8.07/32.1 MB）。见 AstPrinter.cpp 文件头。
//       * 读取器 parseAstText()（--from-ast）**仍然是递归的**
//         （SexpReader::parseForm / Builder::buildXxx / Sexp 的析构链）
//         ⇒ 它的上限是**调用栈**，靠一个**实测定标**的哨兵
//         （kMaxNestingDepth = 8100）把"段错误"换成"一条 error + 非零退出码"。
//       ⇒ 两侧的上限来源不同（内存 vs 调用栈），阈值也各自独立；
//         想抬高读取器那一侧只能把它的递归也迭代化。详见 AstReader.h。
//
// ── 格式（prompt §5）─────────────────────────────────────────────────────
//
//   * **一行一个节点**，子节点比父节点多缩进 2 空格
//   * 运算符用原文（`+` `-` `*` `/` `%` `<` `>` `<=` `>=` `==` `!=` `&&` `||` `!`）
//   * 字面量带原文（`(IntLit 1)` / `(FloatLit 1e-3)`）
//   * LVal 带名字与下标；Call 带被调名与实参
//   * 结尾的右括号**全部收在最后一行**（与 prompt §5 的例子逐字节一致）
//
//   示例：`int main() { return 1 + 2 * 3; }`
//
//     (CompUnit
//       (FuncDef main :int
//         (params)
//         (Block
//           (Return
//             (+
//               (IntLit 1)
//               (*
//                 (IntLit 2)
//                 (IntLit 3)))))))
//
// ── 无歧义性（这是轨 A 有意义的前提）────────────────────────────────────
//
//   ① 括号配对本身表达了树形；
//   ② 每个节点的**子节点个数由节点类型唯一决定**（不是靠缩进猜的）；
//   ③ 所有"关键字/类型/修饰"记号一律带 `:` 前缀（`:int` `:float` `:void`
//      `:const` `:param`），而 `:` 在 SysY 标识符里是非法字符
//      ⇒ 任何标识符都不可能被误认成关键字（实测语料 1569 个标识符全不含 `:`）；
//   ④ 空维打印成显式的 `(Dim)` ⇒ `[]` 与"没有这一维"不会混。
//   ⑤ 空初始化列表 `{}` 打印成 `(InitVal)`；标量初始化打印成
//      `(InitVal <expr>)` ⇒ 两者不会混（`= {}` 与 `= {0}` 语义不同）。
// ============================================================================
#ifndef SYSY_FRONTEND_ASTPRINTER_H
#define SYSY_FRONTEND_ASTPRINTER_H

#include <memory>
#include <string>

#include "frontend/Ast.h"

namespace sysy {

class DiagnosticEngine;

// 【前置】unit 非空。【后置】返回该编译单元的 S-表达式文本（**以 '\n' 结尾**）。
// 不抛异常、不修改 unit。
std::string printAst(const CompUnit& unit);

// 【前置】无。【后置】把 S-表达式文本读回成一棵编译单元树。
//   * 语法错误 → 报诊断（经 diag）并尽量恢复；**仍返回非 nullptr**（可能是空的）
//   * 读取器**本身不接触 Lexer**：AST 文本不是 SysY（prompt §6.1 的明确建议）
//
// 【重要前置条件 · 生命期】text 必须比返回的 AST 活得久：
// 字面量节点（IntLit/FloatLit）的 text 字段是**指向 text 内部的 string_view**
// （与 Parser 指向 SourceFile::text() 的做法完全一致，零拷贝）。
// 典型用法就是把 SourceFile::text() 传进来 —— 它的生命期覆盖整个 main。
std::unique_ptr<CompUnit> parseAstText(const std::string& text, DiagnosticEngine& diag);

}  // namespace sysy

#endif  // SYSY_FRONTEND_ASTPRINTER_H
