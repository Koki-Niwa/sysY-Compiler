// ============================================================================
// SemaDump —— `--emit=sema` 的类型注解转储（S03 交付物 #5）
//
// 格式是 `docs/prompts/S03-01-任务prompt.md` §五 冻结的**对外契约**。
// 本文件只提供"注解策略"（`SemaAnno`）与 `(RuntimeLib ...)` 头，
// **布局本身**在 AstSexpLayout.h 的 `SexpLayout` 里 —— 那份布局同时服务
// `--emit=ast`，于是"只做加法"是结构上成立的（prompt §一）。
//
// ── 记号拼写（**照 §五 的例子，别自己发明**）────────────────────────────
//   (a) `(VarDef N :t int ...)` / `(Param a :t int[] (Dim))`
//   (c) `(LVal a :obj int[] (LVal i :obj int :int) :int)`
//   (b) 表达式的值类型：
//        * `IntLit` / `FloatLit` / `LVal` / `Call` / `Cast` ⇒ 在**末尾**
//          （全部子节点之后、右括号之前）
//        * `Binary` / `Unary` ⇒ 在运算符**之后**（§五 的例子里写的是
//          `(< :int` 与 `(+ :float`）
//   (d) `(Cast :IntToFloat :float <子节点> :float)`
//       —— 中间是目标类型，末尾是它自己的值类型，两者必须相同
//
//   ⚠️ **与 prompt 的冲突（已在报告里写明）**：§五(b) 与 §七 第 1 条都写
//      "值类型记号在末尾"，但 §五 的"完整例子"（§十② 要求逐字节一致）把
//      二元/一元节点的类型记号放在**运算符之后**。两者不可兼得；本实现按
//      **例子**（它是可执行的判据，而规则文字不是），检查器也按同一规则校验。
//
//   `Dim` / `Decl` / `Block` / `While` / `If` / `Return` 等**非表达式**节点
//   不加类型记号；`FuncDef` 的返回类型是 S02 已有的记号，不再加 `:t`。
// ============================================================================
#ifndef SYSY_FRONTEND_SEMADUMP_H
#define SYSY_FRONTEND_SEMADUMP_H

#include <string>

#include "frontend/Ast.h"

namespace sysy {

// 【前置】无（tree 可以来自 Sema 之前，此时注解为 `:?`）。
// 【后置】返回 `--emit=sema` 的完整文本：第一行固定是 `(RuntimeLib`，
//         其后 13 个 `RuntimeFunc`，然后是 Sema 之后的 `(CompUnit ...)`。
//         以 '\n' 结尾。不抛异常、不修改 tree。
std::string printSemaDump(const CompUnit& unit);

// ============================================================================
// ★ S04 增加的两个**加法**接口（`--emit=initplan` 复用同一份打印器）
//
//   prompt §4.3 要求 `--emit=initplan` 里 `StoreExpr` 的子节点"沿用
//   `--emit=sema` 的表达式格式（**同一个打印器、同一套类型记号**）"。
//   把布局复制一份出来是最糟的做法（两份真相必然漂移），所以这里把
//   `SemaAnno` 的两个部件**导出**给 InitLoweringDump 用：
//
//     * `appendValueTypeAnnotation` —— 某种表达式节点的类型记号写法
//     * `printExprBody`             —— 一棵式子树的正文（不含节点头与注解）
//
//   两处都是**只读**：`printSemaDump` 与 `--emit=ast` 的输出逐字节不变。
// ============================================================================

// 【前置】无。
// 【后置】按 `--emit=sema` 的写法，把表达式节点 e 的**值类型记号**追加到 out。
//         （`IntLit`/`FloatLit`/`LVal`/`Call`/`Cast` 写在末尾，`Unary`/`Binary`
//          写在运算符之后 —— 顺序由调用方决定，本函数只管记号本身。）
void appendValueTypeAnnotation(std::string& out, const Expr& e);

// 【前置】indent >= 0。
// 【后置】把 `(RuntimeLib ...)` 整块（13 个运行时函数各一行）追加到 out：
//         首行缩进 indent 个空格，`RuntimeFunc` 各行的缩进是 indent + 2，
//         **不含结尾换行**。与 `printSemaDump` 用的是同一份实现 ⇒ 两个 emit
//         的这 13 行（在缩进之外）逐字节相同（`--emit=initplan` 的契约要求）。
void appendRuntimeLibBlock(std::string& out);

// 【前置】无（e 可以在 Sema 之前或之后）。
// 【后置】返回 e 的**子树正文**（用与 `--emit=sema` 相同的布局与记号渲染）：
//         不含节点头、不含 e 自己的类型注解，但**含**e 自己的右括号与所有
//         子节点的完整文本，以换行结尾。`singleLine = true` 时折叠成一行
//         （无换行）。不抛异常、不修改 e。
std::string printExprBody(const Expr& e, bool singleLine = false);

}  // namespace sysy
#endif  // SYSY_FRONTEND_SEMADUMP_H
