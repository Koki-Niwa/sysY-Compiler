// Compile-time remainder semantics must match S05's measured runtime rule.
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "frontend/InitLowering.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "frontend/Sema.h"
#include "structured/IRGen.h"
#include "structured/StructuredIR.h"
#include "support/Diagnostic.h"
#include "support/SourceFile.h"

namespace {

using sysy::CompUnit;
using sysy::DiagnosticEngine;
using sysy::InitPlan;
using sysy::Lexer;
using sysy::Parser;
using sysy::Sema;
using sysy::SourceFile;
using sysy::sir::Arena;
using sysy::sir::Op;
using sysy::sir::OpKind;
using sysy::sir::Region;
using sysy::sir::Value;

Op* buildStructured(const std::string& source, Arena& arena, DiagnosticEngine& diagnostics) {
  SourceFile file = SourceFile::fromString("dynamic-rem.sy", source);
  diagnostics.setSourceFile(&file);
  Lexer lexer(file, diagnostics);
  Parser parser(lexer, diagnostics);
  std::unique_ptr<CompUnit> unit = parser.parseCompUnit();
  if (unit == nullptr || parser.errorCount() != 0) return nullptr;

  Sema sema(diagnostics);
  sema.run(*unit);
  if (diagnostics.errorCount() != 0) return nullptr;
  const InitPlan plan = sysy::runInitLowering(*unit, sema);
  if (diagnostics.errorCount() != 0) return nullptr;
  return sysy::sir::buildModule(arena, *unit, plan, "dynamic-rem.sy", diagnostics);
}

Op* findFunction(Op* module, const std::string& name) {
  Op* result = nullptr;
  sysy::sir::forEachOp(module, [&](Op* op) {
    if (op->kind == OpKind::Func && op->strAttr(0) == name) result = op;
  });
  return result;
}

Value firstEntryAlloca(Op* function) {
  Region* body = function == nullptr ? nullptr : function->region(0);
  if (body == nullptr) return nullptr;
  for (Op* op : body->ops()) {
    if (op != nullptr && op->kind == OpKind::Alloca) return op->result(0);
  }
  return nullptr;
}

bool hasStoreOfLoadFrom(Op* root, Value sourceSlot) {
  bool found = false;
  sysy::sir::forEachOp(root, [&](Op* op) {
    if (op->kind != OpKind::Store || op->numOperands() < 2) return;
    Value stored = op->operand(0);
    if (stored == nullptr || stored->definer == nullptr || stored->definer->kind != OpKind::Load) {
      return;
    }
    Op* load = stored->definer;
    if (load->numOperands() >= 1 && load->operand(0) == sourceSlot) found = true;
  });
  return found;
}

bool hasDirectIntStore(const Region* region, int32_t value) {
  if (region == nullptr) return false;
  for (Op* op : region->ops()) {
    if (op == nullptr || op->kind != OpKind::Store || op->numOperands() < 1) continue;
    Value stored = op->operand(0);
    if (stored != nullptr && stored->definer != nullptr &&
        stored->definer->kind == OpKind::Int && stored->definer->intAttr(0) == value) {
      return true;
    }
  }
  return false;
}

bool hasZeroOverflowBranch(Op* function) {
  bool found = false;
  sysy::sir::forEachOp(function, [&](Op* op) {
    if (op->kind == OpKind::If && op->numRegions() == 2 && hasDirectIntStore(op->region(1), 0)) {
      found = true;
    }
  });
  return found;
}

bool testDynamicCodegen() {
  DiagnosticEngine diagnostics;
  Arena arena;
  Op* module = buildStructured(
      "int rem_zero_positive() { int x = 20; return x % 0; }\n"
      "int rem_zero_negative() { int x = -20; return x % 0; }\n"
      "int rem_nonzero_negative() { int x = -20; int y = 6; return x % y; }\n"
      "int rem_int_min() { int x = 0x80000000; return x % -1; }\n"
      "int main() { return rem_zero_positive(); }\n",
      arena, diagnostics);

  const bool built = module != nullptr && diagnostics.errorCount() == 0;
  std::cout << (built ? "PASS" : "FAIL")
            << " dynamic remainder source reaches structured IR generation\n";
  if (!built) return false;

  Op* positive = findFunction(module, "rem_zero_positive");
  Op* negative = findFunction(module, "rem_zero_negative");
  Op* ordinary = findFunction(module, "rem_nonzero_negative");
  Op* overflow = findFunction(module, "rem_int_min");
  const bool positiveDividend = positive != nullptr &&
                                hasStoreOfLoadFrom(positive, firstEntryAlloca(positive));
  const bool negativeDividend = negative != nullptr &&
                                hasStoreOfLoadFrom(negative, firstEntryAlloca(negative));
  const bool ordinaryRemainder = ordinary != nullptr &&
                                 sysy::sir::countOps(ordinary, OpKind::ModI) != 0;
  const bool overflowZero = overflow != nullptr && hasZeroOverflowBranch(overflow);
  std::cout << (positiveDividend ? "PASS" : "FAIL")
            << " dynamic positive x % 0 stores the dividend load\n"
            << (negativeDividend ? "PASS" : "FAIL")
            << " dynamic negative x % 0 stores the dividend load\n"
            << (ordinaryRemainder ? "PASS" : "FAIL")
            << " ordinary x % y retains ModI\n"
            << (overflowZero ? "PASS" : "FAIL")
            << " INT_MIN % -1 retains an explicit zero branch\n";
  if (!positiveDividend || !negativeDividend || !ordinaryRemainder || !overflowZero) {
    std::cout << "FAIL dynamic remainder code generation checks\n";
  }
  return positiveDividend && negativeDividend && ordinaryRemainder && overflowZero;
}

}  // namespace

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
  passed = passed && testDynamicCodegen();
  return passed ? 0 : 1;
}
