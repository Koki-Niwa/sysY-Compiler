#include "analysis/ScalarEvolution.h"
#include <unordered_set>
#include "analysis/LoopAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
std::shared_ptr<SCEV> makeConstant(int64_t value) {
  auto expr = std::make_shared<SCEV>();
  expr->kind = SCEV::Kind::Constant;
  expr->constant = value;
  return expr;
}
std::shared_ptr<SCEV> makeUnknown() { return std::make_shared<SCEV>(); }
std::shared_ptr<SCEV> makeBinary(SCEV::Kind kind, std::shared_ptr<SCEV> left,
                                 std::shared_ptr<SCEV> right) {
  auto expr = std::make_shared<SCEV>();
  expr->kind = kind;
  expr->left = std::move(left);
  expr->right = std::move(right);
  return expr;
}
bool constantOf(const SCEV* expr, int64_t& out) {
  if (expr == nullptr || expr->kind != SCEV::Kind::Constant) return false;
  out = expr->constant;
  return true;
}
class Builder {
 public:
  explicit Builder(const LoopInfoResult& loops) : loops_(loops) {}
  std::shared_ptr<SCEV> get(const Value* value) {
    if (value == nullptr) return makeUnknown();
    if (value->isConstant()) {
      if (value->type() == nullptr || !value->type()->isInt()) return makeUnknown();
      return makeConstant(static_cast<const Constant*>(value)->intVal());
    }
    const auto cached = cache_.find(value);
    if (cached != cache_.end()) return cached->second;
    if (!active_.insert(value).second) return makeUnknown();
    std::shared_ptr<SCEV> result = compute(value);
    active_.erase(value);
    cache_.emplace(value, result);
    return result;
  }
 private:
  std::shared_ptr<SCEV> compute(const Value* value) {
    if (!value->isInst()) return makeUnknown();
    const auto* inst = static_cast<const Instruction*>(value);
    if (inst->op() == Opcode::Phi) {
      // phi [init, preheader], [next, latch]; next = phi + step.
      if (inst->parent() == nullptr || inst->numOperands() < 2) return makeUnknown();
      const size_t header = inst->parent()->index();
      bool isHeader = false;
      for (const LoopEntry& loop : loops_.loops)
        if (loop.header == header) isHeader = true;
      if (!isHeader) return makeUnknown();
      const Value* init = nullptr;
      const Value* next = nullptr;
      for (size_t i = 0; i < inst->numOperands(); ++i) {
        const BasicBlock* pred = inst->succ(i);
        if (pred == nullptr) continue;
        bool inLoop = false;
        for (const LoopEntry& loop : loops_.loops) {
          if (loop.header != header) continue;
          for (size_t block : loop.blocks) if (block == pred->index()) inLoop = true;
        }
        if (inLoop) next = inst->operand(i);
        else init = inst->operand(i);
      }
      if (init == nullptr || next == nullptr || !next->isInst()) return makeUnknown();
      const auto* add = static_cast<const Instruction*>(next);
      if (add->op() != Opcode::Add || add->numOperands() != 2) return makeUnknown();
      const Value* other = nullptr;
      if (add->operand(0) == value) other = add->operand(1);
      else if (add->operand(1) == value) other = add->operand(0);
      else return makeUnknown();
      int64_t start = 0, step = 0;
      if (!constantOf(get(init).get(), start) || !constantOf(get(other).get(), step))
        return makeUnknown();
      auto expr = std::make_shared<SCEV>();
      expr->kind = SCEV::Kind::AddRec;
      expr->header = header;
      expr->left = makeConstant(start);
      expr->right = makeConstant(step);
      return expr;
    }
    if (inst->op() == Opcode::Add || inst->op() == Opcode::Mul) {
      auto left = get(inst->operand(0));
      auto right = get(inst->operand(1));
      if (left->kind == SCEV::Kind::Unknown || right->kind == SCEV::Kind::Unknown)
        return makeUnknown();
      return makeBinary(inst->op() == Opcode::Add ? SCEV::Kind::Add : SCEV::Kind::Mul,
                        std::move(left), std::move(right));
    }
    return makeUnknown();
  }
  const LoopInfoResult& loops_;
  std::unordered_map<const Value*, std::shared_ptr<SCEV>> cache_;
  std::unordered_set<const Value*> active_;
};
}  // namespace
std::string SCEV::describe() const {
  switch (kind) {
    case Kind::Constant: return std::to_string(constant);
    case Kind::AddRec:
      return "{" + (left ? left->describe() : std::string("?")) + ",+," +
             (right ? right->describe() : std::string("?")) + "}";
    case Kind::Add:
      return "(" + (left ? left->describe() : "?") + " + " +
             (right ? right->describe() : "?") + ")";
    case Kind::Mul:
      return "(" + (left ? left->describe() : "?") + " * " +
             (right ? right->describe() : "?") + ")";
    default: return "?";
  }
}
std::optional<SCEVInfo> ScalarEvolutionAnalysis::run(
    const Function& function, AnalysisManager& manager) {
  AnalysisHandle<LoopInfoResult> loops = manager.get<LoopInfoAnalysis>(function);
  if (!loops.isCurrent()) return std::nullopt;
  SCEVInfo info;
  Builder builder(*loops.get());
  for (BasicBlock* block : function.blocks())
    for (Instruction* inst : block->insts())
      if (inst != nullptr) info.expressions[inst] = builder.get(inst);
  return info;
}
}}  // namespace sysy::flat
