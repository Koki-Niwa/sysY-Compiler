#include "opt/GVN.h"
#include <algorithm>
#include <cstddef>
#include <vector>
#include "analysis/DominatorAnalysis.h"
#include "ir/Instruction.h"
namespace sysy { namespace flat {
namespace {
bool pureForGVN(const Instruction& inst) {
  switch (inst.op()) {
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
    case Opcode::SDiv: case Opcode::SRem:
    case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
    case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
    case Opcode::ICmp: case Opcode::FCmp: case Opcode::Select:
    case Opcode::SIToFP: case Opcode::FPToSI: case Opcode::FPExt:
    case Opcode::SExt: case Opcode::ZExt: case Opcode::Trunc:
    case Opcode::BitCast: case Opcode::GEP:
      return inst.numOperands() <= 3;
    default:
      return false;
  }
}
bool commutative(Opcode op) {
  switch (op) {
    case Opcode::Add: case Opcode::Mul:
    case Opcode::And: case Opcode::Or: case Opcode::Xor:
      return true;
    default:
      return false;
  }
}
struct Key {
  Opcode op = Opcode::Add;
  const Type* type = nullptr;
  const Value* a = nullptr;
  const Value* b = nullptr;
  const Value* c = nullptr;
  IPred ipred = IPred::Eq;
  FPred fpred = FPred::Oeq;
  const Type* src = nullptr;
  bool operator==(const Key& other) const {
    return op == other.op && type == other.type && a == other.a &&
           b == other.b && c == other.c && ipred == other.ipred &&
           fpred == other.fpred && src == other.src;
  }
};
struct KeyHash {
  size_t operator()(const Key& key) const {
    size_t h = static_cast<size_t>(key.op);
    auto mix = [&h](const void* p) {
      h ^= reinterpret_cast<size_t>(p) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(key.type); mix(key.a); mix(key.b); mix(key.c); mix(key.src);
    h ^= static_cast<size_t>(key.ipred) * 131u + static_cast<size_t>(key.fpred);
    return h;
  }
};
Key makeKey(const Instruction& inst) {
  Key key;
  key.op = inst.op();
  key.type = inst.type();
  key.ipred = inst.ipred();
  key.fpred = inst.fpred();
  key.src = inst.srcElemType();
  const size_t n = std::min<size_t>(inst.numOperands(), 3);
  const Value* ops[3] = {nullptr, nullptr, nullptr};
  for (size_t i = 0; i < n; ++i) ops[i] = inst.operand(i);
  if (commutative(inst.op()) && std::less<const Value*>()(ops[1], ops[0]))
    std::swap(ops[0], ops[1]);
  key.a = ops[0]; key.b = ops[1]; key.c = ops[2];
  return key;
}
}  // namespace

GVNPass::GVNPass() : FunctionPass("gvn") {}

PassResult GVNPass::run(Function& function, PassContext& context) {
  PassResult result;
  AnalysisHandle<DominatorInfo> dominators =
      context.analyses().get<DominatorAnalysis>(function);
  if (!dominators.isCurrent() || function.entry() == nullptr) return result;
  const size_t count = function.blockCount();
  std::vector<std::vector<size_t>> children(count);
  for (size_t i = 0; i < count; ++i) {
    const BasicBlock* idom = dominators.get()->idom(i);
    if (i != function.entry()->index() && idom != nullptr &&
        idom->index() < count)
      children[idom->index()].push_back(i);
  }
  std::unordered_map<Key, Instruction*, KeyHash> available;
  std::vector<std::vector<Key>> inserted(count);
  std::vector<std::pair<size_t, bool>> stack;
  stack.emplace_back(function.entry()->index(), false);
  bool changed = false;
  while (!stack.empty()) {
    const auto [index, exiting] = stack.back();
    stack.pop_back();
    if (exiting) {
      for (const Key& key : inserted[index]) available.erase(key);
      continue;
    }
    BasicBlock* block = function.at(index);
    if (block == nullptr) continue;
    stack.emplace_back(index, true);
    for (size_t i = children[index].size(); i-- > 0;)
      stack.emplace_back(children[index][i], false);
    std::vector<Instruction*> instructions;
    for (Instruction* inst : block->insts())
      if (inst != nullptr) instructions.push_back(inst);
    for (Instruction* inst : instructions) {
      if (!pureForGVN(*inst)) continue;
      const Key key = makeKey(*inst);
      const auto found = available.find(key);
      if (found != available.end() && found->second != inst) {
        if (context.mutator().replaceAndErase(inst, found->second) ==
            MutationResult::Applied) {
          ++result.stats.counters["gvn.eliminated"];
          changed = true;
        }
        continue;
      }
      if (found == available.end()) {
        available.emplace(key, inst);
        inserted[index].push_back(key);
      }
    }
  }
  result.status = changed ? PassStatus::Applied : PassStatus::NoChange;
  return result;
}
}}  // namespace sysy::flat
