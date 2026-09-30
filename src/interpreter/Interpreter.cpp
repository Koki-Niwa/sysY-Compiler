#include "interpreter/InterpreterInternal.h"
namespace sysy::interp {
namespace detail {

bool Engine::fail(RunStatus status, SourceLoc loc, std::string message) {
  if (result_.status == RunStatus::Returned || result_.status == RunStatus::InvalidAst) {
    result_.status = status;
    result_.location = loc;
    result_.diagnostic = std::move(message);
  }
  return false;
}
bool Engine::tick(SourceLoc loc) {
  if (steps_++ >= limits_.maxSteps)
    return fail(RunStatus::ResourceLimit, loc, "interpreter step limit exceeded");
  return true;
}
bool Engine::append(SourceLoc loc, std::string_view text) {
  if (result_.stdoutText.size() + text.size() > limits_.maxOutputBytes)
    return fail(RunStatus::ResourceLimit, loc, "interpreter output limit exceeded");
  result_.stdoutText.append(text.data(), text.size());
  return true;
}
const FuncDef* Engine::function(const std::string& name) const {
  const auto it = functions_.find(name);
  return it == functions_.end() ? nullptr : it->second;
}
Binding* Engine::lookup(const std::string& name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    const auto found = it->find(name);
    if (found != it->end()) return &found->second;
  }
  return nullptr;
}
const Binding* Engine::lookup(const std::string& name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    const auto found = it->find(name);
    if (found != it->end()) return &found->second;
  }
  return nullptr;
}
std::shared_ptr<Object> Engine::makeObject(const Type* type, bool isConst, SourceLoc loc) {
  if (type == nullptr) { fail(RunStatus::InvalidAst, loc, "declaration has no analyzed type"); return nullptr; }
  size_t count = 0;
  if (!typeCount(type, limits_.maxElements, count) || (count == 0 && !type->isArray())) {
    fail(RunStatus::ResourceLimit, loc, "object size is unsupported or exceeds interpreter limit");
    return nullptr;
  }
  const uint64_t count64 = static_cast<uint64_t>(count);
  if (count64 > limits_.maxTotalElements || totalElements_ > limits_.maxTotalElements - count64) {
    fail(RunStatus::ResourceLimit, loc, "total interpreter element limit exceeded");
    return nullptr;
  }
  auto object = std::make_shared<Object>();
  object->type = type; object->isConst = isConst; object->storage = std::make_shared<Storage>();
  object->storage->cells.assign(count, isFloatType(elementType(type)) ? Scalar::floating(0.0f)
                                                                       : Scalar::integer(0));
  object->storage->initialized.assign(count, 0);
  totalElements_ += count64;
  return object;
}
View Engine::objectView(const Object& object) const {
  return View{object.storage, 0, typeShape(object.type), isFloatType(elementType(object.type))};
}

View Engine::bindingView(const Binding& binding) const {
  return binding.isArrayParameter ? binding.parameter : objectView(*binding.object);
}

bool Engine::validateUnit() {
  const FuncDef* main = nullptr;
  for (const auto& item : unit_.items) {
    if (item == nullptr) continue;
    if (item->nodeKind() == std::string_view("FuncDef")) {
      const auto* fn = static_cast<const FuncDef*>(item.get());
      if (functions_.find(fn->name) != functions_.end())
        return fail(RunStatus::InvalidAst, fn->loc, "duplicate function in analyzed AST");
      functions_[fn->name] = fn;
      if (fn->name == "main") main = fn;
    } else if (item->nodeKind() != std::string_view("Decl")) {
      return fail(RunStatus::Unsupported, item->loc, "unsupported AST top-level node");
    }
  }
  if (main == nullptr || main->isVoid || main->retType != BType::Int || !main->params.empty() ||
      main->body == nullptr)
    return fail(RunStatus::InvalidAst, main == nullptr ? SourceLoc() : main->loc,
                "analyzed AST has no valid int main()");
  return true;
}

