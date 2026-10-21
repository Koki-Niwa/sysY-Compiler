// B01: minimal target-independent MIR core with explicit virtual registers,
// frame indices, memory operands and block successors. No code generation.
#ifndef SYSY_MIR_MIR_H
#define SYSY_MIR_MIR_H
#include <cstdint>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace sysy { namespace mir {
enum class Type : uint8_t { I32, I64, F32, Void };
enum class RegClass : uint8_t { GPR, FPR };
struct Reg {
  uint32_t id = 0;
  Type type = Type::I32;
  RegClass cls = RegClass::GPR;
  friend bool operator==(const Reg& a, const Reg& b) {
    return a.id == b.id && a.type == b.type && a.cls == b.cls;
  }
};
enum class OperandKind : uint8_t { Reg, Imm, Frame, Block };
struct Operand {
  OperandKind kind = OperandKind::Imm;
  Reg reg;
  int64_t imm = 0;
  uint32_t index = 0;  // Frame or Block
  static Operand makeReg(Reg r);
  static Operand makeImm(int64_t value);
  static Operand makeFrame(uint32_t index);
  static Operand makeBlock(uint32_t index);
};
struct MemOperand {
  bool valid = false;
  Reg base;
  int64_t offset = 0;
  uint32_t size = 0;
  uint32_t aliasClass = 0;
  bool isVolatile = false;
};
// Register-mask fact for call/clobber boundaries; bits index registers within
// the class, not virtual register ids.
struct RegMask {
  RegClass cls = RegClass::GPR;
  uint64_t bits = 0;
};
struct FrameObject {
  uint32_t id = 0;
  uint32_t size = 0;
  uint32_t align = 1;
  bool spill = false;
};
enum class Opcode : uint8_t {
  Add, Sub, Mul, Div, Copy, Load, Store, Br, CondBr, Ret, Phi
};
struct Instr {
  Opcode opcode = Opcode::Copy;
  std::optional<Reg> def;
  std::vector<Operand> operands;
  MemOperand mem;
  std::vector<RegMask> clobbers;
};
struct Block {
  uint32_t id = 0;
  std::vector<Instr> instructions;
};
struct Function {
  std::string name;
  std::vector<Block> blocks;
  uint32_t frameSlots = 0;
  std::vector<FrameObject> frameObjects;
};
struct Module {
  std::string name;
  std::vector<Function> functions;
};
// Deterministic, human-readable dump. Round-trip parsing is a later B01 slice.
std::string dump(const Module& module);
// Parse the deterministic dump format above. Returns nullopt and a diagnostic
// on malformed text; the verifier is not run implicitly.
std::optional<Module> parse(const std::string& text, std::string* error = nullptr);
// Return one message per violating instruction/block; empty means valid.
std::vector<std::string> verify(const Module& module);
// Block-boundary liveness over virtual-register ids. Phi operands are treated
// as block uses in this first slice; edge-sensitive liveness is later work.
struct Liveness {
  std::vector<std::set<uint32_t>> liveIn;
  std::vector<std::set<uint32_t>> liveOut;
  // Live values on a specific CFG edge, including phi operands that belong to
  // that predecessor. Key is (predecessor block, successor block).
  std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> edgeLive;
};
Liveness analyzeLiveness(const Function& function);
// SSA virtual-register interference over the liveness result. Pairs that are
// simultaneously live get an undirected edge; no coloring/RA is performed.
struct InterferenceGraph {
  std::vector<std::set<uint32_t>> neighbors;
};
InterferenceGraph buildInterference(const Function& function, const Liveness& liveness);
// Deterministic greedy coloring of virtual-register interference. Color 0 is
// unused; registers with no available color are marked spilled. No spill code
// or target register assignment is emitted in this prototype.
struct Allocation {
  std::vector<int> color;   // indexed by virtual id; -1 = spilled/unused
  std::vector<bool> spilled;
  size_t colors = 0;
};
Allocation greedyColor(const InterferenceGraph& graph, size_t maxColors);
// Map spilled virtual ids to frame slots. No load/store rewrite is performed.
struct SpillPlan {
  std::vector<int> slot;              // virtual id -> frame id, -1 if not spilled
  std::vector<FrameObject> objects;
};
SpillPlan planSpills(const Allocation& allocation, uint32_t slotBytes = 8,
                     uint32_t alignment = 8);
}}  // namespace sysy::mir
#endif
