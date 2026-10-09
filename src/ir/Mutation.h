// F01 tracked facts only; analysis invalidation belongs to F02.
#ifndef SYSY_IR_MUTATION_H
#define SYSY_IR_MUTATION_H
#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace sysy { namespace flat {
class Function;

enum class MutationKind : uint32_t {
  None = 0, DataFlow = 1u, CFG = 2u, Memory = 4u,
  Calls = 8u, Globals = 16u, Interface = 32u
};
inline MutationKind operator|(MutationKind a, MutationKind b) {
  return static_cast<MutationKind>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline MutationKind& operator|=(MutationKind& a, MutationKind b) {
  return a = a | b;
}
inline bool hasMutation(MutationKind kinds, MutationKind bit) {
  return (static_cast<uint32_t>(kinds) & static_cast<uint32_t>(bit)) != 0;
}
struct IRRevision {
  uint64_t any = 0, dataFlow = 0, cfg = 0, memory = 0;
  uint64_t calls = 0, globals = 0, interface = 0;
  void bump(MutationKind kinds) {
    if (kinds == MutationKind::None) return;
    ++any;
    if (hasMutation(kinds, MutationKind::DataFlow)) ++dataFlow;
    if (hasMutation(kinds, MutationKind::CFG)) ++cfg;
    if (hasMutation(kinds, MutationKind::Memory)) ++memory;
    if (hasMutation(kinds, MutationKind::Calls)) ++calls;
    if (hasMutation(kinds, MutationKind::Globals)) ++globals;
    if (hasMutation(kinds, MutationKind::Interface)) ++interface;
  }
};
struct MutationSummary {
  size_t operations = 0;
  MutationKind kinds = MutationKind::None;
  std::unordered_set<Function*> affectedFunctions;
  bool changed() const { return operations != 0; }
};
enum class MutationResult { Applied, NoChange, Rejected };
}} // namespace sysy::flat
#endif
