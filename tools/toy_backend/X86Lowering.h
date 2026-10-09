#pragma once
#include "ToyIR.h"
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace toy {
struct Slot { uint64_t offset = 0; Type type; };
class X86Lowering {
 public:
  X86Lowering(const Module& module, std::ostream& out) : module_(module), out_(out) {}
  void run();
 private:
  const Module& module_;
  std::ostream& out_;
  const Function* function_ = nullptr;
  std::map<std::string, Slot> locals_;
  std::map<std::string, uint64_t> objects_;
  std::map<std::string, uint64_t> phiTemps_;
  uint64_t cursor_ = 0, frame_ = 0, outgoing_ = 0;
  size_t functionIndex_ = 0, labelIndex_ = 0;
  void emit(const std::string& line);
  uint64_t reserve(uint64_t size, uint64_t align = 8);
  std::string loc(uint64_t offset) const;
  std::string slot(const std::string& name) const;
  std::string blockLabel(const std::string& name) const;
  std::string uniqueLabel();
  void global(const Global& g);
  void function(const Function& f);
  void layout();
  void incoming();
  void copies(const std::string& source, const std::string& destination);
  void branch(const Inst& i, const std::string& source);
  void call(const Inst& i);
  void instruction(const Inst& i, const std::string& block);
  void integer(const Inst& i);
  void floating(const Inst& i);
  void cast(const Inst& i);
  void compare(const Inst& i);
  void loadGP(const Value& v, const std::string& reg);
  void storeGP(const std::string& dst, const Type& ty, const std::string& reg);
  void loadXMM(const Value& v, const std::string& reg);
  void storeXMM(const std::string& dst, const Type& ty, const std::string& reg);
  static std::string reg32(const std::string& reg);
  static bool wide(const Type& t);
};
} // namespace toy
