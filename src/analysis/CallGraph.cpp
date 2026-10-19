#include "analysis/CallGraph.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
// Iterative Tarjan. Recursion depth must not follow the function count.
struct Frame {
  size_t node;
  size_t edge;
};
void sortUnique(std::vector<size_t>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}
void sortUnique(std::vector<std::string>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}
}  // namespace

CallLikeKind classifyCallLike(const Instruction& instruction) {
  const Opcode op = instruction.op();
  if (op != Opcode::Call && op != Opcode::LLVMMemCpy &&
      op != Opcode::LLVMMemSet)
    return CallLikeKind::None;
  // These are the only canonical Flat ABI spellings.  In particular, do not
  // recognize LLVM's target-suffixed names by prefix.
  if (instruction.callee() == "llvm.memcpy" &&
      (op == Opcode::Call || op == Opcode::LLVMMemCpy))
    return CallLikeKind::Memcpy;
  if (instruction.callee() == "llvm.memset" &&
      (op == Opcode::Call || op == Opcode::LLVMMemSet))
    return CallLikeKind::Memset;
  return CallLikeKind::Call;
}

std::optional<CallGraphInfo> CallGraphAnalysis::run(
    const Module& module, AnalysisManager&) {
  CallGraphInfo info;
  info.module = &module;
  info.callsRevision = module.revision().calls;
  info.interfaceRevision = module.revision().interface;
  std::unordered_map<std::string, size_t> byName;
  for (Function* function : module.functions()) {
    if (function == nullptr || function->parent() != &module ||
        function->isDeclaration())
      continue;
    if (!byName.emplace(function->name(), info.nodes.size()).second)
      return std::nullopt;   // duplicate definitions cannot form a sound graph
    CallGraphNode node;
    node.function = function;
    info.nodes.push_back(std::move(node));
  }
  if (info.nodes.empty()) return info;

  for (size_t index = 0; index < info.nodes.size(); ++index) {
    Function* function = info.nodes[index].function;
    for (BasicBlock* block : function->blocks()) {
      for (Instruction* inst : block->insts()) {
        if (inst == nullptr) continue;
        const CallLikeKind kind = classifyCallLike(*inst);
        if (kind == CallLikeKind::None) continue;
        // Intrinsic opcode forms are external by construction.  A malformed
        // specialized form is still conservative and cannot become an
        // internal edge merely because a same-named definition exists.
        if (kind == CallLikeKind::Memcpy || kind == CallLikeKind::Memset ||
            inst->op() != Opcode::Call) {
          info.nodes[index].externalCallees.push_back(inst->callee());
          continue;
        }
        const auto found = byName.find(inst->callee());
        if (found == byName.end()) {
          info.nodes[index].externalCallees.push_back(inst->callee());
          continue;
        }
        info.nodes[index].callees.push_back(found->second);
      }
    }
  }
  const size_t count = info.nodes.size();
  for (size_t index = 0; index < count; ++index) {
    sortUnique(info.nodes[index].callees);
    sortUnique(info.nodes[index].externalCallees);
    for (size_t callee : info.nodes[index].callees) {
      if (callee >= count) return std::nullopt;
      info.nodes[callee].callers.push_back(index);
    }
  }
  for (size_t index = 0; index < count; ++index) sortUnique(info.nodes[index].callers);

  std::vector<int64_t> indexOf(count, -1), low(count, 0);
  std::vector<char> onStack(count, 0);
  std::vector<size_t> stack;
  std::vector<Frame> dfs;
  int64_t nextIndex = 0;
  for (size_t root = 0; root < count; ++root) {
    if (indexOf[root] != -1) continue;
    indexOf[root] = low[root] = nextIndex++;
    stack.push_back(root);
    onStack[root] = 1;
    dfs.push_back({root, 0});
    while (!dfs.empty()) {
      Frame& frame = dfs.back();
      const size_t node = frame.node;
      if (frame.edge < info.nodes[node].callees.size()) {
        const size_t callee = info.nodes[node].callees[frame.edge++];
        if (indexOf[callee] == -1) {
          indexOf[callee] = low[callee] = nextIndex++;
          stack.push_back(callee);
          onStack[callee] = 1;
          dfs.push_back({callee, 0});
        } else if (onStack[callee]) {
          low[node] = std::min(low[node], indexOf[callee]);
        }
        continue;
      }
      dfs.pop_back();
      if (!dfs.empty())
        low[dfs.back().node] = std::min(low[dfs.back().node], low[node]);
      if (low[node] != indexOf[node]) continue;
      CallGraphScc scc;
      while (true) {
        const size_t member = stack.back();
        stack.pop_back();
        onStack[member] = 0;
        info.nodes[member].scc = info.sccs.size();
        scc.members.push_back(member);
        if (member == node) break;
      }
      std::sort(scc.members.begin(), scc.members.end());
      scc.recursive = scc.members.size() > 1 ||
                      std::find(info.nodes[node].callees.begin(),
                                info.nodes[node].callees.end(), node) !=
                          info.nodes[node].callees.end();
      info.sccs.push_back(std::move(scc));
    }
  }

  // Tarjan emits callee SCCs before caller SCCs. Make that an explicit
  // precondition instead of an implicit property consumers may trust.
  for (size_t node = 0; node < count; ++node) {
    for (size_t callee : info.nodes[node].callees) {
      const size_t callerScc = info.nodes[node].scc;
      const size_t calleeScc = info.nodes[callee].scc;
      if (callerScc == calleeScc) continue;
      if (calleeScc >= callerScc) return std::nullopt;
    }
  }
  for (size_t scc = 0; scc < info.sccs.size(); ++scc)
    for (size_t member : info.sccs[scc].members)
      if (info.nodes[member].scc != scc) return std::nullopt;
  return info;
}
}}  // namespace sysy::flat
