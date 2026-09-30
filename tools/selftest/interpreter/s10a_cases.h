#ifndef SYSY_SELFTEST_S10A_CASES_H
#define SYSY_SELFTEST_S10A_CASES_H

#include <cstdint>
#include <string>
#include <vector>

namespace sysy::selftest::s10a {

struct FixedCase {
  const char* name;
  const char* source;
  const char* input;
  const char* expectedOutput;
  int32_t expectedReturn;
  const char* tags;
  const char* expectedStderr = "";
};

const std::vector<FixedCase>& fixedCases();

}  // namespace sysy::selftest::s10a

#endif  // SYSY_SELFTEST_S10A_CASES_H
