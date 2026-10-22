#include "mir/MIRInternal.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace sysy { namespace mir {
namespace {
struct EdgeNeed {
  uint32_t target = 0;
  size_t phiIndex = 0;
  size_t operandIndex = 0;
  uint32_t predecessor = 0;
  Reg source;
  Reg replacement;
};

struct EdgeGroup {
  uint32_t predecessor = 0;
  uint32_t target = 0;
  std::vector<size_t> needs;
  bool split = false;
  uint32_t edgeBlock = 0;
};

bool powerOfTwo(uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }

bool hasSpill(const SpillPlan& plan, uint32_t id) {
  return id != 0 && static_cast<size_t>(id) < plan.slot.size() && plan.slot[id] >= 0;
}

std::vector<uint32_t> successors(const Function& function, uint32_t block) {
  std::vector<uint32_t> result;
  const Instr& terminator = function.blocks[block].instructions.back();
  if (terminator.opcode == Opcode::Br)
    result.push_back(terminator.operands[0].index);
  else if (terminator.opcode == Opcode::CondBr) {
    result.push_back(terminator.operands[1].index);
    result.push_back(terminator.operands[2].index);
  }
  return result;
}

Instr frameLoad(uint32_t id, Reg result) {
  Instr load;
  load.opcode = Opcode::Load;
  load.def = result;
  load.mem.valid = true;
  load.mem.frameBase = true;
  load.mem.frame = id;
  load.mem.size = detail::typeSize(result.type);
  return load;
}

Instr frameStore(uint32_t id, Reg value) {
  Instr store;
  store.opcode = Opcode::Store;
  store.operands.push_back(Operand::makeReg(value));
  store.mem.valid = true;
  store.mem.frameBase = true;
  store.mem.frame = id;
  store.mem.size = detail::typeSize(value.type);
  return store;
}
}  // namespace