bool Engine::executeGlobalDecl(const Decl& decl) {
  for (const auto& def : decl.defs) {
    if (def == nullptr || def->semType == nullptr)
      return fail(RunStatus::InvalidAst, decl.loc, "global declaration is not analyzed");
    auto object = makeObject(def->semType, decl.isConst, def->loc);
    if (object == nullptr) return false;
    scopes_.back()[def->name] = Binding{def->semType, decl.isConst, object, View(), false};
    if (!initializeObject(*object, def->init.get(), def->loc)) return false;
  }
  return true;
}

bool Engine::executeDecl(const Decl& decl) {
  for (const auto& def : decl.defs) {
    if (!tick(def == nullptr ? decl.loc : def->loc)) return false;
    if (def == nullptr || def->semType == nullptr)
      return fail(RunStatus::InvalidAst, decl.loc, "declaration is not analyzed");
    auto object = makeObject(def->semType, decl.isConst, def->loc);
    if (object == nullptr) return false;
    scopes_.back()[def->name] = Binding{def->semType, decl.isConst, object, View(), false};
    if (def->init != nullptr && !initializeObject(*object, def->init.get(), def->loc)) return false;
  }
  return true;
}

bool Engine::initializeObject(Object& object, const InitVal* init, SourceLoc loc) {
  if (init == nullptr) {
    if (scopes_.size() == 1) {
      std::fill(object.storage->initialized.begin(), object.storage->initialized.end(), 1);
    }
    return true;
  }
  if (!object.type->isArray()) return initializeScalar(object, init, loc);
  std::fill(object.storage->cells.begin(), object.storage->cells.end(),
            isFloatType(elementType(object.type)) ? Scalar::floating(0.0f) : Scalar::integer(0));
  const bool global = scopes_.size() == 1;
  std::fill(object.storage->initialized.begin(), object.storage->initialized.end(), global ? 1 : 0);
  if (!initializeArray(object, init, loc)) return false;
  if (!global) std::fill(object.storage->initialized.begin(), object.storage->initialized.end(), 1);
  return true;
}

bool Engine::initializeScalar(Object& object, const InitVal* init, SourceLoc loc) {
  const InitVal* current = init;
  while (current != nullptr && current->expr == nullptr && current->list.size() == 1)
    current = current->list[0].get();
  if (current == nullptr || (current->expr == nullptr && current->list.empty())) {
    object.storage->cells[0] = isFloatType(object.type) ? Scalar::floating(0.0f) : Scalar::integer(0);
    object.storage->initialized[0] = 1;
    return true;
  }
  if (current->expr == nullptr)
    return fail(RunStatus::InvalidAst, current->loc, "too many scalar initializer elements");
  Value value;
  if (!evalExpr(current->expr.get(), value)) return false;
  Value converted;
  if (!coerce(value, object.type, converted, current->expr->loc)) return false;
  return writeElement(Cell{object.storage, 0, false}, converted.scalar, loc);
}

bool Engine::initializeArray(Object& object, const InitVal* init, SourceLoc loc) {
  (void)loc;
  if (init->expr != nullptr)
    return fail(RunStatus::InvalidAst, init->loc, "array has a scalar initializer");
  return initGroup(object, *init, 0, 1, 0);
}

