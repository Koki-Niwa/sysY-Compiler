// ============================================================================
// AllocaHoist.cpp —— `alloca` 提升到函数入口 Region（规格见 AllocaHoist.h）
//
// 实现要点（全部为了"稳定"，prompt §3.5）：
//   ① 对每个 `FuncOp` 单独处理（**不跨函数**）；
//   ② 收集该函数**整个子树**里的 `AllocaOp`，顺序 = **先序**（原有相对顺序）；
//   ③ 从每个 Region 里摘掉它们（它们可能在入口、也可能在深处的
//      `IfOp`/`WhileOp`/`ForOp` 的 Region 里）；
//   ④ 把 alloca 序列前置到入口 Region，后面接入口 Region 原有的其余 Op。
//
// ★ 遍历**必须显式工作栈**（prompt §五.3：病态输入不许递归）。
//   第一版写成了递归的 `stripAllocas()`，在 4000 层块嵌套上会爆栈 ——
//   本文件因此改成"先整体收集、再逐个 Region 过滤"的两趟写法。
//   ⚠️ "整体先收集"这一趟**必须真先序**：用一个迭代器栈（而不是"弹出后把
//      子 Region 压进去"的简单 LIFO）——否则一个 Op 的多个子 Region 会被
//      交错展开，兄弟 Region 的顺序就乱了。顺序是数据布局的可复现前提。
// ============================================================================
#include "structured/AllocaHoist.h"

#include <unordered_set>
#include <vector>

