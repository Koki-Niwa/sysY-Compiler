// S24c-c matcher: canonical matmul nest and a local-accumulator negative.
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "opt/FlatPassManager.h"
#include "opt/MatmulBlocking.h"
using namespace sysy::flat;
namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}

std::string replaceOnce(const std::string& text, const std::string& from,
                        const std::string& to) {
  const size_t at = text.find(from);
  if (at == std::string::npos) return text;
  std::string result = text;
  result.replace(at, from.size(), to);
  return result;
}

std::string replaceAll(std::string text, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return text;
  size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) {
    text.replace(at, from.size(), to);
    at += to.size();
  }
  return text;
}

std::string addConstantFlag(const std::string& text) {
  return replaceOnce(text, "L0:\n  br L1 @line 6",
                     "L0:\n  %40 = icmp slt i32 %4, %4 @line 6\n"
                     "  br L1 @line 6");
}

void rejectFixture(const std::string& name, const std::string& text) {
  sysy::DiagnosticEngine diags;
  Module* module = parseFlatModule(text, diags);
  check(module != nullptr, (name + ": parses").c_str());
  if (module == nullptr) return;
  const auto inputViolations = verifyFlatModule(*module);
  check(inputViolations.empty(), (name + ": input verifies").c_str());
  Function* function = module->findFunction("main");
  check(function != nullptr && !matchMatmulNest(*function).has_value(),
        (name + ": matcher rejects").c_str());
  if (function != nullptr) {
    const std::string before = dumpModule(*module);
    FlatPassManager pm;
    pm.addPass(std::make_unique<MatmulBlockingPass>());
    PassManagerOptions options;
    options.verifyEach = true;
    options.instrument = true;
    const auto run = pm.run(*module, options);
    check(run.ok() && !run.changed && run.invocations.size() == 1 &&
              run.invocations.front().reportedStatus == PassStatus::NoChange &&
              dumpModule(*module) == before && verifyFlatModule(*module).empty(),
          (name + ": pass is no-change and atomic").c_str());
  }
  delete module;
}