bool Engine::initGroup(Object& object, const InitVal& group, int level, int depth, size_t start) {
  if (!tick(group.loc)) return false;
  if (static_cast<uint32_t>(depth) > limits_.maxInitDepth)
    return fail(RunStatus::ResourceLimit, group.loc, "initializer nesting limit exceeded");
  const int rankValue = rank(object.type);
  size_t cap = 1;
  if (level < rankValue && !typeCount(dropDims(object.type, level), limits_.maxElements, cap))
    return fail(RunStatus::ResourceLimit, group.loc, "array initializer extent exceeds interpreter limit");
  if (start > object.storage->cells.size() || cap > object.storage->cells.size() - start)
    return fail(RunStatus::InvalidAst, group.loc, "array initializer exceeds object size");
  const size_t end = start + cap;
  size_t position = start;
  for (const auto& child : group.list) {
    if (child == nullptr) continue;
    if (child->expr != nullptr) {
      if (position >= end || position >= object.storage->cells.size())
        return fail(RunStatus::InvalidAst, child->loc, "array initializer exceeds object size");
      Value value;
      if (!evalExpr(child->expr.get(), value)) return false;
      Value converted;
      if (!coerce(value, elementType(object.type), converted, child->expr->loc)) return false;
      if (!writeElement(Cell{object.storage, position, false}, converted.scalar, child->loc))
        return false;
      ++position;
      continue;
    }
    const int nestedDepth = depth + 1;
    const InitVal* inner = child.get();
    if (nestedDepth > rankValue) {
      while (inner->expr == nullptr && inner->list.size() == 1) inner = inner->list[0].get();
      if (inner->expr != nullptr) {
        if (position >= end) return fail(RunStatus::InvalidAst, child->loc, "initializer exceeds dimension");
        Value value;
        if (!evalExpr(inner->expr.get(), value)) return false;
        Value converted;
        if (!coerce(value, elementType(object.type), converted, inner->expr->loc)) return false;
        if (!writeElement(Cell{object.storage, position, false}, converted.scalar, child->loc))
          return false;
        ++position;
      } else if (!inner->list.empty()) {
        return fail(RunStatus::InvalidAst, child->loc, "initializer is nested beyond array rank");
      }
      continue;
    }
    int childLevel = rankValue;
    for (int k = level + 1; k <= rankValue - 1; ++k) {
      size_t childSize = 0;
      if (!typeCount(dropDims(object.type, k), limits_.maxElements, childSize))
        return fail(RunStatus::ResourceLimit, child->loc, "array initializer extent exceeds interpreter limit");
      if (childSize > 0 && position % childSize == 0) {
        childLevel = k; break;
      }
    }
    if (!initGroup(object, *child, childLevel, nestedDepth, position)) return false;
    size_t childSize = 1;
    if (childLevel < rankValue &&
        !typeCount(dropDims(object.type, childLevel), limits_.maxElements, childSize))
      return fail(RunStatus::ResourceLimit, child->loc, "array initializer extent exceeds interpreter limit");
    if (childSize > object.storage->cells.size() - position)
      return fail(RunStatus::InvalidAst, child->loc, "array initializer exceeds object size");
    position += childSize;
  }
  if (position > end || position > object.storage->cells.size())
    return fail(RunStatus::InvalidAst, group.loc, "array initializer exceeds dimension");
  return true;
}

bool Engine::writeElement(const Cell& cell, Scalar value, SourceLoc loc) {
  if (cell.storage == nullptr || cell.index >= cell.storage->cells.size())
    return fail(RunStatus::RuntimeError, loc, "array write is out of bounds");
  if (cell.isConst) return fail(RunStatus::RuntimeError, loc, "assignment to const object");
  cell.storage->cells[cell.index] = value; cell.storage->initialized[cell.index] = 1; return true;
}

bool Engine::readElement(const Cell& cell, Scalar& value, SourceLoc loc) {
  if (cell.storage == nullptr || cell.index >= cell.storage->cells.size())
    return fail(RunStatus::RuntimeError, loc, "array read is out of bounds");
  if (cell.storage->initialized[cell.index] == 0)
    return fail(RunStatus::RuntimeError, loc, "read of uninitialized local object");
  value = cell.storage->cells[cell.index]; return true;
}

