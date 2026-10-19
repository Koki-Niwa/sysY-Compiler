#include "opt/AddressPlan.h"
namespace sysy { namespace flat {
AddressPlan chooseAddressPlan(const CostVector& indexed,
                              const CostVector& pointer,
                              const CostContext& context) {
  AddressPlan plan;
  plan.indexedLatency = indexed.latency;
  plan.pointerLatency = pointer.latency;
  plan.usedTargetFacts = context.hasTargetFacts && context.confidence > 0.0;
  if (!plan.usedTargetFacts) {
    plan.strategy = AddressStrategy::Indexed;
    plan.reason = "no target facts: indexed fallback";
    return plan;
  }
  plan.strategy = pointer.latency < indexed.latency ? AddressStrategy::PointerInduction
                                                    : AddressStrategy::Indexed;
  plan.reason = plan.strategy == AddressStrategy::PointerInduction
                    ? "pointer induction is cheaper"
                    : "indexed addressing is cheaper or tied";
  return plan;
}
}}  // namespace sysy::flat
