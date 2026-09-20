// ============================================================================
// InitLowering —— AST 的初始化器 → **初始化计划**（S04 交付物 #2）
//
// ── 输入与前置条件（**重要**）─────────────────────────────────────────────
//   `runInitLowering` 的输入是**已经跑过 Sema** 的树：
//     * `VarDef::semType` 已填（声明类型）
//     * 初始化器里的隐式转换已经**物化成 `Cast` 节点**
//     * 初始化器的**结构**已经过 S03 的形状检查
//   理由（prompt §3.2 的注）：S03 已经把 `E-INIT-SHAPE` / `E-CONST-INIT` / `E-TYPE`
//   报完了，S04 **不重复报**。若 S04 再自己判一遍"合法性"，那就有两份真相，
//   而两份真相必然漂移（本项目已经栽过一次：S03 的 `arrayOf` 去重表键写错）。
//   ⇒ 本模块**信任树是合法的**，并在内部**处处带边界检查**（越界一律忽略），
//     于是遇到"Sema 已经报过错的树"也只是产出一份保守的计划，绝不崩。
//
// ── 它做什么 ──────────────────────────────────────────────────────────────
//   ① 按 C11 §6.7.9 的"当前对象"模型把 `InitVal` 树**展平**
//      （规范 §3 ConstDef 3 明写数组定义的语义与 C 相同；§3 ConstDef 6 用
//        七个例子把它钉死，包括 `{{},{3,4},5,6}` 与 `{1,2,{3},5,6}`）
//   ② 每个元素：能用 `ConstEvaluator` 求出常量 ⇒ 常量；否则 ⇒ 运行期表达式
//      （只有**局部**允许；全局必须是常量表达式，规范 §3 Initial Values 1）
//   ③ 变成 `InitPlan`：全局走"默认值 + 稀疏非零"，局部走"零填充 + 常量段 +
//      逐元素表达式"（prompt §4.2 的规模策略表）
//
// ── ★ 规模是正确性的一部分（prompt §二）──────────────────────────────────
//   语料实测：6 个文件各有 2×864 MB 的全局数组；`int a[30000010]`、`int
//   buffer[50000000] = {}`。因此本模块：
//     * **绝不按元素物化**任何对象；常量值只存在于
//       `nonzero`（稀疏表）与"连续常量段"（≤ 一段）里；
//     * 转储的体量由"非零元素个数"决定，**不由数组长度决定**。
//
// ── 遍历必须是**显式工作栈**（prompt §九）─────────────────────────────────
//   S02/S03 的教训：打印器与读取器都栽在递归上。所以：
//     * `InitVal` 树的展平用显式栈（`{{{{…}}}}` 的病态输入不崩）
//     * 顶层/函数体/语句的遍历用显式栈（4000 层块不崩）
//   唯一的递归是 `evalConst`/`exprIsConst` 里的**表达式**遍历 —— 它们
//   与 `ConstEvaluator::eval` 同构（显式栈），不是调用栈递归。
// ============================================================================
#ifndef SYSY_FRONTEND_INITLOWERING_H
#define SYSY_FRONTEND_INITLOWERING_H

#include <string>

#include "frontend/Ast.h"
#include "frontend/InitPlan.h"

namespace sysy {

class ConstEnv;

// 【前置】unit 已跑过 `runSema`（`VarDef::semType` 已填）。
//   ⚠️ 而且 env 必须**在 Sema 结束之后仍然能查到符号常量**：本模块要再求值
//      一遍初始化器（判断"常量还是运行期"、算出常量值），它依赖
//      `ConstEnv::findConst`。`Sema` 的 `run()` 结束时**不会**清掉全局层，
//      就是为了这个（见 Sema.cpp 末尾的说明）。
// 【后置】返回一个完整的计划：全局对象按源码顺序、局部对象按源码顺序；
//         每个带初始化器的 VarDef 恰好产出一条记录（局部用 `函数名/变量名`）。
//         不抛异常、不报诊断、不修改 unit。
InitPlan runInitLowering(const CompUnit& unit, const ConstEnv& env);

// ── `tools/selftest/unit/test_initlowering.cpp` 与调试用的可读摘要 ─────────
// 【后置】返回人类可读的多行文本（**不是** `--emit=initplan` 的冻结格式，
//         那份在 InitLoweringDump.h）。每个对象一行：名字、类型、动作数。
std::string initPlanSummary(const InitPlan& plan);

}  // namespace sysy
#endif  // SYSY_FRONTEND_INITLOWERING_H
