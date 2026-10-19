// ============================================================================
// opt/Mem2Reg.h -- conservative scalar alloca promotion for flat IR.
//
// The pass accepts a verified flat module, promotes only direct scalar alloca
// accesses, and leaves a candidate untouched when its safety cannot be proved.
// F04: the FunctionPass is the sole promotion implementation; the module API
// below is a compatible FlatPassManager wrapper.
// ============================================================================
#ifndef SYSY_OPT_MEM2REG_H
#define SYSY_OPT_MEM2REG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ir/Module.h"
#include "opt/FlatPassManager.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

enum class Mem2RegStatus : uint8_t {
  Disabled = 0,
  InvalidInput,
  NoChange,
  Changed,
  // The formal pass pipeline failed (contract violation, verification failure,
  // exception, or pass registration failure). Never claims a successful edit.
  Failed
};

// Reasons are stable short categories suitable for diagnostics and tests.
enum class Mem2RegSkipReason : uint8_t {
  NotEntryAlloca = 0,
  UnsupportedObjectType,
  EscapedOrDerivedUse,
  WrongUseType,
  UnreachableUse,
  ReadBeforeDefinition,
  UnresolvedDefinition,
  AnalysisUnavailable,
  Count
};

const char* mem2RegSkipReasonName(Mem2RegSkipReason reason);

struct Mem2RegOptions {
  bool enabled = true;
  bool captureDumps = false;
};

struct Mem2RegCandidate {
  std::string function;
  SourceLoc loc;
  bool promoted = false;
  Mem2RegSkipReason reason = Mem2RegSkipReason::NotEntryAlloca;
};

struct Mem2RegResult {
  Mem2RegStatus status = Mem2RegStatus::NoChange;
  bool success = false;
  bool changed = false;
  bool inputValid = false;
  size_t promotedAllocas = 0;
  size_t removedLoads = 0;
  size_t removedStores = 0;
  size_t generatedPhis = 0;
  size_t eliminatedPhis = 0;
  std::string diagnostic;
  std::string beforeDump;
  std::string afterDump;
  std::vector<Mem2RegCandidate> candidates;
};

// Input: one verified, non-declaration Function reached through
// FlatPassManager. Live edits use only the invocation's tracked IRMutator and
// coupled CFGEditor; every candidate re-requests the shared F02
// ReachabilityAnalysis handle instead of caching a raw pointer.
//
// Output: PassStatus is derived from the real MutationSummary (Applied when
// tracked edits exist, NoChange otherwise), so a decline is NoChange and the
// manager, not the pass, validates status/mutation agreement.
// Preserved: none claimed; F01 revisions plus F02 dependency facts decide
// actual freshness. Resource budget: worklists and temporary storage are
// linear in blocks, candidate accesses, and generated phi inputs; no internal
// wall-clock or heap cap.
// Diagnostics: none of its own; the manager wraps a thrown exception or a
// rejected mutation into a fail-closed pass failure.
// Failure atomicity: detached scratch phis are discarded before any live
// commit; live commits are made only after the plan is complete. A rejection
// after a live commit is an internal invariant violation: it throws, the
// manager stops the pipeline, and the product path refuses to emit an
// artifact. The pass itself does not roll back live edits, so a failure after
// a commit leaves the in-memory Module partially rewritten and unusable for
// emission.
// Module-level exception: canonical zero Constants may be interned in the
// Module's immutable constant table. Constants are not analyzed today and are
// not instruction state, so this does not use or invalidate the mutation
// authority.
//
// `aggregate()` exposes accumulated compatibility counters and candidates for
// the module wrapper. The pass owns that aggregate, so no external result
// object can dangle or be shared across Modules/runs. On failure the aggregate
// is partial and only meaningful when `success` is false (status `Failed`).
class Mem2RegPass final : public FunctionPass {
 public:
  Mem2RegPass();
  PassResult run(Function& function, PassContext& context) override;
  const Mem2RegResult& aggregate() const { return aggregate_; }
 private:
  Mem2RegResult aggregate_;
};

// Preconditions: the caller supplies a finalized verified flat module.
// F01 maintains exact live CFG predecessor and use-def links incrementally;
// this pass does not repair/rebuild them. Generated phi predecessors are sorted.
//
// Input/output: direct i32/f32 entry-block allocas with definite initialization
// are rewritten to SSA. Terminators, block order, and unrelated instructions are
// preserved; live CFG and use-def links are updated incrementally per commit.
//
// Resource behavior: each candidate uses finite CFG worklists. Temporary memory
// is linear in blocks, candidate accesses, and generated phi inputs; there is no
// internal wall-time cutoff. Full-stage verification supplies the process memory
// and timeout limits.
//
// Failure atomicity: invalid input returns InvalidInput before mutation. A
// declined candidate leaves that candidate unchanged; unresolved SSA plans use
// unowned temporary phis and are discarded before commit. Successful earlier
// candidates remain committed if a later candidate is declined.
//
// Failure reporting: when the formal pipeline fails, `success` is false and
// `status` is `Failed` (never `Changed`); the aggregate counters describe only
// completed invocations, so callers must not treat them as a total. The module
// may then be partially rewritten, and callers must not emit it as an artifact.
Mem2RegResult runMem2Reg(Module& module,
                         const Mem2RegOptions& options = Mem2RegOptions());

}  // namespace flat
}  // namespace sysy

#endif  // SYSY_OPT_MEM2REG_H
