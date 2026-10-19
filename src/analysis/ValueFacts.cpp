#include "analysis/ValueFacts.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
ValueRange unknown() { return ValueRange{}; }
ValueRange exact(int64_t value) { return ValueRange{true, value, value}; }
bool fitsI32(int64_t value) {
  return value >= std::numeric_limits<int32_t>::min() &&
         value <= std::numeric_limits<int32_t>::max();
}
ValueRange knownRange(int64_t lo, int64_t hi) {
  if (lo > hi || !fitsI32(lo) || !fitsI32(hi)) return unknown();
  return ValueRange{true, lo, hi};
}
ValueRange merge(const ValueRange& a, const ValueRange& b) {
  if (!a.known || !b.known) return unknown();
  return knownRange(std::min(a.lo, b.lo), std::max(a.hi, b.hi));
}
class Resolver {
 public:
  Resolver() = default;
  ValueRange range(const Value* value) {
    if (value == nullptr) return unknown();
    if (value->isConstant()) {
      if (value->type() == nullptr || !value->type()->isInt()) return unknown();
      return exact(static_cast<const Constant*>(value)->intVal());
    }
    if (!value->isInst()) return unknown();
    const auto* inst = static_cast<const Instruction*>(value);
    const auto cached = ranges_.find(inst);
    if (cached != ranges_.end()) return cached->second;
    if (!active_.insert(inst).second) return unknown();
    ValueRange result = compute(inst);
    active_.erase(inst);
    ranges_.emplace(inst, result);
    return result;
  }
 private:
  ValueRange compute(const Instruction* inst) {
    const Value* a = inst->numOperands() > 0 ? inst->operand(0) : nullptr;
    const Value* b = inst->numOperands() > 1 ? inst->operand(1) : nullptr;
    switch (inst->op()) {
      case Opcode::Add:
      case Opcode::Sub:
      case Opcode::Mul: {
        const ValueRange left = range(a);
        const ValueRange right = range(b);
        if (!left.known || !right.known) return unknown();
        if (inst->op() == Opcode::Add)
          return knownRange(left.lo + right.lo, left.hi + right.hi);
        if (inst->op() == Opcode::Sub)
          return knownRange(left.lo - right.hi, left.hi - right.lo);
        int64_t values[4] = {left.lo * right.lo, left.lo * right.hi,
                             left.hi * right.lo, left.hi * right.hi};
        return knownRange(*std::min_element(values, values + 4),
                          *std::max_element(values, values + 4));
      }
      case Opcode::SDiv: {
        const ValueRange left = range(a);
        const ValueRange right = range(b);
        if (!left.known || !right.known || (right.lo <= 0 && right.hi >= 0))
          return unknown();
        const int64_t values[4] = {left.lo / right.lo, left.lo / right.hi,
                                   left.hi / right.lo, left.hi / right.hi};
        return knownRange(*std::min_element(values, values + 4),
                          *std::max_element(values, values + 4));
      }
      case Opcode::SRem: {
        const ValueRange left = range(a);
        const ValueRange right = range(b);
        if (!left.known || !right.known || (right.lo <= 0 && right.hi >= 0))
          return unknown();
        if (left.lo == left.hi && right.lo == right.hi)
          return knownRange(left.lo % right.lo, left.lo % right.lo);
        const int64_t bound = std::max(std::llabs(right.lo), std::llabs(right.hi));
        return knownRange(-(bound - 1), bound - 1);
      }
      case Opcode::ICmp:
        return knownRange(0, 1);
      case Opcode::Select:
        return merge(range(inst->operand(1)), range(inst->operand(2)));
      case Opcode::Phi: {
        ValueRange merged;
        bool first = true;
        for (size_t i = 0; i < inst->numOperands(); ++i) {
          const ValueRange operand = range(inst->operand(i));
          if (first) { merged = operand; first = false; }
          else merged = merge(merged, operand);
        }
        return merged;
      }
      default:
        return unknown();
    }
  }
  std::unordered_set<const Instruction*> active_;
  std::unordered_map<const Value*, ValueRange> ranges_;
};
}  // namespace
std::optional<ValueFactsInfo> ValueFactsAnalysis::run(
    const Function& function, AnalysisManager&) {
  if (function.blockCount() == 0) return std::nullopt;
  Resolver resolver;
  ValueFactsInfo info;
  for (const Argument* argument : function.params())
    info.ranges_[argument] = unknown();
  for (BasicBlock* block : function.blocks()) {
    for (Instruction* inst : block->insts()) {
      if (inst == nullptr || inst->type() == nullptr) continue;
      info.ranges_[inst] = resolver.range(inst);
    }
  }
  return info;
}
}}  // namespace sysy::flat {
