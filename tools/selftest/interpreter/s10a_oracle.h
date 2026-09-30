#ifndef SYSY_SELFTEST_S10A_ORACLE_H
#define SYSY_SELFTEST_S10A_ORACLE_H

#include <cstdint>
#include <string>

#include "s10a_generator.h"

namespace sysy::selftest::s10a {

// This oracle consumes the generator's serialized recipe.  It does not inspect
// or execute the generated source and shares no expression-building helpers
// with the generator or the AST interpreter.
bool evaluateGeneratedRecipe(const GeneratedCase& generated, int32_t& result,
                             std::string& error);
const char* generatedOracleVersion();

}  // namespace sysy::selftest::s10a

#endif  // SYSY_SELFTEST_S10A_ORACLE_H
