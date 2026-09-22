// ============================================================================
// ir/FlatVerifier.h —— 平面 IR 的**不变量检查器**（S06 交付物 #6，轨 B）
//
//   规格（prompt §六 轨 B）——六项，逐条对应一个检查：
//     ① **use-def 双向一致**：每个操作数都能找到定义；每个定义的
//        `users()` 与真实使用者一致（**两个方向都查**，只查一边会漏掉
//        "反向边是陈旧的"——那正是 S17–S20 最难查的一类 bug）；
//     ② **SSA 支配**：每个值的使用点被它的定义点**支配**。这需要**支配树**
//        ——本文件**真的实现它**（Cooper-Harvey-Kennedy 迭代数据流），
//        不用"看起来对"的近似（prompt §六 轨 B.2 的明文要求）。
//        S14 会复用这套算法（这里先立住）。
//     ③ **φ 的合法性**：只在块首；入值个数 == 前驱个数；**前驱集合与入值块
//        集合完全相同**；每个入值支配对应的前驱块；φ 至少有**一个非自身
//        入值**（判据 3）。
//     ④ **终结符**：每个块恰好一个，且是最后一条（`ret`/`br`/`unreachable`）。
//     ⑤ **`alloca` 全在入口块**（不变量 2，后端据此一次性布局栈帧）。
//     ⑥ **指令集封闭**：opcode 全在 `iset.txt` 的 35 个里（轨 B 另有一份
//        **独立手写**的清单在 `check_flat.py`，那是权威；这里只是编译期自查）。
//
// ── 为什么检查器要"能报出反例"（本项目的规矩，见 checkpoints ⑬）──────────
//   "全过"可能只是空转。所以 `unit/test_flat.cpp` 与 `check_flat.py` 都会
//   **手工构造 ≥6 份坏 IR**，逐条要求报红 —— 检查器必须先证明抓得住错。
// ============================================================================
#ifndef SYSY_IR_FLATVERIFIER_H
#define SYSY_IR_FLATVERIFIER_H

#include <string>
#include <vector>

#include "ir/Module.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

// 一条不变量违反。`invariant` 是**稳定的短名**（机器可 grep、测试可断言）。
struct Violation {
  std::string invariant;   // "V1".."V6"（见上）
  SourceLoc loc;
  std::string message;
  std::string func;
};

// 【后置】检查整个平面模块，返回全部违反（**不早退**：一次报全，便于定位）。
//         `loc` 是尽力而为的（IR 读回自文本时行号可能缺失）。
std::vector<Violation> verifyFlatModule(const Module& m);

// 【后置】违反列表 → 人类可读文本（`main.cpp` 的错误消息与测试都用它）。
std::string formatViolations(const std::vector<Violation>& vs);

// ============================================================================
// 支配树（SSA 检查的基础；S14 会复用这套算法）
// ============================================================================
// 【前置】`f` 的块表非空。
// 【后置】`idom[i]` = 块 i 的直接支配者（入口块为 nullptr）；
//         `dominates(a, b)` = 块 a 支配块 b；不可达块之间返回 false。
// 【算法】Cooper-Harvey-Kennedy 迭代（"A Simple, Fast Dominance Algorithm"）：
//   * 块按 RPO（逆后序）处理，`idom[b] = intersect(idom[p] for p in preds[b])`；
//   * 迭代到不动点。**迭代**（不用递归）：几千个块的 CFG 也不会碰调用栈。
//   * 后序编号用**显式工作栈**算（prompt §八：任何随规模增长的遍历都要迭代）。
class DominatorTree {
 public:
  explicit DominatorTree(const Function& f);

  bool reachable(size_t blockIdx) const { return reach_[blockIdx]; }
  BasicBlock* idom(size_t blockIdx) const { return idom_[blockIdx]; }
  // 【后置】a 是否支配 b（a == b 时**为真** —— 支配是自反的）。
  //         任一块不可达 → false（不可达块之间不谈支配）。
  bool dominates(const BasicBlock* a, const BasicBlock* b) const;
  const std::vector<int>& postOrder() const { return post_; }

 private:
  std::vector<BasicBlock*> idom_;
  std::vector<char> reach_;
  std::vector<int> post_;      // 块下标 → 逆后序号
};

}  // namespace flat
}  // namespace sysy
#endif  // SYSY_IR_FLATVERIFIER_H
