#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace toy {
struct Type {
  std::string name;
  uint64_t size = 0;
  uint64_t count = 0;
  std::vector<Type> children;
  bool isFloat() const { return name == "float" || name == "double"; }
  bool isInteger() const { return name == "i1" || name == "i8" || name == "i32" || name == "i64"; }
  bool isArray() const { return name == "array"; }
};
struct Value {
  Type type;
  std::string text;
};
struct PhiEdge {
  std::string predecessor;
  std::string source;
};
struct Inst {
  std::string op, dst, pred, callee, target, alternate, source;
  Type type, sourceType;
  std::vector<Value> values;
  std::vector<PhiEdge> incoming;
  int line = 0;
};
struct Block {
  std::string label;
  std::vector<Inst> instructions;
};
struct Function {
  std::string name;
  Type result;
  std::vector<Value> params;
  std::vector<Block> blocks;
  bool variadic = false;
  bool declaration = false;
};
struct Global {
  std::string name;
  Type type;
  uint64_t align = 4;
  bool external = false;
  std::map<uint64_t, uint32_t> words;
};
struct Module {
  std::vector<Global> globals;
  std::vector<Function> functions;
  std::map<std::string, uint64_t> opcodes;
  std::map<std::string, uint64_t> checks;
};
struct ParseError : std::runtime_error {
  int line;
  std::string source;
  ParseError(int ln, std::string raw, const std::string& reason)
      : std::runtime_error(reason), line(ln), source(std::move(raw)) {}
};
class Cursor {
 public:
  explicit Cursor(const std::string& line) : line_(line) {}
  std::string peek();
  std::string take();
  bool accept(const std::string& token);
  void expect(const std::string& token);
  void end();
 private:
  const std::string& line_;
  size_t pos_ = 0;
  bool cached_ = false;
  std::string next_;
};
Type parseType(Cursor& c);
Value parseValue(Cursor& c, Type type);
std::string symbolName(const std::string& token);
std::string gasSymbol(const std::string& name);
uint32_t floatBits(const std::string& literal);
uint64_t doubleBits(const std::string& literal);
std::string jsonQuote(const std::string& text);
std::string sha256File(const std::string& path);
Module parseModule(const std::string& path);
void lowerModule(const Module& module, const std::string& path);
}  // namespace toy
