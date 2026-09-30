#include "s10a_oracle.h"

#include <cstdint>

namespace sysy::selftest::s10a {

namespace {

int32_t checkedResult(int64_t value) {
  // Generator operands and loop bounds keep this inside int32_t.  The explicit
  // conversion documents the source-language result type at the oracle edge.
  return static_cast<int32_t>(value);
}

}  // namespace

bool evaluateGeneratedRecipe(const GeneratedCase& generated, int32_t& result,
                             std::string& error) {
  error.clear();
  int64_t value = 0;
  switch (generated.mode) {
    case GeneratedMode::ArrayCycle: {
      const int32_t values[] = {generated.a, generated.b, generated.c};
      for (uint32_t i = 0; i < generated.loopIterations; ++i) value += values[i % 3];
      break;
    }
    case GeneratedMode::RepeatedAdd:
      value = static_cast<int64_t>(generated.a) +
              static_cast<int64_t>(generated.loopIterations) * generated.b + generated.c;
      break;
    case GeneratedMode::Alternating:
      for (uint32_t i = 0; i < generated.loopIterations; ++i)
        value += i % 2 == 0 ? generated.a : -static_cast<int64_t>(generated.b);
      value += generated.c;
      break;
    default:
      error = "unknown generated recipe mode";
      return false;
  }
  result = checkedResult(value);
  return true;
}

const char* generatedOracleVersion() { return "s10a-recipe-oracle-v1"; }

}  // namespace sysy::selftest::s10a
