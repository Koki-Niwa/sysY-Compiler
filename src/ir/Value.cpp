#include "ir/Value.h"
#include <algorithm>
#include <unordered_set>
#include "ir/Instruction.h"

namespace sysy {
namespace flat {
void Value::addUse(Instruction* user, size_t index) {
  uses_.push_back(Use{user, index});
}
void Value::removeUse(Instruction* user, size_t index) {
  const auto it = std::find_if(uses_.begin(), uses_.end(), [=](const Use& use) {
    return use.user == user && use.operandIndex == index;
  });
  if (it != uses_.end()) uses_.erase(it);
}
bool Value::replaceAllUsesWith(Value* replacement) {
  if (replacement == this) return true;
  if (replacement == nullptr || replacement->type() != type()) return false;
  struct Edge {
    Instruction* user;
    size_t index;
    bool operator==(const Edge& other) const {
      return user == other.user && index == other.index;
    }
  };
  struct Hash {
    size_t operator()(const Edge& edge) const {
      return std::hash<Instruction*>{}(edge.user) ^
             (std::hash<size_t>{}(edge.index) << 1);
    }
  };
  std::unordered_set<Edge, Hash> seen;
  for (const Use& use : uses_) {
    if (use.user == nullptr || use.operandIndex >= use.user->numOperands() ||
        use.user->operand(use.operandIndex) != this ||
        !seen.insert(Edge{use.user, use.operandIndex}).second) return false;
  }
  const std::vector<Use> edges = uses_;
  for (const Use& use : edges) use.user->setOperand(use.operandIndex, replacement);
  return true;
}
}  // namespace flat
}  // namespace sysy
