// F03: one bounded function-pass traversal over Flat IR; no adaptors/fixpoint.
#ifndef SYSY_OPT_FLATPASSMANAGER_H
#define SYSY_OPT_FLATPASSMANAGER_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>
#include "analysis/AnalysisManager.h"
#include "ir/CFGEditor.h"
#include "ir/IRMutator.h"

namespace sysy { namespace flat {

// A declaration, never an instruction to override F01 revisions or F02 caches.
class PreservedAnalyses {
 public:
  static PreservedAnalyses all() { return PreservedAnalyses(true); }
  static PreservedAnalyses none() { return PreservedAnalyses(false); }
  template <typename Analysis> void preserve() { types_.insert(typeid(Analysis)); }
  template <typename Analysis> bool preserves() const {
    return all_ || types_.count(typeid(Analysis)) != 0;
  }
  bool preservesAll() const { return all_; }
 private:
  explicit PreservedAnalyses(bool all) : all_(all) {}
  bool all_;
  std::set<std::type_index> types_;
};

enum class PassStatus { NoChange, Applied, Declined, Failed };

// Sorted keys make counters independent of insertion/hash order.
struct PassStats {
  std::map<std::string, uint64_t> counters;
};

// Semantic result only. A pass cannot report 'changed'.
struct PassResult {
  PassStatus status = PassStatus::NoChange;
  PreservedAnalyses preserved = PreservedAnalyses::none();
  PassStats stats;
  std::string diagnostic;
};

// Created by the manager for one invocation only. The AnalysisManager is
// shared for the entire pipeline run; the mutator and CFGEditor are fresh.
// Mutation of finalized live IR must use these tracked editors.
class PassContext {
 public:
  PassContext(const PassContext&) = delete;
  PassContext& operator=(const PassContext&) = delete;
  AnalysisManager& analyses() const { return analyses_; }
  IRMutator& mutator() const { return mutator_; }
  CFGEditor& cfg() const { return cfg_; }
 private:
  friend class FlatPassManager;
  PassContext(AnalysisManager& analyses, IRMutator& mutator, CFGEditor& cfg)
      : analyses_(analyses), mutator_(mutator), cfg_(cfg) {}
  AnalysisManager& analyses_;
  IRMutator& mutator_;
  CFGEditor& cfg_;
};

class FunctionPass {
 public:
  explicit FunctionPass(std::string name);
  virtual ~FunctionPass() = default;
  FunctionPass(const FunctionPass&) = delete;
  FunctionPass& operator=(const FunctionPass&) = delete;
  const std::string& name() const { return name_; }
  virtual PassResult run(Function&, PassContext&) = 0;
 private:
  const std::string name_;
};

enum class VerificationOutcome { NotRun, Passed, Failed };
enum class PipelineStatus {
  Completed, InvalidInput, PassFailed, ContractViolation, VerificationFailed
};

struct InvocationRecord {
  std::string passName;
  std::string functionName;
  size_t functionIndex = 0;  // Module function order (including declarations).
  bool disabled = false;     // Not a NoChange invocation.
  std::optional<PassStatus> reportedStatus;
  std::optional<PassStatus> finalStatus;
  bool changed = false;      // Only IRMutator::summary().changed().
  MutationSummary mutation;  // Authoritative tracked operations/kinds/functions.
  // Stable, ordered projection of mutation.affectedFunctions (no pointer output).
  std::vector<size_t> affectedFunctionIndices;
  PreservedAnalyses preserved = PreservedAnalyses::none();
  PassStats stats;
  VerificationOutcome verification = VerificationOutcome::NotRun;
  bool contractViolation = false;
  std::string diagnostic;
  std::optional<uint64_t> elapsedNanoseconds;
  std::string beforeDump, afterDump;
};

struct PassManagerOptions {
  bool verifyEach = false;
  bool instrument = false;
  bool captureDumps = false;
  bool measureTime = false;
  std::set<std::string> disabledPassNames;
};

struct PipelineResult {
  PipelineStatus status = PipelineStatus::Completed;
  bool changed = false;  // Derived from tracked mutations even without instrumentation.
  std::string diagnostic;
  std::vector<InvocationRecord> invocations;
  bool ok() const { return status == PipelineStatus::Completed; }
};

class FlatPassManager {
 public:
  // Reject null or duplicate names. FunctionPass constructor rejects empty names.
  bool addPass(std::unique_ptr<FunctionPass> pass);
  PipelineResult run(Module& module, const PassManagerOptions& options = {});
 private:
  struct Entry {
    std::string name;
    std::unique_ptr<FunctionPass> pass;
  };
  std::vector<Entry> passes_;
};
}} // namespace sysy::flat
#endif
