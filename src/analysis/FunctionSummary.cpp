#include "analysis/FunctionSummary.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
const FunctionSummary* findMember(const SccSummary& summary, size_t functionIndex) {
  for (const FunctionSummary& member : summary.members)
    if (member.functionIndex == functionIndex) return &member;
  return nullptr;
}
}  // namespace

std::optional<SccSummary> SccSummaryAnalysis::run(
    const CallGraphInfo& graph, size_t sccIndex, AnalysisManager& manager) {
  if (sccIndex >= graph.sccs.size()) return std::nullopt;
  if (graph.module != &manager.module()) return std::nullopt;
  const IRRevision& revision = manager.module().revision();
  // A CallGraphInfo from an older calls/interface revision would mix SCC
  // numbering with current callees; fail closed instead of trusting it.
  if (graph.callsRevision != revision.calls ||
      graph.interfaceRevision != revision.interface)
    return std::nullopt;
  const CallGraphScc& scc = graph.sccs[sccIndex];
  const size_t count = scc.members.size();
  if (count == 0) return std::nullopt;

  std::vector<char> readsLocal(count, 0), writesLocal(count, 0);
  std::vector<char> externalLocal(count, 0);
  std::unordered_map<size_t, size_t> localOf;   // Function index -> member slot
  for (size_t slot = 0; slot < count; ++slot) {
    const size_t functionIndex = scc.members[slot];
    if (functionIndex >= graph.nodes.size()) return std::nullopt;
    const Function* function = graph.nodes[functionIndex].function;
    if (function == nullptr || function->isDeclaration()) return std::nullopt;
    localOf[functionIndex] = slot;
    const auto& externalCallees = graph.nodes[functionIndex].externalCallees;
    externalLocal[slot] = externalCallees.empty() ? 0 : 1;
    for (BasicBlock* block : function->blocks()) {
      for (Instruction* inst : block->insts()) {
        if (inst == nullptr) continue;
        if (inst->op() == Opcode::Store) writesLocal[slot] = 1;
        else if (inst->op() == Opcode::Load) readsLocal[slot] = 1;
        const CallLikeKind callKind = classifyCallLike(*inst);
        if (callKind == CallLikeKind::Memcpy) {
          // memcpy may observe the source and modifies the destination.
          readsLocal[slot] = 1;
          writesLocal[slot] = 1;
        } else if (callKind == CallLikeKind::Memset) {
          writesLocal[slot] = 1;
        } else if (callKind == CallLikeKind::Call &&
                   std::binary_search(externalCallees.begin(),
                                      externalCallees.end(), inst->callee())) {
          // Unknown external calls are conservatively both reading and
          // writing memory.  Exact intrinsic names were handled above.
          readsLocal[slot] = 1;
          writesLocal[slot] = 1;
        }
      }
    }
  }

  // Callee SCCs are already complete (bottom-up numbering) and are fetched
  // through the manager, so their generations become our dependencies.
  std::unordered_map<size_t, const SccSummary*> calleeSccs;
  std::vector<AnalysisHandle<SccSummary>> calleeHandles;
  for (size_t slot = 0; slot < count; ++slot) {
    for (size_t callee : graph.nodes[scc.members[slot]].callees) {
      if (callee >= graph.nodes.size()) return std::nullopt;
      const size_t calleeScc = graph.nodes[callee].scc;
      if (calleeScc == sccIndex || calleeSccs.count(calleeScc) != 0) continue;
      AnalysisHandle<SccSummary> handle =
          manager.get<SccSummaryAnalysis>(graph, calleeScc);
      if (!handle.isCurrent()) return std::nullopt;
      calleeSccs.emplace(calleeScc, handle.get());
      calleeHandles.push_back(std::move(handle));
    }
  }
  auto calleeSummary = [&](size_t callee) -> const FunctionSummary* {
    const size_t calleeScc = graph.nodes[callee].scc;
    if (calleeScc == sccIndex) return nullptr;   // resolved through local slots
    const auto it = calleeSccs.find(calleeScc);
    if (it == calleeSccs.end() || it->second == nullptr) return nullptr;
    return findMember(*it->second, callee);
  };
  auto localCallee = [&](size_t callee) -> long {
    const auto it = localOf.find(callee);
    return it == localOf.end() ? -1 : static_cast<long>(it->second);
  };

  // Purity: greatest fixed point. A function starts pure only when its local
  // reads, writes, and external calls are all absent; it is removed when a
  // callee is impure. Reads are excluded because a load may observe mutable
  // memory (globals), which would make the function non-deterministic for
  // CSE/DCE. Mutual recursion without effects therefore stays pure; no
  // function is added back, so the number of rounds is bounded by `count`.
  std::vector<char> pure(count, 0);
  for (size_t slot = 0; slot < count; ++slot)
    pure[slot] = (!readsLocal[slot] && !writesLocal[slot] && !externalLocal[slot])
                    ? 1 : 0;
  for (size_t round = 0; round <= count; ++round) {
    bool changed = false;
    for (size_t slot = 0; slot < count; ++slot) {
      if (!pure[slot]) continue;
      for (size_t callee : graph.nodes[scc.members[slot]].callees) {
        bool calleePure = false;
        const long local = localCallee(callee);
        if (local >= 0) {
          calleePure = pure[static_cast<size_t>(local)] != 0;
        } else {
          const FunctionSummary* summary = calleeSummary(callee);
          if (summary == nullptr) return std::nullopt;
          calleePure = summary->pure;
        }
        if (!calleePure) { pure[slot] = 0; changed = true; break; }
      }
    }
    if (!changed) break;
  }

  // Effects: least fixed point. Recursion alone never creates an effect.
  std::vector<char> reads = readsLocal, writes = writesLocal, external = externalLocal;
  for (size_t round = 0; round <= count; ++round) {
    bool changed = false;
    for (size_t slot = 0; slot < count; ++slot) {
      for (size_t callee : graph.nodes[scc.members[slot]].callees) {
        bool read = false, write = false, ext = false;
        const long local = localCallee(callee);
        if (local >= 0) {
          const size_t at = static_cast<size_t>(local);
          read = reads[at] != 0;
          write = writes[at] != 0;
          ext = external[at] != 0;
        } else {
          const FunctionSummary* summary = calleeSummary(callee);
          if (summary == nullptr) return std::nullopt;
          read = summary->readsMemory;
          write = summary->writesMemory;
          ext = summary->callsExternal;
        }
        if (read && !reads[slot]) { reads[slot] = 1; changed = true; }
        if (write && !writes[slot]) { writes[slot] = 1; changed = true; }
        if (ext && !external[slot]) { external[slot] = 1; changed = true; }
      }
    }
    if (!changed) break;
  }

  SccSummary result;
  result.scc = sccIndex;
  result.pure = true;
  for (size_t slot = 0; slot < count; ++slot) {
    FunctionSummary member;
    member.function = graph.nodes[scc.members[slot]].function;
    member.functionIndex = scc.members[slot];
    member.scc = sccIndex;
    member.pure = pure[slot] != 0;
    member.readsMemory = reads[slot] != 0;
    member.writesMemory = writes[slot] != 0;
    member.callsExternal = external[slot] != 0;
    member.recursive = scc.recursive;
    result.pure = result.pure && member.pure;
    result.readsMemory = result.readsMemory || member.readsMemory;
    result.writesMemory = result.writesMemory || member.writesMemory;
    result.callsExternal = result.callsExternal || member.callsExternal;
    result.members.push_back(std::move(member));
  }
  return result;
}

std::optional<FunctionSummaryInfo> FunctionSummaryAnalysis::run(
    const Module&, AnalysisManager& manager) {
  AnalysisHandle<CallGraphInfo> graphHandle = manager.get<CallGraphAnalysis>();
  const CallGraphInfo* graph = graphHandle.get();
  if (graph == nullptr) return std::nullopt;
  FunctionSummaryInfo info;
  info.perFunction.resize(graph->nodes.size());
  info.sccs.resize(graph->sccs.size());
  for (size_t scc = 0; scc < graph->sccs.size(); ++scc) {
    AnalysisHandle<SccSummary> handle =
        manager.get<SccSummaryAnalysis>(*graph, scc);
    const SccSummary* summary = handle.get();
    if (summary == nullptr) return std::nullopt;
    info.sccs[scc] = *summary;
    for (size_t slot = 0; slot < graph->sccs[scc].members.size(); ++slot) {
      const size_t functionIndex = graph->sccs[scc].members[slot];
      if (functionIndex >= info.perFunction.size()) return std::nullopt;
      info.perFunction[functionIndex] = summary->members[slot];
    }
  }
  return info;
}
}}  // namespace sysy::flat
