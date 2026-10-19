// S12: module call graph with SCC condensation and bottom-up order.
#ifndef SYSY_ANALYSIS_CALLGRAPH_H
#define SYSY_ANALYSIS_CALLGRAPH_H
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "analysis/AnalysisManager.h"
namespace sysy { namespace flat {
class Function;
class Instruction;

// The classifier recognizes Opcode::Call and both specialized intrinsic
// opcodes. Exact Flat intrinsic names select the intrinsic kinds; `Call`
// remains unresolved until the module graph checks whether its callee is
// defined. Suffixed LLVM ABI spellings stay ordinary calls and are treated as
// unknown external calls when no matching definition exists.
enum class CallLikeKind : uint8_t { None, Call, Memcpy, Memset };
CallLikeKind classifyCallLike(const Instruction&);

// One non-declaration Function. `callees`/`callers` are indices into
// CallGraphInfo::nodes (declarations are omitted). A call-like instruction
// whose name has no definition is an external call (runtime library, exact
// llvm.memcpy/memset intrinsic, or an unresolved declaration).
struct CallGraphNode {
  Function* function = nullptr;
  std::vector<size_t> callees;
  std::vector<size_t> callers;
  std::vector<std::string> externalCallees;   // sorted unique names
  size_t scc = 0;
};

struct CallGraphScc {
  std::vector<size_t> members;   // sorted Function indices
  bool recursive = false;        // >1 member or a self edge
};

struct CallGraphInfo {
  const Module* module = nullptr;     // provenance guard for CGSCC queries
  std::vector<CallGraphNode> nodes;   // Module Function order
  std::vector<CallGraphScc> sccs;     // bottom-up: callee SCCs before callers
  // Revision facts captured when the graph was built; a stale graph passed to
  // a CGSCC analysis must fail closed instead of mixing SCC data.
  uint64_t callsRevision = 0;
  uint64_t interfaceRevision = 0;
};

struct CallGraphAnalysis {
  using Result = CallGraphInfo;
  static constexpr AnalysisScope scope = AnalysisScope::Module;
  static constexpr MutationKind revisionDependencies() {
    return MutationKind::Calls | MutationKind::Interface;
  }
  // Postcondition: every node belongs to exactly one SCC and every inter-SCC
  // edge goes from a higher-index SCC to a lower-index one (bottom-up order).
  static std::optional<Result> run(const Module&, AnalysisManager&);
};
}}  // namespace sysy::flat
#endif  // SYSY_ANALYSIS_CALLGRAPH_H
