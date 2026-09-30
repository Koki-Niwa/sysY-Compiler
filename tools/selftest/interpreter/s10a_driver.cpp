#include "interpreter/Interpreter.h"
#include "s10a_cases.h"
#include "s10a_generator.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace {

using sysy::CompUnit;
using sysy::DiagnosticEngine;
using sysy::Lexer;
using sysy::Parser;
using sysy::SourceFile;
using sysy::interp::Limits;
using sysy::interp::RunResult;
using sysy::interp::RunStatus;
using sysy::selftest::s10a::FixedCase;

bool analyze(const std::string& source, std::unique_ptr<CompUnit>& unit) {
  SourceFile file = SourceFile::fromString("s10a.sy", source);
  DiagnosticEngine diagnostics;
  diagnostics.setSourceFile(&file);
  Lexer lexer(file, diagnostics);
  Parser parser(lexer, diagnostics);
  unit = parser.parseCompUnit();
  if (unit == nullptr || parser.errorCount() != 0 || diagnostics.hasError()) return false;
  sysy::Sema sema(diagnostics);
  sema.run(*unit);
  return !diagnostics.hasError();
}

RunResult runSource(const std::string& source, const std::string& input,
                   const Limits& limits = Limits()) {
  std::unique_ptr<CompUnit> unit;
  if (!analyze(source, unit)) {
    RunResult result;
    result.status = RunStatus::InvalidAst;
    result.diagnostic = "parser or sema rejected source";
    return result;
  }
  return sysy::interp::run(*unit, input, limits);
}

bool sameResult(const RunResult& a, const RunResult& b) {
  return a.status == b.status && a.mainReturn == b.mainReturn && a.stdoutText == b.stdoutText &&
         a.stderrText == b.stderrText && a.diagnostic == b.diagnostic;
}

bool checkFixed(const FixedCase& test, bool& deterministic) {
  const RunResult first = runSource(test.source, test.input);
  const RunResult second = runSource(test.source, test.input);
  deterministic = sameResult(first, second);
  return first.status == RunStatus::Returned && first.mainReturn == test.expectedReturn &&
         first.stdoutText == test.expectedOutput && first.stderrText == test.expectedStderr;
}

bool isResourceLimit(const RunResult& result, const char* diagnostic, bool hasLocation) {
  return result.status == RunStatus::ResourceLimit && result.diagnostic == diagnostic &&
         (hasLocation ? (result.location.line == 1 && result.location.col != 0)
                      : result.location == sysy::SourceLoc());
}

std::unordered_map<std::string, size_t> tagCounts(const std::vector<FixedCase>& cases) {
  std::unordered_map<std::string, size_t> counts;
  for (const FixedCase& test : cases) {
    std::stringstream tags(test.tags);
    std::string tag;
    while (std::getline(tags, tag, ',')) ++counts[tag];
  }
  return counts;
}

}  // namespace

