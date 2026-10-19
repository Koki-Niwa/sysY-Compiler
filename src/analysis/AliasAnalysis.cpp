#include "analysis/AliasAnalysis.h"
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>
#include "analysis/CallGraph.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
struct RetFact {
  enum class Kind : uint8_t { Bottom, Unknown, Global, Param, Fresh };
  Kind kind = Kind::Bottom;
  const void* object = nullptr;
  size_t index = 0;
  bool exact = true;   // false once a non-zero/dynamic GEP moves the pointer
};
bool sameFact(const RetFact& a, const RetFact& b) {
  return a.kind == b.kind && a.object == b.object && a.index == b.index &&
         a.exact == b.exact;
}
RetFact unknownFact() {
  RetFact fact;
  fact.kind = RetFact::Kind::Unknown;
  return fact;
}
RetFact mergeFacts(const RetFact& a, const RetFact& b) {
  if (a.kind == RetFact::Kind::Bottom) return b;
  if (b.kind == RetFact::Kind::Bottom) return a;
  if (a.kind == RetFact::Kind::Unknown || b.kind == RetFact::Kind::Unknown)
    return unknownFact();
  if (a.kind == b.kind && a.object == b.object && a.index == b.index) {
    RetFact merged = a;
    merged.exact = a.exact && b.exact;
    return merged;
  }
  return unknownFact();   // differing concrete facts merge to Unknown
}
bool sameBase(const AliasBase& a, const AliasBase& b) {
  return a.kind == b.kind && a.object == b.object && a.index == b.index;
}

class Builder {
 public:
  Builder(const Module& module, AnalysisManager& manager)
      : module_(module), manager_(manager) {}
  std::optional<AliasInfo> build();

 private:
  RetFact retFact(const Value* value, const Function* function,
                  std::unordered_set<const Value*>& stack) const;
  RetFact callFact(const Instruction* call, const Function* function,
                   std::unordered_set<const Value*>& stack) const;
  AliasBase resolve(const Value* value);
  AliasBase resolvePhi(const Instruction* phi, std::unordered_set<const Value*>& stack);
  AliasBase mergeBases(const AliasBase& a, const AliasBase& b) const;

  const Module& module_;
  AnalysisManager& manager_;
  std::vector<const Function*> functions_;
  std::unordered_map<const Function*, RetFact> rets_;
  std::unordered_map<const Value*, AliasBase> bases_;
  std::unordered_set<const Value*> resolving_;
};

RetFact Builder::retFact(const Value* value, const Function* function,
                         std::unordered_set<const Value*>& stack) const {
  if (value == nullptr) return unknownFact();
  if (value->isConstant()) return unknownFact();
  if (!value->isInst()) {
    if (value->isArgument()) {
      const auto* argument = static_cast<const Argument*>(value);
      RetFact fact;
      fact.kind = RetFact::Kind::Param;
      fact.object = argument->owner();
      fact.index = argument->index();
      return fact;
    }
    if (value->isGlobalAddr()) {
      RetFact fact;
      fact.kind = RetFact::Kind::Global;
      fact.object = static_cast<const GlobalAddr*>(value)->global();
      return fact;
    }
    return unknownFact();
  }
  const auto* inst = static_cast<const Instruction*>(value);
  if (!stack.insert(inst).second) return RetFact{};   // phi cycle: Bottom
  RetFact result = unknownFact();
  switch (inst->op()) {
    case Opcode::Alloca:
      // Returning a local address is invalid/undefined; stay conservative.
      return unknownFact();
    case Opcode::BitCast:
      if (inst->numOperands() > 0)
        result = retFact(inst->operand(0), function, stack);
      break;
    case Opcode::GEP:
      if (inst->numOperands() > 1) {
        result = retFact(inst->operand(0), function, stack);
        const Value* index = inst->operand(1);
        const bool zero = index != nullptr && index->isConstant() &&
                          static_cast<const Constant*>(index)->intVal() == 0;
        if (!zero) result.exact = false;
      }
      break;
    case Opcode::Phi: {
      RetFact merged;
      for (size_t i = 0; i < inst->numOperands(); ++i)
        merged = mergeFacts(merged, retFact(inst->operand(i), function, stack));
      result = merged;
      break;
    }
    case Opcode::Call:
      result = callFact(inst, function, stack);
      break;
    default:
      break;
  }
  stack.erase(inst);
  return result;
}

