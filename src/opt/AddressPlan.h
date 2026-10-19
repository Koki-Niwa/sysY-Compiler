// S20: target-aware choice between indexed and pointer-induction addressing.
#ifndef SYSY_OPT_ADDRESSPLAN_H
#define SYSY_OPT_ADDRESSPLAN_H
#include "opt/CostModel.h"
namespace sysy { namespace flat {
enum class AddressStrategy { Indexed, PointerInduction };
struct AddressPlan {
  AddressStrategy strategy = AddressStrategy::Indexed;
  double indexedLatency = 0.0;
  double pointerLatency = 0.0;
  bool usedTargetFacts = false;
  const char* reason = "fallback";
};
// Without target facts the indexed form is the conservative fallback. With
// facts, the lower latency wins; a tie stays indexed.
AddressPlan chooseAddressPlan(const CostVector& indexed,
                              const CostVector& pointer,
                              const CostContext& context);
}}  // namespace sysy::flat
#endif
