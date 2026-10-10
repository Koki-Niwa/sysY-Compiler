// ============================================================================
// opt/Mem2Reg.h -- conservative scalar alloca promotion for flat IR.
//
// The pass accepts a verified flat module, promotes only direct scalar alloca
// accesses, and leaves a candidate untouched when its safety cannot be proved.
// It is deliberately a small explicit API; no general pass manager is needed.
// ============================================================================
#ifndef SYSY_OPT_MEM2REG_H
#define SYSY_OPT_MEM2REG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ir/Module.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {

enum class Mem2RegStatus : uint8_t {
  Disabled = 0,
  InvalidInput,
  NoChange,
  Changed
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
Mem2RegResult runMem2Reg(Module& module,
                         const Mem2RegOptions& options = Mem2RegOptions());

}  // namespace flat
}  // namespace sysy

#endif  // SYSY_OPT_MEM2REG_H