bool Engine::arrayOffset(const View& view, const std::vector<int32_t>& indices,
                         size_t& offset, SourceLoc loc) {
  if (indices.size() > view.shape.size())
    return fail(RunStatus::RuntimeError, loc, "too many array indices");
  offset = view.base;
  for (size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] < 0 || static_cast<size_t>(indices[i]) >= view.shape[i])
      return fail(RunStatus::RuntimeError, loc, "array index is out of bounds");
    size_t stride = 1;
    for (size_t j = i + 1; j < view.shape.size(); ++j) {
      if (!checkedMultiply(stride, view.shape[j], stride))
        return fail(RunStatus::ResourceLimit, loc, "array stride exceeds interpreter limit");
    }
    size_t term = 0;
    if (!checkedMultiply(static_cast<size_t>(indices[i]), stride, term) ||
        offset > std::numeric_limits<size_t>::max() - term)
      return fail(RunStatus::ResourceLimit, loc, "array offset exceeds interpreter limit");
    offset += term;
  }
  return true;
}

bool Engine::evalView(const LVal& lval, View& out) {
  const Binding* binding = lookup(lval.name);
  if (binding == nullptr) return fail(RunStatus::RuntimeError, lval.loc, "unknown lvalue");
  View view = bindingView(*binding);
  std::vector<int32_t> indices;
  for (const auto& index : lval.indices) {
    Value value;
    if (!evalExpr(index.get(), value)) return false;
    int32_t i = 0;
    if (!asInt(value, i, index == nullptr ? lval.loc : index->loc)) return false;
    indices.push_back(i);
  }
  size_t offset = 0;
  if (!arrayOffset(view, indices, offset, lval.loc)) return false;
  view.base = offset;
  view.shape.erase(view.shape.begin(), view.shape.begin() + indices.size());
  out = std::move(view); return true;
}

bool Engine::evalCell(const LVal& lval, Cell& out) {
  const Binding* binding = lookup(lval.name);
  if (binding == nullptr) return fail(RunStatus::RuntimeError, lval.loc, "unknown lvalue");
  if (binding->type == nullptr) return fail(RunStatus::InvalidAst, lval.loc, "lvalue has no type");
  if (!binding->type->isArray() && lval.indices.empty()) {
    out = Cell{binding->object->storage, 0, binding->isConst}; return true;
  }
  View view;
  if (!evalView(lval, view)) return false;
  if (!view.shape.empty()) return fail(RunStatus::RuntimeError, lval.loc, "array lvalue is not scalar");
  out = Cell{view.storage, view.base, binding->isConst}; return true;
}

bool Engine::evalLVal(const LVal& lval, Value& out) {
  if (lval.type != nullptr && lval.type->isArray()) {
    View view;
    if (!evalView(lval, view)) return false;
    out = Value::arrayValue(std::move(view)); return true;
  }
  Cell cell;
  if (!evalCell(lval, cell)) return false;
  Scalar scalar;
  if (!readElement(cell, scalar, lval.loc)) return false;
  out = Value::scalarValue(scalar); return true;
}

bool Engine::asInt(const Value& value, int32_t& out, SourceLoc loc) {
  if (value.kind != Value::Kind::Scalar) return fail(RunStatus::RuntimeError, loc, "expected scalar integer");
  out = value.scalar.isFloat ? satFloatToInt(value.scalar.f) : value.scalar.i; return true;
}

bool Engine::asBool(const Value& value, bool& out, SourceLoc loc) {
  if (value.kind != Value::Kind::Scalar) return fail(RunStatus::RuntimeError, loc, "expected scalar condition");
  out = value.scalar.isFloat ? value.scalar.f != 0.0f : value.scalar.i != 0; return true;
}

bool Engine::coerce(const Value& value, const Type* target, Value& out, SourceLoc loc) {
  if (target == nullptr || target->isArray()) return fail(RunStatus::RuntimeError, loc, "invalid scalar conversion");
  if (value.kind != Value::Kind::Scalar) return fail(RunStatus::RuntimeError, loc, "array used as scalar");
  out = Value::scalarValue(isFloatType(target) ? Scalar::floating(value.scalar.isFloat ? value.scalar.f
                                                                                       : static_cast<float>(value.scalar.i))
                                               : Scalar::integer(value.scalar.isFloat ? satFloatToInt(value.scalar.f)
                                                                                      : value.scalar.i));
  return true;
}

