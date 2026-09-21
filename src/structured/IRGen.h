// ============================================================================
// IRGen —— AST（Sema 之后）+ InitPlan → 结构化 IR（S05 交付物 #3）
//
// 语义规格：prompt §五 的 12 条（每条都有出处）。本文件只做"逐条降级"，
// **不做任何优化**（§十一 第 4 条：不做常量传播、不做 DCE、不做循环变换）。
//
// ── 输入的所有权与前置条件 ────────────────────────────────────────────────
//   【前置】unit 已跑过 `runSema`（Expr::type / LVal::objType / VarDef::semType
//           / Param::semType 已填、隐式转换已物化成 `Cast` 节点），并且
//           `plan` 是这台 unit 的 `runInitLowering` 产物。
//   ★ **只读**这些字段，不重新推导类型、不重新解释初始化语义（硬约束第 3 条）。
//
// ── 铁律 1（前端禁止值传播）在这里的落点 ──────────────────────────────────
//   每一次**变量读出**都发一条 `LoadOp`：`int x = 1; return x;` 产出
//   `Alloca/Store 1/Load/Return %load`，**绝不**把 `Int 1` 直接喂给 return。
//   自研 mem2reg(S09) 的输入就是这些 load；前端"顺手优化"掉一个 load，
//   就等于把那条主线做废。`test_structured.cpp` 里有一条形状断言钉死它。
//
// ── 遍历策略：按"语句树"递归（**有正当理由**，不是疏忽）─────────────────
//   S02/S03/S05 反复强调"遍历必须显式工作栈"，这里为什么可以用递归？
//   因为**被递归的结构是语句的嵌套深度**，而不是表达式的链长：
//     * 表达式：**迭代求值**（`genExpr` 用显式帧栈），`1+1+…`（3 万个 `+`）
//       与 `86_long_code2`（树深 4007）走的是这里，绝不递归；
//     * 语句：递归深度 = 块/if/while 的**嵌套层数**，而 Parser 的
//       `kMaxDepth` 已经把它限制在 4000 以内（且语料实测最深是几百）。
//   为"语句嵌套"再写一套显式栈的代价是**可读性**（控制流生成的状态机很难
//   对着 prompt §五 逐条核对），而收益是零 —— 真正深的是表达式，那部分是迭代的。
//   ⇒ 这是一个**刻意的、写在报告里的**决定，不是忘了 S02 的教训。
// ============================================================================
#ifndef SYSY_STRUCTURED_IRGEN_H
#define SYSY_STRUCTURED_IRGEN_H

#include <string>

#include "structured/StructuredIR.h"
#include "support/SourceLoc.h"

namespace sysy {

struct CompUnit;
struct InitPlan;
class DiagnosticEngine;

namespace sir {

// 【前置】unit 已跑过 Sema；plan 是同一棵树的 InitLowering 产物。
// 【后置】返回 ModuleOp（`kind == OpKind::Module`；attrs = [源文件基名]；
//         regions = [模块 Region]）。模块 Region 里先全部 `GlobalVarOp`、
//         再全部 `FuncOp` —— 「所有全局在所有函数之前」是**可断言的顺序性质**
//         （轨 B 会查），因为下游按这个顺序分配数据段。
//         遇到"上游已经报过错"的残缺树时**不抛异常**：产出保守的 IR，
//         `diag` 里补充说明（任何"超限就放弃"的分支必须先报 error，§九.6）。
Op* buildModule(Arena& arena, const CompUnit& unit, const InitPlan& plan,
                const std::string& sourceBaseName, DiagnosticEngine& diag);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_IRGEN_H
