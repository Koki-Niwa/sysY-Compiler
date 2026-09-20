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

}  // namespace sysy
#endif  // SYSY_FRONTEND_SEMADUMP_H
