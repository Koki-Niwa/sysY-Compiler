// ============================================================================
// LoopRewrite.h —— `while` → `for` 的**判定 + 改写**（S05b 交付物 #1 的实现半）
//
//   与 `LoopNormalize.cpp`（驱动半：遍历模块、构造 `ForOp`、统计）分开的理由：
//   §C4 的单文件 600 行硬上限。
//
//   本文件里的 `Analyzer` 负责：
//     ① `continue` 消解的**只读判定**（§3.2 的规范形式见 `.cpp` 的文件头）
//     ② IV 识别   ③ 边界归一化 + 参数符号提升   ④ 每条路径自增证明
//     ⑤ 判定通过后的**改写**（`applyStep`：消解 continue + 摘掉全部 IV 自增）
//
//   ★ 判定**只读**（`analyzeOnce` 不改树）⇒ 判定失败时那几个循环**逐字节零改动**
//     （prompt §六 轨 C.3 的"不触发用例零改动"因此天然成立，不需要回滚）。
// ============================================================================
#ifndef SYSY_STRUCTURED_LOOPREWRITE_H
#define SYSY_STRUCTURED_LOOPREWRITE_H

#include <string>

#include "structured/LoopNormalize.h"
#include "structured/StructuredIR.h"

namespace sysy {
namespace sir {
namespace detail {

// 【后置】对 `loop`（一个 `WhileOp`）做完整的判定 + 改写。
//         成功 → true，`forBuilt` 的那一步（构造 `ForOp`）由调用方做；
//                 `iv`/`upper`/`ivLoad` 被填好。
//         失败 → false，`fail` 给出原因；**树一个字节都没动**。
struct RewriteResult {
  bool ok = false;
  Value iv = nullptr;          // IV 的槽（`AllocaOp` 的结果）
  Value upper = nullptr;       // 边界表达式的值（`ForOp` 的 upper）
  const Op* ivLoad = nullptr;  // 条件里那条 `Load iv`（IV 名字从它算）
  size_t continuesResolved = 0;   // 被包装的 continue 数
  size_t tailContinues = 0;       // 换成 `(Yield)` 的分支/尾部 continue 数
  SkipReason fail = SkipReason::CondNotLt;
};

// 【前置】loop->kind == OpKind::While 且两个 Region 都非空。
// 【后置】成功时**已就地改写**（continue 消解 + 摘掉 IV 自增）；失败时零改动。
RewriteResult analyzeAndRewriteLoop(Op* loop, Arena& arena);

}  // namespace detail
}  // namespace sir
}  // namespace sysy
#endif  // SYSY_STRUCTURED_LOOPREWRITE_H
