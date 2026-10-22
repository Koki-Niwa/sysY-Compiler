#include "mir/MIR.h"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
namespace sysy { namespace mir {
Operand Operand::makeReg(Reg r) {
  Operand o; o.kind = OperandKind::Reg; o.reg = r; return o;
}
Operand Operand::makeImm(int64_t value) {
  Operand o; o.kind = OperandKind::Imm; o.imm = value; return o;
}
Operand Operand::makeFrame(uint32_t index) {
  Operand o; o.kind = OperandKind::Frame; o.index = index; return o;
}
Operand Operand::makeBlock(uint32_t index) {
  Operand o; o.kind = OperandKind::Block; o.index = index; return o;
}
namespace {
bool isTerminator(Opcode opcode) {
  return opcode == Opcode::Br || opcode == Opcode::CondBr ||
         opcode == Opcode::Ret;
}
bool hasDef(Opcode opcode) {
  return opcode != Opcode::Store && opcode != Opcode::Br &&
         opcode != Opcode::CondBr && opcode != Opcode::Ret;
}
std::vector<std::vector<uint32_t>> successors(const Function& function) {
  std::vector<std::vector<uint32_t>> result(function.blocks.size());
  for (size_t b = 0; b < function.blocks.size(); ++b) {
    const Block& block = function.blocks[b];
    if (block.instructions.empty()) continue;
    const Instr& term = block.instructions.back();
    if (term.opcode == Opcode::Br && term.operands.size() == 1 &&
        term.operands[0].kind == OperandKind::Block)
      result[b].push_back(term.operands[0].index);
    if (term.opcode == Opcode::CondBr && term.operands.size() == 3 &&
        term.operands[1].kind == OperandKind::Block &&
        term.operands[2].kind == OperandKind::Block) {
      result[b].push_back(term.operands[1].index);
      result[b].push_back(term.operands[2].index);
    }
  }
  return result;
}
}  // namespace
std::vector<std::string> verify(const Module& module) {
  std::vector<std::string> errors;
  std::set<std::string> functionNames;
  for (const Function& function : module.functions) {
    if (function.name.empty()) errors.push_back("empty function name");
    if (!functionNames.insert(function.name).second)
      errors.push_back("duplicate function " + function.name);
    if (function.blocks.empty()) {
      errors.push_back(function.name + ": no blocks");
      continue;
    }
    std::set<uint32_t> frameIds;
    for (size_t i = 0; i < function.frameObjects.size(); ++i) {
      const FrameObject& object = function.frameObjects[i];
      if (object.id != i)
        errors.push_back(function.name + ": frame object ids must be contiguous");
      if (!frameIds.insert(object.id).second)
        errors.push_back(function.name + ": duplicate frame object");
      if (object.size == 0)
        errors.push_back(function.name + ": zero-sized frame object");
      if (object.align == 0 || (object.align & (object.align - 1)) != 0)
        errors.push_back(function.name + ": frame alignment must be a power of two");
    }
    std::set<uint32_t> blockIds;
    for (size_t b = 0; b < function.blocks.size(); ++b) {
      const Block& block = function.blocks[b];
      if (block.id != b) {
        errors.push_back(function.name + ": block ids must be contiguous (B" +
                         std::to_string(b) + " expected)");
        continue;
      }
      if (!blockIds.insert(block.id).second)
        errors.push_back(function.name + ": duplicate block B" +
                         std::to_string(block.id));
      if (block.instructions.empty()) {
        errors.push_back(function.name + ": empty block B" +
                         std::to_string(block.id));
        continue;
      }
      bool terminatorSeen = false;
      for (const Instr& inst : block.instructions) {
        if (terminatorSeen)
          errors.push_back(function.name + ": instruction after terminator");
        if (isTerminator(inst.opcode)) terminatorSeen = true;
      }
      if (!terminatorSeen)
        errors.push_back(function.name + ": block B" + std::to_string(block.id) +
                         " has no terminator");
      const Instr& term = block.instructions.back();
      if (term.opcode == Opcode::Br &&
          (term.operands.size() != 1 ||
           term.operands[0].kind != OperandKind::Block))
        errors.push_back(function.name + ": malformed br");
      if (term.opcode == Opcode::CondBr &&
          (term.operands.size() != 3 ||
           term.operands[1].kind != OperandKind::Block ||
           term.operands[2].kind != OperandKind::Block))
        errors.push_back(function.name + ": malformed condbr");
    }
    const auto succ = successors(function);
    std::vector<std::vector<uint32_t>> preds(function.blocks.size());
    for (size_t b = 0; b < succ.size(); ++b) {
      for (uint32_t target : succ[b]) {
        if (target >= function.blocks.size())
          errors.push_back(function.name + ": branch target out of range");
        else
          preds[target].push_back(static_cast<uint32_t>(b));
      }
    }
    // Dominator sets over reachable blocks; unreachable blocks are rejected
    // because no definition can dominate their uses.
    const size_t count = function.blocks.size();
    std::vector<bool> reachable(count, false);
    std::vector<uint32_t> work{0};
    reachable[0] = true;
    while (!work.empty()) {
      const uint32_t b = work.back();
      work.pop_back();
      for (uint32_t target : succ[b])
        if (target < count && !reachable[target]) {
          reachable[target] = true;
          work.push_back(target);
        }
    }
    for (size_t b = 0; b < count; ++b)
      if (!reachable[b])
        errors.push_back(function.name + ": unreachable block B" + std::to_string(b));
    std::vector<std::set<uint32_t>> dominators(count);
    for (size_t b = 0; b < count; ++b)
      if (reachable[b]) {
        if (b == 0) dominators[b] = {0};
        else for (size_t i = 0; i < count; ++i) dominators[b].insert(i);
      }
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 1; b < count; ++b) {
        if (!reachable[b] || preds[b].empty()) continue;
        std::set<uint32_t> next;
        bool first = true;
        for (uint32_t pred : preds[b]) {
          if (!reachable[pred]) continue;
          if (first) { next = dominators[pred]; first = false; }
          else {
            std::set<uint32_t> intersection;
            for (uint32_t value : next)
              if (dominators[pred].count(value)) intersection.insert(value);
            next = std::move(intersection);
          }
        }
        next.insert(static_cast<uint32_t>(b));
        if (next != dominators[b]) {
          dominators[b] = std::move(next);
          changed = true;
        }
      }
    }
    auto dominates = [&](uint32_t definition, uint32_t use) {
      return reachable[use] && dominators[use].count(definition) != 0;
    };
    struct Def {
      Reg reg;
      uint32_t block = 0;
      size_t index = 0;
    };
    std::map<uint32_t, Def> defs;
    for (size_t b = 0; b < count; ++b) {
      for (size_t i = 0; i < function.blocks[b].instructions.size(); ++i) {
        const Instr& inst = function.blocks[b].instructions[i];
        if (!inst.def) continue;
        if (!hasDef(inst.opcode))
          errors.push_back(function.name + ": unexpected def on terminator");
        if (inst.def->type == Type::Void)
          errors.push_back(function.name + ": void def");
        const auto found = defs.find(inst.def->id);
        if (found != defs.end())
          errors.push_back(function.name + ": duplicate definition of r" +
                           std::to_string(inst.def->id));
        else
          defs[inst.def->id] = Def{*inst.def, static_cast<uint32_t>(b), i};
      }
    }
    auto checkUse = [&](const Reg& reg, uint32_t block, size_t index,
                        bool phiValue, uint32_t phiPred) {
      const auto found = defs.find(reg.id);
      if (found == defs.end()) {
        errors.push_back(function.name + ": use of undefined register r" +
                         std::to_string(reg.id));
        return;
      }
      if (!(found->second.reg == reg))
        errors.push_back(function.name + ": register type/class mismatch");
      if (phiValue) {
        if (!dominates(found->second.block, phiPred))
          errors.push_back(function.name + ": phi value does not dominate predecessor");
      } else if (!dominates(found->second.block, block) ||
                 (found->second.block == block && found->second.index >= index)) {
        errors.push_back(function.name + ": use does not follow a dominating definition");
      }
    };
    for (size_t b = 0; b < count; ++b) {
      const Block& block = function.blocks[b];
      for (size_t i = 0; i < block.instructions.size(); ++i) {
        const Instr& inst = block.instructions[i];
        if (inst.opcode == Opcode::Phi) {
          if (inst.operands.size() != preds[b].size() * 2) {
            errors.push_back(function.name + ": phi arity does not match predecessors");
          } else {
            for (size_t pair = 0; pair < preds[b].size(); ++pair) {
              const Operand& value = inst.operands[pair * 2];
              const Operand& edge = inst.operands[pair * 2 + 1];
              if (value.kind != OperandKind::Reg || edge.kind != OperandKind::Block ||
                  edge.index != preds[b][pair])
                errors.push_back(function.name + ": malformed phi edge pair");
              else
                checkUse(value.reg, static_cast<uint32_t>(b), i, true, edge.index);
            }
          }
          continue;
        }
        for (const Operand& operand : inst.operands) {
          if (operand.kind == OperandKind::Reg)
            checkUse(operand.reg, static_cast<uint32_t>(b), i, false, 0);
          if (operand.kind == OperandKind::Frame) {
            const uint32_t limit = function.frameObjects.empty()
                ? function.frameSlots
                : static_cast<uint32_t>(function.frameObjects.size());
            if (operand.index >= limit)
              errors.push_back(function.name + ": frame index out of range");
          }
        }
        if (inst.mem.valid) {
          if (inst.mem.size == 0) errors.push_back(function.name + ": zero memory size");
          if (inst.mem.base.id != 0)
            checkUse(inst.mem.base, static_cast<uint32_t>(b), i, false, 0);
        }
        for (const RegMask& mask : inst.clobbers)
          if (mask.cls != RegClass::GPR && mask.cls != RegClass::FPR)
            errors.push_back(function.name + ": invalid register mask class");
      }
    }
  }
  return errors;
}
}}  // namespace sysy::mir

