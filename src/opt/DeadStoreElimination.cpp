#include "opt/DeadStoreElimination.h"
#include <functional>
#include <unordered_map>
#include <vector>
#include "analysis/CallGraph.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
struct StoreKey {
  const Value* pointer = nullptr;
  const Type* storedType = nullptr;
  bool operator==(const StoreKey& other) const {
    return pointer == other.pointer && storedType == other.storedType;
  }
};

struct StoreKeyHash {
  size_t operator()(const StoreKey& key) const {
    return std::hash<const Value*>{}(key.pointer) ^
           (std::hash<const Type*>{}(key.storedType) << 1);
  }
};

bool allocaDerived(const Value* value, size_t depth = 0) {
  if (value == nullptr || depth > 16 || !value->isInst()) return false;
  const auto* inst = static_cast<const Instruction*>(value);
  if (inst->op() == Opcode::Alloca) return true;
  if (inst->op() == Opcode::GEP || inst->op() == Opcode::BitCast)
    return allocaDerived(inst->operand(0), depth + 1);
  return false;
}
bool isMemoryBarrier(const Instruction& instruction) {
  return instruction.op() == Opcode::Load ||
         classifyCallLike(instruction) != CallLikeKind::None;
}
}  // namespace

DeadStoreEliminationPass::DeadStoreEliminationPass()
    : FunctionPass("dse") {}

PassResult DeadStoreEliminationPass::run(Function& function, PassContext& context) {
  PassResult result;
  bool changed = false;
  for (BasicBlock* block : function.blocks()) {
    // A pending entry is the nearest later store for one exact typed pointer.
    // Reverse DSE removes the current (earlier) store and leaves that later
    // store pending so a still earlier store can also be removed safely.
    std::unordered_map<StoreKey, Instruction*, StoreKeyHash> pending;
    std::vector<Instruction*> instructions;
    for (Instruction* inst : block->insts())
      if (inst != nullptr) instructions.push_back(inst);
    for (size_t i = instructions.size(); i-- > 0;) {
      Instruction* inst = instructions[i];
      if (isMemoryBarrier(*inst)) {
        // Loads and every call-like operation may observe any address.  The
        // shared classifier also covers both specialized memory intrinsics.
        pending.clear();
        continue;
      }
      if (inst->op() != Opcode::Store || inst->numOperands() < 2) continue;
      // Global/argument/unknown stores are visible in the project's write-trace
      // oracle, so DSE only removes overwritten stores to non-escaping stack
      // slots. Any other pointer conservatively flushes the candidates.
      if (!allocaDerived(inst->operand(1))) {
        pending.clear();
        continue;
      }
      const StoreKey key{inst->operand(1), inst->operand(0)->type()};
      const auto it = pending.find(key);
      if (it != pending.end()) {
        if (context.mutator().eraseInstruction(inst) == MutationResult::Applied) {
          ++result.stats.counters["dse.removed"];
          changed = true;
        }
      }
      // Keep the later store as the candidate for earlier stores.  If the
      // current store could not be erased, the later store is still the only
      // valid candidate for this key; replacing it would reintroduce the old
      // wrong-direction deletion.
      if (it == pending.end()) pending.emplace(key, inst);
    }
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