bool rewriteSpills(Function& function, const SpillPlan& plan,
                   size_t* errorCount) {
  if (errorCount) *errorCount = 0;
  const auto fail = [&]() {
    if (errorCount) ++*errorCount;
    return false;
  };
  if (!detail::verifyFunction(function).empty()) return fail();
  // Legacy frameSlots have no object sizes, and frameObjects may belong to
  // locals/callee saves. This prototype declines both namespaces atomically.
  if (function.frameSlots != 0 || !function.frameObjects.empty()) return fail();
  if (plan.objects.size() > UINT32_MAX) return fail();

  uint32_t maxReg = 0;
  for (const Block& block : function.blocks)
    for (const Instr& inst : block.instructions) {
      if (inst.def) maxReg = std::max(maxReg, inst.def->id);
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg) maxReg = std::max(maxReg, operand.reg.id);
      if (inst.mem.valid && !inst.mem.frameBase)
        maxReg = std::max(maxReg, inst.mem.base.id);
    }
  if (plan.slot.size() > static_cast<size_t>(maxReg) + 1) return fail();
  std::vector<bool> objectUsed(plan.objects.size(), false);
  for (size_t id = 0; id < plan.slot.size(); ++id) {
    const int slot = plan.slot[id];
    if (slot < -1 || (slot >= 0 && static_cast<size_t>(slot) >= plan.objects.size()))
      return fail();
    if (slot >= 0) {
      if (id == 0 || objectUsed[slot]) return fail();
      objectUsed[slot] = true;
    }
  }
  for (size_t slot = 0; slot < plan.objects.size(); ++slot) {
    const FrameObject& object = plan.objects[slot];
    if (object.id != slot || object.size == 0 || !powerOfTwo(object.align) ||
        !object.spill || !objectUsed[slot]) return fail();
  }

  std::map<std::pair<uint32_t, uint32_t>, EdgeGroup> groups;
  std::vector<EdgeNeed> needs;
  for (uint32_t target = 0; target < function.blocks.size(); ++target) {
    const Block& block = function.blocks[target];
    for (size_t phiIndex = 0; phiIndex < block.instructions.size(); ++phiIndex) {
      const Instr& phi = block.instructions[phiIndex];
      if (phi.opcode != Opcode::Phi) break;
      for (size_t pair = 0; pair + 1 < phi.operands.size(); pair += 2) {
        const Reg source = phi.operands[pair].reg;
        if (!hasSpill(plan, source.id)) continue;
        const uint32_t predecessor = phi.operands[pair + 1].index;
        EdgeNeed need;
        need.target = target;
        need.phiIndex = phiIndex;
        need.operandIndex = pair;
        need.predecessor = predecessor;
        need.source = source;
        const size_t needIndex = needs.size();
        needs.push_back(need);
        auto key = std::make_pair(predecessor, target);
        EdgeGroup& group = groups[key];
        group.predecessor = predecessor;
        group.target = target;
        group.needs.push_back(needIndex);
      }
    }
  }
  const size_t generatedLoads = needs.size();
  if (generatedLoads > static_cast<size_t>(UINT32_MAX)) return fail();
  for (auto& item : groups) {
    EdgeGroup& group = item.second;
    const std::vector<uint32_t> outgoing = successors(function, group.predecessor);
    size_t occurrences = 0;
    for (uint32_t target : outgoing) if (target == group.target) ++occurrences;
    if (occurrences != 1) return fail();
    group.split = outgoing.size() > 1;
  }

  uint64_t nextReg = maxReg == UINT32_MAX ? uint64_t(UINT32_MAX) + 1
                                          : static_cast<uint64_t>(maxReg) + 1;
  if (generatedLoads > 0 && (nextReg == 0 ||
                             nextReg + generatedLoads - 1 > UINT32_MAX)) return fail();
  // Generic non-phi uses add one load per occurrence. Count them before any
  // mutation so a fresh-id overflow cannot leave a partly rewritten function.
  size_t genericLoads = 0;
  for (const Block& block : function.blocks)
    for (const Instr& inst : block.instructions) {
      if (inst.opcode != Opcode::Phi) {
        for (const Operand& operand : inst.operands)
          if (operand.kind == OperandKind::Reg && hasSpill(plan, operand.reg.id)) ++genericLoads;
        if (inst.mem.valid && !inst.mem.frameBase && hasSpill(plan, inst.mem.base.id))
          ++genericLoads;
      }
    }
  if (nextReg + generatedLoads + genericLoads > uint64_t(UINT32_MAX) + 1) return fail();

  Function candidate = function;
  for (auto& item : groups) {
    EdgeGroup& group = item.second;
    if (!group.split) continue;
    if (candidate.blocks.size() > UINT32_MAX) return fail();
    group.edgeBlock = static_cast<uint32_t>(candidate.blocks.size());
    Block edge;
    edge.id = group.edgeBlock;
    for (size_t needIndex : group.needs) {
      EdgeNeed& need = needs[needIndex];
      const uint32_t slot = static_cast<uint32_t>(plan.slot[need.source.id]);
      need.replacement = Reg{static_cast<uint32_t>(nextReg++), need.source.type,
                             need.source.cls};
      edge.instructions.push_back(frameLoad(slot, need.replacement));
    }
    Instr branch;
    branch.opcode = Opcode::Br;
    branch.operands.push_back(Operand::makeBlock(group.target));
    edge.instructions.push_back(std::move(branch));
    candidate.blocks.push_back(std::move(edge));
    Instr& terminator = candidate.blocks[group.predecessor].instructions.back();
    if (terminator.opcode == Opcode::Br) {
      terminator.operands[0].index = group.edgeBlock;
    } else {
      for (size_t operand = 1; operand < terminator.operands.size(); ++operand)
        if (terminator.operands[operand].kind == OperandKind::Block &&
            terminator.operands[operand].index == group.target)
          terminator.operands[operand].index = group.edgeBlock;
    }
    for (Instr& phi : candidate.blocks[group.target].instructions) {
      if (phi.opcode != Opcode::Phi) break;
      for (size_t pair = 1; pair < phi.operands.size(); pair += 2)
        if (phi.operands[pair].kind == OperandKind::Block &&
            phi.operands[pair].index == group.predecessor)
          phi.operands[pair].index = group.edgeBlock;
    }
  }

  std::map<uint32_t, std::vector<Instr>> directLoads;
  for (auto& item : groups) {
    EdgeGroup& group = item.second;
    if (group.split) continue;
    for (size_t needIndex : group.needs) {
      EdgeNeed& need = needs[needIndex];
      need.replacement = Reg{static_cast<uint32_t>(nextReg++), need.source.type,
                             need.source.cls};
      directLoads[group.predecessor].push_back(
          frameLoad(static_cast<uint32_t>(plan.slot[need.source.id]), need.replacement));
    }
  }
  for (const auto& item : directLoads) {
    Block& block = candidate.blocks[item.first];
    std::vector<Instr> instructions;
    instructions.reserve(block.instructions.size() + item.second.size());
    for (size_t index = 0; index + 1 < block.instructions.size(); ++index)
      instructions.push_back(block.instructions[index]);
    instructions.insert(instructions.end(), item.second.begin(), item.second.end());
    instructions.push_back(block.instructions.back());
    block.instructions = std::move(instructions);
  }
  for (const EdgeNeed& need : needs)
    candidate.blocks[need.target].instructions[need.phiIndex]
        .operands[need.operandIndex].reg = need.replacement;

  // Block insertion changes the order in which predecessors are discovered.
  // Re-key every phi's parallel pairs against that final CFG order before the
  // output verifier checks exact predecessor coverage.
  std::vector<std::vector<uint32_t>> predecessors(candidate.blocks.size());
  for (uint32_t block = 0; block < candidate.blocks.size(); ++block) {
    const std::vector<uint32_t> outgoing = successors(candidate, block);
    for (uint32_t target : outgoing) {
      if (target >= candidate.blocks.size()) return fail();
      predecessors[target].push_back(block);
    }
  }
  for (uint32_t target = 0; target < candidate.blocks.size(); ++target) {
    for (Instr& phi : candidate.blocks[target].instructions) {
      if (phi.opcode != Opcode::Phi) break;
      std::vector<bool> used(phi.operands.size() / 2, false);
      std::vector<Operand> reordered;
      reordered.reserve(phi.operands.size());
      for (uint32_t predecessor : predecessors[target]) {
        size_t found = used.size();
        for (size_t pair = 0; pair < used.size(); ++pair)
          if (!used[pair] && phi.operands[pair * 2 + 1].index == predecessor) {
            found = pair;
            break;
          }
        if (found == used.size()) return fail();
        used[found] = true;
        reordered.push_back(phi.operands[found * 2]);
        reordered.push_back(phi.operands[found * 2 + 1]);
      }
      if (reordered.size() != phi.operands.size()) return fail();
      phi.operands = std::move(reordered);
    }
  }

  for (Block& block : candidate.blocks) {
    const std::vector<Instr> original = block.instructions;
    std::vector<Instr> rewritten;
    rewritten.reserve(original.size() + genericLoads + 2);
    size_t index = 0;
    while (index < original.size() && original[index].opcode == Opcode::Phi) {
      rewritten.push_back(original[index++]);
    }
    // All phi definitions are parallel; stores belong after the complete
    // prefix so no store changes the value observed by a later phi.
    for (size_t phiIndex = 0; phiIndex < index; ++phiIndex) {
      const Instr& phi = original[phiIndex];
      if (hasSpill(plan, phi.def->id))
        rewritten.push_back(frameStore(static_cast<uint32_t>(plan.slot[phi.def->id]),
                                       *phi.def));
    }
    for (; index < original.size(); ++index) {
      Instr inst = original[index];
      auto emitLoad = [&](const Reg& source) {
        if (!hasSpill(plan, source.id)) return;
        const Reg replacement{static_cast<uint32_t>(nextReg++), source.type, source.cls};
        rewritten.push_back(frameLoad(static_cast<uint32_t>(plan.slot[source.id]),
                                      replacement));
        for (Operand& operand : inst.operands)
          if (operand.kind == OperandKind::Reg && operand.reg == source)
            operand.reg = replacement;
        if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base == source)
          inst.mem.base = replacement;
      };
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg) emitLoad(operand.reg);
      if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base.id != 0)
        emitLoad(inst.mem.base);
      rewritten.push_back(inst);
      if (inst.def && hasSpill(plan, inst.def->id))
        rewritten.push_back(frameStore(static_cast<uint32_t>(plan.slot[inst.def->id]),
                                       *inst.def));
    }
    block.instructions = std::move(rewritten);
  }
  for (const FrameObject& object : plan.objects) {
    FrameObject copy = object;
    copy.id = static_cast<uint32_t>(candidate.frameObjects.size());
    candidate.frameObjects.push_back(copy);
  }
  if (!detail::verifyFunction(candidate).empty()) return fail();
  function = std::move(candidate);
  return true;
}
}}  // namespace sysy::mir
