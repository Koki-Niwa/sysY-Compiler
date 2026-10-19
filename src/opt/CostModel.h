// S15: target-cost protocol consumed by inlining; no MIR/backend include.
#ifndef SYSY_OPT_COSTMODEL_H
#define SYSY_OPT_COSTMODEL_H
#include <cstddef>
namespace sysy { namespace flat {
class Instruction;

struct CostVector {
  double latency = 0.0;
  double codeSize = 0.0;
};

// Confidence is provided by the caller: 0 means no target facts were injected.
struct CostContext {
  double confidence = 0.0;
  bool hasTargetFacts = false;
};

// Immutable external service. It is deliberately not an AnalysisManager
// analysis: target facts must not be invalidated by IR mutations.
class TargetCostModel {
 public:
  virtual ~TargetCostModel() = default;
  virtual double callOverhead(const CostContext&) const = 0;
  virtual double instructionCost(const Instruction&, const CostContext&) const = 0;
  // Hard ceiling used by the V0 pass; conservative when no target facts exist.
  virtual size_t maxInlineInstructions(const CostContext& context) const = 0;
};

// No backend facts: a call costs a fixed overhead, every instruction costs 1,
// and only tiny single-block callees are inlined.
class ConservativeCostModel final : public TargetCostModel {
 public:
  double callOverhead(const CostContext&) const override { return 5.0; }
  double instructionCost(const Instruction&, const CostContext&) const override {
    return 1.0;
  }
  size_t maxInlineInstructions(const CostContext& context) const override {
    return context.hasTargetFacts ? 8u : 2u;
  }
};
}}  // namespace sysy::flat
#endif
