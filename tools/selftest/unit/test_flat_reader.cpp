// Flat reader boundaries: undefined value ids and legal forward references.
#include <cstdio>
#include <string>

#include "ir/FlatDump.h"
#include "ir/FlatVerifier.h"
#include "support/Diagnostic.h"

using sysy::DiagnosticEngine;
using sysy::flat::Module;
using sysy::flat::dumpModule;
using sysy::flat::parseFlatModule;
using sysy::flat::verifyFlatModule;

namespace {
int checks = 0;
int failures = 0;
void check(bool ok, const char* what) {
  ++checks;
  failures += ok ? 0 : 1;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
}
}  // namespace

int main() {
  {
    DiagnosticEngine diags;
    const std::string text =
        "define i32 @main() {\n"
        "  %10 = i32 7\n"
        "L0:\n"
        "  ret i32 %0\n"
        "}\n";
    Module* module = parseFlatModule(text, diags);
    check(module == nullptr, "undefined value id is rejected");
    check(diags.hasError(), "undefined value id produces a diagnostic");
    delete module;
  }
  {
    DiagnosticEngine diags;
    // L1 appears before L2 but L2 dominates L1: %2 is a legal forward ref.
    const std::string text =
        "define i32 @main() {\n"
        "  %1 = i32 0\n"
        "  %3 = i32 1\n"
        "L0:\n"
        "  br L2\n"
        "L1:\n"
        "  %0 = add i32 %1, %2\n"
        "  br L3\n"
        "L2:\n"
        "  %2 = add i32 %1, %3\n"
        "  br L1\n"
        "L3:\n"
        "  ret i32 %0\n"
        "}\n";
    Module* module = parseFlatModule(text, diags);
    check(module != nullptr, "legal forward reference is accepted");
    if (module != nullptr) {
      check(verifyFlatModule(*module).empty(), "forward-ref module verifies");
      check(dumpModule(*module) == text, "forward ref round-trips byte-identically");
      delete module;
    }
  }
  std::printf("FLAT_READER_CHECKS=%d FAILURES=%d\n", checks, failures);
  return failures ? 1 : 0;
}