namespace sysy {
namespace sir {
namespace {

// 迭代器栈的一帧 = 一个 Region + 下一个要看的元素下标（真先序遍历）。
struct Frame {
  Region* r = nullptr;
  size_t i = 0;
};

// 【后置】按**先序**把 `entry` 子树里的 `AllocaOp` 追加到 `out`（不改树）。
void collectAllocas(Region* entry, std::vector<Op*>& out, std::unordered_set<const Op*>& set) {
  if (entry == nullptr) return;
  std::vector<Frame> st;
  st.push_back(Frame{entry, 0});
  while (!st.empty()) {
    Frame& f = st.back();
    if (f.i >= f.r->size()) { st.pop_back(); continue; }
    Op* op = f.r->at(f.i++);
    if (op == nullptr) continue;
    if (op->kind == OpKind::Alloca) {
      out.push_back(op);
      set.insert(op);
      continue;                          // Alloca 没有子 Region
    }
    // 子 Region **逆序**压栈 ⇒ 弹出时是正序（真先序）
    for (size_t k = op->numRegions(); k-- > 0;) {
      Region* sub = op->region(k);
      if (sub != nullptr) st.push_back(Frame{sub, 0});
    }
  }
}

// 【后置】入口 Region 的**后代 Region**（不含入口自己的直接语句）里还有没有
//         `AllocaOp`。判据：入口 Region 的直接语句 + 后代 Region 里找到的
//         alloca 总数 > 直接语句里的 alloca 数 ⇒ 有嵌套的。
//   ⚠️ 这里必须**只数一次性**：第一版写成"用 first 标记的 DFS"，结果把入口
//      Region 里 `if` 之后的**直接**语句也当成了"后代"（栈帧被弹出后标记就
//      翻成了 false）⇒ 误判有嵌套 alloca ⇒ 白白重建入口 Region。
bool hasNestedAlloca(const Region* entry) {
  if (entry == nullptr) return false;
  size_t direct = 0;
  for (size_t i = 0; i < entry->size(); ++i) {
    const Op* op = entry->at(i);
    if (op != nullptr && op->kind == OpKind::Alloca) ++direct;
  }
  size_t total = 0;
  std::vector<Frame> st;
  st.push_back(Frame{const_cast<Region*>(entry), 0});
  while (!st.empty()) {
    Frame& f = st.back();
    if (f.i >= f.r->size()) { st.pop_back(); continue; }
    Op* op = f.r->at(f.i++);
    if (op == nullptr) continue;
    if (op->kind == OpKind::Alloca) { ++total; continue; }
    for (size_t k = op->numRegions(); k-- > 0;) {
      Region* sub = op->region(k);
      if (sub != nullptr) st.push_back(Frame{sub, 0});
    }
  }
  return total > direct;
}

// 【后置】把入口子树里所有属于 `set` 的 Op 从各自 Region 摘掉。
void stripAllocas(Region* entry, const std::unordered_set<const Op*>& set) {
  if (entry == nullptr) return;
  std::vector<Frame> st;
  st.push_back(Frame{entry, 0});
  while (!st.empty()) {
    Frame& f = st.back();
    if (f.i >= f.r->size()) { st.pop_back(); continue; }
    Op* op = f.r->at(f.i);
    if (op == nullptr) { ++f.i; continue; }
    if (set.count(op) != 0) {
      std::vector<Op*> kept = f.r->ops();
      for (size_t k = f.i; k + 1 < kept.size(); ++k) kept[k] = kept[k + 1];
      kept.pop_back();
      f.r->replaceAll(kept);
      continue;                          // 同一个下标再看一次（新来的那个）
    }
    ++f.i;
    for (size_t k = op->numRegions(); k-- > 0;) {
      Region* sub = op->region(k);
      if (sub != nullptr) st.push_back(Frame{sub, 0});
    }
  }
}

}  // namespace

size_t hoistAllocas(Op* module) {
  if (module == nullptr || module->kind != OpKind::Module) return 0;
  size_t moved = 0;
  const Region* modR = module->region(0);
  if (modR == nullptr) return 0;
  for (size_t i = 0; i < modR->size(); ++i) {
    Op* fn = modR->at(i);
    if (fn == nullptr || fn->kind != OpKind::Func) continue;
    Region* entry = fn->region(0);
    if (entry == nullptr) continue;

    std::vector<Op*> allocas;
    std::unordered_set<const Op*> set;
    collectAllocas(entry, allocas, set);
    if (allocas.empty()) continue;

    // "原本就在入口最前面、且相对顺序一致"的个数（诚实统计 moved：
    //   moved = 总数 − 这一段的长度。当前 IRGen 把 alloca 全放入口 ⇒ moved=0）
    size_t already = 0;
    for (size_t k = 0; k < entry->size() && k < allocas.size(); ++k) {
      if (entry->at(k) == allocas[k]) ++already;
      else break;
    }
    // ★ **只在真的需要移动时**才重建 Region：入口的后代 Region 里没有
    //   `AllocaOp` ⇒ 恒等变换（一个字节都不动）。
    //   为什么必须这样：本 pass 的验收口径是"不触发的用例逐字节零改动"
    //   （prompt §二），而"重建 Region"会把入口 Region 的**相对顺序**改掉
    //   ——`GetArg/Alloca/Store` 会被重排成 `Alloca/…/GetArg/Store`，dump
    //   因此不同（实测：`shuffle0.sy` 的 `hash` 函数）。而这两条**都是合法
    //   的 `alloca` 位置**（后端不变量 2 只要求"在入口 Region"）。
    //   当前 IRGen 已经把全部 alloca 写进入口 ⇒ 对本项目的语料
    //   **`allocas-hoisted` 恒为 0、dump 恒等**；而手工构造的"alloca 在深层
    //   Region"的 IR 仍然会被真的搬上来（`unit/test_loopnorm.cpp` 钉死）。
    if (!hasNestedAlloca(entry)) continue;
    stripAllocas(entry, set);
    std::vector<Op*> rebuilt;
    rebuilt.reserve(entry->size() + allocas.size());
    for (Op* a : allocas) rebuilt.push_back(a);      // ① 全部 alloca 前置（原顺序）
    for (size_t k = 0; k < entry->size(); ++k) rebuilt.push_back(entry->at(k));
    if (allocas.size() > already) moved += allocas.size() - already;
    entry->replaceAll(rebuilt);
  }
  return moved;
}

}  // namespace sir
}  // namespace sysy
