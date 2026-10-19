#include "analysis/LoopAnalysis.h"
#include <algorithm>
#include <unordered_set>
#include <utility>
#include "analysis/DominatorAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
std::optional<LoopInfoResult> LoopInfoAnalysis::run(
    const Function& function, AnalysisManager& manager) {
  AnalysisHandle<DominatorInfo> dominators =
      manager.get<DominatorAnalysis>(function);
  const DominatorInfo* dom = dominators.get();
  if (dom == nullptr) return std::nullopt;
  const size_t count = function.blockCount();
  std::vector<std::vector<size_t>> tailsByHeader(count);
  for (size_t index = 0; index < count; ++index) {
    BasicBlock* block = function.at(index);
    if (block == nullptr || block->parent() != &function) continue;
    Instruction* terminator = block->terminator();
    if (terminator == nullptr) continue;
    for (size_t s = 0; s < terminator->numSuccs(); ++s) {
      BasicBlock* successor = terminator->succ(s);
      if (successor == nullptr || successor->parent() != &function ||
          successor->index() >= count)
        continue;
      if (dom->dominates(successor, block))
        tailsByHeader[successor->index()].push_back(index);
    }
  }
  LoopInfoResult result;
  for (size_t header = 0; header < count; ++header) {
    if (tailsByHeader[header].empty()) continue;
    LoopEntry loop;
    loop.header = header;
    std::vector<char> member(count, 0);
    member[header] = 1;
    std::vector<size_t> stack;
    for (size_t tail : tailsByHeader[header]) {
      if (tail == header) continue;   // self loop: header alone
      if (!member[tail]) { member[tail] = 1; stack.push_back(tail); }
    }
    while (!stack.empty()) {
      const size_t current = stack.back();
      stack.pop_back();
      BasicBlock* block = function.at(current);
      if (block == nullptr) continue;
      for (size_t p = 0; p < block->numPreds(); ++p) {
        BasicBlock* predecessor = block->pred(p);
        if (predecessor == nullptr || predecessor->parent() != &function ||
            predecessor->index() >= count || member[predecessor->index()])
          continue;
        member[predecessor->index()] = 1;
        stack.push_back(predecessor->index());
      }
    }
    for (size_t i = 0; i < count; ++i)
      if (member[i]) loop.blocks.push_back(i);
    result.loops.push_back(std::move(loop));
  }
  // Parent: the smallest enclosing loop (its block set contains this header).
  for (size_t i = 0; i < result.loops.size(); ++i) {
    size_t best = result.loops.size();
    for (size_t j = 0; j < result.loops.size(); ++j) {
      if (i == j || result.loops[j].blocks.size() <= result.loops[i].blocks.size())
        continue;
      if (std::find(result.loops[j].blocks.begin(), result.loops[j].blocks.end(),
                    result.loops[i].header) == result.loops[j].blocks.end())
        continue;
      if (best == result.loops.size() ||
          result.loops[j].blocks.size() < result.loops[best].blocks.size())
        best = j;
    }
    if (best != result.loops.size()) result.loops[i].parent = static_cast<long>(best);
  }
  for (size_t i = 0; i < result.loops.size(); ++i) {
    size_t depth = 0;
    long parent = result.loops[i].parent;
    while (parent >= 0) {
      ++depth;
      parent = result.loops[static_cast<size_t>(parent)].parent;
    }
    result.loops[i].depth = depth;
  }
  return result;
}

std::optional<LoopView> LoopViewAnalysis::run(
    const Function& function, size_t loopIndex, AnalysisManager& manager) {
  AnalysisHandle<LoopInfoResult> info = manager.get<LoopInfoAnalysis>(function);
  const LoopInfoResult* loops = info.get();
  if (loops == nullptr || loopIndex >= loops->loops.size()) return std::nullopt;
  const LoopEntry& entry = loops->loops[loopIndex];
  LoopView view;
  view.loop = loopIndex;
  view.header = entry.header;
  view.parent = entry.parent;
  view.depth = entry.depth;
  view.blockCount = entry.blocks.size();
  return view;
}
}}  // namespace sysy::flat