namespace sysy { namespace mir {
Liveness analyzeLiveness(const Function& function) {
  Liveness result;
  const size_t count = function.blocks.size();
  result.liveIn.resize(count);
  result.liveOut.resize(count);
  std::vector<std::set<uint32_t>> use(count), def(count);
  std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> phiUse;
  for (size_t b = 0; b < count; ++b) {
    for (const Instr& inst : function.blocks[b].instructions) {
      if (inst.opcode == Opcode::Phi) {
        if (inst.def) def[b].insert(inst.def->id);
        for (size_t pair = 0; pair + 1 < inst.operands.size(); pair += 2) {
          const Operand& value = inst.operands[pair];
          const Operand& edge = inst.operands[pair + 1];
          if (value.kind == OperandKind::Reg && edge.kind == OperandKind::Block)
            phiUse[{static_cast<uint32_t>(b), edge.index}].insert(value.reg.id);
        }
        continue;
      }
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg && !def[b].count(operand.reg.id))
          use[b].insert(operand.reg.id);
      if (inst.mem.valid && inst.mem.base.id != 0 &&
          !def[b].count(inst.mem.base.id))
        use[b].insert(inst.mem.base.id);
      if (inst.def) def[b].insert(inst.def->id);
    }
  }
  auto successors = [&](size_t b) {
    std::vector<size_t> targets;
    const Block& block = function.blocks[b];
    if (block.instructions.empty()) return targets;
    const Instr& term = block.instructions.back();
    if (term.opcode == Opcode::Br && term.operands.size() == 1)
      targets.push_back(term.operands[0].index);
    if (term.opcode == Opcode::CondBr && term.operands.size() == 3) {
      targets.push_back(term.operands[1].index);
      targets.push_back(term.operands[2].index);
    }
    return targets;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t b = count; b-- > 0;) {
      std::set<uint32_t> out;
      for (size_t target : successors(b)) {
        if (target >= count) continue;
        std::set<uint32_t> edge = result.liveIn[target];
        const auto found = phiUse.find({static_cast<uint32_t>(target),
                                        static_cast<uint32_t>(b)});
        if (found != phiUse.end())
          edge.insert(found->second.begin(), found->second.end());
        auto& slot = result.edgeLive[{static_cast<uint32_t>(b),
                                      static_cast<uint32_t>(target)}];
        if (slot != edge) {
          slot = edge;
          changed = true;
        }
        out.insert(edge.begin(), edge.end());
      }
      std::set<uint32_t> in = use[b];
      for (uint32_t id : out)
        if (!def[b].count(id)) in.insert(id);
      if (out != result.liveOut[b] || in != result.liveIn[b]) {
        result.liveOut[b] = std::move(out);
        result.liveIn[b] = std::move(in);
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
        if (operand.kind == OperandKind::Reg) maxId = std::max(maxId, operand.reg.id);
      if (inst.mem.valid) maxId = std::max(maxId, inst.mem.base.id);
    }
  graph.neighbors.resize(maxId + 1);
  auto link = [&](uint32_t a, uint32_t b) {
    if (a == 0 || b == 0 || a == b) return;
    graph.neighbors[a].insert(b);
    graph.neighbors[b].insert(a);
  };
  for (size_t b = 0; b < liveness.liveIn.size(); ++b) {
    for (uint32_t a : liveness.liveIn[b])
      for (uint32_t c : liveness.liveIn[b]) link(a, c);
    for (uint32_t a : liveness.liveOut[b])
      for (uint32_t c : liveness.liveOut[b]) link(a, c);
    for (const Instr& inst : function.blocks[b].instructions)
      if (inst.def)
        for (uint32_t live : liveness.liveOut[b]) link(inst.def->id, live);
  }
  return graph;
}
}}  // namespace sysy::mir

