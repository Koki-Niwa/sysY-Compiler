// Cooper-Harvey-Kennedy dominator tree for a structurally valid flat CFG.
#include "ir/FlatVerifier.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace sysy {
namespace flat {

DominatorTree::DominatorTree(const Function& f) : owner_(&f) {
  const size_t n = f.blockCount();
  reach_.assign(n, 0);
  post_.assign(n, -1);
  idom_.assign(n, nullptr);
  blocks_.assign(n, nullptr);
  bool validShape = true;
  std::unordered_set<BasicBlock*> seenBlocks;
  for (size_t i = 0; i < n; ++i) {
    BasicBlock* block = f.at(i);
    if (block == nullptr || block->parent() != &f || block->index() != i ||
        !seenBlocks.insert(block).second) {
      validShape = false;
      continue;
    }
    blocks_[i] = block;
  }
  if (!validShape || n == 0 || f.entry() == nullptr) return;

  // From the entry, compute postorder with an explicit stack.
  std::vector<int> order;
  order.reserve(n);
  std::vector<std::pair<BasicBlock*, size_t>> stack;
  std::vector<char> seen(n, 0);
  stack.emplace_back(f.entry(), 0);
  seen[0] = 1;
  while (!stack.empty()) {
    std::pair<BasicBlock*, size_t>& top = stack.back();
    BasicBlock* block = top.first;
    Instruction* terminator = block->terminator();
    const size_t successorCount = terminator == nullptr ? 0 : terminator->numSuccs();
    bool advanced = false;
    while (top.second < successorCount) {
      BasicBlock* successor = terminator->succ(top.second++);
      if (successor == nullptr || successor->parent() != &f) continue;
      const size_t index = successor->index();
      if (index >= n || blocks_[index] != successor || seen[index]) continue;
      seen[index] = 1;
      stack.emplace_back(successor, 0);
      advanced = true;
      break;
    }
    if (advanced) continue;
    order.push_back(static_cast<int>(block->index()));
    stack.pop_back();
  }
  for (size_t i = 0; i < order.size(); ++i) {
    post_[static_cast<size_t>(order[i])] = static_cast<int>(i);
  }
  for (size_t i = 0; i < n; ++i) reach_[i] = seen[i];

  std::vector<BasicBlock*> rpo;
  rpo.reserve(order.size());
  for (size_t i = order.size(); i-- > 0;) {
    rpo.push_back(f.at(static_cast<size_t>(order[i])));
  }

  auto intersect = [&](BasicBlock* left, BasicBlock* right) -> BasicBlock* {
    while (left != right) {
      if (left == nullptr || right == nullptr) return nullptr;
      while (post_[left->index()] < post_[right->index()]) {
        left = idom_[left->index()];
        if (left == nullptr) return nullptr;
      }
      while (post_[right->index()] < post_[left->index()]) {
        right = idom_[right->index()];
        if (right == nullptr) return nullptr;
      }
    }
    return left;
  };

  idom_[0] = f.entry();
  bool changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock* block : rpo) {
      if (block == f.entry()) continue;
      BasicBlock* newIdom = nullptr;
      for (BasicBlock* predecessor : block->preds()) {
        if (predecessor == nullptr || predecessor->parent() != &f) continue;
        const size_t index = predecessor->index();
        if (index >= n || blocks_[index] != predecessor || idom_[index] == nullptr) continue;
        newIdom = newIdom == nullptr ? predecessor : intersect(predecessor, newIdom);
        if (newIdom == nullptr) break;
      }
      const size_t index = block->index();
      if (newIdom != nullptr && idom_[index] != newIdom) {
        idom_[index] = newIdom;
        changed = true;
      }
    }
  }
  idom_[0] = nullptr;
}

bool DominatorTree::dominates(const BasicBlock* a, const BasicBlock* b) const {
  if (a == nullptr || b == nullptr || owner_ == nullptr) return false;
  auto member = [&](const BasicBlock* block) {
    if (block->parent() != owner_) return false;
    const size_t index = block->index();
    return index < blocks_.size() && blocks_[index] == block;
  };
  if (!member(a) || !member(b)) return false;
  const size_t aIndex = a->index();
  const size_t bIndex = b->index();
  if (aIndex >= reach_.size() || bIndex >= reach_.size() || !reach_[aIndex] || !reach_[bIndex]) {
    return false;
  }
  if (a == b) return true;
  const BasicBlock* current = b;
  size_t guard = 0;
  while (current != nullptr && guard++ <= idom_.size()) {
    if (current == a) return true;
    const size_t index = current->index();
    if (index >= idom_.size()) return false;
    current = idom_[index];
  }
  return false;
}

}  // namespace flat
}  // namespace sysy