RetFact Builder::callFact(const Instruction* call, const Function* function,
                          std::unordered_set<const Value*>& stack) const {
  const auto callee = module_.findFunction(call->callee());
  if (callee == nullptr || callee->isDeclaration()) return unknownFact();
  const auto it = rets_.find(callee);
  if (it == rets_.end()) return unknownFact();
  const RetFact& fact = it->second;
  if (fact.kind == RetFact::Kind::Global || fact.kind == RetFact::Kind::Unknown ||
      fact.kind == RetFact::Kind::Bottom)
    return fact;
  if (fact.kind == RetFact::Kind::Fresh) {
    RetFact fresh;
    fresh.kind = RetFact::Kind::Fresh;
    fresh.object = call;
    return fresh;
  }
  if (fact.kind == RetFact::Kind::Param) {
    if (fact.index >= call->numOperands()) return unknownFact();
    return retFact(call->operand(fact.index), function, stack);
  }
  return unknownFact();
}

AliasBase Builder::mergeBases(const AliasBase& a, const AliasBase& b) const {
  if (a.kind == AliasBase::Kind::Unknown || b.kind == AliasBase::Kind::Unknown)
    return AliasBase{};
  if (!sameBase(a, b)) return AliasBase{};
  AliasBase merged = a;
  if (!a.offsetKnown || !b.offsetKnown || a.offset != b.offset)
    merged.offsetKnown = false;
  return merged;
}

AliasBase Builder::resolvePhi(const Instruction* phi,
                              std::unordered_set<const Value*>& /*stack*/) {
  // resolve() already owns the cycle guard for this phi; adding it again here
  // would make the first (legal) visit look like a cycle.
  AliasBase result;
  bool first = true;
  for (size_t i = 0; i < phi->numOperands(); ++i) {
    AliasBase operand = resolve(phi->operand(i));
    result = first ? operand : mergeBases(result, operand);
    first = false;
  }
  return result;
}

AliasBase Builder::resolve(const Value* value) {
  if (value == nullptr) return AliasBase{};
  if (value->isGlobalAddr()) {
    AliasBase base;
    base.kind = AliasBase::Kind::Global;
    base.object = static_cast<const GlobalAddr*>(value)->global();
    return base;
  }
  if (value->isArgument()) {
    const auto* argument = static_cast<const Argument*>(value);
    AliasBase base;
    base.kind = AliasBase::Kind::Param;
    base.object = argument->owner();
    base.index = argument->index();
    return base;
  }
  if (!value->isInst()) return AliasBase{};
  const auto* inst = static_cast<const Instruction*>(value);
  const auto cached = bases_.find(inst);
  if (cached != bases_.end()) return cached->second;
  if (!resolving_.insert(inst).second) return AliasBase{};
  AliasBase result;
  switch (inst->op()) {
    case Opcode::Alloca:
      result.kind = AliasBase::Kind::Alloca;
      result.object = inst;
      break;
    case Opcode::GEP: {
      if (inst->numOperands() < 2) break;
      result = resolve(inst->operand(0));
      if (result.kind == AliasBase::Kind::Unknown) break;
      const Value* index = inst->operand(1);
      const int64_t elementSize = typeByteSize(inst->srcElemType());
      if (index != nullptr && index->isConstant() && elementSize > 0) {
        const int64_t indexValue = static_cast<const Constant*>(index)->intVal();
        if (result.offsetKnown) result.offset += indexValue * elementSize;
      } else {
        result.offsetKnown = false;
      }
      break;
    }
    case Opcode::BitCast:
      if (inst->numOperands() > 0) result = resolve(inst->operand(0));
      break;
    case Opcode::Phi:
      result = resolvePhi(inst, resolving_);
      break;
    case Opcode::Call: {
      const auto callee = module_.findFunction(inst->callee());
      const auto it = callee != nullptr ? rets_.find(callee) : rets_.end();
      if (it == rets_.end() || it->second.kind == RetFact::Kind::Unknown ||
          it->second.kind == RetFact::Kind::Bottom)
        break;
      if (it->second.kind == RetFact::Kind::Global) {
        result.kind = AliasBase::Kind::Global;
        result.object = it->second.object;
        // The callee may return an interior pointer (&A[i]); only an exact
        // return summary keeps offset zero.
        if (!it->second.exact) result.offsetKnown = false;
      } else if (it->second.kind == RetFact::Kind::Fresh) {
        result.kind = AliasBase::Kind::Fresh;
        result.object = inst;
      } else if (it->second.kind == RetFact::Kind::Param &&
                 it->second.index < inst->numOperands()) {
        result = resolve(inst->operand(it->second.index));
        if (!it->second.exact) result.offsetKnown = false;
      }
      break;
    }
    default:
      break;
  }
  resolving_.erase(inst);
  bases_.emplace(inst, result);
  return result;
}

