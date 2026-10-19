// Flat reader boundaries: undefined value ids and legal forward references.
#include <cstdio>
#include <memory>
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

void testForwardCallArgument() {
  using namespace sysy::flat;
  Module original;
  const Type* i32 = typePool().i32();
  Function* function = original.createFunction("forward", typePool().voidTy());
  BasicBlock* entry = original.createBlock(function);
  BasicBlock* consumer = original.createBlock(function);
  BasicBlock* definition = original.createBlock(function);
  auto append = [&](BasicBlock* block, Instruction* inst) {
    original.ownInst(inst);
    block->addInst(inst);
  };
  Constant* seven = original.getIntConst(i32, 7);
  Instruction* value = createBin(Opcode::Add, seven, seven, {});
  append(entry, createBr(definition, {}));
  append(consumer, createCall("putint", nullptr, {value}, {}));
  append(consumer, createRet(nullptr, {}));
  append(definition, value);
  append(definition, createBr(consumer, {}));
  original.rebuildCFG();
  check(verifyFlatModule(original).empty(), "forward call fixture verifies");
  // The only instruction result is %0. Its definition dominates the call,
  // but appears later in block storage order and therefore in the real dump.
  const std::string text = dumpModule(original);
  check(text.find("call void @putint(i32 %0)") != std::string::npos,
        "real dump uses a forward call argument numbered zero");
  DiagnosticEngine diags;
  std::unique_ptr<Module> parsed(parseFlatModule(text, diags));
  check(parsed != nullptr && !diags.hasError(), "forward call argument %0 is accepted");
  if (parsed != nullptr) {
    check(verifyFlatModule(*parsed).empty(), "forward call roundtrip verifies");
    check(dumpModule(*parsed) == text, "forward call roundtrip is byte-identical");
    Function* rebuilt = parsed->function(0);
    check(rebuilt->at(1)->at(0)->operand(0) == rebuilt->at(2)->at(0),
          "forward call binds to the actual dominating instruction");
  }
  std::string nonzero = text;
  for (size_t at = 0; (at = nonzero.find("%0", at)) != std::string::npos; at += 2)
    nonzero.replace(at, 2, "%8");
  DiagnosticEngine nonzeroDiags;
  std::unique_ptr<Module> nonzeroParsed(parseFlatModule(nonzero, nonzeroDiags));
  check(nonzeroParsed != nullptr && !nonzeroDiags.hasError() &&
        verifyFlatModule(*nonzeroParsed).empty() && dumpModule(*nonzeroParsed) == text,
        "nonzero forward call argument retains its binding");

  for (const std::string& argument : {std::string("i32"), std::string("i32 %99")}) {
    DiagnosticEngine badDiags;
    const std::string invalid =
        "define void @bad() {\nL0:\n  call void @putint(" + argument +
        ")\n  ret void\n}\n";
    std::unique_ptr<Module> bad(parseFlatModule(invalid, badDiags));
    check(bad == nullptr && badDiags.hasError(),
          "missing or undefined call argument is rejected");
  }
}
}  // namespace

int main() {
  testForwardCallArgument();
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
