#include "mir/MIR.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace sysy { namespace mir {
namespace {
std::vector<size_t> successors(const Function& function, size_t blockIndex) {
  std::vector<size_t> result;
  if (blockIndex >= function.blocks.size() || function.blocks[blockIndex].instructions.empty())
    return result;
  const Instr& terminator = function.blocks[blockIndex].instructions.back();
  if (terminator.opcode == Opcode::Br && terminator.operands.size() == 1 &&
      terminator.operands[0].kind == OperandKind::Block)
    result.push_back(terminator.operands[0].index);
  else if (terminator.opcode == Opcode::CondBr && terminator.operands.size() == 3 &&
           terminator.operands[1].kind == OperandKind::Block &&
           terminator.operands[2].kind == OperandKind::Block) {
    result.push_back(terminator.operands[1].index);
    result.push_back(terminator.operands[2].index);
  }
  return result;
}

void addInstructionUses(const Instr& inst, std::set<uint32_t>& live) {
  for (const Operand& operand : inst.operands)
    if (operand.kind == OperandKind::Reg && operand.reg.id != 0)
      live.insert(operand.reg.id);
  if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base.id != 0)
    live.insert(inst.mem.base.id);
}

void linkSet(const std::set<uint32_t>& values,
            std::vector<std::set<uint32_t>>& neighbors) {
  for (auto first = values.begin(); first != values.end(); ++first)
    for (auto second = std::next(first); second != values.end(); ++second) {
      if (*first == 0 || *second == 0 || *first >= neighbors.size() ||
          *second >= neighbors.size()) continue;
      neighbors[*first].insert(*second);
      neighbors[*second].insert(*first);
    }
}
}  // namespace

Liveness analyzeLiveness(const Function& function) {
  Liveness result;
  const size_t count = function.blocks.size();
  result.liveIn.resize(count);
  result.liveOut.resize(count);
  std::vector<std::set<uint32_t>> use(count), defs(count);
  std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> phiUses;
  for (size_t block = 0; block < count; ++block) {
    for (const Instr& inst : function.blocks[block].instructions) {
      if (inst.opcode == Opcode::Phi) {
        if (inst.def && inst.def->id != 0) defs[block].insert(inst.def->id);
        for (size_t pair = 0; pair + 1 < inst.operands.size(); pair += 2) {
          const Operand& value = inst.operands[pair];
          const Operand& edge = inst.operands[pair + 1];
          if (value.kind == OperandKind::Reg && edge.kind == OperandKind::Block &&
              value.reg.id != 0)
            phiUses[{static_cast<uint32_t>(block), edge.index}].insert(value.reg.id);
        }
        continue;
      }
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg && operand.reg.id != 0 &&
            !defs[block].count(operand.reg.id))
          use[block].insert(operand.reg.id);
      if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base.id != 0 &&
          !defs[block].count(inst.mem.base.id))
        use[block].insert(inst.mem.base.id);
      if (inst.def && inst.def->id != 0) defs[block].insert(inst.def->id);
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t block = count; block-- > 0;) {
      std::set<uint32_t> out;
      for (size_t target : successors(function, block)) {
        if (target >= count) continue;
        std::set<uint32_t> edge = result.liveIn[target];
        const auto found = phiUses.find({static_cast<uint32_t>(target),
                                         static_cast<uint32_t>(block)});
        if (found != phiUses.end())
          edge.insert(found->second.begin(), found->second.end());
        auto& edgeSlot = result.edgeLive[{static_cast<uint32_t>(block),
                                          static_cast<uint32_t>(target)}];
        if (edgeSlot != edge) {
          edgeSlot = edge;
          changed = true;
        }
        out.insert(edge.begin(), edge.end());
      }
      std::set<uint32_t> in = use[block];
      for (uint32_t id : out)
        if (!defs[block].count(id)) in.insert(id);
      if (out != result.liveOut[block] || in != result.liveIn[block]) {
        result.liveOut[block] = std::move(out);
        result.liveIn[block] = std::move(in);
        changed = true;
      }
    }
  }
  return result;
}

InterferenceGraph buildInterference(const Function& function,
                                    const Liveness& liveness) {
  InterferenceGraph graph;
  uint32_t maxId = 0;
  for (const Block& block : function.blocks)
    for (const Instr& inst : block.instructions) {
      if (inst.def) maxId = std::max(maxId, inst.def->id);
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg)
          maxId = std::max(maxId, operand.reg.id);
      if (inst.mem.valid && !inst.mem.frameBase)
        maxId = std::max(maxId, inst.mem.base.id);
    }
  graph.neighbors.resize(static_cast<size_t>(maxId) + 1);
  for (const std::set<uint32_t>& values : liveness.liveIn) linkSet(values, graph.neighbors);
  for (const std::set<uint32_t>& values : liveness.liveOut) linkSet(values, graph.neighbors);
  for (const auto& edge : liveness.edgeLive) linkSet(edge.second, graph.neighbors);

  for (size_t block = 0; block < function.blocks.size(); ++block) {
    std::set<uint32_t> live = block < liveness.liveOut.size()
        ? liveness.liveOut[block] : std::set<uint32_t>{};
    const std::vector<Instr>& instructions = function.blocks[block].instructions;
    for (size_t index = instructions.size(); index-- > 0;) {
      const Instr& inst = instructions[index];
      if (inst.opcode == Opcode::Phi) continue;
      if (inst.def && inst.def->id != 0) {
        for (uint32_t liveId : live)
          if (inst.def->id < graph.neighbors.size() && liveId < graph.neighbors.size() &&
              inst.def->id != liveId) {
            graph.neighbors[inst.def->id].insert(liveId);
            graph.neighbors[liveId].insert(inst.def->id);
          }
        live.erase(inst.def->id);
      }
      addInstructionUses(inst, live);
    }
    std::vector<uint32_t> phiDefs;
    for (const Instr& inst : instructions) {
      if (inst.opcode != Opcode::Phi) break;
      if (inst.def && inst.def->id != 0) phiDefs.push_back(inst.def->id);
    }
    for (uint32_t def : phiDefs)
      for (uint32_t liveId : live)
        if (def < graph.neighbors.size() && liveId < graph.neighbors.size() && def != liveId) {
          graph.neighbors[def].insert(liveId);
          graph.neighbors[liveId].insert(def);
        }
    linkSet(std::set<uint32_t>(phiDefs.begin(), phiDefs.end()), graph.neighbors);
  }
  return graph;
}
}}  // namespace sysy::mir
