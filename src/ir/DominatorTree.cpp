// ir/DominatorTree.cpp —— Cooper-Harvey-Kennedy 支配树实现。
#include "ir/FlatVerifier.h"

#include <utility>
#include <vector>

namespace sysy {
namespace flat {

DominatorTree::DominatorTree(const Function& f) {
  const size_t n = f.blockCount();
  reach_.assign(n, 0);
  post_.assign(n, -1);
  idom_.assign(n, nullptr);
  if (n == 0) return;

  // 从入口块出发做显式栈 DFS，得到后序编号。
  std::vector<int> order;
  order.reserve(n);
  std::vector<std::pair<BasicBlock*, size_t>> stack;
  std::vector<char> seen(n, 0);
  stack.emplace_back(f.entry(), 0);
  seen[0] = 1;
  while (!stack.empty()) {
    std::pair<BasicBlock*, size_t>& top = stack.back();
    BasicBlock* b = top.first;
    Instruction* term = b->terminator();
    const size_t successorCount = term != nullptr ? term->numSuccs() : 0;
    bool advanced = false;
    while (top.second < successorCount) {
      BasicBlock* successor = term->succ(top.second++);
      if (successor == nullptr || successor->parent() != &f) continue;
      const size_t index = successor->index();
      if (index >= n || seen[index]) continue;
      seen[index] = 1;
      stack.emplace_back(successor, 0);
      advanced = true;
      break;
    }
    if (advanced) continue;
    order.push_back(static_cast<int>(b->index()));
    stack.pop_back();
  }
  for (size_t i = 0; i < order.size(); ++i) {
    post_[static_cast<size_t>(order[i])] = static_cast<int>(i);
  }
  for (size_t i = 0; i < n; ++i) reach_[i] = seen[i];

  // 逆后序（RPO）：后序的逆序。
  std::vector<BasicBlock*> rpo;
  rpo.reserve(order.size());
  for (size_t i = order.size(); i-- > 0;) {
    rpo.push_back(f.at(static_cast<size_t>(order[i])));
  }

  // 迭代求 idom 到不动点。
  auto intersect = [&](BasicBlock* a, BasicBlock* b) -> BasicBlock* {
    while (a != b) {
      while (post_[a->index()] < post_[b->index()]) a = idom_[a->index()];
      while (post_[b->index()] < post_[a->index()]) b = idom_[b->index()];
    }
    return a;
  };
  idom_[f.entry()->index()] = f.entry();
  bool changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock* b : rpo) {
      if (b == f.entry()) continue;
      BasicBlock* newIdom = nullptr;
      for (size_t i = 0; i < b->numPreds(); ++i) {
        BasicBlock* pred = b->pred(i);
        if (pred == nullptr || pred->parent() != &f) continue;
        if (idom_[pred->index()] == nullptr) continue;
        newIdom = newIdom == nullptr ? pred : intersect(pred, newIdom);
      }
      if (newIdom != nullptr && idom_[b->index()] != newIdom) {
        idom_[b->index()] = newIdom;
        changed = true;
      }
    }
  }
  // 入口块对外没有支配者。
  idom_[f.entry()->index()] = nullptr;
}

bool DominatorTree::dominates(const BasicBlock* a, const BasicBlock* b) const {
  if (a == nullptr || b == nullptr) return false;
  if (a == b) return true;
  const size_t blockIndex = b->index();
  if (blockIndex >= reach_.size() || !reach_[blockIndex]) return false;
  const BasicBlock* current = b;
  size_t guard = 0;
  while (current != nullptr && guard++ <= reach_.size()) {
    if (current == a) return true;
    const size_t currentIndex = current->index();
    if (currentIndex >= idom_.size()) return false;
    current = idom_[currentIndex];
  }
  return false;
}

}  // namespace flat
}  // namespace sysy
