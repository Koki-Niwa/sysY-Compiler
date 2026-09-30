#ifndef SYSY_SELFTEST_S10A_GENERATOR_H
#define SYSY_SELFTEST_S10A_GENERATOR_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sysy::selftest::s10a {

struct GeneratorConfig {
  uint64_t seed = 0x5a10a2026ULL;
  uint32_t count = 32;
  uint32_t maxSourceBytes = 512;
  uint32_t maxLoopIterations = 8;
};

struct GeneratedCase {
  uint32_t index = 0;
  uint32_t loopIterations = 0;
  int32_t expectedReturn = 0;
  uint64_t sourceHash = 0;
  std::string source;
};

std::vector<GeneratedCase> generate(const GeneratorConfig& config);
uint64_t sourceHash(std::string_view source);
bool satisfiesConstraints(const GeneratedCase& generated, const GeneratorConfig& config,
                          std::string& reason);

}  // namespace sysy::selftest::s10a

#endif  // SYSY_SELFTEST_S10A_GENERATOR_H
