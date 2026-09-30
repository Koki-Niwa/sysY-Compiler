// Flat IR input and optional optimization pipeline.
#include "main/FlatPipeline.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/Mem2Reg.h"

namespace sysy {
namespace mainstage {

bool applyMem2Reg(flat::Module& module, DiagnosticEngine& diags, bool enabled,
                  bool reportStats) {
  if (!enabled) return true;
  flat::Mem2RegOptions options;
  const flat::Mem2RegResult result = flat::runMem2Reg(module, options);
  if (!result.success) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG", result.diagnostic);
    return false;
  }
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-MEM2REG-VERIFY",
                 "mem2reg 输出违反平面 IR 不变量：\n" + flat::formatViolations(violations));
    return false;
  }
  if (reportStats) {
    std::cerr << "mem2reg promoted " << result.promotedAllocas
              << " allocas generated-phis " << result.generatedPhis
              << " eliminated-phis " << result.eliminatedPhis << "\n";
  }
  return true;
}

std::string renderFromFlat(const SourceFile& src, DiagnosticEngine& diags,
                           bool flatStats, bool mem2reg) {
  std::unique_ptr<flat::Module> holder(flat::parseFlatModule(src.text(), diags));
  if (holder == nullptr) return std::string();
  flat::Module& module = *holder;
  const std::vector<flat::Violation> violations = flat::verifyFlatModule(module);
  if (!violations.empty()) {
    diags.report(DiagLevel::Error, SourceLoc(0, 0), "E-FLAT-VERIFY",
                 "读回的平面 IR 违反不变量：\n" + flat::formatViolations(violations));
  }
  if (!diags.hasError()) applyMem2Reg(module, diags, mem2reg, flatStats);
  if (flatStats && !diags.hasError()) std::cerr << module.formatStats();
  return flat::dumpModule(module);
}

}  // namespace mainstage
}  // namespace sysy
