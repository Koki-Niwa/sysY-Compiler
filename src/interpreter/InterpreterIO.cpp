#include "interpreter/InterpreterInternal.h"

namespace sysy::interp::detail {

bool Engine::readInputInt(int32_t& out, SourceLoc loc) {
  std::string token;
  if (!input_.token(token)) return fail(RunStatus::RuntimeError, loc, "missing integer input");
  char* end = nullptr;
  errno = 0;
  const long long value = std::strtoll(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0' || errno == ERANGE)
    return fail(RunStatus::RuntimeError, loc, "invalid integer input");
  out = static_cast<int32_t>(static_cast<uint32_t>(value));
  return true;
}

bool Engine::readInputFloat(float& out, SourceLoc loc) {
  std::string token;
  if (!input_.token(token)) return fail(RunStatus::RuntimeError, loc, "missing float input");
  char* end = nullptr;
  errno = 0;
  out = std::strtof(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0' || errno == ERANGE)
    return fail(RunStatus::RuntimeError, loc, "invalid float input");
  return true;
}

bool Engine::readInputChar(char& out, SourceLoc loc) {
  if (!input_.character(out)) return fail(RunStatus::RuntimeError, loc, "missing character input");
  return true;
}

bool Engine::appendInt(SourceLoc loc, int32_t value) { return append(loc, std::to_string(value)); }

bool Engine::appendFloat(SourceLoc loc, float value) {
  char text[128];
  const int n = std::snprintf(text, sizeof(text), "%a", static_cast<double>(value));
  if (n < 0 || static_cast<size_t>(n) >= sizeof(text))
    return fail(RunStatus::RuntimeError, loc, "float formatting failed");
  return append(loc, std::string_view(text, static_cast<size_t>(n)));
}

bool Engine::appendArray(SourceLoc loc, int32_t count, const View& view, bool floats) {
  if (count < 0) return append(loc, std::to_string(count) + ":\n");
  size_t available = 0;
  if (!checkedShapeProduct(view.shape, available))
    return fail(RunStatus::ResourceLimit, loc, "array shape exceeds interpreter limit");
  if (static_cast<size_t>(count) > available || static_cast<size_t>(count) > limits_.maxElements)
    return fail(RunStatus::RuntimeError, loc, "array I/O count exceeds view");
  if (!append(loc, std::to_string(count) + ":")) return false;
  for (int32_t i = 0; i < count; ++i) {
    if (!append(loc, " ")) return false;
    const size_t step = static_cast<size_t>(i);
    if (view.base > std::numeric_limits<size_t>::max() - step)
      return fail(RunStatus::ResourceLimit, loc, "array output offset exceeds interpreter limit");
    const size_t idx = view.base + step;
    Scalar value;
    if (!readElement(Cell{view.storage, idx, false}, value, loc)) return false;
    if (floats) {
      if (!appendFloat(loc, value.isFloat ? value.f : static_cast<float>(value.i))) return false;
    } else if (!appendInt(loc, value.isFloat ? satFloatToInt(value.f) : value.i)) {
      return false;
    }
  }
  return append(loc, "\n");
}

}  // namespace sysy::interp::detail