bool Engine::evalExpr(const Expr* expr, Value& out) {
  if (expr == nullptr) return fail(RunStatus::InvalidAst, SourceLoc(), "missing expression");
  if (expressionDepth_ >= limits_.maxExpressionDepth)
    return fail(RunStatus::ResourceLimit, expr->loc, "expression depth limit exceeded");
  ++expressionDepth_;
  struct ExpressionDepthGuard {
    uint32_t& depth;
    ~ExpressionDepthGuard() { --depth; }
  } depthGuard{expressionDepth_};
  if (!tick(expr->loc)) return false;
  const std::string_view kind = expr->nodeKind();
  if (kind == "IntLit") {
    int32_t value = 0; std::string error;
    if (!ConstEvaluator::parseIntLit(*static_cast<const IntLit*>(expr), value, error))
      return fail(RunStatus::InvalidAst, expr->loc, error);
    out = Value::scalarValue(Scalar::integer(value)); return true;
  }
  if (kind == "FloatLit") {
    float value = 0.0f; std::string error;
    if (!ConstEvaluator::parseFloatLit(*static_cast<const FloatLit*>(expr), value, error))
      return fail(RunStatus::InvalidAst, expr->loc, error);
    out = Value::scalarValue(Scalar::floating(value)); return true;
  }
  if (kind == "LVal") return evalLVal(*static_cast<const LVal*>(expr), out);
  if (kind == "Call") return evalCall(*static_cast<const Call*>(expr), out);
  if (kind == "Cast") {
    const auto* cast = static_cast<const Cast*>(expr);
    Value operand;
    if (!evalExpr(cast->operand.get(), operand)) return false;
    if (cast->kind == CastKind::ToBool) {
      bool value = false; if (!asBool(operand, value, expr->loc)) return false;
      out = Value::scalarValue(Scalar::integer(value ? 1 : 0)); return true;
    }
    return coerce(operand, cast->target, out, expr->loc);
  }
  if (kind == "Unary") {
    const auto* unary = static_cast<const Unary*>(expr);
    Value operand; if (!evalExpr(unary->operand.get(), operand)) return false;
    if (unary->op == TokKind::Not) { bool value; if (!asBool(operand, value, expr->loc)) return false;
      out = Value::scalarValue(Scalar::integer(value ? 0 : 1)); return true; }
    if (operand.kind != Value::Kind::Scalar)
      return fail(RunStatus::RuntimeError, expr->loc, "unary operator needs scalar");
    if (unary->op == TokKind::Plus) { out = operand; return true; }
    out = Value::scalarValue(operand.scalar.isFloat ? Scalar::floating(-operand.scalar.f)
                                                     : Scalar::integer(wrapSub(0, operand.scalar.i)));
    return true;
  }
  if (kind == "Binary") {
    const auto* binary = static_cast<const Binary*>(expr);
    Value lhs; if (!evalExpr(binary->lhs.get(), lhs)) return false;
    if (binary->op == TokKind::AmpAmp || binary->op == TokKind::PipePipe) {
      bool left = false; if (!asBool(lhs, left, expr->loc)) return false;
      if ((binary->op == TokKind::AmpAmp && !left) || (binary->op == TokKind::PipePipe && left)) {
        out = Value::scalarValue(Scalar::integer(binary->op == TokKind::PipePipe ? 1 : 0)); return true;
      }
      Value rhs; if (!evalExpr(binary->rhs.get(), rhs)) return false;
      bool right = false; if (!asBool(rhs, right, expr->loc)) return false;
      out = Value::scalarValue(Scalar::integer(right ? 1 : 0)); return true;
    }
    Value rhs; if (!evalExpr(binary->rhs.get(), rhs)) return false;
    if (lhs.kind != Value::Kind::Scalar || rhs.kind != Value::Kind::Scalar)
      return fail(RunStatus::RuntimeError, expr->loc, "binary operator needs scalars");
    const bool fp = lhs.scalar.isFloat || rhs.scalar.isFloat;
    if (binary->op == TokKind::Less || binary->op == TokKind::Greater ||
        binary->op == TokKind::LessEq || binary->op == TokKind::GreaterEq ||
        binary->op == TokKind::EqEq || binary->op == TokKind::NotEq) {
      bool value = false;
      if (fp) {
        const float a = lhs.scalar.isFloat ? lhs.scalar.f : static_cast<float>(lhs.scalar.i);
        const float b = rhs.scalar.isFloat ? rhs.scalar.f : static_cast<float>(rhs.scalar.i);
        switch (binary->op) { case TokKind::Less: value = a < b; break; case TokKind::Greater: value = a > b; break;
          case TokKind::LessEq: value = a <= b; break; case TokKind::GreaterEq: value = a >= b; break;
          case TokKind::EqEq: value = a == b; break; case TokKind::NotEq: value = a != b; break; default: break; }
      } else {
        const int32_t a = lhs.scalar.i, b = rhs.scalar.i;
        switch (binary->op) { case TokKind::Less: value = a < b; break; case TokKind::Greater: value = a > b; break;
          case TokKind::LessEq: value = a <= b; break; case TokKind::GreaterEq: value = a >= b; break;
          case TokKind::EqEq: value = a == b; break; case TokKind::NotEq: value = a != b; break; default: break; }
      }
      out = Value::scalarValue(Scalar::integer(value ? 1 : 0)); return true;
    }
    if (binary->op == TokKind::Percent) {
      if (fp) return fail(RunStatus::RuntimeError, expr->loc, "floating remainder in analyzed AST");
      out = Value::scalarValue(Scalar::integer(normRem(lhs.scalar.i, rhs.scalar.i))); return true;
    }
    if (fp) {
      const float a = lhs.scalar.isFloat ? lhs.scalar.f : static_cast<float>(lhs.scalar.i);
      const float b = rhs.scalar.isFloat ? rhs.scalar.f : static_cast<float>(rhs.scalar.i);
      float value = 0.0f;
      switch (binary->op) { case TokKind::Plus: value = a + b; break; case TokKind::Minus: value = a - b; break;
        case TokKind::Star: value = a * b; break; case TokKind::Slash: value = a / b; break;
        default: return fail(RunStatus::RuntimeError, expr->loc, "unsupported floating operator"); }
      out = Value::scalarValue(Scalar::floating(value)); return true;
    }
    int32_t value = 0;
    switch (binary->op) { case TokKind::Plus: value = wrapAdd(lhs.scalar.i, rhs.scalar.i); break;
      case TokKind::Minus: value = wrapSub(lhs.scalar.i, rhs.scalar.i); break;
      case TokKind::Star: value = wrapMul(lhs.scalar.i, rhs.scalar.i); break;
      case TokKind::Slash: value = normDiv(lhs.scalar.i, rhs.scalar.i); break;
      default: return fail(RunStatus::RuntimeError, expr->loc, "unsupported integer operator"); }
    out = Value::scalarValue(Scalar::integer(value)); return true;
  }
  return fail(RunStatus::Unsupported, expr->loc, "unsupported AST expression node");
}