std::optional<AliasInfo> Builder::build() {
  AnalysisHandle<CallGraphInfo> graph = manager_.get<CallGraphAnalysis>();
  if (!graph.isCurrent()) return std::nullopt;
  for (const CallGraphNode& node : graph.get()->nodes)
    functions_.push_back(node.function);
  for (const Function* function : functions_) rets_[function] = RetFact{};

  // Return provenance is a small fixpoint over functions: Bottom is "no fact
  // yet", and merging concrete facts with unknown yields Unknown. Recursion
  // therefore converges to the concrete base case or stays conservative.
  for (size_t round = 0; round <= functions_.size(); ++round) {
    bool changed = false;
    for (const Function* function : functions_) {
      RetFact merged;
      for (BasicBlock* block : function->blocks()) {
        for (Instruction* inst : block->insts()) {
          if (inst->op() != Opcode::Ret || inst->numOperands() == 0) continue;
          std::unordered_set<const Value*> stack;
          merged = mergeFacts(merged, retFact(inst->operand(0), function, stack));
        }
      }
      if (!sameFact(merged, rets_[function])) {
        rets_[function] = merged;
        changed = true;
      }
    }
    if (!changed) break;
  }

  for (const Function* function : functions_) {
    for (const Argument* argument : function->params())
      bases_[argument] = resolve(argument);
    for (BasicBlock* block : function->blocks()) {
      for (Instruction* inst : block->insts()) {
        if (inst->type() != nullptr && inst->type()->isPtr())
          bases_[inst] = resolve(inst);
      }
    }
  }
  for (const GlobalAddr* address : module_.globalAddresses())
    bases_[address] = resolve(address);
  return AliasInfo(std::move(bases_));
}
}  // namespace

const AliasBase* AliasInfo::baseOf(const Value* value) const {
  const auto it = bases_.find(value);
  return it == bases_.end() ? nullptr : &it->second;
}

AliasKind AliasInfo::query(const Value* a, const Value* b) const {
  if (a == nullptr || b == nullptr) return AliasKind::MayAlias;
  if (a == b) return AliasKind::MustAlias;
  const AliasBase* left = baseOf(a);
  const AliasBase* right = baseOf(b);
  if (left == nullptr || right == nullptr ||
      left->kind == AliasBase::Kind::Unknown ||
      right->kind == AliasBase::Kind::Unknown)
    return AliasKind::MayAlias;
  if (sameBase(*left, *right)) {
    if (left->offsetKnown && right->offsetKnown)
      return left->offset == right->offset ? AliasKind::MustAlias
                                           : AliasKind::NoAlias;
    return AliasKind::MayAlias;
  }
  // Distinct objects: only disjoint object classes prove NoAlias.
  if (left->kind == AliasBase::Kind::Alloca ||
      right->kind == AliasBase::Kind::Alloca)
    return AliasKind::NoAlias;   // a local alloca cannot be a caller object
  if (left->kind == AliasBase::Kind::Global &&
      right->kind == AliasBase::Kind::Global)
    return AliasKind::NoAlias;
  if (left->kind == AliasBase::Kind::Fresh ||
      right->kind == AliasBase::Kind::Fresh) {
    if (left->kind == AliasBase::Kind::Fresh &&
        right->kind == AliasBase::Kind::Fresh)
      return AliasKind::NoAlias;
    if (left->kind == AliasBase::Kind::Param ||
        right->kind == AliasBase::Kind::Param)
      return AliasKind::MayAlias;
    return AliasKind::NoAlias;
  }
  return AliasKind::MayAlias;
}

std::optional<AliasInfo> AliasAnalysis::run(const Module& module,
                                            AnalysisManager& manager) {
  Builder builder(module, manager);
  return builder.build();
}
}}  // namespace sysy::flat
