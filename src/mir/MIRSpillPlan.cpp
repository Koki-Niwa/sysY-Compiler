#include "mir/MIR.h"
namespace sysy { namespace mir {
SpillPlan planSpills(const Allocation& allocation, uint32_t slotBytes,
                     uint32_t alignment) {
  SpillPlan plan;
  plan.slot.assign(allocation.spilled.size(), -1);
  for (size_t id = 1; id < allocation.spilled.size(); ++id) {
    if (!allocation.spilled[id]) continue;
    FrameObject object;
    object.id = static_cast<uint32_t>(plan.objects.size());
    object.size = slotBytes;
    object.align = alignment;
    object.spill = true;
    plan.slot[id] = static_cast<int>(object.id);
    plan.objects.push_back(object);
  }
  return plan;
}
}}  // namespace sysy::mir

namespace sysy { namespace mir {
}}  // namespace sysy::mir
