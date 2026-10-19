#include "opt/RewriteEngine.h"
#include <utility>
#include "ir/IRMutator.h"
#include "ir/Module.h"
namespace sysy { namespace flat {
Pattern& Pattern::operand(OperandPattern pattern) {
  operands_.push_back(std::move(pattern));
  return *this;
}

bool Pattern::matches(const Instruction& inst, MatchContext& context) const {
  if (hasOp_ && inst.op() != op_) return false;
  if (inst.numOperands() != operands_.size()) return false;
  if (intResult_ && (inst.type() == nullptr || !inst.type()->isInt())) return false;
  for (size_t i = 0; i < operands_.size(); ++i) {
    const OperandPattern& pattern = operands_[i];
    const Value* value = inst.operand(i);
    switch (pattern.kind) {
      case OperandPatternKind::Any:
        break;
      case OperandPatternKind::Constant:
        if (value == nullptr || !value->isConstant()) return false;
        break;
      case OperandPatternKind::ConstantInt: {
        if (value == nullptr || !value->isConstant()) return false;
        const auto* constant = static_cast<const Constant*>(value);
        if (constant->type() == nullptr || !constant->type()->isInt() ||
            constant->intVal() != pattern.value)
          return false;
        break;
      }
      case OperandPatternKind::Capture: {
        const auto it = context.captures.find(pattern.name);
        if (it == context.captures.end()) {
          context.captures.emplace(pattern.name, value);
        } else if (it->second != value) {
          return false;
        }
        break;
      }
      case OperandPatternKind::SameAs: {
        const auto it = context.captures.find(pattern.name);
        if (it == context.captures.end() || it->second != value) return false;
        break;
      }
    }
  }
  return true;
}

Value* RewriteContext::intConstant(const Type* type, int64_t value) const {
  return module.getIntConst(type, value);
}

void RewriteEngine::addRule(RewriteRule rule) {
  rules_.push_back(std::move(rule));
}

bool RewriteEngine::rewrite(Instruction* inst, RewriteContext& context) {
  if (inst == nullptr) return false;
  for (const RewriteRule& rule : rules_) {
    MatchContext match;
    match.inst = inst;
    if (!rule.pattern.matches(*inst, match)) continue;
    if (rule.onlyIf && !rule.onlyIf(match)) {
      ++counters_[rule.name + ".guarded"];
      continue;
    }
    Value* replacement = rule.build ? rule.build(match, context) : nullptr;
    if (replacement == nullptr) {
      ++counters_[rule.name + ".declined"];
      continue;
    }
    if (context.mutator.replaceAndErase(inst, replacement) != MutationResult::Applied) {
      ++counters_[rule.name + ".rejected"];
      continue;
    }
    ++counters_[rule.name];
    return true;
  }
  return false;
}

namespace {
void addIdentity(RewriteEngine& engine, const char* name, Opcode op,
                 const OperandPattern& left, const OperandPattern& right) {
  RewriteRule rule;
  rule.name = name;
  rule.pattern = Pattern(op).operand(left).operand(right).intResult();
  rule.build = [](const MatchContext& match, RewriteContext&) -> Value* {
    return const_cast<Value*>(match.find("x"));
  };
  engine.addRule(std::move(rule));
}
}  // namespace

void installBasicRules(RewriteEngine& engine) {
  const auto x = OperandPattern::capture("x");
  const auto y = OperandPattern::capture("y");
  // add x,0 / add 0,x
  addIdentity(engine, "add-x-0", Opcode::Add, x, OperandPattern::constantInt(0));
  addIdentity(engine, "add-0-x", Opcode::Add, OperandPattern::constantInt(0), x);
  // sub x,0
  addIdentity(engine, "sub-x-0", Opcode::Sub, x, OperandPattern::constantInt(0));
  // mul x,1 / mul 1,x
  addIdentity(engine, "mul-x-1", Opcode::Mul, x, OperandPattern::constantInt(1));
  addIdentity(engine, "mul-1-x", Opcode::Mul, OperandPattern::constantInt(1), x);
  // and x,-1 / or x,0 / xor x,0 / shifts by 0
  addIdentity(engine, "and-x-allones", Opcode::And, x, OperandPattern::constantInt(-1));
  addIdentity(engine, "or-x-0", Opcode::Or, x, OperandPattern::constantInt(0));
  addIdentity(engine, "xor-x-0", Opcode::Xor, x, OperandPattern::constantInt(0));
  addIdentity(engine, "shl-x-0", Opcode::Shl, x, OperandPattern::constantInt(0));
  addIdentity(engine, "lshr-x-0", Opcode::LShr, x, OperandPattern::constantInt(0));
  addIdentity(engine, "ashr-x-0", Opcode::AShr, x, OperandPattern::constantInt(0));
  // mul x,0 -> 0 (integer only)
  {
    RewriteRule rule;
    rule.name = "mul-x-0";
    rule.pattern = Pattern(Opcode::Mul)
                       .operand(x)
                       .operand(OperandPattern::constantInt(0))
                       .intResult();
    rule.build = [](const MatchContext& match, RewriteContext& ctx) -> Value* {
      return ctx.intConstant(match.inst->type(), 0);
    };
    engine.addRule(std::move(rule));
  }
  // sub x,x -> 0
  {
    RewriteRule rule;
    rule.name = "sub-x-x";
    rule.pattern = Pattern(Opcode::Sub)
                       .operand(x)
                       .operand(OperandPattern::sameAs("x"))
                       .intResult();
    rule.build = [](const MatchContext& match, RewriteContext& ctx) -> Value* {
      return ctx.intConstant(match.inst->type(), 0);
    };
    engine.addRule(std::move(rule));
  }
  // select c,x,x -> x
  {
    RewriteRule rule;
    rule.name = "select-same-arms";
    rule.pattern = Pattern(Opcode::Select)
                       .operand(OperandPattern::any())
                       .operand(x)
                       .operand(OperandPattern::sameAs("x"));
    rule.build = [](const MatchContext& match, RewriteContext&) -> Value* {
      return const_cast<Value*>(match.find("x"));
    };
    engine.addRule(std::move(rule));
  }
  // icmp pred x,x -> 0/1; the guard checks the predicate.
  const struct { IPred pred; const char* name; int64_t result; } comparisons[] = {
      {IPred::Eq, "icmp-eq-self", 1},   {IPred::Ne, "icmp-ne-self", 0},
      {IPred::Sle, "icmp-sle-self", 1}, {IPred::Sge, "icmp-sge-self", 1},
      {IPred::Slt, "icmp-slt-self", 0}, {IPred::Sgt, "icmp-sgt-self", 0},
  };
  for (const auto& entry : comparisons) {
    RewriteRule rule;
    rule.name = entry.name;
    rule.pattern = Pattern(Opcode::ICmp)
                       .operand(x)
                       .operand(OperandPattern::sameAs("x"));
    rule.onlyIf = [pred = entry.pred](const MatchContext& match) {
      return match.inst->ipred() == pred;
    };
    rule.build = [value = entry.result](const MatchContext& match,
                                        RewriteContext& ctx) -> Value* {
      return ctx.intConstant(match.inst->type(), value);
    };
    engine.addRule(std::move(rule));
  }
}

RewritePass::RewritePass() : FunctionPass("rewrite-v0") {}

PassResult RewritePass::run(Function& function, PassContext& context) {
  RewriteEngine engine;
  installBasicRules(engine);
  RewriteContext rewrite{context.mutator(), context.analyses().module()};
  PassResult result;
  bool changed = false;
  for (size_t round = 0; round < 128; ++round) {
    bool roundChanged = false;
    std::vector<Instruction*> instructions;
    for (BasicBlock* block : function.blocks())
      for (Instruction* inst : block->insts())
        if (inst != nullptr) instructions.push_back(inst);
    for (Instruction* inst : instructions) {
      if (engine.rewrite(inst, rewrite)) {
        roundChanged = changed = true;
        ++result.stats.counters["rewritten"];
      }
    }
    if (!roundChanged) break;
  }
  for (const auto& entry : engine.counters())
    result.stats.counters[entry.first] += entry.second;
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
