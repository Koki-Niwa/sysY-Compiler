// ============================================================================
// AllocaHoist —— 把每个函数的 `AllocaOp` 全部提升到**入口 Region**（S05b 交付物 #2）
//
// 规格来源：`docs/handoff/03-设计/架构设计-前中端.md` §4.8
// （"审计发现的假不变量"）+ AGENT-CONTEXT §5 的**后端不变量 2**。
//
// ── 为什么必须做（这是后端简化栈帧管理的前提）────────────────────────────
//   后端依赖"栈帧一次性布局"：所有 `alloca` 在函数入口 ⇒ 一条 `sub sp, #N`
//   搞定。若循环里能出现 `alloca`，后端**必须实现动态栈调整**（每轮迭代
//   `sub sp`），复杂度大增且性能差。
//
// ── 为什么这样做是安全的（**这条决定敢做的全部理由**）────────────────────
//   * `alloca` **只分配、不初始化** ⇒ 移动它不改变任何可观测状态；
//   * SysY **没有 VLA**（数组维度是编译期常量）、**没有 `goto`**、
//     **没有取地址运算** ⇒ 变量的作用域由 Sema 保证，与**内存位置**无关；
//   * 局部变量的**初始化仍然留在声明点**（`store` 一个都不动）——
//     只有 `alloca` 本体上移。
//
// ── 稳定性要求（prompt §3.5）──────────────────────────────────────────────
//   ① **保持原有相对顺序**（同一函数内 alloca 的相对次序不变 —— 它是
//      数据布局的可复现前提）；
//   ② **不跨函数**（每个函数各自提升进自己的入口 Region）；
//   ③ 提升是一次**稳定分区**：`alloca* ++ 其余`，不是"任意重排"。
//
// ── 它在本项目的现状（**必须如实说明**）──────────────────────────────────
//   `IRGen`（S05）**已经**把所有 `alloca` 写在入口 Region
//   （`Gen::emitAlloca` 的注释与 `StructuredVerifier` 的检查都在那里），
//   所以对当前前端产出的 IR，本 pass 是**恒等变换**（`moved == 0`）——
//   这正是"不触发则零改动"能成立的原因。
//   它仍然是**真实现**而不是空壳：跨函数、嵌套深、`alloca` 在深层 `Region`
//   的 IR 都会被它搬到入口（`unit/test_loopnorm.cpp` 用手工构造的 IR 钉死；
//   `check_loopnorm.py` 的轨 F 会**真的去数**每个函数的 alloca 位置）。
//   保留它的第二个理由：`--normalize` 是对外开关，后端契约必须由**代码**
//   保证，而不是由"IRGen 现在恰好这么做"这个**此刻为真的事实**保证。
// ============================================================================
#ifndef SYSY_STRUCTURED_ALLOCAHOIST_H
#define SYSY_STRUCTURED_ALLOCAHOIST_H

#include <cstddef>

#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {

// 【前置】module == nullptr 或 module->kind == OpKind::Module。
// 【后置】每个 `FuncOp` 的**入口 Region** 前部是"该函数全部 `AllocaOp`"
//         （按它们在函数内**先序**出现的原顺序），其后是其余 Op（原顺序）。
//         非 `FuncOp` 的 Region 一个字节都不动。
// 【副作用】改写 Op 树（只改 Region 的顺序，不新建/删除任何 Op）。
// 【幂等】已经满足时是恒等变换（`moved == 0`、dump 逐字节不变）。
size_t hoistAllocas(Op* module);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_ALLOCAHOIST_H
