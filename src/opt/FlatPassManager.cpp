#include "opt/FlatPassManager.h"
#include <chrono>
#include <exception>
#include <stdexcept>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"

namespace sysy { namespace flat {
FunctionPass::FunctionPass(std::string name) : name_(std::move(name)) {
  if (name_.empty()) throw std::invalid_argument("FunctionPass name must be nonempty");
}

bool FlatPassManager::addPass(std::unique_ptr<FunctionPass> pass) {
  if (!pass || pass->name().empty()) return false;
  for (const Entry& entry : passes_)
    if (entry.name == pass->name()) return false;
  const std::string name = pass->name();
  passes_.push_back({name, std::move(pass)});
  return true;
}

PipelineResult FlatPassManager::run(Module& module, const PassManagerOptions& options) {
  PipelineResult result;
  if (options.verifyEach) {
    const auto errors = verifyFlatModule(module);
    if (!errors.empty()) {
      result.status = PipelineStatus::InvalidInput;
      result.diagnostic = "FlatPassManager entry verification failed: " +
                          formatViolations(errors);
      return result;
    }
  }
  // F02 owns all cache truth: preservation claims never update/revive a cache.
  AnalysisManager analyses(module);
  const bool record = options.instrument || options.captureDumps || options.measureTime;
  // Capture one finite function order; declarations never get an invocation.
  const std::vector<Function*> functions = module.functions();
  for (const Entry& entry : passes_) {
    for (size_t index = 0; index < functions.size(); ++index) {
      Function* fn = functions[index];
      if (fn->isDeclaration()) continue;
      InvocationRecord item;
      item.passName = entry.name;
      item.functionName = fn->name();
      item.functionIndex = index;
      if (options.disabledPassNames.count(entry.name)) {
        item.disabled = true;
        if (record) result.invocations.push_back(std::move(item));
        continue;
      }
      // A new session per invocation, shared by both tracked edit authorities.
      IRMutator mutator(module);
      CFGEditor cfg(mutator);
      PassContext context(analyses, mutator, cfg);
      if (options.captureDumps) item.beforeDump = dumpModule(module);
      PassResult passResult;
      const auto started = options.measureTime ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
      try {
        passResult = entry.pass->run(*fn, context);
      } catch (const std::exception& error) {
        passResult.status = PassStatus::Failed;
        passResult.diagnostic = std::string("pass threw exception: ") + error.what();
      } catch (...) {
        passResult.status = PassStatus::Failed;
        passResult.diagnostic = "pass threw non-standard exception";
      }
      if (options.measureTime) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        item.elapsedNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
      }
      item.reportedStatus = passResult.status;
      item.finalStatus = passResult.status;
      item.preserved = std::move(passResult.preserved);
      item.stats = std::move(passResult.stats);
      item.diagnostic = std::move(passResult.diagnostic);
      item.mutation = mutator.summary();
      item.changed = item.mutation.changed();
      result.changed |= item.changed;
      for (size_t i = 0; i < functions.size(); ++i)
        if (item.mutation.affectedFunctions.count(functions[i]))
          item.affectedFunctionIndices.push_back(i);
      if (options.captureDumps) item.afterDump = dumpModule(module);
      if (options.verifyEach) {
        const auto errors = verifyFlatModule(module);
        item.verification = errors.empty() ? VerificationOutcome::Passed
                                           : VerificationOutcome::Failed;
        if (!errors.empty()) {
          if (!item.diagnostic.empty()) item.diagnostic += "; ";
          item.diagnostic += "FlatVerifier: " + formatViolations(errors);
        }
      }
      item.contractViolation =
          (item.changed && passResult.status != PassStatus::Applied) ||
          (!item.changed && passResult.status == PassStatus::Applied);
      if (item.contractViolation) {
        item.finalStatus = PassStatus::Failed;
        if (!item.diagnostic.empty()) item.diagnostic += "; ";
        item.diagnostic += "pass contract violation: reported status contradicts "
                           "tracked live mutation";
        result.status = PipelineStatus::ContractViolation;
      } else if (item.verification == VerificationOutcome::Failed) {
        item.finalStatus = PassStatus::Failed;
        result.status = PipelineStatus::VerificationFailed;
      } else if (passResult.status == PassStatus::Failed) {
        result.status = PipelineStatus::PassFailed;
        if (item.diagnostic.empty()) item.diagnostic = "pass reported failure";
      }
      if (!result.ok()) {
        result.diagnostic = "pass '" + entry.name + "' function '" +
            item.functionName + "' (index " + std::to_string(index) + "): " +
            item.diagnostic;
      }
      if (record || !result.ok()) result.invocations.push_back(std::move(item));
      if (!result.ok()) return result;
    }
  }
  return result;
}
}} // namespace sysy::flat
