// S18: a small, testable peephole rewrite DSL over Flat IR.
#ifndef SYSY_OPT_REWRITEENGINE_H
#define SYSY_OPT_REWRITEENGINE_H
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "ir/Instruction.h"
#include "opt/FlatPassManager.h"
namespace sysy { namespace flat {
class Module;
class IRMutator;

enum class OperandPatternKind { Any, Constant, Capture, SameAs, ConstantInt };

struct OperandPattern {
  OperandPatternKind kind = OperandPatternKind::Any;
  std::string name;
  int64_t value = 0;
  static OperandPattern any() { return {}; }
  static OperandPattern constant() { OperandPattern p; p.kind = OperandPatternKind::Constant; return p; }
  static OperandPattern capture(std::string n) {
    OperandPattern p; p.kind = OperandPatternKind::Capture; p.name = std::move(n); return p;
  }
  static OperandPattern sameAs(std::string n) {
    OperandPattern p; p.kind = OperandPatternKind::SameAs; p.name = std::move(n); return p;
  }
  static OperandPattern constantInt(int64_t v) {
    OperandPattern p; p.kind = OperandPatternKind::ConstantInt; p.value = v; return p;
  }
};

struct MatchContext {
  const Instruction* inst = nullptr;
  std::unordered_map<std::string, const Value*> captures;
  const Value* find(const std::string& name) const {
    const auto it = captures.find(name);
    return it == captures.end() ? nullptr : it->second;
  }
};

struct RewriteContext {
  IRMutator& mutator;
  Module& module;
  Value* intConstant(const Type* type, int64_t value) const;
};

using RewriteGuard = std::function<bool(const MatchContext&)>;
using RewriteBuilder = std::function<Value*(const MatchContext&, RewriteContext&)>;

class Pattern {
 public:
  Pattern() = default;
  explicit Pattern(Opcode op) : op_(op), hasOp_(true) {}
  Pattern& operand(OperandPattern pattern);
  Pattern& intResult() { intResult_ = true; return *this; }
  bool matches(const Instruction& inst, MatchContext& context) const;
  size_t operandCount() const { return operands_.size(); }
 private:
  Opcode op_ = Opcode::Add;
  bool hasOp_ = false;
  bool intResult_ = false;
  std::vector<OperandPattern> operands_;
};

struct RewriteRule {
  std::string name;
  Pattern pattern;
  RewriteGuard onlyIf;   // empty means always allowed
  RewriteBuilder build;
};

class RewriteEngine {
 public:
  void addRule(RewriteRule rule);
  // Applies at most the first matching, guard-passing rule.
  bool rewrite(Instruction* inst, RewriteContext& context);
  size_t ruleCount() const { return rules_.size(); }
  const std::unordered_map<std::string, size_t>& counters() const { return counters_; }
 private:
  std::vector<RewriteRule> rules_;
  std::unordered_map<std::string, size_t> counters_;
};

// Production peephole rules: add/sub/mul identities, x-x, self-comparison,
// duplicate select arms, and bitwise/shift identities.
void installBasicRules(RewriteEngine& engine);

class RewritePass final : public FunctionPass {
 public:
  RewritePass();
  PassResult run(Function& function, PassContext& context) override;
};
}}  // namespace sysy::flat
#endif
