#include "s10a_generator.h"

#include <string>

namespace sysy::selftest::s10a {
namespace {

uint64_t next(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

int32_t small(uint64_t value) { return static_cast<int32_t>(value % 19ULL) - 9; }

}  // namespace

uint64_t sourceHash(std::string_view source) {
  // FNV-1a 64-bit offset basis.
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : source) { hash ^= byte; hash *= 1099511628211ULL; }
  return hash;
}

std::vector<GeneratedCase> generate(const GeneratorConfig& config) {
  std::vector<GeneratedCase> result;
  result.reserve(config.count);
  uint64_t state = config.seed;
  const uint32_t maxLoop = config.maxLoopIterations == 0 ? 1 : config.maxLoopIterations;
  for (uint32_t index = 0; index < config.count; ++index) {
    const int32_t a = small(next(state));
    const int32_t b = small(next(state));
    const int32_t c = small(next(state));
    const uint32_t n = 1 + static_cast<uint32_t>(next(state) % maxLoop);
    const GeneratedMode mode = static_cast<GeneratedMode>(next(state) % 3ULL);
    std::string source;
    if (mode == GeneratedMode::ArrayCycle) {
      source = "int main(){int a[3]={" + std::to_string(a) + "," + std::to_string(b) + "," +
               std::to_string(c) + "};int i=0,s=0;while(i<" + std::to_string(n) +
               "){s=s+a[i%3];i=i+1;}return s;}";
    } else if (mode == GeneratedMode::RepeatedAdd) {
      source = "int add(int x,int y){return x+y;}int main(){int i=0,s=" + std::to_string(a) +
               ";while(i<" + std::to_string(n) + "){s=add(s," + std::to_string(b) +
               ");i=i+1;}return s+" + std::to_string(c) + ";}";
    } else {
      source = "int main(){int i=0,s=0;while(i<" + std::to_string(n) +
               "){if(i%2==0)s=s+" + std::to_string(a) + ";else s=s-" + std::to_string(b) +
               ";i=i+1;}return s+" + std::to_string(c) + ";}";
    }
    result.push_back(GeneratedCase{index, mode, a, b, c, n, sourceHash(source), std::move(source)});
  }
  return result;
}

bool satisfiesConstraints(const GeneratedCase& generated, const GeneratorConfig& config,
                          std::string& reason) {
  if (generated.source.empty()) { reason = "empty source"; return false; }
  if (generated.loopIterations == 0 || generated.loopIterations > config.maxLoopIterations) {
    reason = "loop bound exceeded"; return false;
  }
  if (generated.source.size() > config.maxSourceBytes) { reason = "source size exceeded"; return false; }
  if (generated.sourceHash != sourceHash(generated.source)) { reason = "source hash mismatch"; return false; }
  if (generated.source.find("for") != std::string::npos || generated.source.find("tensor") != std::string::npos) {
    reason = "unsupported syntax in generated source"; return false;
  }
  if (generated.source.find("while(i<") == std::string::npos) {
    reason = "generated source has no bounded loop"; return false;
  }
  return true;
}

}  // namespace sysy::selftest::s10a
