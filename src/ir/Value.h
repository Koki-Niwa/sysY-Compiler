#ifndef SYSY_IR_VALUE_H
#define SYSY_IR_VALUE_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include "ir/Type.h"
#include "support/SourceLoc.h"

namespace sysy {
namespace flat {
class Instruction;
class Function;
class Module;

struct Use {
  Instruction* user;
  size_t operandIndex;
};

enum class ValueKind : uint8_t { Inst, Argument, Constant, GlobalAddr, BlockAddr };

class Value {
 public:
  Value(ValueKind kind, const Type* type) : kind_(kind), type_(type) {}
  virtual ~Value() = default;
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  ValueKind kind() const { return kind_; }
  const Type* type() const { return type_; }
  // Set exactly once by the owning Module; detached scratch has no owner.
  Module* ownerModule() const { return ownerModule_; }
  void setType(const Type* type) { type_ = type; }
  bool isInst() const { return kind_ == ValueKind::Inst; }
  bool isArgument() const { return kind_ == ValueKind::Argument; }
  bool isConstant() const { return kind_ == ValueKind::Constant; }
  bool isGlobalAddr() const { return kind_ == ValueKind::GlobalAddr; }
  bool isBlockAddr() const { return kind_ == ValueKind::BlockAddr; }
  const std::vector<Use>& uses() const { return uses_; }
  size_t useCount() const { return uses_.size(); }
  bool hasUses() const { return !uses_.empty(); }
  // Legacy builder-local ordering tag. FlatDump never uses this for numbering.
  uint64_t id() const { return id_; }
  void setId(uint64_t value) { id_ = value; }

 private:
  friend class IRMutator;
  // Internal-only F00 primitive; optimizer RAUW is IRMutator.
  bool replaceAllUsesWith(Value* replacement);
  friend class Instruction;
  friend class Module;
  void addUse(Instruction* user, size_t index);
  void removeUse(Instruction* user, size_t index);
  void clearUses() { uses_.clear(); }
  ValueKind kind_;
  const Type* type_;
  Module* ownerModule_ = nullptr;
  std::vector<Use> uses_;
  uint64_t id_ = 0;
};

class Argument : public Value {
 public:
  Argument(const Type* type, SourceLoc loc)
      : Value(ValueKind::Argument, type), loc_(loc) {}
  Function* owner() const { return owner_; }
  size_t index() const { return index_; }
  SourceLoc loc() const { return loc_; }

 private:
  friend class Function;
  void registerOwner(Function* function, size_t index) {
    if (owner_ == nullptr) { owner_ = function; index_ = index; }
  }
  Function* owner_ = nullptr;
  size_t index_ = 0;
  SourceLoc loc_;
};

// Canonical identity is (type, raw bits), independent of source locations.
class Constant : public Value {
 public:
  Constant(const Type* type, uint64_t bits)
      : Value(ValueKind::Constant, type), bits_(bits) {}
  bool isFloat() const { return type() != nullptr && type()->isFloat(); }
  uint64_t rawBits() const { return bits_; }
  int64_t intVal() const {
    if (isFloat()) return 0;
    if (type() != nullptr && type()->kind == TypeKind::I32)
      return static_cast<int32_t>(static_cast<uint32_t>(bits_));
    return static_cast<int64_t>(bits_);
  }
  int32_t i32Val() const { return static_cast<int32_t>(static_cast<uint32_t>(bits_)); }
  uint32_t fbits() const { return static_cast<uint32_t>(bits_); }

 private:
  uint64_t bits_;
};
}  // namespace flat
}  // namespace sysy
#endif