namespace sysy { namespace mir {
Allocation greedyColor(const InterferenceGraph& graph, size_t maxColors) {
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
  for (uint32_t id : order) {
    std::vector<bool> used(maxColors, false);
    for (uint32_t neighbor : graph.neighbors[id])
      if (neighbor < count && result.color[neighbor] >= 0 &&
          static_cast<size_t>(result.color[neighbor]) < maxColors)
        used[result.color[neighbor]] = true;
    int chosen = -1;
    for (size_t color = 0; color < maxColors; ++color)
      if (!used[color]) { chosen = static_cast<int>(color); break; }
    if (chosen < 0) {
      result.spilled[id] = true;
    } else {
      result.color[id] = chosen;
      result.colors = std::max(result.colors, static_cast<size_t>(chosen + 1));
    }
  }
  return result;
}
}}  // namespace sysy::mir

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
bool rewriteSpills(Function& function, const SpillPlan& plan,
                   size_t* errorCount) {
  if (errorCount) *errorCount = 0;
  auto fail = [&]() {
    if (errorCount) ++*errorCount;
    return false;
  };
  if (!function.frameObjects.empty()) return fail();
  uint32_t nextReg = 1;
  for (const Block& block : function.blocks)
    for (const Instr& inst : block.instructions) {
      if (inst.def) nextReg = std::max(nextReg, inst.def->id + 1);
      for (const Operand& operand : inst.operands)
        if (operand.kind == OperandKind::Reg)
          nextReg = std::max(nextReg, operand.reg.id + 1);
    }
  auto sizeOf = [](Type type) -> uint32_t {
    switch (type) {
      case Type::I32: return 4;
      case Type::I64: return 8;
      case Type::F32: return 4;
      case Type::Void: return 0;
    }
    return 0;
  };
  auto frameMem = [&](uint32_t frame, uint32_t size) {
    MemOperand mem;
    mem.valid = true;
    mem.frameBase = true;
    mem.frame = frame;
    mem.size = size;
    return mem;
  };
  // Phi operands are live on predecessor edges: load a spilled value in the
  // predecessor before its terminator and replace the phi operand with the
  // fresh load result.
  for (Block& block : function.blocks) {
    for (Instr& inst : block.instructions) {
      if (inst.opcode != Opcode::Phi) continue;
      for (size_t pair = 0; pair + 1 < inst.operands.size(); pair += 2) {
        Operand& value = inst.operands[pair];
        const Operand& edge = inst.operands[pair + 1];
        if (value.kind != OperandKind::Reg || edge.kind != OperandKind::Block ||
            value.reg.id == 0 ||
            static_cast<size_t>(value.reg.id) >= plan.slot.size() ||
            plan.slot[value.reg.id] < 0)
          continue;
        if (edge.index >= function.blocks.size()) return fail();
        Block& predecessor = function.blocks[edge.index];
        if (predecessor.instructions.empty()) return fail();
        const uint32_t size = sizeOf(value.reg.type);
        if (size == 0) return fail();
        Instr load;
        load.opcode = Opcode::Load;
        load.def = Reg{nextReg++, value.reg.type, value.reg.cls};
        load.mem = frameMem(static_cast<uint32_t>(plan.slot[value.reg.id]), size);
        value.reg = *load.def;
        predecessor.instructions.insert(predecessor.instructions.end() - 1,
                                        std::move(load));
      }
    }
  }
  for (Block& block : function.blocks) {
    std::vector<Instr> rewritten;
    for (Instr inst : block.instructions) {
      if (inst.opcode == Opcode::Phi) {
        if (inst.def && inst.def->id != 0 &&
            static_cast<size_t>(inst.def->id) < plan.slot.size() &&
            plan.slot[inst.def->id] >= 0) {
          const uint32_t size = sizeOf(inst.def->type);
          if (size == 0) return fail();
          rewritten.push_back(inst);
          Instr store;
          store.opcode = Opcode::Store;
          store.operands.push_back(Operand::makeReg(*inst.def));
          store.mem = frameMem(static_cast<uint32_t>(plan.slot[inst.def->id]), size);
          rewritten.push_back(std::move(store));
        } else {
          rewritten.push_back(inst);
        }
        continue;
      }
      auto emitLoad = [&](const Reg& original) -> std::optional<Reg> {
        if (original.id == 0 || static_cast<size_t>(original.id) >= plan.slot.size() ||
            plan.slot[original.id] < 0)
          return std::nullopt;
        const uint32_t size = sizeOf(original.type);
        if (size == 0) return std::nullopt;
        Instr load;
        load.opcode = Opcode::Load;
        load.def = Reg{nextReg++, original.type, original.cls};
        load.mem = frameMem(static_cast<uint32_t>(plan.slot[original.id]), size);
        const Reg replacement = *load.def;
        rewritten.push_back(std::move(load));
        return replacement;
      };
      for (Operand& operand : inst.operands) {
        if (operand.kind != OperandKind::Reg) continue;
        const auto replacement = emitLoad(operand.reg);
        if (replacement) operand.reg = *replacement;
      }
      if (inst.mem.valid && !inst.mem.frameBase && inst.mem.base.id != 0) {
        const auto replacement = emitLoad(inst.mem.base);
        if (replacement) inst.mem.base = *replacement;
      }
      if (inst.def && inst.def->id != 0 &&
          static_cast<size_t>(inst.def->id) < plan.slot.size() &&
          plan.slot[inst.def->id] >= 0) {
        const uint32_t size = sizeOf(inst.def->type);
        if (size == 0) return fail();
        rewritten.push_back(inst);
        Instr store;
        store.opcode = Opcode::Store;
        store.operands.push_back(Operand::makeReg(*inst.def));
        store.mem = frameMem(static_cast<uint32_t>(plan.slot[inst.def->id]), size);
        rewritten.push_back(std::move(store));
      } else {
        rewritten.push_back(inst);
      }
    }
    block.instructions = std::move(rewritten);
  }
  for (const FrameObject& object : plan.objects) {
    FrameObject copy = object;
    copy.id = static_cast<uint32_t>(function.frameObjects.size());
    function.frameObjects.push_back(copy);
  }
  return true;
}
}}  // namespace sysy::mir
