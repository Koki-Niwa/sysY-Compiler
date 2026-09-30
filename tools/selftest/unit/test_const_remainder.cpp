// Compile-time remainder semantics must match S05's measured runtime rule.
#include <iostream>
#include <memory>

#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

int main() {
  sysy::SourceFile file = sysy::SourceFile::fromString(
      "const-rem.sy", "const int result = 20 % 0;\nint main() { return result; }\n");
  sysy::DiagnosticEngine diagnostics;
  diagnostics.setSourceFile(&file);
  sysy::Lexer lexer(file, diagnostics);
  sysy::Parser parser(lexer, diagnostics);
  std::unique_ptr<sysy::CompUnit> unit = parser.parseCompUnit();
  bool passed = unit != nullptr && parser.errorCount() == 0;
  sysy::InitPlan plan;
  if (passed) {
    sysy::Sema sema(diagnostics);
    sema.run(*unit);
    plan = sysy::runInitLowering(*unit, sema);
    passed = diagnostics.errorCount() == 0;
  }

  const sysy::InitPlan::GlobalData* result = nullptr;
  for (const sysy::InitPlan::GlobalData& global : plan.globals) {
    if (global.name == "result") result = &global;
  }
  passed = passed && result != nullptr && !result->allZero && result->nonzero.size() == 1 &&
           result->nonzero[0].first == 0 && !result->nonzero[0].second.isFloat &&
           result->nonzero[0].second.i == 20;
  std::cout << (passed ? "PASS" : "FAIL")
            << " constant 20 % 0 folds to the dividend 20\n"
            << "检查项 1 个，失败 " << (passed ? 0 : 1) << " 个\n";
  return passed ? 0 : 1;
}
