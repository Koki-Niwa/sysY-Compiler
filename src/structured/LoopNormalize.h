// ============================================================================
// LoopNormalize —— 结构化层的**第一个变换**（S05b 交付物 #1，规格见
// `docs/handoff/03-设计/结构化IR设计.md` §1.3 / §3.1① 与 S05b prompt §三）
//
//   处理顺序**不能反**（§3.1）：
//     ① `continue` 消解   ② IV 识别 + 每条路径自增证明
//     ③ 边界归一化        ④ 参数符号提升   ⑤ 构造 `ForOp`
//   ①②③④ 在本文件里是**同一次遍历里的连续判定**，⑤ 是唯一的改写动作。
//
// ── 这是本项目第一个**变换**（前六关全是降级）────────────────────────────
//   降级的判据是"有没有丢东西"，而变换的判据只有一个：**行为不变**。
//   所以本 pass 的纪律是**宁可不动**：
//     * 成功条件（四条）**全**满足才改写；
//     * 任一条不满足 ⇒ **保留 `WhileOp`、零改动**（安全降级，不报诊断）；
//     * 但"没规范化"必须**可观测**（`--dump-loopnorm-stats` 打印原因直方图）
//       —— "静默不规范化"和"静默截断"是同一类问题（S02/S04 各栽过一次）。
//
// ── ★ `continue` 消解的规范形式（**这是本关唯一没有唯一答案的地方**）─────
//   为什么必须写下来：轨 D 是"独立实现的第二份规范化器"，它按 prompt §三 自己
//   推导这个形式。若这里不写明，两份实现必然在"用 then 还是 else 装剩余体"
//   上分叉 —— 那不是它错了。
//
//   规范形式（**字面照 prompt §3.2**）：`continue` 归一化为 `if (!cond) { B }`
//   的**嵌套**，即把 `continue` 之前的部分包进条件分支：
//
//     IRGen 的降级（S05）把 `continue` 与 `break` **都**降成 `BreakOp`，所以
//     "这段 Body 里的 `BreakOp` 分别是什么"由**形状**决定：
//
//       Body = [ P1, (If C) { A1 } { A2 }, P2, (Break) ]        ← 源码里的 continue
//                                   ^^^^ 真 then/else 分支（A1/A2）
//         ↓ 消解（规定：**剩余体进 else，then 只留 `(Yield)`**）
//       Body = [ P1, (If C) { (Yield) } { A1, P2 } ]
//       若 A2 非空：Body = [ P1, (If C) { (Yield) } { A2 } , (If !C) { (Yield) } { P2 } ]
//
//   三条必须与轨 D 对齐的细节：
//     ① **剩余体进 `else`，`then` 只留 `(Yield)`** —— 不是 `if (!C) { B }` 的
//        "then 装 B"写法。我们照 prompt 的字面（`if (!cond) { B }`）取
//        **`else` 装 B** 的实现，`then` 留一个空的 `(Yield)` 以满足 I4。
//     ② 反转条件用 `Eq C 0`（**新发射一条 `Int 0`**），不是"交换比较谓词"
//        —— 后者要理解 `Lt`/`Le`/… 的补运算，属于算术改写（§十一.3 禁止）。
//        条件为 false 时 `Eq C 0` 为真 ⇒ 走 `then` ⇒ `Yield` ⇒ 等价于 continue。
//     ③ `Body` **最后一个** Op 是 `(Break)`（Region 末尾）时，它是"尾部
//        continue"（IRGen 对 `i = i + 1; continue;` 也产出这个形状）⇒ 直接换成
//        `(Yield)`，**不新建 `IfOp`**（没有剩余体可包）。
//
// ── `break` 的处置（prompt §3.3）──────────────────────────────────────────
//   设计文档 I3 要求 `ForOp` 体内没有 `break`。所以**含 `break` 的 `while`
//   一律不规范化**（第一版不留"可数 break"的口子）。判定用形状：
//   `BreakOp` 出现在**非尾部位置**（它所在 `Region` 里它之后还有 Op，或它的
//   父 Op 不是体/`IfOp`）⇒ 那是真 `break` ⇒ 整个循环放弃。
//   `BreakOp` 在体 `Region` 的最后一行 ⇒ 它是"尾部 continue"（见上）。
//
// ── 成功条件（四条，**全**满足才改写，prompt §3.3）───────────────────────
//   1. 体内 IV 恰好**每条路径** `+1`（`i = i + 1`，或"两个分支都以 `i = i + 1`
//      收尾"的 `IfOp`）—— 这是 `ForOp` 的 `<step>` 的唯一来源；
//   2. 边界**仿射**（`LtOp(iv, B)`，且 `B` 是"循环不变"的纯表达式：只读
//      循环外定义的槽/常量/参数，体内不写它，体内无 `CallOp`）；
//   3. 体内**无 `break`**；
//   4. 条件形如 `LtOp(iv, bound)`。
//
// ── 为什么"体内无 `CallOp`"是**边界仿射**的一部分 ────────────────────────
//   边界从"每次迭代求值"变成"循环外求值一次"，这要求它是循环不变的。
//   函数调用**可能通过指针形参写任何东西**（我们没有别名分析，S13 才有）
//   ⇒ 只要体内有调用，"这个边界不被写"就**证不出来** ⇒ 安全降级。
//   同理，边界表达式自身也不许含 `CallOp`（求值次数从 N 次变 1 次，
//   有副作用的表达式不能动）。
//
// ── 本 pass 幂等（§C3）────────────────────────────────────────────────────
//   `run(run(X)) == run(X)`：`ForOp` 不是本 pass 的输入（只认 `WhileOp`），
//   第二次运行看到的是"已经规范化过 + 剩下的 `WhileOp`"，后者这次仍然不满足
//   成功条件 ⇒ 逐字节零改动。`--from-structured` 路径上有关卡级的实测。
// ============================================================================
#ifndef SYSY_STRUCTURED_LOOPNORMALIZE_H
#define SYSY_STRUCTURED_LOOPNORMALIZE_H

