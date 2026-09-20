// ============================================================================
// InitLoweringDump —— `--emit=initplan` 的转储（S04 交付物 #3）
//
// 这是**冻结的对外契约**（prompt §4.3）。格式与 `--emit=ast` / `--emit=sema`
// 同族：S-表达式，一行一个节点，缩进 2 空格。骨架照抄 §4.3 的示例：
//
//     (InitPlan
//       (RuntimeLib
//         ...)                                  ; 与 --emit=sema 的 13 行逐字节相同
//       (Global x :t int[600][600][600] :zero)
//       (Global a :t int[3][2] :data
//         (0 1) (4 2) (8 3) (12 4) (16 5) (20 6))   ; 偏移(字节) 值，升序，只列非零
//       (Func main
//         (Local arr :t int[10] :actions
//           (Zero 0 40)
//           (StoreConst 0 :int 1)
//           (StoreExpr 4 :int
//             (LVal k :obj int :int))
//           (MemcpyConst 8 :int
//             3 4 5))))
//
// ── 规则（§4.3，逐条）────────────────────────────────────────────────────
//   * `Global` 与 `Local` 的顺序 = 源文件中的声明顺序；局部用 `函数名/变量名`
//     （`(Local main/arr ...)`），同名遮蔽时也分得开。
//   * 字节偏移一律**十进制无符号**；浮点常量打印成 `:float` 加**原文**
//     （沿用 `FloatLit::text`）。
//   * `:data` 里**不出现零值**；`:zero` 就是"整片为零"，两者互斥。
//   * `StoreExpr` 的子节点沿用 `--emit=sema` 的表达式格式（**同一个打印器、
//     同一套类型记号** —— 见 SemaDump.h 的 `printExprBody`/`appendValueType
//     Annotation`，本文件一个记号都不自己拼）。
//   * 转储**不要求**能被读回（单向视图）。
//
// ── 两处我做了决定的地方（报告 §4 有交代）────────────────────────────────
//   ① `StoreExpr` 的动作行**折叠成一行**：`(StoreExpr 4 :int (LVal k :obj int :int))`。
//      理由：§4.3 把它画成"动作行 + 子节点行"，而 §十⑥ 又要求单文件转储
//      < 2 MB；对一棵深表达式树（语料里树深到 4007），多行写法会让一个动作
//      占几千行。折叠用的是**布局引擎自己的单行模式**，不是另写一份打印器。
//   ② 顶层 `(Global ...)` 直接挂在 `(InitPlan` 下（§4.3 的示例就是这样），
//      函数用 `(Func <名字> ...)` 分节。两者**都按源码顺序**出现，所以
//      "Global 与 Local 的顺序与源文件一致"在跨函数时也成立。
//
// 【前置】plan 由 `runInitLowering` 产出，且与 unit 来自同一棵树
//         （`StoreExpr::expr` 是指向它的**借用**指针）。
// 【后置】返回完整文本，以 '\n' 结尾；不抛异常、不修改任何输入。
// ============================================================================
#ifndef SYSY_FRONTEND_INITLOWERINGDUMP_H
#define SYSY_FRONTEND_INITLOWERINGDUMP_H

#include <string>

#include "frontend/InitPlan.h"

namespace sysy {

std::string printInitPlanDump(const InitPlan& plan);

}  // namespace sysy
#endif  // SYSY_FRONTEND_INITLOWERINGDUMP_H
