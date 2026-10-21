// S24c-c matcher: canonical matmul nest and a local-accumulator negative.
#include <cstdio>
#include <string>
#include "ir/FlatDump.h"
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
  sysy::DiagnosticEngine diags;
  Module* module = parseFlatModule(text, diags);
  check(module != nullptr, "matmul fixture parses");
  if (module != nullptr) {
    Function* function = module->findFunction("main");
    check(function != nullptr, "matmul function exists");
    if (function != nullptr) {
      const auto candidate = matchMatmulNest(*function);
      check(candidate.has_value(), "canonical matmul nest is recognized");
      check(candidate && candidate->outerBound == 64 &&
                candidate->innerBound == 64 && candidate->reductionBound == 64,
            "matmul bounds are 64x64x64");
    }
    std::string bad = text;
    const std::string from = "store i32 %3, ptr[i32] %36";
    const std::string to = "store i32 %30, ptr[i32] %36";
    const size_t at = bad.find(from);
    if (at != std::string::npos) bad.replace(at, from.size(), to);
    Module* badModule = parseFlatModule(bad, diags);
    check(badModule != nullptr, "negative fixture parses");
    if (badModule != nullptr && !badModule->functions().empty()) {
      check(!matchMatmulNest(*badModule->functions().front()).has_value(),
            "store of the product, not the accumulator, is rejected");
      delete badModule;
    }
    delete module;
  }
  std::printf("MATMUL_MATCH_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
