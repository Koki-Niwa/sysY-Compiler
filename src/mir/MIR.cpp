#include "mir/MIR.h"
#include <algorithm>
#include <vector>

namespace sysy { namespace mir {
Operand Operand::makeReg(Reg r) {
  Operand operand;
  operand.kind = OperandKind::Reg;
  operand.reg = r;
  return operand;
}

Operand Operand::makeImm(int64_t value) {
  Operand operand;
  operand.kind = OperandKind::Imm;
  operand.imm = value;
  return operand;
}

Operand Operand::makeFrame(uint32_t index) {
  Operand operand;
  operand.kind = OperandKind::Frame;
  operand.index = index;
  return operand;
}

Operand Operand::makeBlock(uint32_t index) {
  Operand operand;
  operand.kind = OperandKind::Block;
  operand.index = index;
  return operand;
}

Allocation greedyColor(const InterferenceGraph& graph, size_t maxColors,
                       const std::vector<std::pair<uint32_t, uint32_t>>& moves) {
  Allocation result;
  const size_t count = graph.neighbors.size();
  result.color.assign(count, -1);
  result.spilled.assign(count, false);
  std::vector<uint32_t> order;
  for (uint32_t id = 1; id < count; ++id) order.push_back(id);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    if (graph.neighbors[a].size() != graph.neighbors[b].size())
      return graph.neighbors[a].size() > graph.neighbors[b].size();
    return a < b;
  });
  std::vector<std::vector<uint32_t>> preferred(count);
  for (const auto& move : moves) {
    if (move.first >= count || move.second >= count) continue;
    preferred[move.first].push_back(move.second);
    preferred[move.second].push_back(move.first);
  }
  for (uint32_t id : order) {
    std::vector<bool> used(maxColors, false);
    for (uint32_t neighbor : graph.neighbors[id])
      if (neighbor < count && result.color[neighbor] >= 0 &&
          static_cast<size_t>(result.color[neighbor]) < maxColors)
        used[result.color[neighbor]] = true;
    int chosen = -1;
    for (uint32_t partner : preferred[id])
      if (partner < count && result.color[partner] >= 0 &&
          static_cast<size_t>(result.color[partner]) < maxColors &&
          !used[result.color[partner]]) {
        chosen = result.color[partner];
        break;
      }
    if (chosen < 0)
      for (size_t color = 0; color < maxColors; ++color)
        if (!used[color]) { chosen = static_cast<int>(color); break; }
    if (chosen < 0) result.spilled[id] = true;
    else {
      result.color[id] = chosen;
      result.colors = std::max(result.colors, static_cast<size_t>(chosen + 1));
    }
  }
  return result;
}

bool coalesceCopies(Function& function, const Allocation& allocation,
                    size_t* errorCount) {
  if (errorCount) *errorCount = 0;
  const auto fail = [&]() {
    if (errorCount) ++*errorCount;
    return false;
  };
  for (Block& block : function.blocks) {
    for (size_t index = 0; index < block.instructions.size();) {
      Instr& inst = block.instructions[index];
      if (inst.opcode != Opcode::Copy || !inst.def || inst.operands.size() != 1 ||
          inst.operands[0].kind != OperandKind::Reg) {
        ++index;
        continue;
      }
      const Reg source = inst.operands[0].reg;
      const Reg destination = *inst.def;
      if (source.id == 0 || destination.id == 0 || source.id == destination.id ||
          source.type != destination.type || source.cls != destination.cls)
        return fail();
      if (static_cast<size_t>(source.id) >= allocation.color.size() ||
          static_cast<size_t>(destination.id) >= allocation.color.size())
        return fail();
      const int sourceColor = allocation.color[source.id];
      const int destinationColor = allocation.color[destination.id];
      if (sourceColor < 0 || sourceColor != destinationColor ||
          (static_cast<size_t>(source.id) < allocation.spilled.size() &&
           allocation.spilled[source.id]) ||
          (static_cast<size_t>(destination.id) < allocation.spilled.size() &&
           allocation.spilled[destination.id])) {
        ++index;
        continue;
      }
      for (Block& useBlock : function.blocks) {
        for (Instr& use : useBlock.instructions) {
          for (Operand& operand : use.operands)
            if (operand.kind == OperandKind::Reg && operand.reg == destination)
              operand.reg = source;
          if (use.mem.valid && !use.mem.frameBase && use.mem.base == destination)
            use.mem.base = source;
        }
      }
      block.instructions.erase(block.instructions.begin() + index);
    }
  }
  return true;
}
}}  // namespace sysy::mir
