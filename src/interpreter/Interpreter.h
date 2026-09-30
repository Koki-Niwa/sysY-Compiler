// S10a: direct AST reference interpreter.
//
// The interpreter consumes a Sema annotated AST.  It owns its value store,
// control flow, and runtime library model; it never consumes any IR.
#ifndef SYSY_INTERPRETER_INTERPRETER_H
#define SYSY_INTERPRETER_INTERPRETER_H

#include <cstdint>
#include <string>
#include <string_view>

#include "frontend/Ast.h"
#include "support/SourceLoc.h"

namespace sysy::interp {

enum class RunStatus {
  Returned,
  InvalidAst,
  Unsupported,
  ResourceLimit,
  RuntimeError,
};

struct Limits {
  uint64_t maxSteps = 200000;
  uint64_t maxElements = 100000;
  uint64_t maxInputBytes = 100000;
  uint64_t maxOutputBytes = 100000;
  uint32_t maxCallDepth = 128;
  uint32_t maxInitDepth = 128;
  // Expression evaluation is recursive over the analyzed AST.  Keep a
  // conservative bound so a Sema-accepted deep expression cannot exhaust the
  // host C++ stack.  The S10a driver can lower this for a focused regression.
  uint32_t maxExpressionDepth = 512;
  // Bounds recursive statement and block descent through the analyzed AST.
  uint32_t maxStatementDepth = 512;
  // Cumulative storage-element budget charged before each object allocation.
  // It bounds aggregate storage across globals, locals, and call scopes.
  uint64_t maxTotalElements = 1000000;
};

struct RunResult {
  RunStatus status = RunStatus::InvalidAst;
  int32_t mainReturn = 0;
  std::string stdoutText;
  std::string stderrText;
  SourceLoc location;
  std::string diagnostic;

  bool ok() const { return status == RunStatus::Returned; }
};

// Run one semantically valid program.  The caller must have run Sema and must
// keep the AST alive for the duration of this call.  Input is an in-memory
// stream using the same token/character distinction as sylib.c.  `getch`
// interprets each input byte as an unsigned byte (0..255), which is the
// interpreter's deterministic oracle rule.  Target comparisons cover the
// ASCII corpus; the high-byte selftest does not claim parity with C's
// implementation-defined signed-char behavior.
RunResult run(const CompUnit& unit, std::string_view input,
              const Limits& limits = Limits());

const char* statusName(RunStatus status);

}  // namespace sysy::interp

#endif  // SYSY_INTERPRETER_INTERPRETER_H