bool Engine::evalCall(const Call& call, Value& out) {
  std::vector<Value> args; args.reserve(call.args.size());
  for (const auto& arg : call.args) { Value value; if (!evalExpr(arg.get(), value)) return false; args.push_back(std::move(value)); }
  if (isRuntimeFuncName(call.callee)) return evalRuntimeCall(call, args, out);
  const FuncDef* fn = function(call.callee);
  if (fn == nullptr) return fail(RunStatus::RuntimeError, call.loc, "unknown function call");
  return invokeUser(*fn, args, out);
}

bool Engine::invokeUser(const FuncDef& fn, const std::vector<Value>& args, Value& out) {
  if (callDepth_++ >= limits_.maxCallDepth) { --callDepth_; return fail(RunStatus::ResourceLimit, fn.loc, "call depth limit exceeded"); }
  if (args.size() != fn.params.size()) { --callDepth_; return fail(RunStatus::InvalidAst, fn.loc, "function argument count mismatch"); }
  scopes_.push_back(Scope());
  for (size_t i = 0; i < args.size(); ++i) {
    const Param& param = fn.params[i];
    if (param.semType == nullptr) { scopes_.pop_back(); --callDepth_; return fail(RunStatus::InvalidAst, param.loc, "parameter is not analyzed"); }
    if (param.semType->isArray()) {
      View view = args[i].array;
      if (rank(param.semType) == 1) {
        size_t count = 0;
        if (!checkedShapeProduct(view.shape, count)) {
          scopes_.pop_back();
          --callDepth_;
          return fail(RunStatus::ResourceLimit, param.loc, "array parameter shape exceeds interpreter limit");
        }
        view.shape.assign(1, count);
      }
      scopes_.back()[param.name] = Binding{param.semType, false, nullptr, std::move(view), true};
    }
    else { auto object = makeObject(param.semType, false, param.loc); if (object == nullptr) { scopes_.pop_back(); --callDepth_; return false; }
      object->storage->cells[0] = args[i].scalar; object->storage->initialized[0] = 1;
      scopes_.back()[param.name] = Binding{param.semType, false, object, View(), false}; }
  }
  Control control;
  const bool ok = fn.body != nullptr && execBlock(*fn.body, control);
  if (!ok) { scopes_.pop_back(); --callDepth_; return false; }
  scopes_.pop_back(); --callDepth_;
  if (control.kind == Control::Kind::Return) { out = std::move(control.value); return true; }
  if (fn.isVoid) { out = Value::voidValue(); return true; }
  // S05 defines a non-void fallthrough as returning the normalized zero value.
  out = Value::scalarValue(fn.retType == BType::Float ? Scalar::floating(0.0f)
                                                       : Scalar::integer(0));
  return true;
}