#include <cstdint>
#include <string>
#include <vector>

#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {

// 未规范化的**原因分类**（prompt §六 轨 C 要求的原因直方图就是按它统计的）。
//   顺序 = 判定顺序，`formatLoopNormStats` 的直方图按这个顺序打印（**稳定**）。
enum class SkipReason : uint8_t {
  CondNotLt = 0,      // 条件不是 `LtOp(iv, bound)`（含 `<=`/`!=`/`&&`/读数组…）
  IvNotIdentified,    // 条件左侧不是"标量槽的 Load"（或该槽不是函数入口的 Alloca）
  NonAffineBound,     // 边界不仿射/非循环不变（表达式含 Call/Store，或边界就是 IV）
  BoundWrittenInBody, // 边界读的某个槽在体内被写 ⇒ 提到循环外会改变语义
  PathStepFail,       // 体内 IV 不是"每条路径恰好 +1"（含"体内根本不写 IV"）
  IvWrittenUnmodeled, // 体内还有**未被识别为步进**的对 IV 槽的访问
  HasBreak,           // 体内有真 `break`（I3：`ForOp` 体内不许有 break）
  CallInBody,         // 体内有 `CallOp` ⇒ "边界不被写"证不出来（见文件头）
  Count
};

// 【后置】原因的分类名（用于直方图与 `--dump-loopnorm-stats` 的文本）。
const char* skipReasonName(SkipReason r);

// 一次规范化运行的统计（**"没规范化"的痕迹就存在这里**，prompt §3.3）。
struct LoopNormStats {
  size_t whileSeen = 0;       // 扫描到的 `WhileOp` 总数（分子分母的口径）
  size_t forBuilt = 0;        // 升为 `ForOp` 的个数
  size_t continuesResolved = 0;   // 消解掉的 `continue` 个数（新发射的 `IfOp`-包装）
  size_t tailContinues = 0;       // 其中"尾部 continue"（`BreakOp` → `YieldOp`）
  size_t skip[static_cast<size_t>(SkipReason::Count)] = {};
  size_t rolledBack = 0;      // 因"有循环被跳过"而整模块回滚的次数（0 或 1）

  // 逐条明细（函数名 / 源码行 / 原因）—— 直方图之外还要能定位到具体那个循环。
  struct Detail {
    std::string func;
    uint32_t line = 0;
    SkipReason reason = SkipReason::CondNotLt;
  };
  std::vector<Detail> skipped;

  // 【后置】记一次"跳过"（自增直方图 + 追加明细）。
  void noteSkip(const std::string& fn, SourceLoc loc, SkipReason r);
};

// 【前置】module == nullptr 或 module->kind == OpKind::Module。
// 【后置】**返回（可能重建的）ModuleOp** —— 调用方必须用它替换原来的指针。
//         每个满足成功条件的 `WhileOp` → `ForOp`；不满足的**逐字节零改动**
//         （见下面的回滚说明）。遍历是**显式工作栈**（prompt §五.3）。
// 【副作用】改写 Op 树；把统计写进 stats（**新 Op 从 `arena` 分配**）。
// 【幂等】run(run(X)) 的 dump == run(X) 的 dump（见文件头）。
//
// ── ★ 为什么返回新指针：失败必须**逐字节零改动**（prompt §六 轨 C.3）──────
//   `continue` 消解**会改写体**（把 `BreakOp` 的形状写成等价的 `IfOp`）。
//   "先消解 → 判定失败 → 保留 `WhileOp`" 会让那个文件的 dump 改变 ⇒
//   "一个循环都没规范化的文件必须逐字节相同"这条判据假红。
//   `continue` 消解**没有便宜的逆操作**（它可能插入 `IfOp` 并搬走一串 Op），
//   所以本 pass 的处置是**整模块回滚**：
//     * 进入时先把模块 dump 成文本（`dumpModule`，纯函数）；
//     * 若有**任何一个**循环被跳过（`stats.skipped` 增加），就把那份文本
//       **读回**（`parseStructuredModule`）得到一个**全新的、逐字节等价的**树，
//       并把它作为返回值 —— 旧树交给 Arena（不释放，但也不再用）。
//   ★ 为什么读回是**忠实**的：`--from-structured` 的往返判据（轨 A）就是
//     "dump → 读回 → 再 dump 逐字节相同"，490+ 个文件在 S05 与 S05b 都验过。
//   ★ 代价：只有"存在被跳过的循环"的文件才付一次 extra dump+parse
//     （实测全量 540 个文件 < 3 s，见报告 §8）。
Op* normalizeLoops(Op* module, Arena& arena, LoopNormStats& stats);

// 【后置】把统计渲染成"人可读 + 机器可 grep"的多行文本（`--dump-loopnorm-stats`）。
//         格式是**新的对外可见输出**（不写进任何冻结的 dump）。
std::string formatLoopNormStats(const LoopNormStats& s);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_LOOPNORMALIZE_H
