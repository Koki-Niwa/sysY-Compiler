// ============================================================================
// LoopAnalysis —— `while` → `for` 的**判定**（S05b 交付物 #1 的判定半）
//
//   与 `LoopNormalize.cpp`（改写半）分开的理由：§C4 的单文件 600 行硬上限，
//   而且"判定"与"改写"是两件事 —— 两趟结构（干跑 + 真跑）要求判定逻辑
//   **一份代码**，改写只在真跑时发生。
//
// ── 只读！本文件的函数**绝不修改 IR**（改写全在 LoopNormalize.cpp）─────────
//
// ── 成功条件（四条，**全**满足才升 `ForOp`，prompt §3.3）─────────────────
//   1. 条件形如 `LtOp(iv, bound)`，`iv` 是"从某个 i32 标量槽读出"的值；
//   2. 边界**仿射且循环不变**（纯表达式 + 体内不写它 + 体内无 `CallOp`）；
//   3. 体内 IV **每条路径恰好 +1**；
//   4. 体内**无 `break`**（设计文档 I3：`ForOp` 体内不许有 `break`）。
//
// ── ★ `BreakOp` 的双重身份（本关最容易错的地方）─────────────────────────
//   S05 的 IRGen 把 `break` 与 `continue` **都**降级成 `BreakOp`。判定规则：
//     * `BreakOp` 是**体 Region 的终结符**：
//         - 若体里还有别的 `BreakOp`（在 `IfOp` 分支末尾）⇒ 这条是 `break`
//           （`if (C) { i=i+1; continue; } …; break;`）⇒ **保留 WhileOp**；
//         - 否则它可能是"尾部 continue"（`i=i+1; continue;`），由步进判定区分
//           （体内每条路径恰好 +1 ⇒ 合法）。
//     * `BreakOp` 是**体里某个 `IfOp` 分支的终结符**：判据是**"这个 `If` 是不是
//       它所在 Region 的最后一条语句"**（★★ S06 修正 ★★）。
//         - **不是**最后一条 ⇒ 它之后还有可达语句被这条 `Break` 跳过 ⇒ 它是
//           `continue`（`if (A[i][k]==1) { i=i+1; continue; } j=0; …`）；
//         - **是**最后一条 ⇒ 它是**真 `break`** ⇒ **保留 WhileOp**。
//       ⚠️ 原判据写的是"分支末尾的 `Break` ⇒ 一定是 `continue`"，理由是
//       "`break` 会让 `if` 之后的语句不可达，所以 `if` 之后若还有语句，它们
//       只可能属于另一条路径"。**这个论证是错的**：真 `break` 之后的那些语句
//       IRGen 的 `terminates()` **根本不生成** ⇒ "`if` 之后没有语句"恰恰是
//       真 `break` 的形状。旧判据把
//         `while (i<8) { if (i==4) { i=i+1; break; } s=s+i; i=i+1; }`
//       静默升成了 `ForOp`（语义从 `break` 变成 `continue` —— 编译产物与源码
//       语义不符）。S06 修掉了它，最小对照在 `compiler/tests/flat/min/`
//       （见 S06 报告 §坑）。**保守方向不变**：判成真 `break` 只是拒绝规范化，
//       语义永远正确；判错成 `continue` 才会改语义。
//   ⇒ 判据落在**"体的直接路径"**上：
//       体 Region 里**除 `IfOp` 之外**的顶层语句 + 每个 `IfOp` 的**回边分支**
//       = "正常走完一轮"的路径；它必须**恰好一个** IV 自增；
//       每个 `BreakOp` 所在的**分支路径**也必须**恰好一个** IV 自增
//       （`continue` 路径也要走到下一轮）。
//
//   例（`01_mm1.sy` 变体 1）：
//     i=0; while (i<n) {                       // 体 = [If, j=0, While_j, i=i+1, Yield]
//       if (A[i][k]==1) { i=i+1; continue; }   // If.then = […Store, Break]  ← continue 路径
//       j=0; while (j<n) {…}                   // If.else = [Yield]
//       i = i + 1;                             // 直接路径的自增
//     }
//     直接路径：If.else(0) + j=0 + While_j(0) + i=i+1 → **1** ✓
//     continue 路径：If.then → **1** ✓
// ============================================================================
#ifndef SYSY_STRUCTURED_LOOPANALYSIS_H
#define SYSY_STRUCTURED_LOOPANALYSIS_H

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {

// Op → (拥有它的 Op, 它所在的 Region)。整个 pass 只建**一次**（每棵树）。
using ParentMap = std::unordered_map<const Op*, std::pair<const Op*, const Region*>>;

// 【后置】建立 `root` 子树的父链（**显式工作栈**，见 prompt §五.3）。
ParentMap buildParents(const Op* root);

// 【后置】`op` 是否在 Region `body` 的子树里（含直接语句与更深的后代）。
bool isInBody(const ParentMap& par, const Region* body, const Op* op);

// 【后置】只读遍历 `r` 的全部后代 Op（显式工作栈）。
template <typename F>
void walkOps(const Region* r, const F& f) {
  std::vector<const Op*> st;
  if (r != nullptr) {
    for (size_t i = r->size(); i-- > 0;) st.push_back(r->at(i));
  }
  while (!st.empty()) {
    const Op* op = st.back();
    st.pop_back();
    if (op == nullptr) continue;
    f(op);
    for (size_t i = op->numRegions(); i-- > 0;) {
      const Region* sub = op->region(i);
      if (sub == nullptr) continue;
      for (size_t j = sub->size(); j-- > 0;) st.push_back(sub->at(j));
    }
  }
}

// 【后置】`op` 是不是 `iv = iv + 1`（`slot` = IV 槽的结果指针）。
//         `addOut != nullptr` 时回填那条 `AddI`。
bool isIvIncrement(const Op* op, const Value slot, const Op** addOut);

// 【后置】`v` 是不是"从 `slot` 读出来"的值。
bool isLoadOf(const Value v, const Value slot);

// 【后置】`v` 是不是字面量 1。
bool isOne(const Value v);

// 【后置】某个 `StoreOp` 写到的**根槽**（沿 `GetElementPtr` 回到 `Alloca`）；
//         不是本地槽（全局/形参指针）→ nullptr。
const Op* rootSlot(const Value p);

// 【后置】Region `body` 里被**写过**的全部本地槽（含经 GEP 的数组元素写）。
std::unordered_set<const Op*> writtenSlotsIn(const Region* body);

// 【后置】边界表达式"纯且循环不变"：
//         * 允许的 Op：常量/算术/比较/转换/`Alloca`（纯定义）/`GetElementPtr`；
//         * `Load`：不许读 IV 槽、不许读**体内被写过**的槽；
//         * 其余（`Call`/`Store`/控制流容器）一律拒绝。
bool provePure(const Value root, const Value ivSlot,
               const std::unordered_set<const Op*>& written);

// 条件 Region **除终结符外**的 Op（"参数符号提升"要搬的那些；顺序不变）。
std::vector<Op*> condCandidates(const Region* condR);

// 【后置】把 `fn` 树里 `target` 的结果在 **dump 里的名字**（`%<函数名>.<序号>`）
//         算出来（规则与 `StructuredDump.cpp` 的 `scanNames` 一致）。
//         `target` 不在 `fn` 里 → 空串。
//   ⚠️ `Result` 上**没有名字**（名字是 dump 期按先序算的）；`ForOp` 的
//      `attrs[0]` 只是给人读的。判据（I1/I2）用的是**操作数身份**，不是名字。
std::string dumpResultName(const Op* fn, const Op* target);

}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_LOOPANALYSIS_H
