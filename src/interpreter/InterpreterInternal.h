#ifndef SYSY_INTERPRETER_INTERNAL_H
#define SYSY_INTERPRETER_INTERNAL_H

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frontend/ConstEval.h"
#include "frontend/RuntimeLib.h"
#include "interpreter/Interpreter.h"

namespace sysy::interp::detail {

struct Scalar {
  bool isFloat = false;
  int32_t i = 0;
  float f = 0.0f;
  static Scalar integer(int32_t v) { Scalar s; s.i = v; return s; }
  static Scalar floating(float v) { Scalar s; s.isFloat = true; s.f = v; return s; }
};
struct Storage { std::vector<Scalar> cells; std::vector<unsigned char> initialized; };
struct View {
  std::shared_ptr<Storage> storage;
  size_t base = 0;
  std::vector<size_t> shape;
  bool isFloat = false;
};
struct Value {
  enum class Kind { Void, Scalar, Array } kind = Kind::Void;
  Scalar scalar;
  View array;
  static Value voidValue() { return Value(); }
  static Value scalarValue(Scalar s) { Value v; v.kind = Kind::Scalar; v.scalar = s; return v; }
  static Value arrayValue(View a) { Value v; v.kind = Kind::Array; v.array = std::move(a); return v; }
};
struct Object { const Type* type = nullptr; bool isConst = false; std::shared_ptr<Storage> storage; };
struct Binding {
  const Type* type = nullptr;
  bool isConst = false;
  std::shared_ptr<Object> object;
  View parameter;
  bool isArrayParameter = false;
};
struct Cell {
  std::shared_ptr<Storage> storage;
  size_t index = 0;
  bool isConst = false;
};
struct Control {
  enum class Kind { Normal, Break, Continue, Return } kind = Kind::Normal;
  Value value;
  static Control normal() { return Control(); }
  static Control signal(Kind k) { Control c; c.kind = k; return c; }
  static Control returned(Value v) { Control c; c.kind = Kind::Return; c.value = std::move(v); return c; }
};
struct Input {
  std::string_view text;
  size_t pos = 0;
  void skipSpace() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos; }
  bool token(std::string& out) {
    skipSpace(); const size_t begin = pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) == 0) ++pos;
    if (begin == pos) return false; out.assign(text.substr(begin, pos - begin)); return true;
  }
  bool character(char& out) { if (pos >= text.size()) return false; out = text[pos++]; return true; }
};

inline bool typeCount(const Type* type, uint64_t limit, size_t& out) {
  if (type == nullptr) return false;
  const uint64_t sizeLimit = std::min<uint64_t>(
      limit, static_cast<uint64_t>(std::numeric_limits<size_t>::max()));
  uint64_t count = 1;
  for (const Type* current = type; current != nullptr && current->isArray();
       current = current->elem) {
    if (current->len < 0) return false;
    const uint64_t extent = static_cast<uint64_t>(current->len);
    if (extent != 0 && count > sizeLimit / extent) return false;
    count *= extent;
  }
  if (count > sizeLimit) return false;
  out = static_cast<size_t>(count);
  return true;
}
inline bool checkedMultiply(size_t lhs, size_t rhs, size_t& out) {
  if (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs) return false;
  out = lhs * rhs;
  return true;
}
inline bool checkedShapeProduct(const std::vector<size_t>& shape, size_t& out) {
  out = 1;
  for (const size_t extent : shape) {
    if (!checkedMultiply(out, extent, out)) return false;
  }
  return true;
}
inline std::vector<size_t> typeShape(const Type* type) {
  std::vector<size_t> shape;
  for (const Type* t = type; t != nullptr && t->isArray(); t = t->elem)
    shape.push_back(static_cast<size_t>(t->len));
  return shape;
}
inline bool isFloatType(const Type* type) { return type != nullptr && type->kind == TypeKind::Float; }
inline int32_t wrapAdd(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t wrapSub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t wrapMul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t normDiv(int32_t a, int32_t b) {
  return b == 0 || (a == std::numeric_limits<int32_t>::min() && b == -1) ? 0 : a / b;
}
inline int32_t normRem(int32_t a, int32_t b) {
  // S05's execution contract preserves the dividend for remainder by zero.
  if (b == 0) return a;
  return (a == std::numeric_limits<int32_t>::min() && b == -1) ? 0 : a % b;
}
inline int32_t satFloatToInt(float v) {
  if (std::isnan(v)) return 0;
  if (v >= 2147483648.0f) return std::numeric_limits<int32_t>::max();
  if (v < -2147483648.0f) return std::numeric_limits<int32_t>::min();
  return static_cast<int32_t>(v);
}

class Engine {
 public:
  Engine(const CompUnit& unit, std::string_view input, const Limits& limits)
      : unit_(unit), input_{input}, limits_(limits) {}
  RunResult execute();

 private:
  using Scope = std::unordered_map<std::string, Binding>;
  const CompUnit& unit_;
  Input input_;
  Limits limits_;
  RunResult result_;
  uint64_t steps_ = 0;
  uint32_t callDepth_ = 0;
  uint32_t expressionDepth_ = 0;
  uint32_t statementDepth_ = 0;
  uint64_t totalElements_ = 0;
  std::vector<Scope> scopes_;
  std::unordered_map<std::string, const FuncDef*> functions_;
  bool fail(RunStatus, SourceLoc, std::string);
  bool tick(SourceLoc);
  bool append(SourceLoc, std::string_view);
  const FuncDef* function(const std::string&) const;
  Binding* lookup(const std::string&);
  const Binding* lookup(const std::string&) const;
  std::shared_ptr<Object> makeObject(const Type*, bool, SourceLoc);
  View objectView(const Object&) const;
  View bindingView(const Binding&) const;
  bool validateUnit();
  bool executeGlobalDecl(const Decl&);
  bool executeDecl(const Decl&);
  bool initializeObject(Object&, const InitVal*, SourceLoc);
  bool initializeScalar(Object&, const InitVal*, SourceLoc);
  bool initializeArray(Object&, const InitVal*, SourceLoc);
  bool initGroup(Object&, const InitVal&, int, int, size_t);
  bool writeElement(const Cell&, Scalar, SourceLoc);
  bool readElement(const Cell&, Scalar&, SourceLoc);
  bool evalExpr(const Expr*, Value&);
  bool evalLVal(const LVal&, Value&);
  bool evalCell(const LVal&, Cell&);
  bool evalView(const LVal&, View&);
  bool evalCall(const Call&, Value&);
  bool evalRuntimeCall(const Call&, const std::vector<Value>&, Value&);
  bool invokeUser(const FuncDef&, const std::vector<Value>&, Value&);
  bool execBlock(const BlockStmt&, Control&);
  bool execStmt(const Node*, Control&);
  bool execWhile(const WhileStmt&, Control&);
  bool coerce(const Value&, const Type*, Value&, SourceLoc);
  bool asInt(const Value&, int32_t&, SourceLoc);
  bool asBool(const Value&, bool&, SourceLoc);
  bool readInputInt(int32_t&, SourceLoc);
  bool readInputFloat(float&, SourceLoc);
  bool readInputChar(char&, SourceLoc);
  bool appendInt(SourceLoc, int32_t);
  bool appendFloat(SourceLoc, float);
  bool appendArray(SourceLoc, int32_t, const View&, bool);
  bool arrayOffset(const View&, const std::vector<int32_t>&, size_t&, SourceLoc);
};

}  // namespace sysy::interp::detail

#endif  // SYSY_INTERPRETER_INTERNAL_H