int main() {
  using namespace sysy::selftest::s10a;
  const std::vector<FixedCase>& cases = fixedCases();
  size_t fixedPass = 0;
  size_t repeatPass = 0;
  for (const FixedCase& test : cases) {
    bool deterministic = false;
    const bool pass = checkFixed(test, deterministic);
    if (pass) ++fixedPass;
    if (deterministic) ++repeatPass;
    if (!pass || !deterministic) {
      const RunResult detail = runSource(test.source, test.input);
      std::cerr << "fixed failure " << test.name << " status=" << sysy::interp::statusName(detail.status)
                << " return=" << detail.mainReturn << " output=[" << detail.stdoutText << "] diagnostic="
                << detail.diagnostic << " deterministic=" << deterministic << "\n";
    }
  }

  const GeneratorConfig config;
  const std::vector<GeneratedCase> generated = generate(config);
  const std::vector<GeneratedCase> replay = generate(config);
  bool replayPass = generated.size() == replay.size();
  bool generatedPass = replayPass;
  size_t generatedRuns = 0;
  for (size_t i = 0; i < generated.size(); ++i) {
    std::string reason;
    if (!satisfiesConstraints(generated[i], config, reason)) {
      generatedPass = false; std::cerr << "generator constraint failure " << i << ": " << reason << "\n";
    }
    if (i >= replay.size() || generated[i].index != replay[i].index ||
        generated[i].loopIterations != replay[i].loopIterations ||
        generated[i].expectedReturn != replay[i].expectedReturn ||
        generated[i].source != replay[i].source ||
        generated[i].sourceHash != replay[i].sourceHash) replayPass = false;
    const RunResult first = runSource(generated[i].source, "");
    const RunResult second = runSource(generated[i].source, "");
    if (first.status != RunStatus::Returned || first.mainReturn != generated[i].expectedReturn ||
        !sameResult(first, second)) generatedPass = false;
    else ++generatedRuns;
  }

  const bool hashVectorsPass = sourceHash("") == 14695981039346656037ULL &&
                               sourceHash("a") == 12638187200555641996ULL;

  std::string deepExpression = "int main(){return 1";
  for (int i = 0; i < 32; ++i) deepExpression += "+1";
  deepExpression += ";}";
  const RunResult expressionLimit = runSource(
      deepExpression, "", Limits{10000, 100, 100, 100, 8, 8, 8});

  std::string deepStatements = "int main(){";
  for (int i = 0; i < 32; ++i) deepStatements += "{";
  deepStatements += "return 0;";
  for (int i = 0; i < 32; ++i) deepStatements += "}";
  deepStatements += "}";
  const RunResult statementLimit = runSource(
      deepStatements, "", Limits{10000, 100, 100, 100, 8, 8, 512, 8, 1000000});

  const RunResult totalAllocationLimit = runSource(
      "int main(){int a[6];int b[6];return 0;}", "",
      Limits{10000, 100, 100, 100, 8, 8, 512, 512, 10});

  const std::string largeArraySource =
      "int main(){int a[2147483647][2147483647][2147483647];return 0;}";
  std::unique_ptr<CompUnit> largeArrayUnit;
  const bool largeArraySemaAccepted = analyze(largeArraySource, largeArrayUnit);
  RunResult largeArrayLimit;
  if (largeArraySemaAccepted) largeArrayLimit = sysy::interp::run(*largeArrayUnit, "");
  const bool expressionLimitPass =
      isResourceLimit(expressionLimit, "expression depth limit exceeded", true);
  const bool statementLimitPass =
      isResourceLimit(statementLimit, "statement nesting depth limit exceeded", true);
  const bool totalAllocationLimitPass =
      isResourceLimit(totalAllocationLimit, "total interpreter element limit exceeded", true);
  const bool largeArrayLimitPass = largeArraySemaAccepted && isResourceLimit(
      largeArrayLimit, "object size is unsupported or exceeds interpreter limit", true);

  const RunResult limit = runSource("int main(){while(1){}return 0;}", "", Limits{30, 8, 100, 100, 8, 8});
  const RunResult callLimit = runSource("int f(int x){return f(x+1);}int main(){return f(0);}", "",
                                        Limits{1000, 100, 100, 100, 8, 8});
  const RunResult objectLimit = runSource("int main(){int a[10];return 0;}", "",
                                          Limits{1000, 4, 100, 100, 8, 8});
  const RunResult outputLimit = runSource("int main(){putint(1);return 0;}", "",
                                          Limits{1000, 100, 100, 0, 8, 8});
  const RunResult inputLimit = runSource("int main(){return getint();}", "12345",
                                         Limits{1000, 100, 3, 100, 8, 8});
  const bool limitPass =
      isResourceLimit(limit, "interpreter step limit exceeded", true) &&
      isResourceLimit(callLimit, "call depth limit exceeded", true) &&
      isResourceLimit(objectLimit, "object size is unsupported or exceeds interpreter limit", true) &&
      isResourceLimit(outputLimit, "interpreter output limit exceeded", true) &&
      isResourceLimit(inputLimit, "interpreter input limit exceeded", false);
  const RunResult uninitialized = runSource("int main(){int x;return x;}", "");
  const bool uninitializedPass = uninitialized.status == RunStatus::RuntimeError;
  const RunResult missingReturn = runSource("int f(int x){if(x)return 1;}int main(){return f(0);}", "");
  const bool missingReturnPass = missingReturn.status == RunStatus::Returned &&
                                 missingReturn.mainReturn == 0;
  if (!missingReturnPass)
    std::cerr << "missing-return status=" << sysy::interp::statusName(missingReturn.status)
              << " diagnostic=" << missingReturn.diagnostic << "\n";
  sysy::CompUnit invalidUnit;
  const RunResult invalidAst = sysy::interp::run(invalidUnit, "");
  const bool invalidAstPass = invalidAst.status == RunStatus::InvalidAst;
  const bool invalidInputPass = runSource("int main( { return 0; }", "").status == RunStatus::InvalidAst;

  FixedCase corrupted = cases.front();
  corrupted.expectedReturn = corrupted.expectedReturn + 1;
  bool corruptedDeterministic = false;
  const bool corruptedMatches = checkFixed(corrupted, corruptedDeterministic);
  const bool negativeControlPass = !corruptedMatches && corruptedDeterministic;

  const auto counts = tagCounts(cases);
  const char* features[] = {"arith", "float", "branch", "loop", "break", "continue", "call",
                            "recursion", "array", "init", "scope", "global", "short", "io", "input",
                            "cast", "runtime", "timing", "wrap", "ub-normalized", "boundary"};
  bool coveragePass = true;
  std::cout << "S10a coverage\n";
  for (const char* feature : features) {
    const auto it = counts.find(feature);
    const size_t count = it == counts.end() ? 0 : it->second;
    std::cout << "  " << feature << "=" << count << "\n";
    if (count == 0) coveragePass = false;
  }
  std::cout << "S10a fixed=" << fixedPass << "/" << cases.size() << " repeat=" << repeatPass << "/"
            << cases.size() << " generated=" << generatedRuns << "/" << generated.size()
            << " seed=" << config.seed << " replay=" << (replayPass ? "PASS" : "FAIL")
            << " constraints=" << (generatedPass ? "PASS" : "FAIL")
            << " hash-vectors=" << (hashVectorsPass ? "PASS" : "FAIL")
            << " limits=" << (limitPass ? "PASS" : "FAIL")
            << " expression-limit=" << (expressionLimitPass ? "PASS" : "FAIL")
            << " statement-limit=" << (statementLimitPass ? "PASS" : "FAIL")
            << " total-allocation-limit=" << (totalAllocationLimitPass ? "PASS" : "FAIL")
            << " large-array-limit=" << (largeArrayLimitPass ? "PASS" : "FAIL")
            << " invalid-input=" << (invalidInputPass ? "PASS" : "FAIL")
            << " invalid-ast=" << (invalidAstPass ? "PASS" : "FAIL")
            << " uninitialized=" << (uninitializedPass ? "PASS" : "FAIL")
            << " missing-return=" << (missingReturnPass ? "PASS" : "FAIL")
            << " negative-control=" << (negativeControlPass ? "PASS" : "FAIL") << "\n";
  const bool pass = fixedPass == cases.size() && repeatPass == cases.size() && generatedPass &&
                    replayPass && generatedRuns == generated.size() && hashVectorsPass && limitPass &&
                    expressionLimitPass && statementLimitPass && totalAllocationLimitPass &&
                    largeArrayLimitPass && invalidInputPass &&
                    invalidAstPass && uninitializedPass && missingReturnPass && negativeControlPass && coveragePass;
  std::cout << (pass ? "S10a PASS\n" : "S10a FAIL\n");
  return pass ? 0 : 1;
}