void acceptFixture(const std::string& name, const std::string& text,
                   bool requireDirectJToK = false,
                   const std::string& reductionLabel = "L9",
                   size_t expectedConditionalEdgesToK = 1) {
  sysy::DiagnosticEngine diags;
  Module* module = parseFlatModule(text, diags);
  check(module != nullptr, (name + ": parses").c_str());
  if (module == nullptr) return;
  check(verifyFlatModule(*module).empty(), (name + ": input verifies").c_str());
  Function* function = module->findFunction("main");
  const auto candidate = function == nullptr ? std::nullopt :
                                              matchMatmulNest(*function);
  check(candidate.has_value(), (name + ": matcher accepts").c_str());
  check(candidate && candidate->outerBound == 64 &&
            candidate->innerBound == 64 && candidate->reductionBound == 64,
        (name + ": bounds are 64x64x64").c_str());
  if (function != nullptr) {
    const std::string before = dumpModule(*module);
    FlatPassManager pm;
    pm.addPass(std::make_unique<MatmulBlockingPass>());
    PassManagerOptions options;
    options.verifyEach = true;
    options.instrument = true;
    const auto run = pm.run(*module, options);
    check(run.ok() && run.changed && !run.invocations.empty() &&
              run.invocations.front().reportedStatus == PassStatus::Applied &&
              verifyFlatModule(*module).empty() && dumpModule(*module) != before,
          (name + ": pass applies and output verifies").c_str());
    if (requireDirectJToK) {
      size_t conditionalEdgesToK = 0;
      const std::string after = dumpModule(*module);
      size_t lineStart = 0;
      while (lineStart < after.size()) {
        const size_t lineEnd = after.find('\n', lineStart);
        const std::string line = after.substr(
            lineStart, lineEnd == std::string::npos ? std::string::npos :
                                                         lineEnd - lineStart);
        if (line.find("br i1") != std::string::npos &&
            line.find("label " + reductionLabel) != std::string::npos)
          ++conditionalEdgesToK;
        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
      }
      check(conditionalEdgesToK == expectedConditionalEdgesToK,
            "blocked matmul has a direct conditional J-to-K edge");
    }
  }
  delete module;
}
}  // namespace
int main() {
  const std::string text = R"(@A = global [64 x [64 x i32]] zeroinitializer @line 1
@B = global [64 x [64 x i32]] zeroinitializer @line 2
@C = global [64 x [64 x i32]] zeroinitializer @line 3
define i32 @main() {
  %4 = i32 0 @line 5
  %6 = i32 64 @line 6
  %8 = i32 1 @line 6
  %11 = ptr[[64 x [64 x i32]]] @C
  %20 = ptr[[64 x [64 x i32]]] @A
  %26 = ptr[[64 x [64 x i32]]] @B
L0:
  br L1 @line 6
L1:
  %0 = phi i32 [(%4 L0), (%7 L3)] @line 6
  %5 = icmp slt i32 %0, %6 @line 6
  br i1 %5, label L2, label L4 @line 6
L2:
  br L5 @line 8
L3:
  %7 = add i32 %0, %8 @line 6
  br L1 @line 6
L4:
  %9 = sext i32 %4 to i64 @line 20
  %10 = getelementptr [64 x i32], ptr[[64 x [64 x i32]]] %11, i64 %9 @line 20
  %12 = getelementptr i32, ptr[[64 x i32]] %10, i64 %9 @line 20
  %13 = load i32, ptr[i32] %12 @line 20
  ret i32 %13 @line 20
L5:
  %1 = phi i32 [(%4 L2), (%16 L7)] @line 8
  %15 = icmp slt i32 %1, %6 @line 8
  br i1 %15, label L6, label L8 @line 8
L6:
  br L9 @line 11
L7:
  %16 = add i32 %1, %8 @line 8
  br L5 @line 8
L8:
  br L3 @line 6
L9:
  %2 = phi i32 [(%4 L6), (%32 L11)] @line 11
  %3 = phi i32 [(%4 L6), (%31 L11)] @line 9
  %17 = icmp slt i32 %2, %6 @line 11
  br i1 %17, label L10, label L12 @line 11
L10:
  %18 = sext i32 %0 to i64 @line 12
  %19 = getelementptr [64 x i32], ptr[[64 x [64 x i32]]] %20, i64 %18 @line 12
  %21 = sext i32 %2 to i64 @line 12
  %22 = getelementptr i32, ptr[[64 x i32]] %19, i64 %21 @line 12
  %23 = load i32, ptr[i32] %22 @line 12
  %24 = sext i32 %2 to i64 @line 12
  %25 = getelementptr [64 x i32], ptr[[64 x [64 x i32]]] %26, i64 %24 @line 12
  %27 = sext i32 %1 to i64 @line 12
  %28 = getelementptr i32, ptr[[64 x i32]] %25, i64 %27 @line 12
  %29 = load i32, ptr[i32] %28 @line 12
  %30 = mul i32 %23, %29 @line 12
  %31 = add i32 %3, %30 @line 12
  br L11 @line 11
L11:
  %32 = add i32 %2, %8 @line 11
  br L9 @line 11
L12:
  %33 = sext i32 %0 to i64 @line 15
  %34 = getelementptr [64 x i32], ptr[[64 x [64 x i32]]] %11, i64 %33 @line 15
  %35 = sext i32 %1 to i64 @line 15
  %36 = getelementptr i32, ptr[[64 x i32]] %34, i64 %35 @line 15
  store i32 %3, ptr[i32] %36 @line 15
  br L7 @line 8
})";
  acceptFixture("canonical matmul", text, true);

  std::string directJToK = replaceOnce(
      text, "br i1 %15, label L6, label L8",
      "br i1 %15, label L9, label L8");
  directJToK = replaceAll(directJToK, "(%4 L6)", "(%4 L5)");
  directJToK = replaceOnce(directJToK, "L6:\n  br L9 @line 11\n", "");
  // Keep Flat IR's block indices dense after removing the K preheader.
  for (int label = 12; label >= 7; --label)
    directJToK = replaceAll(directJToK, "L" + std::to_string(label),
                            "Q" + std::to_string(label));
  for (int label = 7; label <= 12; ++label)
    directJToK = replaceAll(directJToK, "Q" + std::to_string(label),
                            "L" + std::to_string(label - 1));
  acceptFixture("direct J-to-K input", directJToK, true, "L8", 2);

  // Every loop header must branch on the ICmp that was matched against its
  // induction phi. A dominating constant i1 keeps these inputs verifier-valid
  // while making the stale compare genuinely unused.
  rejectFixture("outer uses an unused compare", replaceOnce(
      addConstantFlag(text), "br i1 %5, label L2, label L4",
      "br i1 %40, label L2, label L4"));
  rejectFixture("inner uses an unused compare", replaceOnce(
      addConstantFlag(text), "br i1 %15, label L6, label L8",
      "br i1 %40, label L6, label L8"));
  rejectFixture("reduction uses an unused compare", replaceOnce(
      addConstantFlag(text), "br i1 %17, label L10, label L12",
      "br i1 %40, label L10, label L12"));

  rejectFixture("conditional outer preheader", replaceOnce(
      text, "L0:\n  br L1 @line 6",
      "L0:\n  %40 = icmp slt i32 %4, %4 @line 6\n"
      "  br i1 %40, label L4, label L1 @line 6"));

  std::string exitPhi = replaceOnce(
      text, "L4:\n  %9 = sext i32 %4 to i64 @line 20",
      "L4:\n  %40 = phi i32 [(%4 L1)] @line 20\n"
      "  %9 = sext i32 %4 to i64 @line 20");
  exitPhi = replaceOnce(exitPhi, "ret i32 %13 @line 20",
                        "ret i32 %40 @line 20");
  rejectFixture("exit has a phi", exitPhi);

  std::string skippedPathDefinition = replaceOnce(
      text, "L2:\n  br L5 @line 8",
      "L2:\n  %40 = add i32 %4, %8 @line 8\n"
      "  br L5 @line 8");
  skippedPathDefinition = replaceOnce(
      skippedPathDefinition, "%31 = add i32 %3, %30 @line 12",
      "%41 = add i32 %40, %30 @line 12\n"
      "  %31 = add i32 %3, %41 @line 12");
  rejectFixture("skipped path definition feeds retained body",
                skippedPathDefinition);

  rejectFixture("outer latch has an unused pure definition", replaceOnce(
      text, "L3:\n  %7 = add i32 %0, %8 @line 6",
      "L3:\n  %40 = add i32 %4, %8 @line 6\n"
      "  %7 = add i32 %0, %8 @line 6"));
  rejectFixture("inner latch has an unused pure definition", replaceOnce(
      text, "L7:\n  %16 = add i32 %1, %8 @line 8",
      "L7:\n  %40 = add i32 %4, %8 @line 8\n"
      "  %16 = add i32 %1, %8 @line 8"));
  rejectFixture("reduction latch has an unused pure definition", replaceOnce(
      text, "L11:\n  %32 = add i32 %2, %8 @line 11",
      "L11:\n  %40 = add i32 %4, %8 @line 11\n"
      "  %32 = add i32 %2, %8 @line 11"));

  std::string extraEntry = replaceOnce(
      addConstantFlag(text), "%0 = phi i32 [(%4 L0), (%7 L3)] @line 6",
      "%0 = phi i32 [(%4 L0), (%7 L3), (%4 L8)] @line 6");
  extraEntry = replaceOnce(extraEntry, "L8:\n  br L3 @line 6",
                           "L8:\n  br i1 %40, label L1, label L3 @line 6");
  rejectFixture("outer has an additional entry", extraEntry);

  std::string extraOutputStore = replaceOnce(
      text, "  store i32 %3, ptr[i32] %36 @line 15",
      "  store i32 %1, ptr[i32] %36 @line 15\n"
      "  store i32 %3, ptr[i32] %36 @line 15");
  rejectFixture("output has an additional store", extraOutputStore);

  std::string inputWrite = replaceOnce(
      text, "  br L11 @line 11",
      "  store i32 %23, ptr[i32] %22 @line 12\n"
      "  br L11 @line 11");
  rejectFixture("body writes an input", inputWrite);

  std::string accumulatorLiveOut = replaceOnce(
      text, "L7:\n  %16 = add i32 %1, %8 @line 8",
      "L7:\n  %16 = add i32 %1, %8 @line 8\n"
      "  %40 = add i32 %3, %4 @line 8");
  rejectFixture("accumulator has a live-out use in J latch",
                accumulatorLiveOut);

  rejectFixture("noncanonical C index", replaceOnce(
      text, "%35 = sext i32 %1 to i64 @line 15",
      "%35 = sext i32 %0 to i64 @line 15"));

  rejectFixture("store of non-accumulator is rejected", replaceOnce(
      text, "store i32 %3, ptr[i32] %36",
      "store i32 %0, ptr[i32] %36"));

  const std::string swappedPhiOrder = replaceOnce(
      text,
      "  %2 = phi i32 [(%4 L6), (%32 L11)] @line 11\n"
      "  %3 = phi i32 [(%4 L6), (%31 L11)] @line 9",
      "  %3 = phi i32 [(%4 L6), (%31 L11)] @line 9\n"
      "  %2 = phi i32 [(%4 L6), (%32 L11)] @line 11");
  acceptFixture("swapped reduction phi order", swappedPhiOrder, true);

  std::printf("MATMUL_MATCH_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
