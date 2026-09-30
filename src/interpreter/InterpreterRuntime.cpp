#include "interpreter/InterpreterInternal.h"

#include "support/RuntimeLib.h"

namespace sysy::interp::detail {

bool Engine::evalRuntimeCall(const Call& call, const std::vector<Value>& args, Value& out) {
  const runtime::Signature* signature = runtime::lookup(call.callee);
  if (signature == nullptr) {
    return fail(RunStatus::Unsupported, call.loc, "unsupported runtime function");
  }

  switch (signature->behavior) {
    case runtime::Behavior::ReadInt: {
      if (!args.empty())
        return fail(RunStatus::InvalidAst, call.loc, "runtime argument count mismatch");
      int32_t value;
      if (!readInputInt(value, call.loc)) return false;
      out = Value::scalarValue(Scalar::integer(value));
      return true;
    }
    case runtime::Behavior::ReadChar: {
      if (!args.empty())
        return fail(RunStatus::InvalidAst, call.loc, "runtime argument count mismatch");
      char value;
      if (!readInputChar(value, call.loc)) return false;
      out = Value::scalarValue(Scalar::integer(static_cast<unsigned char>(value)));
      return true;
    }
    case runtime::Behavior::ReadFloat: {
      if (!args.empty())
        return fail(RunStatus::InvalidAst, call.loc, "runtime argument count mismatch");
      float value;
      if (!readInputFloat(value, call.loc)) return false;
      out = Value::scalarValue(Scalar::floating(value));
      return true;
    }
    case runtime::Behavior::ReadIntArray:
    case runtime::Behavior::ReadFloatArray: {
      if (args.size() != 1 || args[0].kind != Value::Kind::Array)
        return fail(RunStatus::InvalidAst, call.loc, "runtime array argument mismatch");
      const bool readFloat = signature->behavior == runtime::Behavior::ReadFloatArray;
      int32_t count;
      if (!readInputInt(count, call.loc)) return false;
      if (count < 0) {
        out = Value::scalarValue(Scalar::integer(count));
        return true;
      }
      View view = args[0].array;
      size_t available = 0;
      if (!checkedShapeProduct(view.shape, available))
        return fail(RunStatus::ResourceLimit, call.loc,
                    "array shape exceeds interpreter limit");
      if (static_cast<size_t>(count) > available ||
          static_cast<size_t>(count) > limits_.maxElements) {
        return fail(RunStatus::RuntimeError, call.loc, "array input count exceeds view");
      }
      for (int32_t i = 0; i < count; ++i) {
        const size_t step = static_cast<size_t>(i);
        if (view.base > std::numeric_limits<size_t>::max() - step) {
          return fail(RunStatus::ResourceLimit, call.loc,
                      "array input offset exceeds interpreter limit");
        }
        const size_t index = view.base + step;
        if (readFloat) {
          float value;
          if (!readInputFloat(value, call.loc)) return false;
          if (!writeElement(Cell{view.storage, index, false}, Scalar::floating(value), call.loc))
            return false;
        } else {
          int32_t value;
          if (!readInputInt(value, call.loc)) return false;
          if (!writeElement(Cell{view.storage, index, false}, Scalar::integer(value), call.loc))
            return false;
        }
      }
      out = Value::scalarValue(Scalar::integer(count));
      return true;
    }
    case runtime::Behavior::WriteInt:
    case runtime::Behavior::WriteChar:
    case runtime::Behavior::WriteFloat: {
      if (args.size() != 1 || args[0].kind != Value::Kind::Scalar)
        return fail(RunStatus::InvalidAst, call.loc, "runtime scalar argument mismatch");
      const Scalar& value = args[0].scalar;
      if (signature->behavior == runtime::Behavior::WriteFloat) {
        return appendFloat(call.loc, value.isFloat ? value.f : static_cast<float>(value.i));
      }
      const int32_t integer = value.isFloat ? satFloatToInt(value.f) : value.i;
      if (signature->behavior == runtime::Behavior::WriteChar) {
        const char character = static_cast<char>(integer);
        return append(call.loc, std::string_view(&character, 1));
      }
      return appendInt(call.loc, integer);
    }
    case runtime::Behavior::WriteIntArray:
    case runtime::Behavior::WriteFloatArray: {
      if (args.size() != 2 || args[0].kind != Value::Kind::Scalar ||
          args[1].kind != Value::Kind::Array) {
        return fail(RunStatus::InvalidAst, call.loc, "runtime array output argument mismatch");
      }
      int32_t count;
      if (!asInt(args[0], count, call.loc)) return false;
      const bool writeFloat = signature->behavior == runtime::Behavior::WriteFloatArray;
      return appendArray(call.loc, count, args[1].array, writeFloat);
    }
    case runtime::Behavior::NoOp:
      out = Value::voidValue();
      return true;
    case runtime::Behavior::Unsupported:
      break;
  }
  return fail(RunStatus::Unsupported, call.loc, "unsupported runtime function");
}

}  // namespace sysy::interp::detail