bool Engine::execBlock(const BlockStmt& block, Control& out) {
  if (statementDepth_ >= limits_.maxStatementDepth)
    return fail(RunStatus::ResourceLimit, block.loc, "statement nesting depth limit exceeded");
  ++statementDepth_;
  struct StatementDepthGuard {
    uint32_t& depth;
    ~StatementDepthGuard() { --depth; }
  } depthGuard{statementDepth_};
  scopes_.push_back(Scope());
  for (const auto& item : block.items) {
    if (item == nullptr) continue;
    if (!execStmt(item.get(), out)) { scopes_.pop_back(); return false; }
    if (out.kind != Control::Kind::Normal) break;
  }
  scopes_.pop_back(); return true;
}

bool Engine::execWhile(const WhileStmt& stmt, Control& out) {
  for (;;) {
    Value condition; if (!evalExpr(stmt.cond.get(), condition)) return false;
    bool truth = false; if (!asBool(condition, truth, stmt.loc)) return false;
    if (!truth) { out = Control::normal(); return true; }
    Control body; if (!execStmt(stmt.body.get(), body)) return false;
    if (body.kind == Control::Kind::Return) { out = std::move(body); return true; }
    if (body.kind == Control::Kind::Break) { out = Control::normal(); return true; }
    if (body.kind == Control::Kind::Continue) continue;
  }
}

bool Engine::execStmt(const Node* node, Control& out) {
  if (node == nullptr) return true;
  if (statementDepth_ >= limits_.maxStatementDepth)
    return fail(RunStatus::ResourceLimit, node->loc, "statement nesting depth limit exceeded");
  ++statementDepth_;
  struct StatementDepthGuard {
    uint32_t& depth;
    ~StatementDepthGuard() { --depth; }
  } depthGuard{statementDepth_};
  if (!tick(node->loc)) return false;
  const std::string_view kind = node->nodeKind();
  if (kind == "BlockStmt") return execBlock(*static_cast<const BlockStmt*>(node), out);
  if (kind == "Decl") return executeDecl(*static_cast<const Decl*>(node));
  if (kind == "ExprStmt") { const auto* stmt = static_cast<const ExprStmt*>(node); if (stmt->expr != nullptr) { Value ignored; if (!evalExpr(stmt->expr.get(), ignored)) return false; } out = Control::normal(); return true; }
  if (kind == "AssignStmt") { const auto* stmt = static_cast<const AssignStmt*>(node); Cell cell; if (!evalCell(*stmt->lhs, cell)) return false; Value rhs; if (!evalExpr(stmt->rhs.get(), rhs)) return false; Value converted; if (!coerce(rhs, stmt->lhs->type, converted, stmt->loc)) return false; if (!writeElement(cell, converted.scalar, stmt->loc)) return false; out = Control::normal(); return true; }
  if (kind == "IfStmt") { const auto* stmt = static_cast<const IfStmt*>(node); Value condition; if (!evalExpr(stmt->cond.get(), condition)) return false; bool truth; if (!asBool(condition, truth, stmt->loc)) return false; return truth ? execStmt(stmt->thenS.get(), out) : execStmt(stmt->elseS.get(), out); }
  if (kind == "WhileStmt") return execWhile(*static_cast<const WhileStmt*>(node), out);
  if (kind == "BreakStmt") { out = Control::signal(Control::Kind::Break); return true; }
  if (kind == "ContinueStmt") { out = Control::signal(Control::Kind::Continue); return true; }
  if (kind == "ReturnStmt") { const auto* stmt = static_cast<const ReturnStmt*>(node); if (stmt->value == nullptr) { out = Control::returned(Value::voidValue()); return true; } Value value; if (!evalExpr(stmt->value.get(), value)) return false; out = Control::returned(std::move(value)); return true; }
  return fail(RunStatus::Unsupported, node->loc, "unsupported AST statement node");
}

RunResult Engine::execute() {
  result_.status = RunStatus::InvalidAst;
  if (input_.text.size() > limits_.maxInputBytes) {
    fail(RunStatus::ResourceLimit, SourceLoc(), "interpreter input limit exceeded");
    return result_;
  }
  if (!validateUnit()) return result_;
  scopes_.push_back(Scope());
  for (const auto& item : unit_.items) {
    if (item != nullptr && item->nodeKind() == std::string_view("Decl") &&
        !executeGlobalDecl(*static_cast<const Decl*>(item.get()))) return result_;
  }
  Value value;
  const FuncDef* main = function("main");
  if (main == nullptr || !invokeUser(*main, {}, value)) return result_;
  if (value.kind != Value::Kind::Scalar || value.scalar.isFloat)
    return fail(RunStatus::InvalidAst, main->loc, "main did not return int"), result_;
  result_.mainReturn = value.scalar.i; result_.status = RunStatus::Returned; return result_;
}

}  // namespace detail
RunResult run(const CompUnit& unit, std::string_view input, const Limits& limits) {
  return detail::Engine(unit, input, limits).execute();
}

const char* statusName(RunStatus status) {
  switch (status) { case RunStatus::Returned: return "Returned"; case RunStatus::InvalidAst: return "InvalidAst";
    case RunStatus::Unsupported: return "Unsupported"; case RunStatus::ResourceLimit: return "ResourceLimit";
    case RunStatus::RuntimeError: return "RuntimeError"; }
  return "?";
}
}  // namespace sysy::interp
